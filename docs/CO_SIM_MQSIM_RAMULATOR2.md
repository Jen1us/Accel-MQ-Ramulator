# Accel-Sim + MQSim(HBF) + Ramulator2(HBM) 联合仿真方案（设计草案）

本文目标：把 **Accel-Sim/GPGPU-Sim** 作为 GPU 核心/缓存/互连主仿真器（master timeline），
把 **Ramulator2** 作为 **HBM/HBM2/HBM3（HBM backend）**，把 **MQSim** 作为 **HBF（High Bandwidth Flash backend）**，
三者在一次仿真中协同运行，输出每个 kernel 的周期、访存统计、带宽/利用率，并保持 AccelWattch 的功耗统计入口可用。

> 如果你要看“代码级调用链/实现细节/机制详解”（非常详细、带文件+行号），请看：
> `docs/CO_SIM_MQSIM_RAMULATOR2_DEV.md`

> 约束：我们不再使用 Accel-Sim 现有的 `dram_t` / `hbf_t` 细节时序模型（即 `dram.cc` / `hbf.cc` 的实现逻辑不再作为“真实后端”），
> 但可以复用它们在 Accel-Sim 管线中的**接口形状**（push/full/cycle/returnq），以最小化对上游（L2/partition）代码的侵入。

---

## 0. 当前实现状态（已跑通 minimal trace）

我们已经在 repo 内实现了“外部后端”的**可运行版本**（shared 模式）：

### 0.1 HBM：Ramulator2（shared）
- **dlopen wrapper**：`external_wrappers/ramulator2_wrap/build/libramulator2_wrap.so`
- **GPGPU-Sim 后端**：`gpu-simulator/gpgpu-sim/src/gpgpu-sim/ramulator2_backend.cc`
- **使能选项**：
  - `-hbm_use_ramulator2 1`
  - `-hbm_ramulator2_wrapper external_wrappers/ramulator2_wrap/build/libramulator2_wrap.so`
  - `-hbm_ramulator2_config workloads/ramulator2/hbm2_16ch.yaml`
- **注意**：HBM2 的 Ramulator2 配置里 RowPolicy 不能用 `ClosedRowPolicy`（它依赖 `rank` 和 `close-row` 请求），
  已在 `workloads/ramulator2/hbm2_16ch.yaml` 中改为 `OpenRowPolicy`。

### 0.2 HBF：MQSim（shared, 路线1：直接注入 Flash Transaction）
- **dlopen wrapper**：`external_wrappers/mqsim_wrap/build/libmqsim_wrap.so`
- **GPGPU-Sim 后端**：`gpu-simulator/gpgpu-sim/src/gpgpu-sim/mqsim_backend.cc`
- **MQSim Engine 改动**：支持外部步进 `Initialize_simulation()` / `Run_until(...)`（`MQSim/src/sim/Engine.*`）
- **使能选项**：
  - `-hbf_use_mqsim 1`
  - `-hbf_mqsim_wrapper external_wrappers/mqsim_wrap/build/libmqsim_wrap.so`
  - `-hbf_mqsim_config MQSim/ssdconfig.xml`（后续可换成 HBF 专用 XML）
  - `-hbf_subpage_bytes 512`（用固定 subpage 粒度注入 MQSim）

### 0.3 Mixed（HBF=MQSim + HBM=Ramulator2）
- 示例 overlay：`workloads/minimal_hbf_driver/hbf_mqsim_hbm_ram2.config`
- 分区映射：`[0,8)` 为 HBF，其余为 HBM
- HBF/HBM tagging trace：`hw_run/traces_jenius/minimal_mix_mqsim_ram2/hbf_requests.trace`

---

## 0. 现状：Accel-Sim 内存后端挂接点在哪里？

### 0.1 Partition 级别的后端调用链（当前代码）

Accel-Sim 的 L2 miss（或写回）会进入每个 **memory_sub_partition** 的 `L2_dram_queue`，
随后由 **memory_partition_unit** 仲裁，把请求发往 DRAM 控制器（或 HBF 控制器），并最终从后端 return queue 返回到 `dram_L2_queue`。

关键入口在：
- `gpu-simulator/gpgpu-sim/src/gpgpu-sim/l2cache.cc:76`：`memory_partition_unit::memory_partition_unit(...)`
  - 当前直接 `new dram_t(...)` + `new hbf_t(...)`
- `gpu-simulator/gpgpu-sim/src/gpgpu-sim/l2cache.cc:357`：`memory_partition_unit::dram_cycle()`
  - 每周期：
    1) `m_dram->return_queue_top/pop`、`m_hbf->return_queue_top/pop` 把完成请求送回 sub-partition
    2) `m_dram->cycle()`、`m_hbf->cycle()` 推进后端
    3) 仲裁发射：对每个 sub-partition 取 `L2_dram_queue_top()`，调用 `m_dram->full()/m_hbf->full()` 决定能否发射
    4) 发射成功后调用 `m_dram->push(mf)` 或 `m_hbf->push(mf)`

因此：只要我们提供一个“后端对象”，实现与 `dram_t/hbf_t` 等价的接口语义，
并且保证 `full/push/cycle/return_queue_*` 的流控与时序正确，L2/ICNT/warp 侧不用改。

---

## 1. 总体架构：做一个 External Memory Backend Adapter 层

### 1.1 目标接口（最小侵入版）

保持与现有 `dram_t/hbf_t` 一致的接口集合（概念上）：
- `bool full(bool is_write)`
- `void push(mem_fetch* mf)`
- `void cycle()`：推进后端时间，产生完成项
- `mem_fetch* return_queue_top()` / `return_queue_pop()`
- （可选）`print_stat/visualize/set_power_stats` 维持现有统计/功耗入口

实现方式：
- 新增一个轻量抽象基类 `mem_backend_ifc`（可选），或直接用两个 wrapper 类替换 `dram_t/hbf_t` 的内部实现。
- 对 HBM 分区：使用 `HBMBackendRamulator2`
- 对 HBF 分区：使用 `HBFBackendMQSim`

### 1.2 时间基准：以 GPU cycle 为主，统一换算到 ns

Accel-Sim 的主时间为 `gpu_sim_cycle + gpu_tot_sim_cycle`（见 `l2cache.cc:358-359`）。
外部后端各自有时钟：
- Ramulator2：memory system 的 `tCK`（ns）+ 自身 tick 周期
- MQSim：事件驱动，`sim_time_type` 默认就是 ns（`ONE_SECOND=1e9`）

建议做法（统一口径）：
- 定义 `gpu_clk_ns = 1e9 / core_freq_hz`（从 Accel-Sim config 或现有参数推导）
- 对每个后端维护自己的 `backend_time_ns`
- 在 `cycle()` 被调用时：
  - 计算当前 `gpu_time_ns = gpu_cycle * gpu_clk_ns`
  - 将后端推进到 `gpu_time_ns`（Ramulator2：tick 若干次；MQSim：跑到 target time）

这样做的好处：
- 两个外部后端可以共用同一套“推进到某个 wall-clock(ns)”的驱动逻辑
- 后端内部可以按自己的节拍运行，且 Accel-Sim 仍然是 master

---

## 2. HBM 后端（Ramulator2）集成方案

### 2.1 Ramulator2 提供的“作为库”的推荐接口

Ramulator2 README 已给出 wrapper 模板（你仓库：`ramulator2/README.md`）：
- 关键头文件：
  - `ramulator2/src/base/base.h`
  - `ramulator2/src/base/request.h`
  - `ramulator2/src/base/config.h`
  - `ramulator2/src/frontend/frontend.h`
  - `ramulator2/src/memory_system/memory_system.h`
- 创建与连接：
  - `Config::parse_config_file(...)`
  - `Factory::create_frontend(config)`
  - `Factory::create_memory_system(config)`
  - `frontend->connect_memory_system(ms)`
  - `ms->connect_frontend(frontend)`
- 外部注入请求：
  - 推荐用 `Frontend=GEM5`（实现文件：`ramulator2/src/frontend/impl/external_wrapper/gem5_frontend.cpp`）
  - 调用 `frontend->receive_external_requests(req_type_id, addr, source_id, callback)`
    - `req_type_id`：0=Read，1=Write（`ramulator2/src/base/request.h`）
    - `callback`：请求完成时触发，我们把对应 `mem_fetch*` 放回 returnq

### 2.2 一个 partition 对应一个 Ramulator2 实例，还是共享一个多通道实例？

这是最关键的映射选择，会影响你“partition=channel=带宽单位”的实验可解释性：

方案 A（推荐起步，最稳）：
- **每个 Accel-Sim memory partition 创建 1 个 Ramulator2 memory system**
  - 配置成 1 channel 的 HBM/HBM2/HBM3
  - partition 的所有 HBM 请求都发送到自己的实例
优点：
- 不需要强行对齐两边的地址→channel 映射
- partition-level 的流控、带宽统计最直观（你之前的思路就是“stack=多个 partition”）
缺点：
- 实例数=HBM partitions（例如 32），C++ 对象与统计可能偏重

方案 B（性能更好，但要对齐地址映射）：
- 只创建 1 个（或每 stack 1 个）Ramulator2，多通道 channel_count = partitions
- 让 Ramulator2 自己按地址 bit mapping 决定 channel
问题：
- Accel-Sim 的地址→partition 交织方式与 Ramulator2 的 channel mapping 可能不一致
- 会出现“partition i 发出的地址被 Ramulator2 映射到 channel j”的错位

结论：建议先用方案 A 跑通 end-to-end；等稳定后再考虑 B。

### 2.3 full/push/cycle/returnq 的实现语义（HBMBackendRamulator2）

**push(mf)**：
- 取 `addr = mf->get_addr()`（byte address）
- `req_type_id = mf->is_write() ? 1 : 0`
- `source_id`：可用 `mf->get_sid()` 或统一 0（只是 Ramulator2 统计来源）
- 传入 callback：
  - 在 callback 中记录 `depart/arrive`（Ramulator2 Request 有 arrive/depart）
  - 把 `mem_fetch*` 放入本 backend 的 `returnq`

**full(is_write)**：
- Ramulator2 的 `receive_external_requests` 返回 false 表示没入队（队列满或 backpressure）
- 我们可以做两层流控：
  1) 先用一个本地 `max_outstanding` 限制（避免 Accel-Sim 死等）
  2) 实际 send 失败则认为 full

**cycle()**：
- 推进 Ramulator2 时间到 `gpu_time_ns`
  - 需要从 Ramulator2 `ms->get_tCK()` 取得 memory tick 的 ns
  - 计算要 tick 的次数：`ticks = floor((gpu_time_ns - backend_time_ns) / tCK)`
  - `for i in ticks: ms->tick()`
  - `backend_time_ns += ticks * tCK`

**return_queue_top/pop**：
- 直接返回本地队列头即可

### 2.4 “大小(size)”问题

Ramulator2 的 Request 只有 addr/type，没有 size。
在 Accel-Sim 中，一个 `mem_fetch` 通常代表一个 L2 line 或 sector 访问（取决于配置）。
建议：
- **起步阶段：1 个 mem_fetch = 1 个 Ramulator2 request**（忽略 size）
- 后续若需要更精细：
  - 当 `mf->get_data_size()` > Ramulator2 事务粒度时，拆成多个请求（但会改变 L2 合并效果）

---

## 3. HBF 后端（MQSim）集成方案

MQSim 原生定位是 “NVMe/SATA SSD 端到端 I/O 仿真器”（`externals/MQSim/README.md`），
而我们要的是 “像 HBM 一样的 memory backend（大量小请求、极高并行、无文件系统语义）”。
所以这里必须先明确我们在 MQSim 中**要复用的层级**是哪一段。

### 3.1 两种集成路线

路线 1（推荐起步，侵入小、可控、贴近你的“性能占位模型”诉求）：
- **只复用 MQSim 的 Flash 芯片/通道/TSU/PHY 时序与并行模型**
- 不走 NVMe host/PCIe、不走复杂 FTL（或仅走最简 mapping）
- 由 Accel-Sim 直接注入 `NVM_Transaction_Flash_RD/WR`（带物理地址），让 MQSim 返回完成时刻

路线 2（复用更多 MQSim 逻辑，更“SSD”但不一定更“HBFlash”）：
- 构建完整 Host_System + SSD_Device + Host_Interface_NVMe
- Accel-Sim 把 mem_fetch 映射成 NVMe I/O（LBA/sector），通过 host interface 注入
- 让 MQSim 自己处理 queue/dequeue/缓存/FTL/GC/WL 等
问题：
- 会引入 PCIe/NVMe 等协议延迟，不符合“贴近显存”的 HBF
- 对小粒度 GPU request 的匹配很差（MQSim 更像 4KB/16KB I/O）

结论：**路线 1 更符合你现在“只要性能仿真、不要细节逻辑”的目标**，同时也更容易把“内部并行”写出来并可控。

### 3.2 MQSim 的时间推进：必须支持 RunUntil(time_ns)

MQSim 的引擎是事件驱动：
- `externals/MQSim/src/sim/Engine.cpp:43`：`Engine::Start_simulation()` 会一直跑到事件队列空

为了与 Accel-Sim 联合仿真，需要在 MQSim 增加一个“可增量推进”的 API，例如：
- `Engine::Initialize_once()`：只做 triggers/validate/start（不进入 while(true) 主循环）
- `Engine::RunUntil(sim_time_type t_end)`：处理所有 `Fire_time <= t_end` 的事件，停在 t_end

这样 `HBFBackendMQSim::cycle()` 就可以：
- `mqsim_engine.RunUntil(gpu_time_ns)`
- 完成事件触发回调，把对应的 `mem_fetch*` 放入 returnq

### 3.3 请求粒度与地址映射（把 GPU 地址映射到 channel/die/plane/page）

你之前的 HBF 建模已经有非常明确的结构假设：
- 1 stack = 512GB
- 1 stack = 8 partitions（channel）
- 总 stacks = 4（所以 partitions=32）

我们在 MQSim 路线 1 中建议做一个显式映射：
- `stack_id = (addr / stack_bytes) % num_stacks`
- `channel_id_within_stack = partition_id % partitions_per_stack`（或从 addr 取低位做 striping）
- `page = (addr % stack_bytes) / page_bytes`
- `die_id = page % dies_per_channel`
- `plane_id = (page / dies_per_channel) % planes_per_die`
- `block/page_in_block`：可以用简单 hash 或线性映射（不做 GC/WL 时只要不越界即可）

对应到 MQSim 的物理地址结构 `NVM::FlashMemory::Physical_Page_Address`（见 `externals/MQSim/src/nvm_chip/flash_memory/Physical_Page_Address.h`），
我们直接填入 (ChannelID, ChipID, DieID, PlaneID, BlockID, PageID) 来表达“内部并行度”。

### 3.4 写语义（posted write）如何落到 MQSim？

你前面明确了 HBF 写语义优先按 posted：
- GPU 侧认为 write 在“写数据被接受/进入缓冲”时就完成
- 后台 program（tPROG）只影响缓冲回收与后续写入吞吐

MQSim 天然支持“DRAM cache + destage”的概念（`Data_Cache_Manager_*`）：
- 如果启用 `WRITE_CACHE`，写可以先进入 DRAM cache，再异步落到 flash
- host completion 可以被解释为 posted ack

在路线 1（跳过 host interface）下，我们可以更直接：
- 对 write mem_fetch：
  - 立即（或加一个可配置 ack latency）把 mf 放回 returnq（posted ack）
  - 同时向 MQSim 注入一个 program transaction，用于占用 die/plane/通道资源、影响后续吞吐

这样既满足 posted 语义，又保留“内部并行 + program time”对带宽上限的约束。

### 3.5 full/push/cycle/returnq 的实现语义（HBFBackendMQSim）

**push(mf)**：
- 计算映射得到物理页地址、以及这次访问覆盖的 sector bitmap（可先简化为整页）
- read：
  - 注入 `NVM_Transaction_Flash_RD`，callback=完成时把 mf 推入 returnq
- write：
  - posted ack：先把 mf 推入 returnq（或延迟若干 ns）
  - 同时注入 `NVM_Transaction_Flash_WR`（用于消耗 program 资源），完成回调只用于回收内部 credit，不再把 mf 返回

**full(is_write)**：
- 至少需要本地 `max_outstanding`（否则 Accel-Sim 会无限 push）
- MQSim 侧也要有队列上限（例如每 channel 的 TSU slot/等待队列），否则内存爆

**cycle()**：
- `mqsim_engine.RunUntil(gpu_time_ns)`

**returnq**：
- 本地队列

---

## 4. HBM + HBF 同时存在时（混合路由/随机 tagging）怎么做？

Accel-Sim 当前对每个 mem_fetch 做一次 `is_hbf_request(uid, addr, partition_id)` 判定（见 `l2cache.cc:417-420`），
然后分别进入 `m_dram_latency_queue` 或 `m_hbf_latency_queue`（见 `l2cache.cc:438-448`），最后调用对应后端 `push`。

因此混合路由可以保持不变：
- `m_dram` 换成 `HBMBackendRamulator2`
- `m_hbf` 换成 `HBFBackendMQSim`

需要注意两点：
1) 如果我们未来想让 “同一个 partition 同时连接 HBM 与 HBF（共享端口）”，
   需要定义清楚仲裁规则（目前代码每 cycle 最多 issue 1 个请求，已经天然限制住了）。
2) `dram_latency_queue/hbf_latency_queue` 是“固定延迟占位”（之前你用来模拟 L2->backend 的调度/链路延迟），
   外部后端已有更真实的延迟时，这个固定延迟应当：
   - 要么置 0（推荐），避免 double count
   - 要么只保留为 “NoC/PHY interface” 的固定成本，但要明确口径

---

## 5. 统计/带宽/功耗：怎么在联合仿真里保持可解释？

### 5.1 延迟统计

Accel-Sim 的 offchip 延迟统计在 `mem_latency_stat` 里按 `mem_fetch::mem_backend` 区分。
联合仿真里：
- `mem_fetch` 仍然会被标记为 HBM/HBF（见 `l2cache.cc:417-420`）
- completion 时机由外部后端决定
因此：现有统计机制可以继续用，只要我们保证：
- mf 完成时正确设置 status/时间戳
- returnq 机制不丢包、不重复

### 5.2 带宽口径

建议统一口径（与你之前 sim.log 的 link_util/internal_util 类似）：
- **HBM BW（per partition）**：`完成的read/write字节数 / GPU模拟时间`
- **HBF BW（per stack）**：`8 partitions 之和` 或 `MQSim 内部统计的 channel bytes`

关键是：你之前“1 stack 1.2TB/s”的定义，其实就是：
- 每 stack 的 8 个 partition 同时以各自的 link 速率工作
- stack 带宽 = sum(partition_bandwidth)

联合仿真里：
- Ramulator2：可以从 stats 或我们自己按完成字节数统计
- MQSim：可以从 transaction 的 transfer_time 或我们自己统计注入/完成字节数

### 5.3 功耗

你前面已经定了方向：功耗走现有 AccelWattch/AccelWattch 的框架，只做系数缩放。
联合仿真建议：
- 外部后端只提供“读/写/请求数 + 传输字节数 + 活跃周期数”这类计数
- AccelWattch 侧用你配置的系数换算成能耗
- Ramulator2 自己也能输出能耗/功耗，但口径和 AccelWattch 不一致，建议先不混用

---

## 6. 构建与工程化（避免 C++20 把整个 Accel-Sim 搞崩）

Ramulator2 是 C++20，并且 CMake 会下载依赖；Accel-Sim/GPGPU-Sim 目前是较老的 C++ 标准链路。
为了降低集成风险，建议采用“隔离式 wrapper 动态库”的工程方案：

### 6.1 推荐工程方案：后端 wrapper 单独编译成 .so（C ABI），Accel-Sim 只做 dlopen/调用

- `libramulator2_wrap.so`（C++20 编译）
  - 内部 include Ramulator2 头文件，创建对象、tick、send、poll completion
  - 对外导出 C 接口：
    - `void* ram2_create(const char* yaml_path_or_dump, ...)`
    - `int   ram2_send(void* h, uint64_t addr, int is_write, void* user_ptr)`
    - `int   ram2_tick_to(void* h, uint64_t time_ns)`
    - `int   ram2_poll(void* h, Completion* out)`（返回 user_ptr + 完成时间）

- `libmqsim_wrap.so`（C++11 编译）
  - 内部包含 MQSim 源码或链接静态库
  - 提供 `RunUntil`、`SubmitRead/SubmitWrite`、`Poll` 等接口

Accel-Sim 侧（C++11/14）只需要：
- 持有 `void* handle`
- 在 `cycle()` 里调用 `tick_to(gpu_time_ns)` + `poll` 把完成项放回 returnq

优点：
- 不需要把整个 gpgpu-sim 升级到 C++20
- 外部依赖（yaml-cpp/spdlog）不会污染 Accel-Sim 的构建系统

缺点：
- 多一层 wrapper/ABI 维护（但最可控）

### 6.2 备选：直接把 Accel-Sim 全部切到 C++20 并链接 libramulator.so

可行但风险更高：需要全仓库编译选项联动，且可能引入大量警告/行为差异。

---

## 7. 里程碑 / TODO（建议按这个顺序做，便于快速跑通）

M0. 需求确认（你拍板即可）
- MQSim 只用“Flash 时序+并行”，不引入 NVMe/PCIe/FTL/GC（路线 1）是否 OK？
- HBF 写是否一律 posted（ack 早于 program 完成）？
- HBM/HBF 固定延迟队列（`-dram_latency/-hbf_dram_latency`）是否全部置 0？

M1. 先把 HBM 换成 Ramulator2（单后端验证）
- 只替换 `m_dram`，保留 `m_hbf` 为 no-op 或禁用
- 跑一个 rodinia 或你现有 minimal driver，确保不会死锁、latency 合理

M2. 接入 MQSim（最小 flash backend）
- MQSim engine 增量推进（RunUntil）
- 注入 read transaction，能返回 completion
- 写先做 posted ack + 背景 program 占用资源

M3. 混合路由（HBM+HBF 同时跑）
- 保持 `is_hbf_request()` 路由逻辑不变
- 跑你之前的 mix50 / all_hbf / baseline 三组实验

M4. 统计与带宽对齐
- 输出 per-kernel：HBF/HBM 请求数、完成字节、平均/最大延迟、有效 BW、后端利用率
- 和你现有 sim.log/trace 的口径对齐

---

## 8. 需要你补充/确认的数据（用于把参数填进 MQSim/Ramulator2）

### HBF（MQSim）侧
- stack 数、每 stack 容量（你目前：4 stacks，每 stack 512GB）
- 每 stack 的 channel/partition 数（你目前：8）
- 每 channel 的 die/plane 结构（决定并行度）
- page/subpage/sector 粒度（建议先用 MQSim 的 `Page_Capacity` 或你定义的 subpage）
- tREAD/tPROG/tERASE（你已有范围）
- “目标带宽 1.2TB/s/stack”对应的 link/通道传输速率（需要换算到 MQSim 的 `Channel_Transfer_Rate` 等参数或我们 wrapper 的 link 模型）

### HBM（Ramulator2）侧
- HBM 代际：HBM2 还是 HBM3
- channel 数（与 partitions 对齐还是 1:1 实例）
- tCK/frequency 与 GPU clock 的比例（我们需要决定同步策略）

---

## 9. 我建议我们先讨论的 4 个关键决策点

1) MQSim 走路线 1 还是路线 2？
2) HBM partition 与 Ramulator2 channel 的映射：实例 per partition（稳）还是多通道共享（快）？
3) 写语义：posted 默认 + 可配置 ack latency，是否满足你的论文/实验假设？
4) 地址映射：是否以“stack=512GB 地址区间”做硬映射（你刚刚的想法），还是让地址 hash 分散到全局？
