#ifndef FLASH_PARAMETER_SET_H
#define FLASH_PARAMETER_SET_H

#include "../sim/Sim_Defs.h"
#include "../nvm_chip/flash_memory/FlashTypes.h"
#include "Parameter_Set_Base.h"
#include <map>

class Flash_Parameter_Set : Parameter_Set_Base
{
public:
	static Flash_Technology_Type Flash_Technology;

	struct Flash_Params { 	//补充自定义颗粒参数配置
		Flash_Technology_Type Flash_Technology;
		NVM::FlashMemory::Command_Suspension_Mode CMD_Suspension_Support;
		sim_time_type Page_Read_Latency_LSB;
		sim_time_type Page_Read_Latency_CSB;
		sim_time_type Page_Read_Latency_MSB;
		sim_time_type Page_Program_Latency_LSB;
		sim_time_type Page_Program_Latency_CSB;
		sim_time_type Page_Program_Latency_MSB;
		sim_time_type Block_Erase_Latency;
		unsigned int Block_PE_Cycles_Limit;
		sim_time_type Suspend_Erase_Time;
		sim_time_type Suspend_Program_Time;
		unsigned int Die_No_Per_Chip;
		unsigned int Plane_No_Per_Die;
		unsigned int Block_No_Per_Plane;
		unsigned int Page_No_Per_Block;
		unsigned int Page_Capacity;
		unsigned int Page_Metadat_Capacity;

		


		Flash_Params(){
			//构造函数，默认配置；；》
			Flash_Technology = Flash_Technology_Type::MLC;
			CMD_Suspension_Support = NVM::FlashMemory::Command_Suspension_Mode::ERASE;
			Page_Read_Latency_LSB = 75000 * SIM_TIME_TICK_PER_NANOSECOND;
			Page_Read_Latency_CSB = 75000 * SIM_TIME_TICK_PER_NANOSECOND;
			Page_Read_Latency_MSB = 75000 * SIM_TIME_TICK_PER_NANOSECOND;
			Page_Program_Latency_LSB = 750000 * SIM_TIME_TICK_PER_NANOSECOND;
			Page_Program_Latency_CSB = 750000 * SIM_TIME_TICK_PER_NANOSECOND;
			Page_Program_Latency_MSB = 750000 * SIM_TIME_TICK_PER_NANOSECOND;
			Block_Erase_Latency = 3800000 * SIM_TIME_TICK_PER_NANOSECOND;//Block erase latency in nano-seconds
			Block_PE_Cycles_Limit = 10000;
			Suspend_Erase_Time = 700000 * SIM_TIME_TICK_PER_NANOSECOND;//in nano-seconds
			Suspend_Program_Time = 100000 * SIM_TIME_TICK_PER_NANOSECOND;//in nano-seconds
			Die_No_Per_Chip = 2;
			Plane_No_Per_Die = 2;
			Block_No_Per_Plane = 2048;
			Page_No_Per_Block = 256;//Page no per block
			Page_Capacity = 8192;//Flash page capacity in bytes
			Page_Metadat_Capacity = 1872;//Flash page capacity in bytes
		
		}
	};

	static double SLC_MLC_Ratio; // add ed

	static NVM::FlashMemory::Command_Suspension_Mode CMD_Suspension_Support;
	static sim_time_type Page_Read_Latency_LSB;
	static sim_time_type Page_Read_Latency_CSB;
	static sim_time_type Page_Read_Latency_MSB;
	static sim_time_type Page_Program_Latency_LSB;
	static sim_time_type Page_Program_Latency_CSB;
	static sim_time_type Page_Program_Latency_MSB;
	static sim_time_type Block_Erase_Latency;//Block erase latency in nano-seconds
	static unsigned int Block_PE_Cycles_Limit;
	static sim_time_type Suspend_Erase_Time;//in nano-seconds
	static sim_time_type Suspend_Program_Time;//in nano-seconds
	static unsigned int Die_No_Per_Chip;
	static unsigned int Plane_No_Per_Die;
	static unsigned int Block_No_Per_Plane;
	static unsigned int Page_No_Per_Block;//Page no per block
	static unsigned int Page_Capacity;//Flash page capacity in bytes
	static unsigned int Page_Metadat_Capacity;//Flash page metadata capacity in bytes

	static uint64_t lpn_count_on_SLC;
	//可以把它理解为 NAND 页的 OOB/Spare 区容量（用于存 ECC、LPA、坏块标记等元数据）的“理论配置项”，
	//但当前实现里并没有把“页数据区 + OOB”作为传输/存储的真实字节数来建模。
	void XML_serialize(Utils::XmlWriter& xmlwriter);
	void XML_deserialize(rapidxml::xml_node<> *node);// 一次读取多个参数集
	
	void XML_deserialize_single(rapidxml::xml_node<> *node);//原解析函数-单参数集

	// 支持多种颗粒参数：type调取
	static std::map<Flash_Technology_Type, Flash_Params> Flash_Parameter_Sets;
	static Flash_Params* current_params;	//参数集指针-》颗粒类型
	
	// 根据技术类型获取参数集
	static const Flash_Params& Get_Parameters(Flash_Technology_Type tech_type);
	// 选择配置
	static bool select_configuration(Flash_Technology_Type tech) {
        auto it = Flash_Parameter_Sets.find(tech);
        if (it != Flash_Parameter_Sets.end()) {
            current_params = &(it->second);
            return true;
        }
        return false;
    }
	// 获取当前参数
    static Flash_Params& get_current_params() {
        if (current_params == nullptr) {
            // 尝试选择默认配置
            if (!select_configuration(Flash_Technology_Type::MLC)) {
                throw std::runtime_error("No flash configuration available!");
            }
        }
        return *current_params;
    }
	static Flash_Technology_Type Select_Technology_By_Address(int Channel_count, int channel_cntr, int chip_cntr);

	int Page_Params_dif_tech(int Channel_count, int channel_cntr, int chip_cntr);

	//接下来这边进行，page参数中，自定义sector的函数设计
	static unsigned int Get_sector_num_per_page(Flash_Technology_Type tech_type);
	//获取不同page capacity下每个sector num
	static unsigned int Get_sector_num_by_Address(unsigned int Channel_count, int channel_cntr, int chip_cntr);
	//按ratio或查表，划分Channel下（查表细化到Chip下）颗粒类型

	static unsigned int Get_Max_sector_num_per_page();
};

#endif // !FLASH_PARAMETER_SET_H
