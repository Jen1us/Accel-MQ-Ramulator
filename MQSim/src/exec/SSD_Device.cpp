#include <vector>
#include <stdexcept>
#include <ctime>
#include "SSD_Device.h"
#include "../ssd/ONFI_Channel_Base.h"
#include "../ssd/Flash_Block_Manager.h"
#include "../ssd/Data_Cache_Manager_Flash_Advanced.h"
#include "../ssd/Data_Cache_Manager_Flash_Simple.h"
#include "../ssd/Address_Mapping_Unit_Base.h"
#include "../ssd/Address_Mapping_Unit_Page_Level.h"
#include "../ssd/Address_Mapping_Unit_Hybrid.h"
#include "../ssd/GC_and_WL_Unit_Page_Level.h"
#include "../ssd/TSU_OutofOrder.h"
#include "../ssd/TSU_Priority_OutOfOrder.h"
#include "../ssd/TSU_FLIN.h"
#include "../ssd/ONFI_Channel_NVDDR2.h"
#include "../ssd/NVM_PHY_ONFI_NVDDR2.h"
#include "../utils/Logical_Address_Partitioning_Unit.h"

SSD_Device *SSD_Device::my_instance; //Used in static functions

SSD_Device::SSD_Device(Device_Parameter_Set *parameters, std::vector<IO_Flow_Parameter_Set *> *io_flows) : MQSimEngine::Sim_Object("SSDDevice")
{
	SSD_Device *device = this;
	my_instance = device; //used for static functions
	Simulator->AddObject(device);

	device->Preconditioning_required = parameters->Enabled_Preconditioning;
	device->Memory_Type = parameters->Memory_Type;

	switch (Memory_Type)
	{
	case NVM::NVM_Type::FLASH:
	{
		// sim_time_type *read_latencies, *write_latencies;	//芯片使用独立延迟数组，此处即不统计
		//sim_time_type average_flash_read_latency = 0, average_flash_write_latency = 0; //Required for FTL initialization

		//Step 1: create memory chips (flash chips in our case)
		sim_time_type average_flash_read_latency = 0, average_flash_write_latency = 0;
		
		if (Flash_Parameter_Set::Flash_Parameter_Sets.size() > 0) {
			// 多参数集模式：计算所有类型的平均延迟
			sim_time_type total_read = 0, total_write = 0;
			unsigned int count = 0;
			for (const auto& pair : Flash_Parameter_Set::Flash_Parameter_Sets) {
				const auto& params = pair.second;
				switch (pair.first) {
				case Flash_Technology_Type::SLC:
					total_read += params.Page_Read_Latency_LSB;
					total_write += params.Page_Program_Latency_LSB;
					count++;
					break;
				case Flash_Technology_Type::MLC:
					total_read += (params.Page_Read_Latency_LSB + params.Page_Read_Latency_MSB) / 2;
					total_write += (params.Page_Program_Latency_LSB + params.Page_Program_Latency_MSB) / 2;
					count++;
					break;
				case Flash_Technology_Type::TLC:
					total_read += (params.Page_Read_Latency_LSB + params.Page_Read_Latency_CSB + params.Page_Read_Latency_MSB) / 3;
					total_write += (params.Page_Program_Latency_LSB + params.Page_Program_Latency_CSB + params.Page_Program_Latency_MSB) / 3;
					count++;
					break;
				default:
				throw std::invalid_argument("The specified flash technologies is not supported");
		
				}
			}
			if (count > 0) {
				average_flash_read_latency = total_read / count;
				average_flash_write_latency = total_write / count;
			}
		} else {
			// 单参数集模式：使用原有逻辑
			switch (parameters->Flash_Parameters.Flash_Technology)
			{
			case Flash_Technology_Type::SLC:
				average_flash_read_latency = parameters->Flash_Parameters.Page_Read_Latency_LSB;
				average_flash_write_latency = parameters->Flash_Parameters.Page_Program_Latency_LSB;
				break;
			case Flash_Technology_Type::MLC:
				average_flash_read_latency = (parameters->Flash_Parameters.Page_Read_Latency_LSB + parameters->Flash_Parameters.Page_Read_Latency_MSB) / 2;
				average_flash_write_latency = (parameters->Flash_Parameters.Page_Program_Latency_LSB + parameters->Flash_Parameters.Page_Program_Latency_MSB) / 2;
				break;
			case Flash_Technology_Type::TLC:
				average_flash_read_latency = (parameters->Flash_Parameters.Page_Read_Latency_LSB + parameters->Flash_Parameters.Page_Read_Latency_CSB + parameters->Flash_Parameters.Page_Read_Latency_MSB) / 3;
				average_flash_write_latency = (parameters->Flash_Parameters.Page_Program_Latency_LSB + parameters->Flash_Parameters.Page_Program_Latency_CSB + parameters->Flash_Parameters.Page_Program_Latency_MSB) / 3;
				break;
			default:
				throw std::invalid_argument("The specified flash technologies is not supported");
			}
		}

		//Step 2: create memory channels to connect chips to the controller
		this->Channel_count = parameters->Flash_Channel_Count;
		this->Chip_no_per_channel = parameters->Chip_No_Per_Channel;
		switch (parameters->Flash_Comm_Protocol)
		{
		case SSD_Components::ONFI_Protocol::NVDDR2:
		{
			SSD_Components::ONFI_Channel_NVDDR2 **channels = new SSD_Components::ONFI_Channel_NVDDR2 *[parameters->Flash_Channel_Count];
			for (unsigned int channel_cntr = 0; channel_cntr < parameters->Flash_Channel_Count; channel_cntr++)
			{
				NVM::FlashMemory::Flash_Chip **chips = new NVM::FlashMemory::Flash_Chip *[parameters->Chip_No_Per_Channel];
				for (unsigned int chip_cntr = 0; chip_cntr < parameters->Chip_No_Per_Channel; chip_cntr++)
				{
					// chips[chip_cntr] = new NVM::FlashMemory::Flash_Chip(device->ID() + ".Channel." + std::to_string(channel_cntr) + ".Chip." + std::to_string(chip_cntr),
					// 													channel_cntr, chip_cntr, parameters->Flash_Parameters.Flash_Technology, parameters->Flash_Parameters.Die_No_Per_Chip, parameters->Flash_Parameters.Plane_No_Per_Die,
					// 													parameters->Flash_Parameters.Block_No_Per_Plane, parameters->Flash_Parameters.Page_No_Per_Block,
					// 													read_latencies, write_latencies, parameters->Flash_Parameters.Block_Erase_Latency,
					// 													parameters->Flash_Parameters.Suspend_Program_Time, parameters->Flash_Parameters.Suspend_Erase_Time);

					// 根据地址选择闪存技术类型 (静态成员函数形式调用)
					Flash_Technology_Type selected_tech = 
						//Flash_Parameter_Set::Select_Technology_By_Address(Chip_no_per_channel, channel_cntr, chip_cntr);
						Flash_Parameter_Set::Select_Technology_By_Address(Channel_count, channel_cntr, chip_cntr);//改为分channel对半排布

					// 获取对应技术类型的参数
					const Flash_Parameter_Set::Flash_Params& tech_params = 
						Flash_Parameter_Set::Get_Parameters(selected_tech);

					// 根据选择的技术类型准备延迟数组
					sim_time_type *chip_read_latencies, *chip_write_latencies;
					switch (selected_tech) {
					case Flash_Technology_Type::SLC:
						chip_read_latencies = new sim_time_type[1];
						chip_read_latencies[0] = tech_params.Page_Read_Latency_LSB;
						chip_write_latencies = new sim_time_type[1];
						chip_write_latencies[0] = tech_params.Page_Program_Latency_LSB;
						break;
					case Flash_Technology_Type::MLC:
						chip_read_latencies = new sim_time_type[2];
						chip_read_latencies[0] = tech_params.Page_Read_Latency_LSB;
						chip_read_latencies[1] = tech_params.Page_Read_Latency_MSB;
						chip_write_latencies = new sim_time_type[2];
						chip_write_latencies[0] = tech_params.Page_Program_Latency_LSB;
						chip_write_latencies[1] = tech_params.Page_Program_Latency_MSB;
						break;
					case Flash_Technology_Type::TLC:
						chip_read_latencies = new sim_time_type[3];
						chip_read_latencies[0] = tech_params.Page_Read_Latency_LSB;
						chip_read_latencies[1] = tech_params.Page_Read_Latency_CSB;
						chip_read_latencies[2] = tech_params.Page_Read_Latency_MSB;
						chip_write_latencies = new sim_time_type[3];
						chip_write_latencies[0] = tech_params.Page_Program_Latency_LSB;
						chip_write_latencies[1] = tech_params.Page_Program_Latency_CSB;
						chip_write_latencies[2] = tech_params.Page_Program_Latency_MSB;
						break;
					default:
						throw std::invalid_argument("Unsupported flash technology type");
					}
					// if(DEBUG)std::cout << "Creating Chips...\n" 
					// 			<< device->ID() 
					// 			<< ".Channel." << channel_cntr 
					// 			<< ".Chip." << chip_cntr 
					// 			<< ".type"	<< (tech_params.Flash_Technology == Flash_Technology_Type::SLC ?"SLC":"MLC+")
					// 			<< ".Page_Capacity=" << tech_params.Page_Capacity
					// 			<< ".SectorsPerPage=" << (tech_params.Page_Capacity / SECTOR_SIZE_IN_BYTE)
					// 			<< std::endl;

					chips[chip_cntr] = new NVM::FlashMemory::Flash_Chip(
						device->ID() + ".Channel." + std::to_string(channel_cntr) + ".Chip." + std::to_string(chip_cntr),
						channel_cntr, chip_cntr, selected_tech, 
						tech_params.Die_No_Per_Chip, tech_params.Plane_No_Per_Die,
						tech_params.Block_No_Per_Plane, tech_params.Page_No_Per_Block,
						chip_read_latencies, chip_write_latencies, 
						tech_params.Block_Erase_Latency,
						tech_params.Suspend_Program_Time, tech_params.Suspend_Erase_Time);
					//Simulator->AddObject(chips[chip_cntr]); //Each simulation object (a child of MQSimEngine::Sim_Object) should be added to the engine

					// 清理临时延迟数组
					delete[] chip_read_latencies;
					delete[] chip_write_latencies;
					
					Simulator->AddObject(chips[chip_cntr]); //Each simulation object (a child of MQSimEngine::Sim_Object) should be added to the engine
				}
				channels[channel_cntr] = new SSD_Components::ONFI_Channel_NVDDR2(channel_cntr, parameters->Chip_No_Per_Channel,
																				chips, parameters->Flash_Channel_Width,
																				(sim_time_type)((double)1000* SIM_TIME_TICK_PER_NANOSECOND / parameters->Channel_Transfer_Rate) * 2, (sim_time_type)((double)1000* SIM_TIME_TICK_PER_NANOSECOND / parameters->Channel_Transfer_Rate) * 2);
				device->Channels.push_back(channels[channel_cntr]); //Channels should not be added to the simulator core, they are passive object that do not handle any simulation event
			}

			//Step 3: create channel controller and connect channels to it
			// NVM_PHY_ONFI_NVDDR2 对整条通道使用统一的 die_no / plane_no 分配 bookkeeping（见 NVM_PHY_ONFI_NVDDR2.cpp）。
			// 异构 LC 时：PHY 取各 Flash_Parameter_Sets 中 Die_No_Per_Chip / Plane_No_Per_Die 的**最大值**，
			// 避免某颗 chip 实际 die/plane 更多时访问 Die_book_keeping_records 越界；每颗 Flash_Chip 仍用各自 tech_params 构造。
			unsigned int die_no = parameters->Flash_Parameters.Die_No_Per_Chip;
			unsigned int plane_no = parameters->Flash_Parameters.Plane_No_Per_Die;
			if (Flash_Parameter_Set::Flash_Parameter_Sets.size() > 0) {
				//die_no = Flash_Parameter_Set::Flash_Parameter_Sets.begin()->second.Die_No_Per_Chip;
				//plane_no = Flash_Parameter_Set::Flash_Parameter_Sets.begin()->second.Plane_No_Per_Die;
				die_no = 0;
				plane_no = 0;
				for (const auto& kv : Flash_Parameter_Set::Flash_Parameter_Sets) {
					const auto& p = kv.second;
					if (p.Die_No_Per_Chip > die_no) die_no = p.Die_No_Per_Chip;
					if (p.Plane_No_Per_Die > plane_no) plane_no = p.Plane_No_Per_Die;
				}
			}
			// device->PHY = new SSD_Components::NVM_PHY_ONFI_NVDDR2(device->ID() + ".PHY", channels, parameters->Flash_Channel_Count, parameters->Chip_No_Per_Channel,
			// 													  parameters->Flash_Parameters.Die_No_Per_Chip, parameters->Flash_Parameters.Plane_No_Per_Die);
			
			device->PHY = new SSD_Components::NVM_PHY_ONFI_NVDDR2(device->ID() + ".PHY", channels, parameters->Flash_Channel_Count, parameters->Chip_No_Per_Channel,
			die_no, plane_no);
			//parameters->Flash_Parameters.Die_No_Per_Chip, parameters->Flash_Parameters.Plane_No_Per_Die);
			Simulator->AddObject(device->PHY);
			break;
		}
		default:
			throw std::invalid_argument("No implementation is available for the specified flash communication protocol");
		}
		// delete[] read_latencies;
		// delete[] write_latencies;

		//Steps 4 - 8: create FTL components and connect them together
		const unsigned int max_sector_num_per_page = Flash_Parameter_Set::Get_Max_sector_num_per_page();

		SSD_Components::FTL *ftl = new SSD_Components::FTL(device->ID() + ".FTL", NULL, parameters->Flash_Channel_Count,
														   parameters->Chip_No_Per_Channel, parameters->Flash_Parameters.Die_No_Per_Chip, parameters->Flash_Parameters.Plane_No_Per_Die,
														   parameters->Flash_Parameters.Block_No_Per_Plane, parameters->Flash_Parameters.Page_No_Per_Block,
														   max_sector_num_per_page, average_flash_read_latency, average_flash_write_latency, parameters->Overprovisioning_Ratio,
														   parameters->Flash_Parameters.Block_PE_Cycles_Limit, parameters->Seed++);
		ftl->PHY = (SSD_Components::NVM_PHY_ONFI *)PHY;
		Simulator->AddObject(ftl);
		device->Firmware = ftl;

		//Step 5: create TSU
		SSD_Components::TSU_Base *tsu;
		bool erase_suspension = false, program_suspension = false;
		if (parameters->Flash_Parameters.CMD_Suspension_Support == NVM::FlashMemory::Command_Suspension_Mode::PROGRAM)
		{
			program_suspension = true;
		}
		if (parameters->Flash_Parameters.CMD_Suspension_Support == NVM::FlashMemory::Command_Suspension_Mode::ERASE)
		{
			erase_suspension = true;
		}
		if (parameters->Flash_Parameters.CMD_Suspension_Support == NVM::FlashMemory::Command_Suspension_Mode::PROGRAM_ERASE)
		{
			program_suspension = true;
			erase_suspension = true;
		}
		switch (parameters->Transaction_Scheduling_Policy)
		{
		case SSD_Components::Flash_Scheduling_Type::OUT_OF_ORDER:
			tsu = new SSD_Components::TSU_OutOfOrder(ftl->ID() + ".TSU", ftl, static_cast<SSD_Components::NVM_PHY_ONFI_NVDDR2 *>(device->PHY),
													 parameters->Flash_Channel_Count, parameters->Chip_No_Per_Channel,
													 parameters->Flash_Parameters.Die_No_Per_Chip, parameters->Flash_Parameters.Plane_No_Per_Die,
													 parameters->Preferred_suspend_write_time_for_read, parameters->Preferred_suspend_erase_time_for_read,
													 parameters->Preferred_suspend_erase_time_for_write,
													 erase_suspension, program_suspension);
			break;
		case SSD_Components::Flash_Scheduling_Type::PRIORITY_OUT_OF_ORDER:
			tsu = new SSD_Components::TSU_Priority_OutOfOrder(ftl->ID() + ".TSU", ftl, static_cast<SSD_Components::NVM_PHY_ONFI_NVDDR2 *>(device->PHY),
										  parameters->Flash_Channel_Count, parameters->Chip_No_Per_Channel,
										  parameters->Flash_Parameters.Die_No_Per_Chip, parameters->Flash_Parameters.Plane_No_Per_Die,
										  parameters->Preferred_suspend_write_time_for_read, parameters->Preferred_suspend_erase_time_for_read,
										  parameters->Preferred_suspend_erase_time_for_write,
										  erase_suspension, program_suspension);
			break;
		/*case SSD_Components::Flash_Scheduling_Type::FLIN:
				{
					unsigned int * stream_count_per_priority_class = new unsigned int[4];
					for (int i = 0; i < 4; i++)
						stream_count_per_priority_class[i] = 0;
					for (auto &flow : (*io_flows))
						stream_count_per_priority_class[(int)flow->Priority_Class]++;
					stream_id_type** stream_ids_per_priority_class = new stream_id_type*[4];
					for (int i = 0; i < 4; i++)
						stream_ids_per_priority_class[i] = new stream_id_type[stream_count_per_priority_class[i]];

					tsu = new SSD_Components::TSU_FLIN(ftl->ID() + ".TSU", ftl, static_cast<SSD_Components::NVM_PHY_ONFI_NVDDR2*>(device->PHY),
						parameters->Flash_Channel_Count, parameters->Chip_No_Per_Channel,
						parameters->Flash_Parameters.Die_No_Per_Chip, parameters->Flash_Parameters.Plane_No_Per_Die, parameters->Flash_Parameters.Page_Capacity,
						10000000, 33554432, 262144, 4, (unsigned int)io_flows->size(), stream_count_per_priority_class, stream_ids_per_priority_class,
						0.6,
						parameters->Preferred_suspend_write_time_for_read, parameters->Preferred_suspend_erase_time_for_read, parameters->Preferred_suspend_erase_time_for_write,
						erase_suspension, program_suspension);
					break;
				}*/
		default:
			throw std::invalid_argument("No implementation is available for the specified transaction scheduling algorithm");
		}
		Simulator->AddObject(tsu);
		ftl->TSU = tsu;

		//Step 6: create Flash_Block_Manager
		SSD_Components::Flash_Block_Manager_Base *fbm;
		fbm = new SSD_Components::Flash_Block_Manager(NULL, parameters->Flash_Parameters.Block_PE_Cycles_Limit,
													  (unsigned int)io_flows->size(), parameters->Flash_Channel_Count, parameters->Chip_No_Per_Channel,
													  parameters->Flash_Parameters.Die_No_Per_Chip, parameters->Flash_Parameters.Plane_No_Per_Die,
													  parameters->Flash_Parameters.Block_No_Per_Plane, parameters->Flash_Parameters.Page_No_Per_Block);
		ftl->BlockManager = fbm;

		//Step 7: create Address_Mapping_Unit
		SSD_Components::Address_Mapping_Unit_Base *amu;
		std::vector<std::vector<flash_channel_ID_type>> flow_channel_id_assignments;
		std::vector<std::vector<flash_chip_ID_type>> flow_chip_id_assignments;
		std::vector<std::vector<flash_die_ID_type>> flow_die_id_assignments;
		std::vector<std::vector<flash_plane_ID_type>> flow_plane_id_assignments;
		unsigned int stream_count = 0;
		for (unsigned int i = 0; i < io_flows->size(); i++)
		{
			switch (parameters->HostInterface_Type)
			{
			case HostInterface_Types::SATA:
			{
				stream_count = 1;
				std::vector<flash_channel_ID_type> channel_ids;
				flow_channel_id_assignments.push_back(channel_ids);
				for (unsigned int j = 0; j < parameters->Flash_Channel_Count; j++)
				{
					flow_channel_id_assignments[i].push_back(j);
				}
				std::vector<flash_chip_ID_type> chip_ids;
				flow_chip_id_assignments.push_back(chip_ids);
				for (unsigned int j = 0; j < parameters->Chip_No_Per_Channel; j++)
				{
					flow_chip_id_assignments[i].push_back(j);
				}
				std::vector<flash_die_ID_type> die_ids;
				flow_die_id_assignments.push_back(die_ids);
				for (unsigned int j = 0; j < parameters->Flash_Parameters.Die_No_Per_Chip; j++)
				{
					flow_die_id_assignments[i].push_back(j);
				}
				std::vector<flash_plane_ID_type> plane_ids;
				flow_plane_id_assignments.push_back(plane_ids);
				for (unsigned int j = 0; j < parameters->Flash_Parameters.Plane_No_Per_Die; j++)
				{
					flow_plane_id_assignments[i].push_back(j);
				}
				break;
			}
			case HostInterface_Types::NVME:
			{
				stream_count = (unsigned int)io_flows->size();
				std::vector<flash_channel_ID_type> channel_ids;
				flow_channel_id_assignments.push_back(channel_ids);
				for (int j = 0; j < (*io_flows)[i]->Channel_No; j++)
				{
					flow_channel_id_assignments[i].push_back((*io_flows)[i]->Channel_IDs[j]);
				}
				std::vector<flash_chip_ID_type> chip_ids;
				flow_chip_id_assignments.push_back(chip_ids);
				for (int j = 0; j < (*io_flows)[i]->Chip_No; j++)
				{
					flow_chip_id_assignments[i].push_back((*io_flows)[i]->Chip_IDs[j]);
				}
				std::vector<flash_die_ID_type> die_ids;
				flow_die_id_assignments.push_back(die_ids);
				for (int j = 0; j < (*io_flows)[i]->Die_No; j++)
				{
					flow_die_id_assignments[i].push_back((*io_flows)[i]->Die_IDs[j]);
				}
				std::vector<flash_plane_ID_type> plane_ids;
				flow_plane_id_assignments.push_back(plane_ids);
				for (int j = 0; j < (*io_flows)[i]->Plane_No; j++)
				{
					flow_plane_id_assignments[i].push_back((*io_flows)[i]->Plane_IDs[j]);
				}
				break;
			}
			default:
				break;
			}
		}

		Utils::Logical_Address_Partitioning_Unit::Allocate_logical_address_for_flows(parameters->HostInterface_Type, (unsigned int)io_flows->size(),
																					 parameters->Flash_Channel_Count, parameters->Chip_No_Per_Channel, parameters->Flash_Parameters.Die_No_Per_Chip, parameters->Flash_Parameters.Plane_No_Per_Die,
																					 flow_channel_id_assignments, flow_chip_id_assignments, flow_die_id_assignments, flow_plane_id_assignments,
																					 parameters->Flash_Parameters.Block_No_Per_Plane, parameters->Flash_Parameters.Page_No_Per_Block,
																					 max_sector_num_per_page, parameters->Overprovisioning_Ratio);
		switch (parameters->Address_Mapping)
		{
		case SSD_Components::Flash_Address_Mapping_Type::PAGE_LEVEL:
			amu = new SSD_Components::Address_Mapping_Unit_Page_Level(ftl->ID() + ".AddressMappingUnit", ftl, (SSD_Components::NVM_PHY_ONFI *)device->PHY,
																	  fbm, parameters->Ideal_Mapping_Table, parameters->CMT_Capacity, parameters->Plane_Allocation_Scheme, stream_count,
																	  parameters->Flash_Channel_Count, parameters->Chip_No_Per_Channel, parameters->Flash_Parameters.Die_No_Per_Chip, parameters->Flash_Parameters.Plane_No_Per_Die,
																	  flow_channel_id_assignments, flow_chip_id_assignments, flow_die_id_assignments, flow_plane_id_assignments,
																	  parameters->Flash_Parameters.Block_No_Per_Plane, parameters->Flash_Parameters.Page_No_Per_Block,
																	  max_sector_num_per_page, max_sector_num_per_page * SECTOR_SIZE_IN_BYTE, parameters->Overprovisioning_Ratio,
																	  parameters->CMT_Sharing_Mode);
			break;
		case SSD_Components::Flash_Address_Mapping_Type::HYBRID:
			amu = new SSD_Components::Address_Mapping_Unit_Hybrid(ftl->ID() + ".AddressMappingUnit", ftl, (SSD_Components::NVM_PHY_ONFI *)device->PHY,
																  fbm, parameters->Ideal_Mapping_Table, stream_count,
																  parameters->Flash_Channel_Count, parameters->Chip_No_Per_Channel, parameters->Flash_Parameters.Die_No_Per_Chip,
																  parameters->Flash_Parameters.Plane_No_Per_Die, parameters->Flash_Parameters.Block_No_Per_Plane, parameters->Flash_Parameters.Page_No_Per_Block,
																  max_sector_num_per_page, max_sector_num_per_page * SECTOR_SIZE_IN_BYTE, parameters->Overprovisioning_Ratio);
			break;
		default:
			throw std::invalid_argument("No implementation is available fo the secified address mapping strategy");
		}
		Simulator->AddObject(amu);
		ftl->Address_Mapping_Unit = amu;

		//Step 8: create GC_and_WL_unit
		double max_rho = 0;
		for (unsigned int i = 0; i < io_flows->size(); i++)
		{
			if ((*io_flows)[i]->Initial_Occupancy_Percentage > max_rho)
			{
				max_rho = (*io_flows)[i]->Initial_Occupancy_Percentage;
			}
		}
		max_rho /= 100; //Convert from percentage to a value between zero and 1
		SSD_Components::GC_and_WL_Unit_Base *gcwl;
		gcwl = new SSD_Components::GC_and_WL_Unit_Page_Level(ftl->ID() + ".GCandWLUnit", amu, fbm, tsu, (SSD_Components::NVM_PHY_ONFI *)device->PHY,
															 parameters->GC_Block_Selection_Policy, parameters->GC_Exec_Threshold, parameters->Preemptible_GC_Enabled, parameters->GC_Hard_Threshold,
															 parameters->Flash_Channel_Count, parameters->Chip_No_Per_Channel,
															 parameters->Flash_Parameters.Die_No_Per_Chip, parameters->Flash_Parameters.Plane_No_Per_Die,
															 parameters->Flash_Parameters.Block_No_Per_Plane, parameters->Flash_Parameters.Page_No_Per_Block,
															 max_sector_num_per_page, parameters->Use_Copyback_for_GC, max_rho, 10,
															 parameters->Seed++);
		Simulator->AddObject(gcwl);
		fbm->Set_GC_and_WL_Unit(gcwl);
		ftl->GC_and_WL_Unit = gcwl;

		//Step 9: create Data_Cache_Manager
		SSD_Components::Data_Cache_Manager_Base *dcm;
		SSD_Components::Caching_Mode *caching_modes = new SSD_Components::Caching_Mode[io_flows->size()];
		for (unsigned int i = 0; i < io_flows->size(); i++)
		{
			caching_modes[i] = (*io_flows)[i]->Device_Level_Data_Caching_Mode;
		}

		switch (parameters->Caching_Mechanism)
		{
		case SSD_Components::Caching_Mechanism::SIMPLE:
			dcm = new SSD_Components::Data_Cache_Manager_Flash_Simple(device->ID() + ".DataCache", NULL, ftl, (SSD_Components::NVM_PHY_ONFI *)device->PHY,
																	  parameters->Data_Cache_Capacity, parameters->Data_Cache_DRAM_Row_Size, parameters->Data_Cache_DRAM_Data_Rate,
																	  parameters->Data_Cache_DRAM_Data_Busrt_Size, parameters->Data_Cache_DRAM_tRCD, parameters->Data_Cache_DRAM_tCL, parameters->Data_Cache_DRAM_tRP,
																	  caching_modes, (unsigned int)io_flows->size(),
																	  max_sector_num_per_page, parameters->Flash_Channel_Count * parameters->Chip_No_Per_Channel * parameters->Flash_Parameters.Die_No_Per_Chip * parameters->Flash_Parameters.Plane_No_Per_Die * max_sector_num_per_page);

			break;
		case SSD_Components::Caching_Mechanism::ADVANCED:
			dcm = new SSD_Components::Data_Cache_Manager_Flash_Advanced(device->ID() + ".DataCache", NULL, ftl, (SSD_Components::NVM_PHY_ONFI *)device->PHY,
																		parameters->Data_Cache_Capacity, parameters->Data_Cache_DRAM_Row_Size, parameters->Data_Cache_DRAM_Data_Rate,
																		parameters->Data_Cache_DRAM_Data_Busrt_Size, parameters->Data_Cache_DRAM_tRCD, parameters->Data_Cache_DRAM_tCL, parameters->Data_Cache_DRAM_tRP,
																		caching_modes, parameters->Data_Cache_Sharing_Mode, (unsigned int)io_flows->size(),
																		max_sector_num_per_page, parameters->Flash_Channel_Count * parameters->Chip_No_Per_Channel * parameters->Flash_Parameters.Die_No_Per_Chip * parameters->Flash_Parameters.Plane_No_Per_Die * max_sector_num_per_page);

			break;
		default:
			PRINT_ERROR("Unknown data caching mechanism!")
		}

		Simulator->AddObject(dcm);
		ftl->Data_cache_manager = dcm;
		device->Cache_manager = dcm;

		//Step 10: create Host_Interface
		switch (parameters->HostInterface_Type)
		{
		case HostInterface_Types::NVME:
			device->Host_interface = new SSD_Components::Host_Interface_NVMe(device->ID() + ".HostInterface",
																			 Utils::Logical_Address_Partitioning_Unit::Get_total_device_lha_count(), parameters->IO_Queue_Depth, parameters->IO_Queue_Depth,
																			 (unsigned int)io_flows->size(), parameters->Queue_Fetch_Size, max_sector_num_per_page, dcm);
			break;
		case HostInterface_Types::SATA:
			device->Host_interface = new SSD_Components::Host_Interface_SATA(device->ID() + ".HostInterface",
																			 parameters->IO_Queue_Depth, Utils::Logical_Address_Partitioning_Unit::Get_total_device_lha_count(), max_sector_num_per_page, dcm);

			break;
		default:
			break;
		}
		Simulator->AddObject(device->Host_interface);
		dcm->Set_host_interface(device->Host_interface);
		break;
	}
	default:
		throw std::invalid_argument("Undefined NVM type specified ");
	} // switch (Memory_Type)
}

SSD_Device::~SSD_Device()
{
	for (unsigned int channel_cntr = 0; channel_cntr < Channel_count; channel_cntr++)
	{
		for (unsigned int chip_cntr = 0; chip_cntr < Chip_no_per_channel; chip_cntr++)
		{
			delete ((SSD_Components::ONFI_Channel_NVDDR2 *)this->Channels[channel_cntr])->Chips[chip_cntr];
		}
		delete this->Channels[channel_cntr];
	}

	delete this->PHY;
	delete ((SSD_Components::FTL *)this->Firmware)->TSU;
	delete ((SSD_Components::FTL *)this->Firmware)->BlockManager;
	delete ((SSD_Components::FTL *)this->Firmware)->Address_Mapping_Unit;
	delete ((SSD_Components::FTL *)this->Firmware)->GC_and_WL_Unit;
	delete this->Firmware;
	delete this->Cache_manager;
	delete this->Host_interface;
}

void SSD_Device::Attach_to_host(Host_Components::PCIe_Switch *pcie_switch)
{
	this->Host_interface->Attach_to_device(pcie_switch);
}

void SSD_Device::Perform_preconditioning(std::vector<Utils::Workload_Statistics *> workload_stats)
{
	if (Preconditioning_required)
	{
		time_t start_time = time(0);
		PRINT_MESSAGE("SSD Device preconditioning started .........");
		this->Firmware->Perform_precondition(workload_stats);
		this->Cache_manager->Do_warmup(workload_stats);
		time_t end_time = time(0);
		uint64_t duration = (uint64_t)difftime(end_time, start_time);
		PRINT_MESSAGE("Finished preconditioning. Duration of preconditioning: " << duration / 3600 << ":" << (duration % 3600) / 60 << ":" << ((duration % 3600) % 60));
	}
}

void SSD_Device::Start_simulation()
{
}

void SSD_Device::Validate_simulation_config()
{
}

void SSD_Device::Execute_simulator_event(MQSimEngine::Sim_Event *event)
{
}

void SSD_Device::Report_results_in_XML(std::string name_prefix, Utils::XmlWriter &xmlwriter)
{
	std::string tmp;
	tmp = ID();
	xmlwriter.Write_open_tag(tmp);

	this->Host_interface->Report_results_in_XML(ID(), xmlwriter);
	if (Memory_Type == NVM::NVM_Type::FLASH)
	{
		((SSD_Components::FTL *)this->Firmware)->Report_results_in_XML(ID(), xmlwriter);
		((SSD_Components::FTL *)this->Firmware)->TSU->Report_results_in_XML(ID(), xmlwriter);

		for (unsigned int channel_cntr = 0; channel_cntr < Channel_count; channel_cntr++)
		{
			for (unsigned int chip_cntr = 0; chip_cntr < Chip_no_per_channel; chip_cntr++)
			{
				((SSD_Components::ONFI_Channel_NVDDR2 *)Channels[channel_cntr])->Chips[chip_cntr]->Report_results_in_XML(ID(), xmlwriter);
			}
		}
	}
	xmlwriter.Write_close_tag();
}

unsigned int SSD_Device::Get_no_of_LHAs_in_an_NVM_write_unit()
{
	return Host_interface->Get_no_of_LHAs_in_an_NVM_write_unit();
}

LPA_type SSD_Device::Convert_host_logical_address_to_device_address(LHA_type lha)
{
	return my_instance->Firmware->Convert_host_logical_address_to_device_address(lha);
}

page_status_type SSD_Device::Find_NVM_subunit_access_bitmap(LHA_type lha)
{
	return my_instance->Firmware->Find_NVM_subunit_access_bitmap(lha);
}
