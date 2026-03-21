# Accel-Sim / GPGPU-Sim：HBM(=DRAM) 参数、代码文件与功能梳理

> 说明：本项目里没有一个单独叫 “HBM” 的模块；**HBM/GDDR/DDR 都是通过同一套 DRAM 控制器模型 `dram_t` 来模拟**，区别主要来自配置（时序/带宽/地址映射/队列/调度策略等）。

## 1) 总体数据通路（从 SM 到 DRAM/HBM）

一个 memory 请求在模拟器里的“主要路径”大致如下（省略与本主题无关的细节）：

1. SM 执行到访存指令，产生 memory transaction，并构造 `mem_fetch`
2. `mem_fetch` 经过 L1/L2（以及 interconnect）
3. 进入 `memory_sub_partition`（L2 bank/子分区）和 `memory_partition_unit`（内存分区/通道）
4. 由 `memory_partition_unit` 把请求送入 `dram_t`（DRAM 控制器/通道模型）
5. `dram_t` 在 `cycle()` 中进行 bank/row/col 状态机与调度，完成后把请求放入 `returnq`
6. `memory_partition_unit` 把 `returnq` 的结果送回 `memory_sub_partition`（再回到 L2/icnt/qSM）

与 DRAM 时钟域相关的关键入口：
- `gpu-simulator/gpgpu-sim/src/gpgpu-sim/gpu-sim.cc`：`gpgpu_sim::dram_cycle()` 在 DRAM clock domain 下驱动每个 `memory_partition_unit` 的 `dram_cycle()`（或 `simple_dram_model_cycle()`）

## 2) DRAM/HBM 相关的“参数全集”（命令行/配置文件）

以下参数在 `memory_config::reg_options()` 或地址解码模块里注册（也就是你在 `gpgpusim.config` 里能直接写的那些）。

### 2.0（常用但“间接”）L2/Cache 配置项（会改变 DRAM/HBM 的 traffic）

| 参数 | 默认值 | 含义 | 主要落点 |
|---|---:|---|---|
| `-gpgpu_cache:dl2` | `S:32:128:24,L:B:m:L:P,A:192:4,32:0,32` | L2 配置串（容量/路数/替换/端口等；格式见参数说明） | `gpu-sim.cc` 注册；`gpu-sim.h`：`m_L2_config.init()`；`l2cache.cc`：`l2_cache` |
| `-gpgpu_cache:dl2_texture_only` | `1` | L2 是否只用于 texture（会改变 non-texture 访问路径） | `gpu-sim.cc`/`l2cache.cc` |
| `-l2_ideal` | `0` | 1 时 L2 永远命中（几乎消除 DRAM traffic） | `gpu-sim.cc`/`l2cache.cc`/`gpu-cache.*` |

### 2.1 拓扑/组织（channel/partition/subpartition）

| 参数 | 默认值 | 含义 | 主要落点 |
|---|---:|---|---|
| `-gpgpu_n_mem` | `8` | memory partition/MC 数量（通常可理解为通道数） | `gpu-sim.cc` 注册；`gpu-sim.h` 计算 `m_n_mem_sub_partition`；`l2cache.cc` 创建 partitions |
| `-gpgpu_n_sub_partition_per_mchannel` | `1` | 每个 partition 的 subpartition 数（通常对应 L2 bank/子分区数量） | `gpu-sim.h`/`l2cache.cc` |
| `-gpgpu_n_mem_per_ctrlr` | `1` | 每个 memory controller 下的 memory chip 数（影响单次 burst 能搬运的数据量） | `gpu-sim.h`：用于 `dram_atom_size = BL * busW * gpu_n_mem_per_ctrlr` |

### 2.2 L2/partition 内部队列深度（影响 backpressure）

| 参数 | 默认值 | 含义 | 主要落点 |
|---|---:|---|---|
| `-gpgpu_dram_partition_queues` | `8:8:8:8` | 四个 FIFO 深度：`icnt->L2 : L2->dram : dram->L2 : L2->icnt` | `l2cache.cc`：`memory_sub_partition` 构造 `m_icnt_L2_queue/m_L2_dram_queue/m_dram_L2_queue/m_L2_icnt_queue` |

### 2.3 DRAM 调度/队列（FR-FCFS/FIFO、队列上限）

| 参数 | 默认值 | 含义 | 主要落点 |
|---|---:|---|---|
| `-gpgpu_dram_scheduler` | `1` | `0=fifo`，`1=FR-FCFS` | `dram.cc`：FR-FCFS 会创建 `frfcfs_scheduler` |
| `-gpgpu_frfcfs_dram_sched_queue_size` | `0` | FR-FCFS 调度队列容量（`0=unlimited`） | `dram.cc`：`full()`/`queue_limit()`；`l2cache.cc`：credit 计算 |
| `-gpgpu_dram_return_queue_size` | `0` | DRAM return queue 容量（`0=unlimited`） | `dram.cc`：`returnq` 初始化；`l2cache.cc`：credit 计算 |
| `-dram_seperate_write_queue_enable` | `0` | 是否启用“独立写队列”（FR-FCFS 模式下） | `dram.cc`/`dram_sched.cc` |
| `-dram_write_queue_size` | `32:28:16` | 写队列配置（size:high_watermark:low_watermark） | `gpu-sim.h`：解析并存入 `gpgpu_frfcfs_dram_write_queue_size/write_high_watermark/write_low_watermark` |

### 2.4 带宽/颗粒度（busW、burst、data/command 频率比）

| 参数 | 默认值 | 含义 | 主要落点 |
|---|---:|---|---|
| `-gpgpu_dram_buswidth` | `4` | DRAM data bus 每个 data-cycle 传输的 bytes（注：文案里写“DDR 8B/cycle”） | `gpu-sim.h`：参与 `dram_atom_size` |
| `-gpgpu_dram_burst_length` | `4` | burst length（以 data bus cycle 为单位） | `gpu-sim.h`：参与 `tRTW/tWTR/tWTP` 与 `dram_atom_size` |
| `-dram_data_command_freq_ratio` | `2` | data bus 与 command bus 频率比（如 DDR/GDDR 的建模） | `gpu-sim.h`：参与 `tRTW/tWTR/tWTP` 与 `dram_atom_size` |
| `-icnt_flit_size` | `32` | interconnect flit 大小（会影响 mem_fetch 拆分/占用） | `l2cache.cc`/`mem_fetch.cc` 等 |

> 派生量（不是配置项，但会直接影响行为）：  
> `dram_atom_size = BL * busW * gpu_n_mem_per_ctrlr`（一次 read/write command 传输的字节数）

### 2.4.1 Bank indexing / bank-group 选位 / dual-bus（影响 bank-level 并行与冲突）

| 参数 | 默认值 | 含义 | 主要落点 |
|---|---:|---|---|
| `-dram_bnk_indexing_policy` | `0` | bank 索引策略（`0=linear,1=xor-hash,2=IPoly,3=custom`） | `dram.cc`：`dram_req_t` 选择 `bk` |
| `-dram_bnkgrp_indexing_policy` | `0` | bank group bit 位置策略（`0=high bits,1=low bits`） | `dram.cc`/`addrdec.cc`（映射/解码链路） |
| `-dram_dual_bus_interface` | `0` | 是否启用 dual bus interface（数据/命令等建模差异） | `gpu-sim.h`：作为参数下传给 DRAM 模型 |

### 2.5 DRAM 详细时序参数（`-gpgpu_dram_timing_opt`）

| 参数 | 默认值 | 含义 | 主要落点 |
|---|---:|---|---|
| `-gpgpu_dram_timing_opt` | `4:2:8:12:21:13:34:9:4:5:13:1:0:0` | DRAM timing 参数（支持 legacy 有序写法，或 `nbk=...:CCD=...` 这种具名写法） | `gpu-sim.h`：`memory_config::init()` 解析并计算派生量；`dram.cc`：使用这些计数器驱动 bank/row/col 状态机 |

该参数在 `gpu-sim.h` 中最终会填充/推导出以下字段（均为“周期”单位）：
- bank/row/col：`nbk`, `tCCD`, `tRRD`, `tRCD`, `tRAS`, `tRP`, `tRC`, `CL`, `WL`, `tCDLR`, `tWR`
- bank group：`nbkgrp`, `tCCDL`, `tRTPL`
- 派生：`tRCDWR`, `tRTW`, `tWTR`, `tWTP`, `bk_tag_length`

### 2.6 简化 vs 详细 DRAM 模型切换

| 参数 | 默认值 | 含义 | 主要落点 |
|---|---:|---|---|
| `-gpgpu_simple_dram_model` | `0` | `1` 时绕过 `dram_t` 的 bank 模型，仅用固定延迟/带宽队列近似 | `gpu-sim.cc`：选择调用 `memory_partition_unit::simple_dram_model_cycle()` |

### 2.7 L2/ROP 与“固定延迟”参数（非 DRAM 时序表的一部分）

| 参数 | 默认值 | 含义 | 主要落点 |
|---|---:|---|---|
| `-gpgpu_l2_rop_latency` | `85` | ROP delay queue 的延迟（非 texture path 会先进 ROP 再回到 L2 侧） | `l2cache.cc`：`memory_sub_partition::push()` 使用 |
| `-dram_latency` | `30` | partition 内部 DRAM latency queue 的固定延迟（在送入 `dram_t` 之前的额外延迟） | `l2cache.cc`：`memory_partition_unit::dram_cycle()` 使用 |
| `-dram_elimnate_rw_turnaround` | `0` | 设为 1 时令 `tRTW/tWTR = 0`（消除读写切换惩罚） | `gpu-sim.h`：`memory_config::init()` |

### 2.8 地址解码/分片（影响“去哪个 partition/bank/row/col”）

这些参数来自 `addrdec` 模块（注意它是 DRAM 模型前置的一部分）：

| 参数 | 默认值 | 含义 | 主要落点 |
|---|---:|---|---|
| `-gpgpu_mem_addr_mapping` | `NULL` | 地址映射串：把线性地址映射到 `{chip,row,bk,col,burst,sub_partition}` | `addrdec.cc` |
| `-gpgpu_mem_addr_test` | `0` | 启用 sweep test 检查映射是否别名 | `addrdec.cc` |
| `-gpgpu_mem_address_mask` | `0` | 地址 mask 选择（老/新/翻转 chip/bank 位） | `addrdec.cc` |
| `-gpgpu_memory_partition_indexing` | `0` | partition 索引函数：`0=none,1=xor,2=IPoly,3=custom` | `addrdec.cc` |

### 2.9 统计/日志

| 参数 | 默认值 | 含义 | 主要落点 |
|---|---:|---|---|
| `-gpgpu_memlatency_stat` | `0` | 延迟统计/队列日志开关（文案：`0x2` 使能 MC，`0x4` 使能 queue logs） | `dram.cc`、`mem_latency_stat.*`、`gpu-sim.cc` 等 |

> 另外还有 `-gpgpu_perf_sim_memcpy`（是否在 memcpy 时填充 L2）等，会影响“进入 DRAM 的 traffic 形态”，但不属于 DRAM bank 模型本身。

## 3) 关键代码文件与职责（你改 HBF 时需要对照的部分）

### 3.1 配置与参数承载

- `gpu-simulator/gpgpu-sim/src/gpgpu-sim/gpu-sim.cc`
  - `memory_config::reg_options()`：注册所有 memory/DRAM 相关 config 参数（含 timing、queue、scheduler、address mapping 入口等）
  - `gpgpu_sim::dram_cycle()`：DRAM clock domain 驱动器（调用每个 partition 的 DRAM cycle）

- `gpu-simulator/gpgpu-sim/src/gpgpu-sim/gpu-sim.h`
  - `class memory_config`：保存参数、在 `init()` 中解析 `-gpgpu_dram_timing_opt` 并计算派生时序/带宽量

### 3.2 地址解码（线性地址 → {chip/bank/row/col/...}）

- `gpu-simulator/gpgpu-sim/src/gpgpu-sim/addrdec.h`
- `gpu-simulator/gpgpu-sim/src/gpgpu-sim/addrdec.cc`
  - `linear_to_raw_address_translation::addrdec_tlx()`：核心解码函数
  - `partition_address()`：把地址“挤掉 chip/subpartition 位”得到 partition 内地址

### 3.3 访存请求对象（承载 decoded 地址与状态机）

- `gpu-simulator/gpgpu-sim/src/gpgpu-sim/mem_fetch.h`
- `gpu-simulator/gpgpu-sim/src/gpgpu-sim/mem_fetch.cc`
  - 构造时调用 `config->m_address_mapping.addrdec_tlx()` 填充 `addrdec_t m_raw_addr`
  - `m_request_uid` 用于统计/追踪；`set_status()` 记录流水各阶段

### 3.4 memory_sub_partition / memory_partition_unit：L2 与 DRAM 的胶水层

- `gpu-simulator/gpgpu-sim/src/gpgpu-sim/l2cache.h`
- `gpu-simulator/gpgpu-sim/src/gpgpu-sim/l2cache.cc`
  - `memory_sub_partition`：
    - `m_icnt_L2_queue / m_L2_dram_queue / m_dram_L2_queue / m_L2_icnt_queue`（由 `-gpgpu_dram_partition_queues` 控制）
    - `m_rop`：由 `-gpgpu_l2_rop_latency` 控制的 ROP delay queue（非 texture path）
  - `memory_partition_unit`：
    - 负责在 DRAM clock domain 下把请求送到 `dram_t`，并把 returnq 的回复送回 subpartition
    - 包含 `arbitration_metadata`（credit-based）用于限制不同 subpartition 进入 DRAM 的速度
    - `simple_dram_model_cycle()`：简化模型路径
    - `dram_cycle()`：详细模型路径（调用 `m_dram->cycle()`）

### 3.5 DRAM 控制器（bank 状态机 + bus/turnaround + scheduler）

- `gpu-simulator/gpgpu-sim/src/gpgpu-sim/dram.h`
- `gpu-simulator/gpgpu-sim/src/gpgpu-sim/dram.cc`
  - `dram_req_t`：把 `mem_fetch` 变成 DRAM 视角的请求（抽取 `{bk,row,col}`，并根据 indexing policy 做 bank 选择）
  - `dram_t`：核心 DRAM 模型
    - `push()`：请求进入 MC
    - `cycle()`：每个 DRAM 周期推进 bank 状态机与发命令
    - `returnq`：完成后推回的队列

- `gpu-simulator/gpgpu-sim/src/gpgpu-sim/dram_sched.h`
- `gpu-simulator/gpgpu-sim/src/gpgpu-sim/dram_sched.cc`
  - `frfcfs_scheduler`：FR-FCFS 逻辑（按 row-hit 优先、并可选独立写队列/水位线切换读写模式）

## 4) 示例：SM86_RTX3070 配置里 DRAM 相关项（便于对照）

文件：`gpu-simulator/gpgpu-sim/configs/tested-cfgs/SM86_RTX3070/gpgpusim.config`

典型条目（节选）：
- `-gpgpu_n_mem 16`
- `-gpgpu_dram_partition_queues 64:64:64:64`
- `-gpgpu_memory_partition_indexing 2`
- `-gpgpu_l2_rop_latency 187`
- `-dram_latency 254`
- `-gpgpu_dram_return_queue_size 192`
- `-gpgpu_dram_buswidth 2`
- `-gpgpu_dram_burst_length 16`
- `-dram_data_command_freq_ratio 4`
- `-gpgpu_mem_addr_mapping ...`
- `-gpgpu_dram_timing_opt nbk=16:CCD=4:RRD=12:RCD=24:...`

