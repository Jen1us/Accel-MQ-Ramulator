#include <algorithm>
#include <cstddef>
#include <string.h>
#include <stdexcept>
#include "../sim/Engine.h"
#include "Flash_Parameter_Set.h"
#include "../ssd/SSD_Defs.h"

// 初始化多参数集map
std::map<Flash_Technology_Type, Flash_Parameter_Set::Flash_Params> Flash_Parameter_Set::Flash_Parameter_Sets;
Flash_Parameter_Set::Flash_Params* Flash_Parameter_Set::current_params = nullptr;
bool Channel_Chip_Technology_Type[64] = {};	//	0为SLC	1为MLC

Flash_Technology_Type Flash_Parameter_Set::Flash_Technology = Flash_Technology_Type::MLC;
NVM::FlashMemory::Command_Suspension_Mode Flash_Parameter_Set::CMD_Suspension_Support = NVM::FlashMemory::Command_Suspension_Mode::ERASE;
sim_time_type Flash_Parameter_Set::Page_Read_Latency_LSB = 75000 * SIM_TIME_TICK_PER_NANOSECOND;
sim_time_type Flash_Parameter_Set::Page_Read_Latency_CSB = 75000 * SIM_TIME_TICK_PER_NANOSECOND;
sim_time_type Flash_Parameter_Set::Page_Read_Latency_MSB = 75000 * SIM_TIME_TICK_PER_NANOSECOND;
sim_time_type Flash_Parameter_Set::Page_Program_Latency_LSB = 750000 * SIM_TIME_TICK_PER_NANOSECOND;
sim_time_type Flash_Parameter_Set::Page_Program_Latency_CSB = 750000 * SIM_TIME_TICK_PER_NANOSECOND;
sim_time_type Flash_Parameter_Set::Page_Program_Latency_MSB = 750000 * SIM_TIME_TICK_PER_NANOSECOND;
sim_time_type Flash_Parameter_Set::Block_Erase_Latency = 3800000 * SIM_TIME_TICK_PER_NANOSECOND;//Block erase latency in nano-seconds
unsigned int Flash_Parameter_Set::Block_PE_Cycles_Limit = 10000;
sim_time_type Flash_Parameter_Set::Suspend_Erase_Time = 700000 * SIM_TIME_TICK_PER_NANOSECOND;//in nano-seconds
sim_time_type Flash_Parameter_Set::Suspend_Program_Time = 100000 * SIM_TIME_TICK_PER_NANOSECOND;//in nano-seconds
unsigned int Flash_Parameter_Set::Die_No_Per_Chip = 2;
unsigned int Flash_Parameter_Set::Plane_No_Per_Die = 2;
unsigned int Flash_Parameter_Set::Block_No_Per_Plane = 2048;
unsigned int Flash_Parameter_Set::Page_No_Per_Block = 256;//Page no per block
unsigned int Flash_Parameter_Set::Page_Capacity = 8192;//Flash page capacity in bytes
unsigned int Flash_Parameter_Set::Page_Metadat_Capacity = 1872;//Flash page capacity in bytes

double Flash_Parameter_Set::SLC_MLC_Ratio = 1; // add ed
uint64_t Flash_Parameter_Set::lpn_count_on_SLC = 0;

const Flash_Parameter_Set::Flash_Params& Flash_Parameter_Set::Get_Parameters(Flash_Technology_Type tech_type){
	auto it = Flash_Parameter_Set::Flash_Parameter_Sets.find(tech_type);
        if (it != Flash_Parameter_Set::Flash_Parameter_Sets.end()) {
			Flash_Parameter_Set::current_params = &(it->second);
            return *Flash_Parameter_Set::current_params;
        }
        // 如果找不到参数，抛出异常或返回默认值
        throw std::runtime_error("Flash parameter set not found for technology type");
}

Flash_Technology_Type Flash_Parameter_Set::Select_Technology_By_Address(int Channel_count, int channel_cntr, int chip_cntr)
{
	//return Flash_Technology_Type::SLC;
	//return Flash_Technology_Type::MLC;
	if(SLC_MLC_Ratio > 1)
	{
		if(!Channel_Chip_Technology_Type[ channel_cntr * 8 + chip_cntr * 2])	//查表获得当前颗粒类型
			return Flash_Technology_Type::SLC;
		else return Flash_Technology_Type::MLC;
	
	}
	else {
		// int SLC_num = SLC_MLC_Ratio * Chip_no_per_channel;
		// //int MLC_num = Chip_no_per_channel - SLC_num;
		
		// if(	chip_cntr + 1 > SLC_num)	
		// return Flash_Technology_Type::MLC;
		// else return Flash_Technology_Type::SLC;
		int SLC_num = SLC_MLC_Ratio * Channel_count;//获得分界线 channel_cntr
		int MLC_num = Channel_count - SLC_num;
		
		if(	channel_cntr + 1 > SLC_num)	
		return Flash_Technology_Type::MLC;
		else return Flash_Technology_Type::SLC;
	}
	
}


// int Flash_Parameter_Set::Page_Params_dif_tech(int Channel_count, int channel_cntr, int chip_cntr){
// 	Flash_Technology_Type selected_tech = Select_Technology_By_Address(Channel_count, channel_cntr, chip_cntr);	//added
	
// 	const Flash_Parameter_Set::Flash_Params& tech_params = Flash_Parameter_Set::Get_Parameters(selected_tech);
// 	return tech_params.Page_No_Per_Block;

// }

unsigned int Flash_Parameter_Set::Get_sector_num_per_page(Flash_Technology_Type tech_type)
{
	const auto& params = Flash_Parameter_Set::Get_Parameters(tech_type);
	if (params.Page_Capacity % SECTOR_SIZE_IN_BYTE != 0) {
		throw std::runtime_error("Page_Capacity must be a multiple of SECTOR_SIZE_IN_BYTE");
	}
	return params.Page_Capacity / SECTOR_SIZE_IN_BYTE;
}

unsigned int Flash_Parameter_Set::Get_sector_num_by_Address(unsigned int Channel_count, int channel_cntr, int chip_cntr)
{
	const Flash_Technology_Type tech = Flash_Parameter_Set::Select_Technology_By_Address((int)Channel_count, channel_cntr, chip_cntr);
	return Flash_Parameter_Set::Get_sector_num_per_page(tech);
}

unsigned int Flash_Parameter_Set::Get_Max_sector_num_per_page()
{
	unsigned int max_sectors = 0;
	for (const auto& pair : Flash_Parameter_Set::Flash_Parameter_Sets) {
		const auto& params = pair.second;
		if (params.Page_Capacity % SECTOR_SIZE_IN_BYTE != 0) {
			throw std::runtime_error("Page_Capacity must be a multiple of SECTOR_SIZE_IN_BYTE");
		}
		const unsigned int sector_num = params.Page_Capacity / SECTOR_SIZE_IN_BYTE;
		if (sector_num > max_sectors) max_sectors = sector_num;
	}

	// Fallback to legacy single-parameter static value (single-parameter mode)
	if (max_sectors == 0) {
		if (Flash_Parameter_Set::Page_Capacity % SECTOR_SIZE_IN_BYTE != 0) {
			throw std::runtime_error("Page_Capacity must be a multiple of SECTOR_SIZE_IN_BYTE");
		}
		max_sectors = Flash_Parameter_Set::Page_Capacity / SECTOR_SIZE_IN_BYTE;
	}

	return max_sectors;
}

void Flash_Parameter_Set::XML_serialize(Utils::XmlWriter& xmlwriter)
{
	std::string tmp;
	tmp = "Flash_Parameter_Set";
	xmlwriter.Write_open_tag(tmp);

	std::string attr = "Flash_Technology";
	std::string val;
	switch (Flash_Technology) {
		case Flash_Technology_Type::SLC:
			val = "SLC";
			break;
		case Flash_Technology_Type::MLC:
			val = "MLC";
			break;
		case Flash_Technology_Type::TLC:
			val = "TLC";
			break;
		default:
			break;
	}
	xmlwriter.Write_attribute_string(attr, val);

	attr = "CMD_Suspension_Support";
	switch (CMD_Suspension_Support) {
		case NVM::FlashMemory::Command_Suspension_Mode::NONE:
			val = "NONE";
			break;
		case NVM::FlashMemory::Command_Suspension_Mode::ERASE:
			val = "ERASE";
			break;
		case NVM::FlashMemory::Command_Suspension_Mode::PROGRAM:
			val = "PROGRAM";
			break;
		case NVM::FlashMemory::Command_Suspension_Mode::PROGRAM_ERASE:
			val = "PROGRAM_ERASE";
			break;
		default:
			break;
	}
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Page_Read_Latency_LSB";
	val = std::to_string(Page_Read_Latency_LSB);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Page_Read_Latency_CSB";
	val = std::to_string(Page_Read_Latency_CSB);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Page_Read_Latency_MSB";
	val = std::to_string(Page_Read_Latency_MSB);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Page_Program_Latency_LSB";
	val = std::to_string(Page_Program_Latency_LSB);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Page_Program_Latency_CSB";
	val = std::to_string(Page_Program_Latency_CSB);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Page_Program_Latency_MSB";
	val = std::to_string(Page_Program_Latency_MSB);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Block_Erase_Latency";
	val = std::to_string(Block_Erase_Latency);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Block_PE_Cycles_Limit";
	val = std::to_string(Block_PE_Cycles_Limit);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Suspend_Erase_Time";
	val = std::to_string(Suspend_Erase_Time);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Suspend_Program_Time";
	val = std::to_string(Suspend_Program_Time);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Die_No_Per_Chip";
	val = std::to_string(Die_No_Per_Chip);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Plane_No_Per_Die";
	val = std::to_string(Plane_No_Per_Die);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Block_No_Per_Plane";
	val = std::to_string(Block_No_Per_Plane);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Page_No_Per_Block";
	val = std::to_string(Page_No_Per_Block);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Page_Capacity";
	val = std::to_string(Page_Capacity);
	xmlwriter.Write_attribute_string(attr, val);

	attr = "Page_Metadat_Capacity";
	val = std::to_string(Page_Metadat_Capacity);
	xmlwriter.Write_attribute_string(attr, val);

	xmlwriter.Write_close_tag();
}

void Flash_Parameter_Set::XML_deserialize(rapidxml::xml_node<> *parent_node)
{
	try {
		// 调试：打印父节点名称，确认是否正确
		std::cout << "Parent node name: " << parent_node->name() << std::endl;
		// 调试：检查是否有任何子节点
		if (!parent_node->first_node()) {
			std::cout << "No child nodes found under parent node!" << std::endl;
		} else {
			// 调试：打印所有子节点名称
			for (auto node = parent_node->first_node(); node; node = node->next_sibling()) {
				std::cout << "Child node: " << node->name() << std::endl;
			}
		}

		for (auto set_node = parent_node->first_node("Flash_Parameter_Set"); set_node; set_node = set_node->next_sibling("Flash_Parameter_Set"))
		{
			Flash_Params params;
			for (auto param = set_node->first_node(); param; param = param->next_sibling()) {
				if (strcmp(param->name(), "Flash_Technology") == 0) {
					std::string val = param->value();
					std::transform(val.begin(), val.end(), val.begin(), ::toupper);
					if (strcmp(val.c_str(), "SLC") == 0)
						params.Flash_Technology = Flash_Technology_Type::SLC;
					else if (strcmp(val.c_str(), "MLC") == 0)
						params.Flash_Technology = Flash_Technology_Type::MLC;
					else if (strcmp(val.c_str(), "TLC") == 0)
						params.Flash_Technology = Flash_Technology_Type::TLC;
					else PRINT_ERROR("Unknown flash technology type specified in the input file")
				} else if (strcmp(param->name(), "CMD_Suspension_Support") == 0) {
					std::string val = param->value();
					std::transform(val.begin(), val.end(), val.begin(), ::toupper);
					if (strcmp(val.c_str(), "NONE") == 0) {
						params.CMD_Suspension_Support = NVM::FlashMemory::Command_Suspension_Mode::NONE;
					} else if (strcmp(val.c_str(), "ERASE") == 0) {
						params.CMD_Suspension_Support = NVM::FlashMemory::Command_Suspension_Mode::ERASE;
					} else if (strcmp(val.c_str(), "PROGRAM") == 0) {
						params.CMD_Suspension_Support = NVM::FlashMemory::Command_Suspension_Mode::PROGRAM;
					} else if (strcmp(val.c_str(), "PROGRAM_ERASE") == 0) {
						params.CMD_Suspension_Support = NVM::FlashMemory::Command_Suspension_Mode::PROGRAM_ERASE;
					} else {
						PRINT_ERROR("Unknown command suspension type specified in the input file")
					}
				} else if (strcmp(param->name(), "Page_Read_Latency_LSB") == 0) {
					std::string val = param->value();
					params.Page_Read_Latency_LSB = std::stoull(val) * SIM_TIME_TICK_PER_NANOSECOND;
				} else if (strcmp(param->name(), "Page_Read_Latency_CSB") == 0) {
					std::string val = param->value();
					params.Page_Read_Latency_CSB = std::stoull(val) * SIM_TIME_TICK_PER_NANOSECOND;
				} else if (strcmp(param->name(), "Page_Read_Latency_MSB") == 0) {
					std::string val = param->value();
					params.Page_Read_Latency_MSB = std::stoull(val) * SIM_TIME_TICK_PER_NANOSECOND;
				} else if (strcmp(param->name(), "Page_Program_Latency_LSB") == 0) {
					std::string val = param->value();
					params.Page_Program_Latency_LSB = std::stoull(val) * SIM_TIME_TICK_PER_NANOSECOND;
				} else if (strcmp(param->name(), "Page_Program_Latency_CSB") == 0) {
					std::string val = param->value();
					params.Page_Program_Latency_CSB = std::stoull(val) * SIM_TIME_TICK_PER_NANOSECOND;
				} else if (strcmp(param->name(), "Page_Program_Latency_MSB") == 0) {
					std::string val = param->value();
					params.Page_Program_Latency_MSB = std::stoull(val) * SIM_TIME_TICK_PER_NANOSECOND;
				} else if (strcmp(param->name(), "Block_Erase_Latency") == 0) {
					std::string val = param->value();
					params.Block_Erase_Latency = std::stoull(val) * SIM_TIME_TICK_PER_NANOSECOND;
				} else if (strcmp(param->name(), "Block_PE_Cycles_Limit") == 0) {
					std::string val = param->value();
					params.Block_PE_Cycles_Limit = std::stoul(val);
				} else if (strcmp(param->name(), "Suspend_Erase_Time") == 0) {
					std::string val = param->value();
					params.Suspend_Erase_Time = std::stoull(val) * SIM_TIME_TICK_PER_NANOSECOND;
				} else if (strcmp(param->name(), "Suspend_Program_Time") == 0) {
					std::string val = param->value();
					params.Suspend_Program_Time = std::stoull(val) * SIM_TIME_TICK_PER_NANOSECOND;
				} else if (strcmp(param->name(), "Die_No_Per_Chip") == 0) {
					std::string val = param->value();
					params.Die_No_Per_Chip = std::stoul(val);
				} else if (strcmp(param->name(), "Plane_No_Per_Die") == 0) {
					std::string val = param->value();
					params.Plane_No_Per_Die = std::stoul(val);
				} else if (strcmp(param->name(), "Block_No_Per_Plane") == 0) {
					std::string val = param->value();
					params.Block_No_Per_Plane = std::stoul(val);
				} else if (strcmp(param->name(), "Page_No_Per_Block") == 0) {
					std::string val = param->value();
					params.Page_No_Per_Block = std::stoul(val);
				} else if (strcmp(param->name(), "Page_Capacity") == 0) {
					std::string val = param->value();
					params.Page_Capacity = std::stoul(val);
				} else if (strcmp(param->name(), "Page_Metadat_Capacity") == 0) {
					std::string val = param->value();
					params.Page_Metadat_Capacity = std::stoul(val);
				}
			}
			Flash_Parameter_Sets[params.Flash_Technology] = params;
			
		} 
		
		select_configuration(Flash_Technology_Type::MLC);
		}
		catch (...) {
			PRINT_ERROR("Error in the Flash_Parameter_Set!")
		}

}

void Flash_Parameter_Set::XML_deserialize_single(rapidxml::xml_node<> *node){
	try {
		for (auto param = node->first_node(); param; param = param->next_sibling()) {
			if (strcmp(param->name(), "Flash_Technology") == 0) {
				std::string val = param->value();
				std::transform(val.begin(), val.end(), val.begin(), ::toupper);
				if (strcmp(val.c_str(), "SLC") == 0)
					Flash_Technology = Flash_Technology_Type::SLC;
				else if (strcmp(val.c_str(), "MLC") == 0)
					Flash_Technology = Flash_Technology_Type::MLC;
				else if (strcmp(val.c_str(), "TLC") == 0)
					Flash_Technology = Flash_Technology_Type::TLC;
				else PRINT_ERROR("Unknown flash technology type specified in the input file")
			} else if (strcmp(param->name(), "CMD_Suspension_Support") == 0) {
				std::string val = param->value();
				std::transform(val.begin(), val.end(), val.begin(), ::toupper);
				if (strcmp(val.c_str(), "NONE") == 0) {
					CMD_Suspension_Support = NVM::FlashMemory::Command_Suspension_Mode::NONE;
				} else if (strcmp(val.c_str(), "ERASE") == 0) {
					CMD_Suspension_Support = NVM::FlashMemory::Command_Suspension_Mode::ERASE;
				} else if (strcmp(val.c_str(), "PROGRAM") == 0) {
					CMD_Suspension_Support = NVM::FlashMemory::Command_Suspension_Mode::PROGRAM;
				} else if (strcmp(val.c_str(), "PROGRAM_ERASE") == 0) {
					CMD_Suspension_Support = NVM::FlashMemory::Command_Suspension_Mode::PROGRAM_ERASE;
				} else {
					PRINT_ERROR("Unknown command suspension type specified in the input file")
				}
			} else if (strcmp(param->name(), "Page_Read_Latency_LSB") == 0) {
				std::string val = param->value();
				Page_Read_Latency_LSB = std::stoull(val);
			} else if (strcmp(param->name(), "Page_Read_Latency_CSB") == 0) {
				std::string val = param->value();
				Page_Read_Latency_CSB = std::stoull(val);
			} else if (strcmp(param->name(), "Page_Read_Latency_MSB") == 0) {
				std::string val = param->value();
				Page_Read_Latency_MSB = std::stoull(val);
			} else if (strcmp(param->name(), "Page_Program_Latency_LSB") == 0) {
				std::string val = param->value();
				Page_Program_Latency_LSB = std::stoull(val);
			} else if (strcmp(param->name(), "Page_Program_Latency_CSB") == 0) {
				std::string val = param->value();
				Page_Program_Latency_CSB = std::stoull(val);
			} else if (strcmp(param->name(), "Page_Program_Latency_MSB") == 0) {
				std::string val = param->value();
				Page_Program_Latency_MSB = std::stoull(val);
			} else if (strcmp(param->name(), "Block_Erase_Latency") == 0) {
				std::string val = param->value();
				Block_Erase_Latency = std::stoull(val);
			} else if (strcmp(param->name(), "Block_PE_Cycles_Limit") == 0) {
				std::string val = param->value();
				Block_PE_Cycles_Limit = std::stoul(val);
			} else if (strcmp(param->name(), "Suspend_Erase_Time") == 0) {
				std::string val = param->value();
				Suspend_Erase_Time = std::stoull(val);
			} else if (strcmp(param->name(), "Suspend_Program_Time") == 0) {
				std::string val = param->value();
				Suspend_Program_Time = std::stoull(val);
			} else if (strcmp(param->name(), "Die_No_Per_Chip") == 0) {
				std::string val = param->value();
				Die_No_Per_Chip = std::stoul(val);
			} else if (strcmp(param->name(), "Plane_No_Per_Die") == 0) {
				std::string val = param->value();
				Plane_No_Per_Die = std::stoul(val);
			} else if (strcmp(param->name(), "Block_No_Per_Plane") == 0) {
				std::string val = param->value();
				Block_No_Per_Plane = std::stoul(val);
			} else if (strcmp(param->name(), "Page_No_Per_Block") == 0) {
				std::string val = param->value();
				Page_No_Per_Block = std::stoul(val);
			} else if (strcmp(param->name(), "Page_Capacity") == 0) {
				std::string val = param->value();
				Page_Capacity = std::stoul(val);
			} else if (strcmp(param->name(), "Page_Metadat_Capacity") == 0) {
				std::string val = param->value();
				Page_Metadat_Capacity = std::stoul(val);
			}
		}
	} catch (...) {
		PRINT_ERROR("Error in the Flash_Parameter_Set!")
	}
}
