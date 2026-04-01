#include "mqsim_backend.h"

#include <dlfcn.h>
#include <inttypes.h>

#include <cassert>
#include <cstdio>
#include <cstring>

#include "gpu-sim.h"
#include "l2cache.h"
#include "mem_fetch.h"
#include "mem_latency_stat.h"

namespace {

// C-ABI mirror of the wrapper's completion struct.
struct mq_completion_t {
  void *user_ptr;
  uint64_t finish_time_ns;
};

// C-ABI mirror of the wrapper's create2 params struct.
struct mq_create_params_t {
  uint32_t channel_count;
  uint32_t chip_no_per_channel;
  uint32_t die_no_per_chip;
  uint32_t plane_no_per_die;
  uint32_t block_no_per_plane;
  uint32_t page_no_per_block;
  uint32_t page_size_bytes;

  uint32_t channel_width_bytes;
  uint32_t two_unit_data_in_time;
  uint32_t two_unit_data_out_time;

  uint64_t t_read;
  uint64_t t_prog;
  uint64_t t_erase;
};

}  // namespace

struct mqsim_backend::shared_state_t {
  bool enabled = false;
  bool initialized = false;

  void *dl = nullptr;
  void *h = nullptr;

  using create_f = void *(*)(const char *);
  using create2_f = void *(*)(const char *, const mq_create_params_t *);
  using destroy_f = void (*)(void *);
  using send_f = int (*)(void *, uint32_t, uint64_t, uint32_t, int, int, void *);
  using tick_to_f = void (*)(void *, uint64_t);
  using poll_f = int (*)(void *, mq_completion_t *);

  create_f create = nullptr;
  create2_f create2 = nullptr;
  destroy_f destroy = nullptr;
  send_f send = nullptr;
  tick_to_f tick_to = nullptr;
  poll_f poll = nullptr;

  uint64_t last_tick_ns = 0;

  // Per-partition return queues, and book-keeping.
  std::vector<std::deque<mem_fetch *>> returnq;
  std::vector<memory_partition_unit *> mp_by_part;
  std::vector<unsigned> outstanding;

  unsigned max_outstanding_per_part = 8192;

  // Simple counters for power/stats.
  std::vector<unsigned long long> n_req;
  std::vector<unsigned long long> n_rd;
  std::vector<unsigned long long> n_wr;

  void ensure_vectors(unsigned n_mem) {
    if (returnq.size() == n_mem) return;
    returnq.assign(n_mem, {});
    mp_by_part.assign(n_mem, nullptr);
    outstanding.assign(n_mem, 0);
    n_req.assign(n_mem, 0);
    n_rd.assign(n_mem, 0);
    n_wr.assign(n_mem, 0);
  }
};

mqsim_backend::shared_state_t &mqsim_backend::shared() {
  static shared_state_t st;
  return st;
}

static void *load_sym(void *dl, const char *name) {
  void *p = dlsym(dl, name);
  if (!p) {
    fprintf(stderr, "MQSim backend: dlsym(%s) failed: %s\n", name, dlerror());
  }
  return p;
}

mqsim_backend::mqsim_backend(unsigned partition_id, const memory_config *config,
                             memory_stats_t *stats, memory_partition_unit *mp,
                             gpgpu_sim *gpu)
    : m_id(partition_id), m_config(config), m_stats(stats), m_mp(mp), m_gpu(gpu) {
  auto &st = shared();
  st.enabled = config->hbf_use_mqsim;
  if (!st.enabled) return;

  st.ensure_vectors(config->m_n_mem);
  st.mp_by_part[m_id] = mp;

  if (config->hbf_mqsim_max_outstanding)
    st.max_outstanding_per_part = config->hbf_mqsim_max_outstanding;
  else if (config->hbf_max_outstanding)
    st.max_outstanding_per_part = config->hbf_max_outstanding;

  if (st.initialized) return;

  const char *so_path =
      config->hbf_mqsim_wrapper ? config->hbf_mqsim_wrapper : "libmqsim_wrap.so";
  const char *cfg_path = config->hbf_mqsim_config;
  if (!cfg_path || cfg_path[0] == '\0') {
    fprintf(stderr, "MQSim backend enabled but -hbf_mqsim_config not set\n");
    return;
  }

  st.dl = dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
  if (!st.dl) {
    fprintf(stderr, "MQSim backend: dlopen(%s) failed: %s\n", so_path,
            dlerror());
    return;
  }

  st.create =
      reinterpret_cast<shared_state_t::create_f>(load_sym(st.dl, "mq_create"));
  // Optional: newer wrapper supports parameter override.
  st.create2 = reinterpret_cast<shared_state_t::create2_f>(
      dlsym(st.dl, "mq_create2"));
  st.destroy = reinterpret_cast<shared_state_t::destroy_f>(
      load_sym(st.dl, "mq_destroy"));
  st.send =
      reinterpret_cast<shared_state_t::send_f>(load_sym(st.dl, "mq_send"));
  st.tick_to = reinterpret_cast<shared_state_t::tick_to_f>(
      load_sym(st.dl, "mq_tick_to"));
  st.poll =
      reinterpret_cast<shared_state_t::poll_f>(load_sym(st.dl, "mq_poll"));

  if (!st.create || !st.destroy || !st.send || !st.tick_to || !st.poll) {
    fprintf(stderr, "MQSim backend: missing symbols, disabling\n");
    return;
  }

  if (st.create2) {
    // Override MQSim XML with accel-sim -hbf_* parameters.
    const unsigned core_freq_hz = m_gpu->get_config().get_core_freq();
    const double core_period_ns =
        (core_freq_hz > 0) ? (1e9 / (double)core_freq_hz) : 1.0;

    auto cycles_to_ns = [&](unsigned cycles) -> uint64_t {
      if (cycles == 0) return 0;
      const double ns = (double)cycles * core_period_ns;
      if (ns <= 0.0) return 0;
      return (uint64_t)(ns + 0.5);
    };

    const uint32_t subpage_bytes =
        config->hbf_subpage_bytes ? (uint32_t)config->hbf_subpage_bytes : 512u;

    const double desired_bw_B_per_ns =
        (double)(config->hbf_channel_bytes_per_cycle ? config->hbf_channel_bytes_per_cycle
                                                     : 1u) *
        (double)core_freq_hz / 1e9;

    // MQSim's NVDDR2 model uses (2*ChannelWidth / TwoUnitDataOutTime) B/ns for
    // large transfers, but transfer time is quantized to integer ns. Clamp the
    // width so a single subpage transfer never truncates to 0-time.
    const uint32_t max_width = std::max(1u, subpage_bytes / 2u);
    uint32_t width = (uint32_t)(desired_bw_B_per_ns / 2.0 + 0.5);
    if (width < 1u) width = 1u;
    if (width > max_width) width = max_width;

    mq_create_params_t p{};
    p.channel_count = std::max(1u, config->m_n_mem);
    p.chip_no_per_channel = 1u;
    p.die_no_per_chip =
        config->hbf_dies_per_channel ? config->hbf_dies_per_channel : 1u;
    p.plane_no_per_die =
        config->hbf_planes_per_die ? config->hbf_planes_per_die : 1u;
    // Keep capacity small (we only need timing/parallelism for co-sim).
    p.block_no_per_plane = 1u;
    p.page_no_per_block =
        config->hbf_block_pages ? config->hbf_block_pages : 256u;
    p.page_size_bytes =
        config->hbf_page_bytes ? config->hbf_page_bytes : 4096u;

    p.channel_width_bytes = width;
    p.two_unit_data_in_time = 1u;
    p.two_unit_data_out_time = 1u;

    p.t_read = cycles_to_ns(config->hbf_t_read);
    p.t_prog = cycles_to_ns(config->hbf_t_prog);
    p.t_erase = cycles_to_ns(config->hbf_t_erase);

    st.h = st.create2(cfg_path, &p);
    if (st.h) {
      fprintf(stdout,
              "MQSim backend: overriding XML with accel-sim HBF params: "
              "core_freq=%uHz core_period=%.3fns subpage=%uB link_width=%uB "
              "link_two_unit=1ns (ideal_bw=%.3fB/ns)\n",
              core_freq_hz, core_period_ns, subpage_bytes, width,
              (2.0 * (double)width));
    }
  } else {
    st.h = st.create(cfg_path);
  }
  if (!st.h) {
    fprintf(stderr, "MQSim backend: mq_create failed\n");
    return;
  }

  st.last_tick_ns = 0;
  st.initialized = true;
  fprintf(stdout, "MQSim backend: enabled (shared), so=%s cfg=%s\n", so_path,
          cfg_path);
}

mqsim_backend::~mqsim_backend() {
  // Shared handle is intentionally kept alive for the whole process lifetime.
}

uint64_t mqsim_backend::now_time_ns() const {
  // memory_partition_unit::dram_cycle() is called on DRAM clock edges.
  // gpgpu_sim stores the time of the *next* edge, so subtract one period.
  const double t_s = m_gpu->get_dram_time() - m_gpu->get_dram_period();
  const double t_ns = t_s * 1e9;
  if (t_ns <= 0.0) return 0;
  return static_cast<uint64_t>(t_ns);
}

bool mqsim_backend::full(bool /*is_write*/) const {
  auto &st = shared();
  if (!st.enabled || !st.initialized) return false;
  if (m_id >= st.outstanding.size()) return false;
  return st.outstanding[m_id] >= st.max_outstanding_per_part;
}

void mqsim_backend::push(mem_fetch *data) {
  auto &st = shared();
  assert(st.enabled && st.initialized);

  if (full(data->is_write())) {
    assert(0 && "mqsim_backend::push called while full()");
  }

  const uint32_t part_id = (uint32_t)m_id;
  const uint64_t part_addr = (uint64_t)data->get_partition_addr();
  const uint32_t size_bytes =
      m_config->hbf_subpage_bytes ? (uint32_t)m_config->hbf_subpage_bytes
                                  : (uint32_t)data->get_data_size();
  const int is_write = data->is_write() ? 1 : 0;
  const int source_id = (int)data->get_sid();

  int ok = st.send(st.h, part_id, part_addr, size_bytes, is_write, source_id,
                   data);
  if (!ok) {
    fprintf(stderr, "MQSim backend: mq_send failed (pending queue full?)\n");
    assert(0);
  }

  st.outstanding[m_id]++;
  st.n_req[m_id]++;
  if (is_write)
    st.n_wr[m_id]++;
  else
    st.n_rd[m_id]++;

  data->set_status(IN_PARTITION_MC_INTERFACE_QUEUE,
                   m_gpu->gpu_sim_cycle + m_gpu->gpu_tot_sim_cycle);
  m_stats->memlatstat_dram_access(data);
}

void mqsim_backend::cycle() {
  auto &st = shared();
  if (!st.enabled || !st.initialized) return;

  // Only partition 0 advances time and drains completions once per DRAM tick.
  if (m_id != 0) return;

  const uint64_t t_ns = now_time_ns();
  if (t_ns == st.last_tick_ns) return;
  st.last_tick_ns = t_ns;

  st.tick_to(st.h, t_ns);

  mq_completion_t c;
  while (st.poll(st.h, &c)) {
    mem_fetch *mf = static_cast<mem_fetch *>(c.user_ptr);
    if (!mf) continue;

    const unsigned pid = mf->get_tlx_addr().chip;
    if (pid < st.outstanding.size() && st.outstanding[pid] > 0) {
      st.outstanding[pid]--;
    }

    if (mf->get_access_type() == L1_WRBK_ACC ||
        mf->get_access_type() == L2_WRBK_ACC) {
      // Match dram_t/hbf_t behavior: writebacks are completed internally and
      // deleted.
      if (pid < st.mp_by_part.size() && st.mp_by_part[pid]) {
        st.mp_by_part[pid]->set_done(mf);
      }
      delete mf;
      continue;
    }

    mf->set_reply();
    if (pid < st.returnq.size()) {
      st.returnq[pid].push_back(mf);
    } else {
      delete mf;
    }
  }
}

mem_fetch *mqsim_backend::return_queue_top() {
  auto &st = shared();
  if (!st.enabled || !st.initialized) return nullptr;
  if (m_id >= st.returnq.size()) return nullptr;
  auto &q = st.returnq[m_id];
  return q.empty() ? nullptr : q.front();
}

mem_fetch *mqsim_backend::return_queue_pop() {
  auto &st = shared();
  if (!st.enabled || !st.initialized) return nullptr;
  if (m_id >= st.returnq.size()) return nullptr;
  auto &q = st.returnq[m_id];
  if (q.empty()) return nullptr;
  mem_fetch *mf = q.front();
  q.pop_front();
  return mf;
}

void mqsim_backend::print(FILE *simFile) const {
  auto &st = shared();
  fprintf(simFile, "  HBF backend: MQSim (shared)\n");
  if (m_id < st.outstanding.size()) {
    fprintf(simFile, "    outstanding=%u max_outstanding=%u\n",
            st.outstanding[m_id], st.max_outstanding_per_part);
  }
}

void mqsim_backend::visualize() const {}

void mqsim_backend::print_stat(FILE *simFile) {
  auto &st = shared();
  if (!st.enabled || !st.initialized) return;
  if (m_id >= st.n_req.size()) return;
  fprintf(simFile, "mqsim_backend[%u]: n_req=%llu n_rd=%llu n_wr=%llu\n", m_id,
          st.n_req[m_id], st.n_rd[m_id], st.n_wr[m_id]);
}

void mqsim_backend::visualizer_print(gzFile /*visualizer_file*/) {}

void mqsim_backend::set_hbf_power_stats(unsigned &cmd, unsigned &activity,
                                        unsigned &nop, unsigned &act,
                                        unsigned &pre, unsigned &rd,
                                        unsigned &wr, unsigned &wr_WB,
                                        unsigned &req) const {
  // Request-level counts only (cmd-level is left at 0).
  cmd = activity = nop = act = pre = wr_WB = 0;

  auto &st = shared();
  if (st.enabled && st.initialized && m_id < st.n_req.size()) {
    rd = (unsigned)st.n_rd[m_id];
    wr = (unsigned)st.n_wr[m_id];
    req = (unsigned)st.n_req[m_id];
  } else {
    rd = wr = req = 0;
  }
}
