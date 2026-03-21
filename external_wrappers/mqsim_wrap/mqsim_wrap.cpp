#include <cstdint>

#include <algorithm>
#include <cassert>
#include <deque>
#include <fstream>
#include <string>
#include <vector>

#include "exec/Execution_Parameter_Set.h"
#include "nvm_chip/flash_memory/Flash_Chip.h"
#include "nvm_chip/flash_memory/Physical_Page_Address.h"
#include "sim/Engine.h"
#include "ssd/NVM_PHY_ONFI_NVDDR2.h"
#include "ssd/NVM_Transaction_Flash_ER.h"
#include "ssd/NVM_Transaction_Flash_RD.h"
#include "ssd/NVM_Transaction_Flash_WR.h"
#include "ssd/ONFI_Channel_NVDDR2.h"
#include "ssd/SSD_Defs.h"
#include "ssd/TSU_Base.h"
#include "ssd/User_Request.h"
#include "utils/rapidxml/rapidxml.hpp"

// NOTE: This struct is part of the wrapper's C-ABI surface (used via dlsym).
// Keep it POD and stable. Units are MQSim sim_time_type (ns in stock MQSim).
struct mq_create_params_t {
  // Flash-level geometry
  uint32_t channel_count;
  uint32_t chip_no_per_channel;
  uint32_t die_no_per_chip;
  uint32_t plane_no_per_die;
  uint32_t block_no_per_plane;
  uint32_t page_no_per_block;
  uint32_t page_size_bytes;

  // Link model (NVDDR2 "two-unit" times)
  uint32_t channel_width_bytes;
  uint32_t two_unit_data_in_time;
  uint32_t two_unit_data_out_time;

  // Flash timing
  uint64_t t_read;
  uint64_t t_prog;
  uint64_t t_erase;
};

namespace {

struct mq_completion_t {
  void* user_ptr;
  uint64_t finish_time_ns;
};

struct mq_pending_req_t {
  uint32_t part_id;
  uint64_t part_addr;
  uint32_t size_bytes;
  int is_write;
  int source_id;
  void* user_ptr;
};

struct mq_handle_t;
static mq_handle_t* g_handle = nullptr;

class TSU_HBF_Simple final : public SSD_Components::TSU_Base {
 public:
  TSU_HBF_Simple(const sim_object_id_type& id,
                 SSD_Components::NVM_PHY_ONFI_NVDDR2* nvm_controller,
                 unsigned channel_count, unsigned chip_no_per_channel,
                 unsigned die_no_per_chip, unsigned plane_no_per_die)
      : SSD_Components::TSU_Base(id,
                                /*ftl*/ nullptr, nvm_controller,
                                SSD_Components::Flash_Scheduling_Type::OUT_OF_ORDER,
                                channel_count, chip_no_per_channel,
                                die_no_per_chip, plane_no_per_die,
                                /*EraseSuspensionEnabled*/ false,
                                /*ProgramSuspensionEnabled*/ false,
                                /*WriteReasonableSuspensionTimeForRead*/ 0,
                                /*EraseReasonableSuspensionTimeForRead*/ 0,
                                /*EraseReasonableSuspensionTimeForWrite*/ 0) {
    user_read_q = new SSD_Components::Flash_Transaction_Queue*[channel_count];
    user_write_q = new SSD_Components::Flash_Transaction_Queue*[channel_count];
    for (unsigned ch = 0; ch < channel_count; ++ch) {
      user_read_q[ch] =
          new SSD_Components::Flash_Transaction_Queue[chip_no_per_channel];
      user_write_q[ch] =
          new SSD_Components::Flash_Transaction_Queue[chip_no_per_channel];
      for (unsigned chip = 0; chip < chip_no_per_channel; ++chip) {
        user_read_q[ch][chip].Set_id("HBF_User_Read_Q@" + std::to_string(ch) +
                                     "@" + std::to_string(chip));
        user_write_q[ch][chip].Set_id("HBF_User_Write_Q@" + std::to_string(ch) +
                                      "@" + std::to_string(chip));
      }
    }
  }

  ~TSU_HBF_Simple() override {
    for (unsigned ch = 0; ch < channel_count; ++ch) {
      delete[] user_read_q[ch];
      delete[] user_write_q[ch];
    }
    delete[] user_read_q;
    delete[] user_write_q;
  }

  void Start_simulation() override {}
  void Validate_simulation_config() override {}
  void Execute_simulator_event(MQSimEngine::Sim_Event* /*event*/) override {}

  void Schedule() override {
    opened_scheduling_reqs--;
    if (opened_scheduling_reqs > 0) return;
    if (opened_scheduling_reqs < 0) {
      PRINT_ERROR("TSU_HBF_Simple: Illegal status!");
    }

    if (transaction_receive_slots.empty()) return;

    for (auto* tr : transaction_receive_slots) {
      switch (tr->Type) {
        case SSD_Components::Transaction_Type::READ:
          user_read_q[tr->Address.ChannelID][tr->Address.ChipID].push_back(tr);
          break;
        case SSD_Components::Transaction_Type::WRITE:
          user_write_q[tr->Address.ChannelID][tr->Address.ChipID].push_back(tr);
          break;
        default:
          // Ignore erase / unknown in this minimal model.
          break;
      }
    }
    transaction_receive_slots.clear();

    // If the channel is currently idle, try to issue immediately to reduce
    // queueing artifacts.
    for (flash_channel_ID_type channelID = 0; channelID < channel_count;
         channelID++) {
      if (_NVMController->Get_channel_status(channelID) ==
          SSD_Components::BusChannelStatus::IDLE) {
        for (unsigned i = 0; i < chip_no_per_channel; i++) {
          NVM::FlashMemory::Flash_Chip* chip = _NVMController->Get_chip(
              channelID, Round_robin_turn_of_channel[channelID]);
          process_chip_requests(chip);
          Round_robin_turn_of_channel[channelID] =
              (flash_chip_ID_type)(Round_robin_turn_of_channel[channelID] + 1) %
              chip_no_per_channel;
          if (_NVMController->Get_channel_status(chip->ChannelID) !=
              SSD_Components::BusChannelStatus::IDLE) {
            break;
          }
        }
      }
    }
  }

 private:
  SSD_Components::Flash_Transaction_Queue** user_read_q = nullptr;
  SSD_Components::Flash_Transaction_Queue** user_write_q = nullptr;

  bool service_read_transaction(NVM::FlashMemory::Flash_Chip* chip) override {
    if (user_read_q[chip->ChannelID][chip->ChipID].empty()) return false;
    if (_NVMController->GetChipStatus(chip) != SSD_Components::ChipStatus::IDLE)
      return false;
    issue_command_to_chip(&user_read_q[chip->ChannelID][chip->ChipID], nullptr,
                          SSD_Components::Transaction_Type::READ,
                          /*suspensionRequired*/ false);
    return true;
  }

  bool service_write_transaction(NVM::FlashMemory::Flash_Chip* chip) override {
    if (user_write_q[chip->ChannelID][chip->ChipID].empty()) return false;
    if (_NVMController->GetChipStatus(chip) != SSD_Components::ChipStatus::IDLE)
      return false;
    issue_command_to_chip(&user_write_q[chip->ChannelID][chip->ChipID], nullptr,
                          SSD_Components::Transaction_Type::WRITE,
                          /*suspensionRequired*/ false);
    return true;
  }

  bool service_erase_transaction(NVM::FlashMemory::Flash_Chip* /*chip*/) override {
    return false;
  }
};

struct mq_handle_t {
  // Geometry / timing parameters parsed from MQSim's XML config.
  unsigned channel_count = 0;
  unsigned chip_no_per_channel = 0;
  unsigned die_no_per_chip = 0;
  unsigned plane_no_per_die = 0;
  unsigned block_no_per_plane = 0;
  unsigned page_no_per_block = 0;
  unsigned page_size_bytes = 0;

  // Simulator objects
  std::vector<std::vector<NVM::FlashMemory::Flash_Chip*>> chips;
  std::vector<SSD_Components::ONFI_Channel_NVDDR2*> channels;
  SSD_Components::NVM_PHY_ONFI_NVDDR2* phy = nullptr;
  TSU_HBF_Simple* tsu = nullptr;

  uint64_t now_ns = 0;

  size_t max_pending = 1u << 20;
  std::deque<mq_pending_req_t> pending;
  std::deque<mq_completion_t> completions;
};

static page_status_type compute_sector_bitmap(unsigned page_size_bytes,
                                              uint64_t offset_in_page,
                                              unsigned size_bytes) {
  constexpr unsigned kSector = SECTOR_SIZE_IN_BYTE;  // 512B
  if (page_size_bytes == 0) page_size_bytes = kSector;
  if (size_bytes == 0) size_bytes = 1;

  const unsigned sectors_per_page = std::max(1u, page_size_bytes / kSector);
  const unsigned first_sector = (unsigned)(offset_in_page / kSector);
  const unsigned last_sector = (unsigned)((offset_in_page + size_bytes - 1) /
                                          kSector);

  const unsigned fs = std::min(first_sector, sectors_per_page - 1);
  const unsigned ls = std::min(last_sector, sectors_per_page - 1);
  const unsigned n = ls >= fs ? (ls - fs + 1) : 1;

  // page_status_type is uint64_t in MQSim; clamp.
  if (sectors_per_page >= 64) return FULL_PROGRAMMED_PAGE;
  if (n >= 64) return FULL_PROGRAMMED_PAGE;

  page_status_type mask = (((page_status_type)1) << n) - 1;
  return (mask << fs);
}

static NVM::FlashMemory::Physical_Page_Address map_to_ppa(const mq_handle_t* h,
                                                          uint32_t channel_id,
                                                          uint64_t part_addr) {
  const unsigned page_bytes = h->page_size_bytes ? h->page_size_bytes : 4096;
  const uint64_t page_index = part_addr / (uint64_t)page_bytes;

  const uint64_t unit_count =
      std::max<uint64_t>(1,
                         (uint64_t)h->chip_no_per_channel *
                             (uint64_t)h->die_no_per_chip *
                             (uint64_t)h->plane_no_per_die);

  const uint64_t unit_linear = page_index % unit_count;
  uint64_t rem_u = unit_linear;

  const uint64_t chips = std::max<uint64_t>(1, h->chip_no_per_channel);
  const uint64_t dies = std::max<uint64_t>(1, h->die_no_per_chip);
  const uint64_t planes = std::max<uint64_t>(1, h->plane_no_per_die);
  const uint64_t blocks = std::max<uint64_t>(1, h->block_no_per_plane);
  const uint64_t pages = std::max<uint64_t>(1, h->page_no_per_block);

  const flash_chip_ID_type chip_id = (flash_chip_ID_type)(rem_u % chips);
  rem_u /= chips;
  const flash_die_ID_type die_id = (flash_die_ID_type)(rem_u % dies);
  rem_u /= dies;
  const flash_plane_ID_type plane_id = (flash_plane_ID_type)(rem_u % planes);

  const uint64_t page2 = page_index / unit_count;
  const flash_page_ID_type page_id = (flash_page_ID_type)(page2 % pages);
  const flash_block_ID_type block_id =
      (flash_block_ID_type)((page2 / pages) % blocks);

  return NVM::FlashMemory::Physical_Page_Address(channel_id, chip_id, die_id,
                                                 plane_id, block_id, page_id);
}

static void tx_serviced_cb(SSD_Components::NVM_Transaction_Flash* tr) {
  if (!g_handle || !tr) return;

  void* user_ptr = nullptr;
  if (tr->UserIORequest) user_ptr = tr->UserIORequest->IO_command_info;

  // For co-simulation, we use posted-write semantics: the GPU sees a store ACK
  // as soon as the controller accepts the request (handled in mq_tick_to()).
  // Therefore, only READ transactions generate completion notifications here.
  if (tr->Type == SSD_Components::Transaction_Type::READ) {
    g_handle->completions.push_back({user_ptr, Simulator->Time()});
  }

  // IMPORTANT: MQSim deletes `tr` inside NVM_PHY_ONFI::broadcastTransactionServicedSignal()
  // after notifying all handlers. We must NOT delete `tr` here, or we'll double free.
  //
  // However, the minimal wrapper allocates `User_Request` objects per request, and MQSim
  // does not own/free them. Free them here to avoid unbounded leaks during long runs.
  if (tr->UserIORequest) {
    delete tr->UserIORequest;
    tr->UserIORequest = nullptr;
  }
}

static bool parse_mqsim_xml(const char* xml_path) {
  std::ifstream f(xml_path);
  if (!f) return false;
  std::string xml((std::istreambuf_iterator<char>(f)),
                  std::istreambuf_iterator<char>());
  if (xml.empty()) return false;

  rapidxml::xml_document<> doc;
  // rapidxml requires mutable buffer.
  std::vector<char> buf(xml.begin(), xml.end());
  buf.push_back('\0');
  doc.parse<0>(buf.data());
  rapidxml::xml_node<>* root = doc.first_node("Execution_Parameter_Set");
  if (!root) return false;

  Execution_Parameter_Set exec;
  exec.XML_deserialize(root);
  return true;
}

static mq_handle_t* build_handle_from_config(const char* xml_path) {
  // Device config is stored in static members after XML_deserialize.
  Device_Parameter_Set* p = &Execution_Parameter_Set::SSD_Device_Configuration;

  auto* h = new mq_handle_t;
  h->channel_count = p->Flash_Channel_Count;
  h->chip_no_per_channel = p->Chip_No_Per_Channel;
  h->die_no_per_chip = p->Flash_Parameters.Die_No_Per_Chip;
  h->plane_no_per_die = p->Flash_Parameters.Plane_No_Per_Die;
  h->block_no_per_plane = p->Flash_Parameters.Block_No_Per_Plane;
  h->page_no_per_block = p->Flash_Parameters.Page_No_Per_Block;
  h->page_size_bytes = p->Flash_Parameters.Page_Capacity;

  // Derive read/program latency tables based on flash technology.
  sim_time_type* read_latencies = nullptr;
  sim_time_type* prog_latencies = nullptr;
  unsigned latency_levels = 1;
  switch (p->Flash_Parameters.Flash_Technology) {
    case Flash_Technology_Type::SLC:
      latency_levels = 1;
      break;
    case Flash_Technology_Type::MLC:
      latency_levels = 2;
      break;
    case Flash_Technology_Type::TLC:
      latency_levels = 3;
      break;
    default:
      latency_levels = 1;
      break;
  }
  read_latencies = new sim_time_type[latency_levels];
  prog_latencies = new sim_time_type[latency_levels];
  if (latency_levels == 1) {
    read_latencies[0] = p->Flash_Parameters.Page_Read_Latency_LSB;
    prog_latencies[0] = p->Flash_Parameters.Page_Program_Latency_LSB;
  } else if (latency_levels == 2) {
    read_latencies[0] = p->Flash_Parameters.Page_Read_Latency_LSB;
    read_latencies[1] = p->Flash_Parameters.Page_Read_Latency_MSB;
    prog_latencies[0] = p->Flash_Parameters.Page_Program_Latency_LSB;
    prog_latencies[1] = p->Flash_Parameters.Page_Program_Latency_MSB;
  } else {
    read_latencies[0] = p->Flash_Parameters.Page_Read_Latency_LSB;
    read_latencies[1] = p->Flash_Parameters.Page_Read_Latency_CSB;
    read_latencies[2] = p->Flash_Parameters.Page_Read_Latency_MSB;
    prog_latencies[0] = p->Flash_Parameters.Page_Program_Latency_LSB;
    prog_latencies[1] = p->Flash_Parameters.Page_Program_Latency_CSB;
    prog_latencies[2] = p->Flash_Parameters.Page_Program_Latency_MSB;
  }

  // Create chips + channels.
  h->chips.resize(h->channel_count);
  auto** channels =
      new SSD_Components::ONFI_Channel_NVDDR2*[h->channel_count];

  for (unsigned ch = 0; ch < h->channel_count; ++ch) {
    auto** ch_chips = new NVM::FlashMemory::Flash_Chip*[h->chip_no_per_channel];
    h->chips[ch].resize(h->chip_no_per_channel);
    for (unsigned chip = 0; chip < h->chip_no_per_channel; ++chip) {
      auto* c = new NVM::FlashMemory::Flash_Chip(
          "HBF.Channel." + std::to_string(ch) + ".Chip." + std::to_string(chip),
          ch, chip, p->Flash_Parameters.Flash_Technology, h->die_no_per_chip,
          h->plane_no_per_die, h->block_no_per_plane, h->page_no_per_block,
          read_latencies, prog_latencies, p->Flash_Parameters.Block_Erase_Latency,
          p->Flash_Parameters.Suspend_Program_Time,
          p->Flash_Parameters.Suspend_Erase_Time);
      Simulator->AddObject(c);
      ch_chips[chip] = c;
      h->chips[ch][chip] = c;
    }

    // MQSim expects per-two-unit transfer timing in ns. The stock XML uses a
    // "transfer rate" number where 333 -> ~6ns (i.e., ~333MB/s with 1B width).
    // Keep the same convention but avoid integer truncation to 0 for high rates.
    const double xfer = (p->Channel_Transfer_Rate > 0)
                            ? (1000.0 / (double)p->Channel_Transfer_Rate)
                            : 0.0;
    sim_time_type t_rc = (sim_time_type)(xfer * 2.0);
    sim_time_type t_dsc = (sim_time_type)(xfer * 2.0);
    if (t_rc == 0) t_rc = 1;
    if (t_dsc == 0) t_dsc = 1;
    channels[ch] = new SSD_Components::ONFI_Channel_NVDDR2(
        (flash_channel_ID_type)ch, h->chip_no_per_channel, ch_chips,
        p->Flash_Channel_Width, t_rc, t_dsc);
    h->channels.push_back(channels[ch]);
  }

  h->phy = new SSD_Components::NVM_PHY_ONFI_NVDDR2(
      "HBF.PHY", channels, h->channel_count, h->chip_no_per_channel,
      h->die_no_per_chip, h->plane_no_per_die);
  Simulator->AddObject(h->phy);

  h->tsu = new TSU_HBF_Simple("HBF.TSU", h->phy, h->channel_count,
                             h->chip_no_per_channel, h->die_no_per_chip,
                             h->plane_no_per_die);
  Simulator->AddObject(h->tsu);

  // Hook transaction completion notifications for co-simulation.
  h->phy->ConnectToTransactionServicedSignal(tx_serviced_cb);

  delete[] read_latencies;
  delete[] prog_latencies;

  return h;
}

static void apply_params_to_device_config(const mq_create_params_t* params) {
  if (!params) return;

  Device_Parameter_Set* p = &Execution_Parameter_Set::SSD_Device_Configuration;

  // Geometry
  if (params->channel_count) p->Flash_Channel_Count = params->channel_count;
  if (params->channel_width_bytes)
    p->Flash_Channel_Width = params->channel_width_bytes;
  if (params->chip_no_per_channel)
    p->Chip_No_Per_Channel = params->chip_no_per_channel;

  if (params->die_no_per_chip)
    p->Flash_Parameters.Die_No_Per_Chip = params->die_no_per_chip;
  if (params->plane_no_per_die)
    p->Flash_Parameters.Plane_No_Per_Die = params->plane_no_per_die;
  if (params->block_no_per_plane)
    p->Flash_Parameters.Block_No_Per_Plane = params->block_no_per_plane;
  if (params->page_no_per_block)
    p->Flash_Parameters.Page_No_Per_Block = params->page_no_per_block;
  if (params->page_size_bytes)
    p->Flash_Parameters.Page_Capacity = params->page_size_bytes;

  // Timing
  p->Flash_Parameters.Page_Read_Latency_LSB = params->t_read;
  p->Flash_Parameters.Page_Read_Latency_CSB = params->t_read;
  p->Flash_Parameters.Page_Read_Latency_MSB = params->t_read;
  p->Flash_Parameters.Page_Program_Latency_LSB = params->t_prog;
  p->Flash_Parameters.Page_Program_Latency_CSB = params->t_prog;
  p->Flash_Parameters.Page_Program_Latency_MSB = params->t_prog;
  p->Flash_Parameters.Block_Erase_Latency = params->t_erase;
}

static mq_handle_t* build_handle_from_config_with_params(
    const char* xml_path, const mq_create_params_t* params) {
  if (!parse_mqsim_xml(xml_path)) {
    return nullptr;
  }
  apply_params_to_device_config(params);
  return build_handle_from_config(xml_path);
}

}  // namespace

extern "C" {

typedef struct mq_completion {
  void* user_ptr;
  uint64_t finish_time_ns;
} mq_completion;

void* mq_create(const char* mqsim_xml_config_path) {
  if (!mqsim_xml_config_path) return nullptr;

  // MQSim has a global singleton engine; reset it for each handle.
  Simulator->Reset();
  Simulator->Set_integrated_execution_mode(true);

  if (!parse_mqsim_xml(mqsim_xml_config_path)) return nullptr;
  auto* h = build_handle_from_config(mqsim_xml_config_path);
  if (!h) return nullptr;

  g_handle = h;

  // Initialize objects/triggers without draining the event queue.
  Simulator->Initialize_simulation();

  h->now_ns = 0;
  return h;
}

void* mq_create2(const char* mqsim_xml_config_path,
                 const mq_create_params_t* params) {
  if (!mqsim_xml_config_path || !params) return nullptr;

  Simulator->Reset();
  Simulator->Set_integrated_execution_mode(true);

  // Parse XML for baseline values, then override with accel-sim provided
  // parameters. This keeps any unused/SSD-specific fields well-formed without
  // relying on the XML for HBF timing/geometry.
  auto* h = build_handle_from_config_with_params(mqsim_xml_config_path, params);
  if (!h) return nullptr;

  // Override link timing directly at the channel level (NVDDR2 two-unit
  // transfer times). We patch the per-channel fields after construction.
  // This avoids depending on MQSim's Channel_Transfer_Rate convention.
  for (auto* ch : h->channels) {
    if (!ch) continue;
    if (params->two_unit_data_in_time) ch->TwoUnitDataInTime = params->two_unit_data_in_time;
    if (params->two_unit_data_out_time) ch->TwoUnitDataOutTime = params->two_unit_data_out_time;
  }

  g_handle = h;

  Simulator->Initialize_simulation();

  h->now_ns = 0;
  return h;
}

void mq_destroy(void* handle) {
  auto* h = static_cast<mq_handle_t*>(handle);
  if (!h) return;

  // Best-effort cleanup; the process is short-lived in typical accel-sim runs.
  g_handle = nullptr;
  Simulator->Reset();

  delete h->tsu;
  delete h->phy;
  for (auto* ch : h->channels) delete ch;
  for (auto& v : h->chips) {
    for (auto* c : v) delete c;
  }
  delete h;
}

int mq_send(void* handle, uint32_t part_id, uint64_t part_addr,
            uint32_t size_bytes, int is_write, int source_id, void* user_ptr) {
  auto* h = static_cast<mq_handle_t*>(handle);
  if (!h) return 0;
  if (h->pending.size() >= h->max_pending) return 0;
  h->pending.push_back(
      {part_id, part_addr, size_bytes, is_write, source_id, user_ptr});
  return 1;
}

void mq_tick_to(void* handle, uint64_t time_ns) {
  auto* h = static_cast<mq_handle_t*>(handle);
  if (!h) return;
  if (time_ns <= h->now_ns) return;

  // First, advance the simulator to this time and retire any outstanding flash
  // operations.
  Simulator->Run_until(time_ns);
  h->now_ns = time_ns;

  if (h->pending.empty()) return;

  // Inject all pending requests at this time.
  h->tsu->Prepare_for_transaction_submit();
  for (const auto& preq : h->pending) {
    const uint32_t channel_id =
        h->channel_count ? (preq.part_id % h->channel_count) : 0;
    const uint64_t offset_in_page =
        h->page_size_bytes ? (preq.part_addr % (uint64_t)h->page_size_bytes) : 0;
    const page_status_type bitmap =
        compute_sector_bitmap(h->page_size_bytes, offset_in_page, preq.size_bytes);

    const auto ppa = map_to_ppa(h, channel_id, preq.part_addr);

    auto* ur = new SSD_Components::User_Request();
    ur->IO_command_info = preq.user_ptr;
    ur->Stream_id = (stream_id_type)(preq.source_id);
    ur->STAT_InitiationTime = Simulator->Time();
    ur->Type = preq.is_write ? SSD_Components::UserRequestType::WRITE
                             : SSD_Components::UserRequestType::READ;
    ur->Size_in_byte = preq.size_bytes;
    ur->SizeInSectors =
        (unsigned int)((preq.size_bytes + SECTOR_SIZE_IN_BYTE - 1) /
                       SECTOR_SIZE_IN_BYTE);

    const LPA_type lpa = (LPA_type)(preq.part_addr /
                                    (uint64_t)(h->page_size_bytes
                                                   ? h->page_size_bytes
                                                   : SECTOR_SIZE_IN_BYTE));
    const PPA_type ppa_num = (PPA_type)lpa;

    if (!preq.is_write) {
      auto* tr = new SSD_Components::NVM_Transaction_Flash_RD(
          SSD_Components::Transaction_Source_Type::USERIO, ur->Stream_id,
          preq.size_bytes, lpa, ppa_num, ppa, ur,
          /*content*/ 0, /*related_write*/ nullptr, bitmap, Simulator->Time());
      h->tsu->Submit_transaction(tr);
    } else {
      auto* tr = new SSD_Components::NVM_Transaction_Flash_WR(
          SSD_Components::Transaction_Source_Type::USERIO, ur->Stream_id,
          preq.size_bytes, lpa, ppa_num, ppa, ur,
          /*content*/ 0, /*related_read*/ nullptr, bitmap, Simulator->Time());
      h->tsu->Submit_transaction(tr);

      // Posted write ACK: notify the GPU as soon as the controller accepts the
      // write. The actual flash program latency is still modeled in MQSim and
      // will be reflected in later internal contention (die/plane busy), but
      // it does not block the GPU pipeline.
      h->completions.push_back({preq.user_ptr, Simulator->Time()});
    }
  }
  h->pending.clear();
  h->tsu->Schedule();

  // Some components may schedule immediate events at the current time.
  Simulator->Run_until(time_ns);
}

int mq_poll(void* handle, mq_completion* out) {
  auto* h = static_cast<mq_handle_t*>(handle);
  if (!h || !out) return 0;
  if (h->completions.empty()) return 0;
  auto c = h->completions.front();
  h->completions.pop_front();
  out->user_ptr = c.user_ptr;
  out->finish_time_ns = c.finish_time_ns;
  return 1;
}

}  // extern "C"
