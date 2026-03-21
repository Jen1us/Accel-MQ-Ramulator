# HBF 实现状态

## 范围说明
当前 HBF（High Bandwidth Flash）实现为**性能导向的占位时序模型**。
它**不复用 DRAM 控制器代码**，但提供了与 DRAM 相似的接口
（push/full/cycle/return queue），以便现有仿真管线将请求路由到 HBF 后端。

## 已实现功能

### 已修改/新增文件
- `gpu-simulator/gpgpu-sim/src/gpgpu-sim/hbf.h`
- `gpu-simulator/gpgpu-sim/src/gpgpu-sim/hbf.cc`
- `gpu-simulator/gpgpu-sim/src/gpgpu-sim/l2cache.cc`
- `gpu-simulator/gpgpu-sim/src/gpgpu-sim/gpu-sim.h`
- `gpu-simulator/gpgpu-sim/src/gpgpu-sim/gpu-sim.cc`
- `gpu-simulator/gpgpu-sim/src/gpgpu-sim/mem_latency_stat.cc`
- `gpu-simulator/gpgpu-sim/src/gpgpu-sim/power_stat.h`

### 请求路由与标记
- **HBF 选择策略**：`memory_config::is_hbf_request()` 支持（代码：
  `gpu-simulator/gpgpu-sim/src/gpgpu-sim/gpu-sim.h:420` `memory_config::is_hbf_partition`,
  `gpu-simulator/gpgpu-sim/src/gpgpu-sim/gpu-sim.h:425` `memory_config::is_hbf_request`）：
  - 按分区映射（`-hbf_partition_start`, `-hbf_partition_count`）
  - 按请求伪随机映射（`-hbf_random_access`, `-hbf_random_access_percent`,
    `-hbf_random_access_seed`）
  - 按地址区间映射（`-hbf_addr_range_start`, `-hbf_addr_range_size`）
- **请求后端标记**：`mem_fetch::MEM_BACKEND_HBF` 与
  `mem_fetch::MEM_BACKEND_HBM` 用于统计区分（代码：
  `gpu-simulator/gpgpu-sim/src/gpgpu-sim/l2cache.cc:306` `memory_partition_unit::simple_dram_model_cycle`,
  `gpu-simulator/gpgpu-sim/src/gpgpu-sim/l2cache.cc:416` `memory_partition_unit::dram_cycle`）。
- **L2/ROP 固定延迟**：`-hbf_l2_rop_latency` 与 `-hbf_dram_latency`
  用于 L2/partition 队列中“进入 HBF 前”的固定延迟建模（代码：
  `gpu-simulator/gpgpu-sim/src/gpgpu-sim/l2cache.cc:926` `memory_sub_partition::push`,
  `gpu-simulator/gpgpu-sim/src/gpgpu-sim/l2cache.cc:321` `memory_partition_unit::simple_dram_model_cycle`,
  `gpu-simulator/gpgpu-sim/src/gpgpu-sim/l2cache.cc:431` `memory_partition_unit::dram_cycle`）。
- **请求追踪**：`-hbf_request_trace` 输出 CSV trace（默认
  `hbf_requests.trace`），标记 HBF/HBM、cycle、uid、partition、address、access
  type（代码：
  `gpu-simulator/gpgpu-sim/src/gpgpu-sim/gpu-sim.cc:413` `memory_config::hbf_request_trace_open`,
  `gpu-simulator/gpgpu-sim/src/gpgpu-sim/gpu-sim.cc:432` `memory_config::hbf_request_trace_log`,
  `gpu-simulator/gpgpu-sim/src/gpgpu-sim/l2cache.cc:327` `memory_partition_unit::simple_dram_model_cycle`,
  `gpu-simulator/gpgpu-sim/src/gpgpu-sim/l2cache.cc:437` `memory_partition_unit::dram_cycle`）。

### HBF 控制器核心（时序模型）
- **控制器接口**：`hbf_t` 提供 `push/full/cycle/return_queue`，
  队列与容量由 `-hbf_max_outstanding`、`-hbf_write_buffer_entries`、
  `-hbf_dram_return_queue_size` 限制（代码：
  `gpu-simulator/gpgpu-sim/src/gpgpu-sim/hbf.h:33` `hbf_t`,
  `gpu-simulator/gpgpu-sim/src/gpgpu-sim/hbf.cc:90` `hbf_t::queue_limit/full/returnq_full`,
  `gpu-simulator/gpgpu-sim/src/gpgpu-sim/hbf.cc:210` `hbf_t::push`,
  `gpu-simulator/gpgpu-sim/src/gpgpu-sim/hbf.cc:265` `hbf_t::cycle`）。
- **链路模型**（每个 partition）：
  - 串行传输，带宽由 `-hbf_channel_bytes_per_cycle` 与
    `-hbf_subpage_bytes` 决定。
  - 使用“分数周期”累计，减少整周期舍入误差（代码：
    `gpu-simulator/gpgpu-sim/src/gpgpu-sim/hbf.cc:111` `hbf_t::reserve_link_transfer`）。
- **读时序**：读完成 = `tREAD`（按 plane 串行） + 链路传输时间（代码：
  `gpu-simulator/gpgpu-sim/src/gpgpu-sim/hbf.cc:251` `hbf_t::push`,
  `gpu-simulator/gpgpu-sim/src/gpgpu-sim/hbf.cc:167` `hbf_t::reserve_plane`,
  `gpu-simulator/gpgpu-sim/src/gpgpu-sim/hbf.cc:111` `hbf_t::reserve_link_transfer`）。
- **写时序**：posted write 语义（代码：
  `gpu-simulator/gpgpu-sim/src/gpgpu-sim/hbf.cc:224` `hbf_t::push`,
  `gpu-simulator/gpgpu-sim/src/gpgpu-sim/hbf.cc:167` `hbf_t::reserve_plane`）。
  - ACK 在“写数据传输完成 + `-hbf_posted_write_ack_latency`”之后返回。
  - 后台程序阶段在 `-hbf_t_prog` 之后释放 write buffer（按 plane 串行）。
- **内部并行度**：die/plane 级别串行化建模（`-hbf_dies_per_channel`、
  `-hbf_planes_per_die`）（代码：
  `gpu-simulator/gpgpu-sim/src/gpgpu-sim/hbf.cc:22` `hbf_t::hbf_t`,
  `gpu-simulator/gpgpu-sim/src/gpgpu-sim/hbf.cc:139` `hbf_t::map_to_plane`,
  `gpu-simulator/gpgpu-sim/src/gpgpu-sim/hbf.cc:167` `hbf_t::reserve_plane`）。
  - 请求按 **全局物理页**（`-hbf_page_bytes`）哈希到 plane，避免 DRAM
    交织导致的偏差（代码：
    `gpu-simulator/gpgpu-sim/src/gpgpu-sim/hbf.cc:139` `hbf_t::map_to_plane`）。
- **分区统计**：`sim.log` 输出链路利用率、实际 BW、内部 plane 利用率（代码：
  `gpu-simulator/gpgpu-sim/src/gpgpu-sim/hbf.cc:302` `hbf_t::print`）。

### 统计与功耗
- **延迟统计**：`mem_latency_stat` 统计
  `offchip_hbf_num_mfs`、`offchip_hbf_total_lat`、
  `offchip_hbf_max_mf_latency`（代码：
  `gpu-simulator/gpgpu-sim/src/gpgpu-sim/mem_latency_stat.cc:195` `memory_stats_t::memlatstat_done`）。
- **功耗对接**：`set_hbf_power_stats()` 把读/写/请求数量接入
  AccelWattch（仅计数级别）（代码：
  `gpu-simulator/gpgpu-sim/src/gpgpu-sim/hbf.cc:347` `hbf_t::set_hbf_power_stats`,
  `gpu-simulator/gpgpu-sim/src/gpgpu-sim/l2cache.cc:497` `memory_partition_unit::set_hbf_power_stats`,
  `gpu-simulator/gpgpu-sim/src/gpgpu-sim/power_stat.h:125` `mem_power_stats_pod::hbf_n_rd/hbf_n_wr/hbf_n_req`,
  `gpu-simulator/gpgpu-sim/src/gpgpu-sim/power_stat.h:1155` `power_stat_t::get_hbf_rd/get_hbf_wr`）。
- **死锁检测保护**：HBF 启用时自动放大 deadlock 检测间隔，容忍长 tREAD
  与链路排队（代码：
  `gpu-simulator/gpgpu-sim/src/gpgpu-sim/gpu-sim.h:603` `gpgpu_sim::init`）。

## 未实现 / 仍需完善

### Flash 功能行为
- 无 FTL、磨损均衡、垃圾回收、坏块管理或 ECC。
- 无 block 擦除流程；`-hbf_t_erase` 与 `-hbf_block_pages` 目前未被使用。
- 无读改写、子页更新逻辑（仅固定 subpage 传输大小）。
- 无 page buffer、cache、copyback 或 prefetch。

### 详细时序与调度
- HBF 内部未实现 DRAM 风格调度（FR-FCFS、bank/row timing）。
- 以下配置项**存在但当前模型未使用**：
  - `-hbf_scheduler_type`, `-hbf_frfcfs_dram_sched_queue_size`
  - `-hbf_dram_timing_opt`
  - `-hbf_busW`, `-hbf_BL`, `-hbf_data_command_freq_ratio`
  - `-hbf_dram_bnk_indexing_policy`, `-hbf_dram_bnkgrp_indexing_policy`
  - `-hbf_seperate_write_queue_enabled`, `-hbf_write_queue_size_opt`,
    `-hbf_elimnate_rw_turnaround`
- 每个 partition 仅建模**单链路串行传输**（无多链路聚合）。

### 功耗模型细节
- 仅计数级功耗，无命令级或 plane 级功耗细分，也没有漏电/热模型。

### 拓扑与映射
- 无显式 stack / stack-link 拓扑（partition 等价于 channel）。
- plane 映射为 hash 占位，没有现实的物理布局或严格映射规则。

## 代码位置
- HBF 控制器：`gpu-simulator/gpgpu-sim/src/gpgpu-sim/hbf.cc`
- HBF 接口：`gpu-simulator/gpgpu-sim/src/gpgpu-sim/hbf.h`
- 请求路由与 trace：`gpu-simulator/gpgpu-sim/src/gpgpu-sim/gpu-sim.h`
- 选项注册 + trace writer：`gpu-simulator/gpgpu-sim/src/gpgpu-sim/gpu-sim.cc`
- L2/partition 集成：`gpu-simulator/gpgpu-sim/src/gpgpu-sim/l2cache.cc`
- 延迟统计：`gpu-simulator/gpgpu-sim/src/gpgpu-sim/mem_latency_stat.cc`
- 功耗统计对接：`gpu-simulator/gpgpu-sim/src/gpgpu-sim/power_stat.h`
