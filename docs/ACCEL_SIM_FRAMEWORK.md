## 1. accel流程

1) **Trace 采集**：用 NVBit 动态插桩把 kernel 的动态执行信息抓出来。
2) **Trace 驱动**：`accel-sim.out` 读取 trace，做 cycle-level 仿真，输出统计/latency/power。

---

## 2. 关键部分

### 2.1 `gpu-simulator/`：模拟器本体（GPGPU-Sim + Accel-Sim glue）

- `gpu-simulator/bin/release/accel-sim.out`：最终仿真的可执行文件（trace-driven）。
- `gpu-simulator/setup_environment.sh`：设置 `LD_LIBRARY_PATH` 等环境。
- `gpu-simulator/gpgpu-sim/src/gpgpu-sim/`：GPGPU-Sim 源码（核心微架构模拟、cache、memory partition、dram 等）。
- `gpu-simulator/gpgpu-sim/configs/tested-cfgs/*/gpgpusim.config`：GPU 配置（微架构参数、DRAM timing、L2/NoC 等）。
- `gpu-simulator/configs/tested-cfgs/*/trace.config`：trace-driven 运行时的配置（与 gpgpusim.config 一起传入）。

### 2.2 `util/tracer_nvbit/`：NVBit tracer（采集真实 GPU 动态执行）

- `util/tracer_nvbit/tracer_tool/tracer_tool.so`：NVBit 注入的 tracer（通过 `CUDA_INJECTION64_PATH` 生效）。
- `util/tracer_nvbit/tracer_tool/traces-processing/post-traces-processing`：把原始 trace 处理成 `kernelslist.g` + 压缩 trace 文件（模拟器输入）。

### 2.3 `workloads/`：端到端示例脚本

- 脚本包含：编译模拟器 → 编译 tracer → 编译 CUDA 程序 → NVBit 采集 trace → trace 后处理 → `accel-sim.out` 仿真。
- `workloads/minimal_hbf_driver/run_end2end.sh`

### 2.4 `hw_run/` 与 `sim_run_*`

- `hw_run/`：存放 **GPU 采集出来的 trace**。
- `sim_run_12.8/.../QV100-SASS/`：一次“仿真运行目录”（run dir）的快照，包括：
  - `gpgpusim.config`：当次仿真使用的配置
  - `traces/`：软链接指向 `hw_run/.../traces`
  - `justrun.sh`：运行 `accel-sim.out -trace ... -config ...`

---

## 3. 端到端运行逻辑链（从 CUDA 到仿真结果）

### 3.1 生成 trace（GPU 上跑一遍）

1) 编译 CUDA 程序（得到可在真实 GPU 上运行的二进制）
2) 设置 tracer 环境变量并运行：
   - `CUDA_INJECTION64_PATH=.../tracer_tool.so`：让 NVBit 把 tracer 注入进程
   - `TRACES_FOLDER=...`：输出目录
3) 运行结束后，会在 `TRACES_FOLDER/traces/` 里得到中间 trace
4) 运行 `post-traces-processing`：
   - 得到 `TRACES_FOLDER/traces/kernelslist.g`：作为仿真入口文件，列出每个 kernel 的 trace 文件与元数据

### 3.2 用 trace 驱动 `accel-sim.out` 做 cycle-level 仿真

```bash
source gpu-simulator/setup_environment.sh
gpu-simulator/bin/release/accel-sim.out \
  -trace  <TRACES_FOLDER>/traces/kernelslist.g \
  -config <...>/gpgpusim.config \
  -config <...>/trace.config \
  | tee sim.log
```

输出：
- `sim.log`：统计输出（包含每个 kernel 的统计、latency、模拟总周期等）
- （可选）`accelwattch_power_report.log`：开启 power 模式时的功耗报告
- （可选）`hbf_requests.trace`：打开 `-hbf_request_trace` 后产生的周期级请求的CSV
---

## 4. HBF模块添加

### 4.1 核心：`mem_fetch`

在 GPGPU-Sim 里，一次 cache miss / 一次 L2→MC 的请求会被抽象成 `mem_fetch`：
- 由 core 侧的 load/store 触发（经 L1/L2）
- 被送入 interconnect → memory partition → off-chip backend（HBM 或 HBF）
- backend 完成后把 `mem_fetch` 送回 L2，最终唤醒等待的 warp

HBF/HBM 的区别发生在 **L2 产生 off-chip 请求之后**。

### 4.2 分流点：`memory partition`

关键类在 `gpu-simulator/gpgpu-sim/src/gpgpu-sim/l2cache.h`：
- `memory_sub_partition`：每个 memory partition 下再细分的子分区，包含 L2 slice 与队列
- `memory_partition_unit`：一个 memory partition（对应一个内存通道/partition），负责仲裁并把请求送到 off-chip

`memory_partition_unit` 同时持有两个后端：
- HBM/DRAM：`dram`（原生）→ `gpu-simulator/gpgpu-sim/src/gpgpu-sim/dram.{h,cc}`
- HBF：`hbf`（占位）→ `gpu-simulator/gpgpu-sim/src/gpgpu-sim/hbf.{h,cc}`

所以**HBF 被实现为 memory partition 的“另一个 off-chip 控制器”**，它和 `dram_t` 同级，接口形态尽量对齐（`push/full/cycle/returnq`），但内部逻辑完全独立

### 4.3 选路：`memory_config::is_hbf_request()`

分流决策由 `memory_config` 提供（`gpu-simulator/gpgpu-sim/src/gpgpu-sim/gpu-sim.h`）：

- `bool memory_config::is_hbf_request(request_uid, addr, partition_id) const`
  - **随机打标**（debug）：`-hbf_random_access=1`
  - **地址范围映射**：`-hbf_addr_range_start/-hbf_addr_range_size`
  - **partition 静态划分**：`-hbf_partition_start/-hbf_partition_count`

在 `memory_partition_unit::{dram_cycle|simple_dram_model_cycle}` 里，每次从 `L2_dram_queue` 取出一个待发请求时，会：
1) 调用 `is_hbf_request(...)` 判断走 HBF 还是 HBM
2) 给 `mem_fetch` 打上 backend tag（用于统计/latency）
3) 进入对应的 “L2→MC 固定延迟队列”（HBM 与 HBF 各一条）
4) 固定延迟到点后 `push()` 到对应 controller（`m_dram` 或 `m_hbf`）

### 4.4 HBF controller

HBF controller 的实现在：
- `gpu-simulator/gpgpu-sim/src/gpgpu-sim/hbf.h`
- `gpu-simulator/gpgpu-sim/src/gpgpu-sim/hbf.cc`

- Read：`tREAD + 传输时间(由 hbf_subpage_bytes 与 hbf_channel_bytes_per_cycle 决定`
- Write：posted（写入缓冲即 ACK），后台按 `tPROG` 回收缓冲

功能：
- 队列/容量约束（outstanding、write buffer）
- 完成事件调度（ready_cycle 到了就把 `mem_fetch` 放入 returnq）
- 把返回的 `mem_fetch` 交回给 memory partition（回到 `dram_L2_queue`）

没完成：
- FTL/GC/磨损/plane 级资源冲突等

### 4.5 结果

1) **latency（HBM vs HBF 拆分）**
   - `sim.log` 中的 `offchip_hbm_*` / `offchip_hbf_*`
   - 统计代码：`gpu-simulator/gpgpu-sim/src/gpgpu-sim/mem_latency_stat.{h,cc}`

2) **请求级 trace（每条请求标 HBF/HBM）**
   - 打开：`-hbf_request_trace 1`
   - 输出：`-hbf_request_trace_file <path>`（默认 `hbf_requests.trace`）
   - 生成位置：`memory_config::hbf_request_trace_log(...)`（`gpu-sim.h/.cc`）

3) **功耗（AccelWattch）**
   - memory-controller 计数分成两类：HBM 用 `MEM_RD/MEM_WR`，HBF 用 `HBF_RD/HBF_WR`

---

