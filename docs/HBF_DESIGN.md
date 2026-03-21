## Accel-Sim：HBF（High Bandwidth Flash）性能占位实现（Phase 1）

这份文档描述的是 **“只做性能仿真（时延/带宽/队列/功耗）”** 的 HBF 设计与落地说明：不建模 FTL/GC/die/plane/page-buffer 等细节逻辑，但要在模拟器里把 **HBF 与 HBM(=DRAM)** 明确区分开，并能在 trace 里看到 `HBF/HBM`。

## 0. 目标与非目标

**目标（你当前要的）**
- **独立 HBF 后端模块**：不复用 `dram_t` 的 bank/row/col 逻辑（Flash 与 DRAM 原理不同）。
- **固定时延 + 链路带宽**：读/写用少量参数拟合性能。
- **访问粒度固定**：统一用 `hbf_subpage_bytes`（默认 512B）。
- **写语义 posted**：写入缓冲即 ACK，后台按 `tPROG` 回收缓冲占用。
- **功耗接入现有 GPUWattch/AccelWattch**：不新增复杂组件，先走现有 memory-controller power 路径。
- **可观测**：仿真时输出 `hbf_requests.trace`（CSV），每条请求标 `HBF/HBM`。

**非目标（Phase 1 明确不做）**
- FTL/L2P、GC、磨损均衡、erase、page coalescing、多 plane/die 资源竞争等真实 Flash 行为。

## 1. 代码落点（已实现）

- HBF 控制器（独立模块）：`gpu-simulator/gpgpu-sim/src/gpgpu-sim/hbf.h`、`gpu-simulator/gpgpu-sim/src/gpgpu-sim/hbf.cc`
- 路由与胶水层：`gpu-simulator/gpgpu-sim/src/gpgpu-sim/l2cache.h`、`gpu-simulator/gpgpu-sim/src/gpgpu-sim/l2cache.cc`
  - `memory_partition_unit` 同时持有 `dram_t`（HBM）与 `hbf_t`（HBF），按 `memory_config::is_hbf_request()` 分流。
- 配置与 trace：`gpu-simulator/gpgpu-sim/src/gpgpu-sim/gpu-sim.h`、`gpu-simulator/gpgpu-sim/src/gpgpu-sim/gpu-sim.cc`
  - 增加 `hbf_*` 参数、`is_hbf_request()`、`-hbf_request_trace`（CSV）。
- 功耗接入：`gpu-simulator/gpgpu-sim/src/gpgpu-sim/l2cache.cc`
  - HBM(=DRAM) 与 HBF 分别计数：HBM 用 `MEM_RD/MEM_WR`，HBF 用 `HBF_RD/HBF_WR`（用于独立能耗系数）。

## 2. Phase 1 时序模型（固定时延 + 链路带宽）

> 单位：**模拟器 cycle**。链路带宽通过 `hbf_channel_bytes_per_cycle` 表示。

**Read**
- 完成时间：`tREAD + link_queue_delay + ceil(hbf_subpage_bytes / hbf_channel_bytes_per_cycle)`
  - `link_queue_delay` 来自 **HBF 链路资源串行化**：每个 partition 视为一条独立链路，transfer 会排队占用。

**Write（posted）**
- ACK：写数据 transfer 完成后 `hbf_posted_write_ack_latency` cycles 即返回（posted semantics）
- 后台回收写缓冲：`tPROG`（transfer 已在 ACK 前计入链路占用）

## 3. HBF/HBM 选择策略（现在支持的最小集）

优先级从高到低：
1) `-hbf_random_access`：按 request_uid 做确定性 pseudo-random 打标（方便 smoke test）
2) `-hbf_addr_range_start/-hbf_addr_range_size`：地址范围映射到 HBF
3) `-hbf_partition_start/-hbf_partition_count`：按 memory partition 静态划分

## 4. 你需要提供/确认的参数（Phase 1 最小集）

**真正会影响 HBF timing 的参数**
- `-hbf_t_read`、`-hbf_t_prog`
- `-hbf_subpage_bytes`
- `-hbf_channel_bytes_per_cycle`
- `-hbf_max_outstanding`（读 outstanding 上限，按 partition 计）
- `-hbf_write_buffer_entries`（posted write buffer 容量，按 entry 计）
- `-hbf_posted_write_ack_latency`

**只影响“走哪边”的参数**
- `-hbf_random_access{,_percent,_seed}` 或 `-hbf_addr_range_*` 或 `-hbf_partition_*`

**目前仅占位/未来用（不驱动 Phase 1 timing）**
- `-hbf_page_bytes`、`-hbf_block_pages`、`-hbf_dies_per_channel`、`-hbf_planes_per_die`、`-hbf_t_erase`

> 需要你确认的关键口径：`hbf_channel_bytes_per_cycle` 是“每个 partition/channel 的有效 bytes/cycle”还是“全系统聚合带宽折算到每个 partition”。当前实现按 **每个 partition** 解释。

## 5. 功耗接入方式（HBF/HBM 分开系数）

AccelWattch 当前把 **memory controller** 当作一个组件，用 “访问计数 × 系数” 的方式做能耗标定。为了让 HBF 与 HBM 的能耗系数不同，本项目新增了两条 HBF 专用计数器：
- `HBF_RD`：HBF reads
- `HBF_WR`：HBF writes

模拟器侧：
- `memory_partition_unit::set_dram_power_stats()` 只上报 **HBM(=DRAM)** 计数；
- `memory_partition_unit::set_hbf_power_stats()` 上报 **HBF** 计数；
- power interface 把两者分别传入 AccelWattch。

配置方式（在 AccelWattch XML 里加系数）：
- `<param name="MEM_RD" value="..."/>`、`<param name="MEM_WR" value="..."/>`：HBM/DRAM
- `<param name="HBF_RD" value="..."/>`、`<param name="HBF_WR" value="..."/>`：HBF

兼容性：
- 如果 XML 里 **没有** 写 `HBF_RD/HBF_WR`，默认会回退为 `HBF_RD=MEM_RD`、`HBF_WR=MEM_WR`（即把 HBF 当作 HBM 能耗）。

## 6. 如何验证（看得到 HBF/HBM）

运行 trace-driven 仿真时打开：
- `-hbf_request_trace 1`
- `-hbf_request_trace_file <path>`

输出是 CSV：`cycle,uid,partition,subpartition,addr,is_write,access_type,tag`，其中 `tag` 是 `HBF/HBM`。

## 6.1 直接在 `gpgpusim.config` 里配置 HBF（不依赖 overlay）

现在 `gpu-simulator/gpgpu-sim/configs/tested-cfgs/*/gpgpusim.config` 末尾都带了一段 **HBF 参数块**（默认禁用）。你只要把其中的开关从 `0` 改成你需要的值即可：

- 典型：按 partition 开启 HBF  
  - `-hbf_partition_start 0`  
  - `-hbf_partition_count <n>`（比如设成 `-gpgpu_n_mem` 表示全是 HBF）
- 调试：随机打标（最小 smoke）  
  - `-hbf_random_access 1`  
  - `-hbf_random_access_percent 50`
- 只看 latency：建议打开 `-gpgpu_memlatency_stat 14`，看 sim.log 里的  
  - `offchip_hbm_averagemflatency / offchip_hbm_maxmflatency`  
  - `offchip_hbf_averagemflatency / offchip_hbf_maxmflatency`

## 7. TODO（只保留你当前关心的）

- 你给一组真实的 `tREAD/tPROG/bandwidth/subpage`（以及单位换算口径），我把默认值和 config overlay 调到可用区间。
- 如果你要按 “HBM cache + HBF backing” 做 hot/cold 迁移：下一步在 `is_hbf_request()` 上层补一个策略 hook（先从 address range/LRU 二选一做）。
- 如果你想要 “功耗报告里也拆出 HBF/HBM 两个功耗项”，下一步可以在 AccelWattch component 层面加一个独立的 HBF power component（当前先共用一个 mem-ctrl 组件，只是系数拆分）。

## 8. HBF 性能参数 vs DRAM（HBM）参数对照

结论先行：**当一个请求被打标为 HBF 时，它不再进入 `dram_t` 的 bank/row/scheduler 模型**，而是进入 `hbf_t` 的 “固定时延 + 链路带宽 + 队列” 模型；因此 **DRAM 的绝大多数 timing/bus 参数不会影响 HBF**（除非你显式让 HBF 继承/复用某些“胶水层固定延迟”参数）。

### 8.1 DRAM（HBM）侧：性能由哪些 config 控制？

DRAM 的 off-chip 行为来自两类参数：

1) **L2→MC 固定延迟（胶水层）**
   - `-dram_latency`：L2→DRAM 固定延迟（出队/仲裁后的固定 pipeline）
   - `-gpgpu_l2_rop_latency`：ROP 队列延迟（影响部分写路径）

2) **DRAM controller 模型（`dram_t`）**
   - `-gpgpu_dram_timing_opt`：bank/row 相关 timing（`tRCD/tRAS/tRP/tCCD/...`）
   - `-gpgpu_dram_buswidth`、`-gpgpu_dram_burst_length`、`-dram_data_command_freq_ratio`、`-dram_dual_bus_interface`：决定每次 column command 的传输粒度与节拍
   - `-gpgpu_frfcfs_dram_sched_queue_size`、`-gpgpu_dram_return_queue_size`、`-dram_write_queue_size` 等：队列大小与调度行为

其中 DRAM 每次 column command 的“原子传输字节数”在代码里是：
- `dram_atom_size = BL * busW * gpu_n_mem_per_ctrlr`（见 `gpu-simulator/gpgpu-sim/src/gpgpu-sim/gpu-sim.h:347`）

### 8.2 HBF 侧：性能由哪些 config 控制？

HBF 的 off-chip 行为来自：

1) **L2→HBF 固定延迟（胶水层）**
   - `-hbf_dram_latency`：L2→HBF 固定延迟（默认 `0` 表示继承 `-dram_latency`）
   - `-hbf_l2_rop_latency`：HBF 路径上的 ROP 队列延迟（默认 `0` 继承 `-gpgpu_l2_rop_latency`）

2) **HBF controller（`hbf_t`）Phase 1 模型**
   - `-hbf_t_read`：读固定时延（cycle）
   - `-hbf_subpage_bytes`：固定访问粒度（默认 512B）
   - `-hbf_channel_bytes_per_cycle`：每个 partition 的有效链路带宽（bytes/cycle）
   - `-hbf_max_outstanding`：读 outstanding 上限（每个 partition）
   - `-hbf_write_buffer_entries`、`-hbf_posted_write_ack_latency`、`-hbf_t_prog`：posted write 路径

读完成时间在代码里是：
- `tREAD + link_queue_delay + ceil(subpage_bytes / channel_bytes_per_cycle)`（HBF link 串行化实现见 `gpu-simulator/gpgpu-sim/src/gpgpu-sim/hbf.cc`）

### 8.3 用 QV100-SASS（rodinia trace）这组 config 做一个直观对比

以 `hw_run/traces_jenius/exp3_hbf_latency_srad_v2/1_baseline_hbm_only/sim.log` 打印的参数为例：

- DRAM（HBM）：
  - `-gpgpu_dram_buswidth 16`（见 `hw_run/traces_jenius/exp3_hbf_latency_srad_v2/1_baseline_hbm_only/sim.log:176`）
  - `-gpgpu_dram_burst_length 2`（见 `hw_run/traces_jenius/exp3_hbf_latency_srad_v2/1_baseline_hbm_only/sim.log:177`）
  - 所以 `dram_atom_size = 16*2*1 = 32B`（每次 RD/WR column command 搬 32B）
  - `-dram_latency 100`（见 `hw_run/traces_jenius/exp3_hbf_latency_srad_v2/1_baseline_hbm_only/sim.log:181`）
  - 观测到 `offchip_hbm_averagemflatency ≈ 331` cycles（见 `hw_run/traces_jenius/exp3_hbf_latency_srad_v2/1_baseline_hbm_only/latency_summary.txt:1`）

- HBF（Phase 1）：
  - `-hbf_subpage_bytes 512`（见 `hw_run/traces_jenius/exp3_hbf_latency_srad_v2/1_baseline_hbm_only/sim.log:160`）
  - `-hbf_channel_bytes_per_cycle 512`（见 `hw_run/traces_jenius/exp3_hbf_latency_srad_v2/1_baseline_hbm_only/sim.log:167`）
  - 所以“搬运时间”是 `ceil(512/512)=1` cycle
  - `-hbf_t_read 140000`（见 `hw_run/traces_jenius/exp3_hbf_latency_srad_v2/1_baseline_hbm_only/sim.log:164`）
  - 观测到 `offchip_hbf_averagemflatency ≈ 140300` cycles（见 `hw_run/traces_jenius/exp3_hbf_latency_srad_v2/2_random_mix_hbf_hbm/latency_summary.txt:1`）

直观理解：
- DRAM：**低时延（百级 cycle）**，带宽由 `buswidth/burst/timing/scheduler` 综合决定；
- HBF：**高时延（十万级 cycle）**，但允许 **更深队列与更高有效链路带宽**（靠大并发隐藏时延）。

## 9. 把真实物理参数换算成 HBF config（并做“最大带宽”实验）

你给的目标（物理量）：
- `tREAD = 20us`
- `tPROG = 150us ~ 250us`
- `tERASE = 1ms ~ 2ms`
- 峰值带宽：`1.2 TB/s`

### 9.1 时间：us → 模拟器 cycle

HBF Phase 1 的 `-hbf_t_read/-hbf_t_prog/-hbf_t_erase` 单位是 **模拟器的全局 cycle**（也就是 `sim.log` 里 `gpu_tot_sim_cycle` 的时间基）。

换算公式（用 core domain 的频率）：
- `cycles = time_us * core_freq_MHz`

例如 QV100-SASS 配置中：
- `-gpgpu_clock_domains 1132.0:1132.0:1132.0:850.0`（core=1132MHz）
那么：
- `tREAD=20us => hbf_t_read ≈ 20*1132 = 22640 cycles`
- `tPROG=150us => hbf_t_prog ≈ 169800 cycles`
- `tPROG=250us => hbf_t_prog ≈ 283000 cycles`
- `tERASE=1ms => hbf_t_erase ≈ 1132000 cycles`
- `tERASE=2ms => hbf_t_erase ≈ 2264000 cycles`

> 如果你希望用 DRAM domain 频率作为时间基（850MHz），我们也可以改成按 dram_freq 换算；但当前 HBF 模块和 latency 统计都以全局 cycle（core tick）为主。

### 9.2 带宽：TB/s → `hbf_channel_bytes_per_cycle`

当前 HBF Phase 1 的带宽瓶颈由 `-hbf_channel_bytes_per_cycle` 表达，并且 **在每个 memory partition 内做串行化**（transfer 会排队）。

关键点：你需要先决定 `1.2 TB/s` 的口径。当前实现里 `-hbf_channel_bytes_per_cycle` 是 **每个 memory partition 的链路带宽（bytes/cycle）**，因此如果你的资料给的是“每个 stack 的总带宽”，需要先把它分摊到该 stack 内的 partitions。

**情况 A：`1.2 TB/s` 是 “每个 stack 的总带宽”（你当前的口径）**
- 设 `P = partitions_per_stack`（一个 stack 对应多少个 memory partitions）
- 则 `BW_per_partition = BW_stack / P`
- `hbf_channel_bytes_per_cycle ≈ BW_per_partition / core_freq_Hz`

用 QV100（core=1.132GHz, `-gpgpu_n_mem 32`）举例：如果你假想 **8 stacks**，并把 32 个 partitions 均匀映射到 8 个 stacks，那么 `P=32/8=4`：
- `BW_stack = 1.2e12 B/s`
- `BW_per_partition ≈ 3.0e11 B/s`
- `bytes/cycle ≈ 3.0e11 / 1.132e9 ≈ 265.0`
=> 可设置 `-hbf_channel_bytes_per_cycle 265`
  - 注意：由于 `hbf_subpage_bytes=512B` 且链路是整数 cycle 计时，稳态吞吐会被量化到 `512 / ceil(512/bw)` B/cycle；`bw=265` 时 `ceil(512/265)=2`，实际约 `256 B/cycle`（约 `1.16 TB/s/stack`）。

**情况 B：`1.2 TB/s` 是 “全系统聚合带宽”**
- 设 `N = gpgpu_n_mem`（全系统 partitions 数）
- `BW_per_partition = BW_total / N`
- `hbf_channel_bytes_per_cycle ≈ BW_per_partition / core_freq_Hz`

用 QV100（core=1.132GHz, N=32）举例：
- `BW_total = 1.2e12 B/s`
- `BW_per_partition ≈ 3.75e10 B/s`
- `bytes/cycle ≈ 3.75e10 / 1.132e9 ≈ 33.1`
=> 可设置 `-hbf_channel_bytes_per_cycle 33`（同样会有 512B 量化误差）

### 9.3 “最大带宽实验”建议的开关组合

为了尽量看到“带宽上限”，建议：
- 全 HBF：`-hbf_partition_start 0` + `-hbf_partition_count <gpgpu_n_mem>`
- 降低读延迟：`-hbf_t_read 22640`（20us@1132MHz）
- 设置带宽：按 9.2 换算得到的 `-hbf_channel_bytes_per_cycle`
- 提高并发：`-hbf_max_outstanding` 至少满足 Little’s Law
  - 粗估：`outstanding_total ≈ BW_total * tREAD / subpage_bytes`
  - `1.2TB/s * 20us / 512B ≈ 46875`（这是“全系统总 outstanding”量级）
  - 如果 32 个 partition 平均分摊：每 partition 约 `~1465` outstanding（默认 `8192` 足够）
- 避免 false deadlock：带宽饱和时 stall 可能很长，建议在 overlay 里显式加大
  - `-gpgpu_deadlock_detect_interval 10000000`（或更大）

下一步如果你愿意，我可以给你写一个“极小 trace 尺寸”的 streaming microbenchmark（读为主/写为主各一版），专门用来把 HBF 的链路推到你设定的峰值带宽；同时 HBF 的 `print_stat()` 已经会打印每个 partition 的 `link_bytes/link_busy_cycles/link_util`，方便你看是不是把带宽顶满。
