高带宽闪存（HBF）架构深度调研与技术可行性分析报告
（实现落地：见 `docs/HBF_DESIGN.md`，包含 Accel-Sim 侧的模块设计与 TODO 列表。）
执行摘要
随着万亿参数级大语言模型（LLM）与混合专家模型（MoE）的爆发式增长，AI 基础设施正面临前所未有的“内存墙”危机。尽管 GPU 算力呈现指数级增长，但传统的高带宽内存（HBM）受限于昂贵的制程成本与容量瓶颈，难以满足模型权重与 KV Cache 对海量存储的需求。作为一种颠覆性的存储架构，高带宽闪存（High Bandwidth Flash, HBF）通过 3D 堆叠 NAND Flash 与高性能逻辑基底（Logic Die），旨在提供接近 HBM 的带宽（目标 1.6 TB/s）与 SSD 级别的容量（单栈 512GB+）。
本报告基于 SanDisk、SK Hynix、Nvidia 及学术界（如 KAIST、BaM/SCADA 研究）的最新技术披露，对 HBF 的接口语义、物理组织参数、时序特性及闪存管理机制进行了详尽的调研与分析。分析表明，HBF 并非简单的“高速 SSD”，而是一种基于**GPU 发起（GPU-Initiated）**访问模型的新型内存层级。其通过逻辑基底实现细粒度（512 Byte）并行访问，配合 HBM 作为写回缓存（Write-Back Cache），有效解决了 NAND 原生的高延迟与块擦除限制。HBF 的成功将依赖于软硬件的深度协同——即底层 BiCS NAND 的多平面并行化与上层 SCADA/BaM 软件栈的指令级调度。
1. 引言：AI 时代的内存墙与 HBF 的诞生背景
1.1 计算与存储的剪刀差
在过去的二十年中，处理器的峰值算力增长了约 60,000 倍，而 DRAM 的带宽仅增长了约 100 倍，互连带宽增长了约 30 倍。这种极端的不平衡导致了“内存墙”问题的加剧。对于现代 AI 负载，尤其是自回归生成的 Decoding 阶段，计算往往处于饥饿状态，等待数据从内存搬运至寄存器。
1.2 HBM 的经济学困境
尽管 HBM 通过 TSV（硅通孔）技术显著提升了带宽，但其成本结构并未随摩尔定律线性下降。由于封装良率与 DRAM Cell 微缩的物理极限，HBM 的单位容量成本（$/GB）不降反升。相比之下，NAND Flash 凭借 3D 堆叠层数（如 218 层 BiCS8乃至 300+ 层）的快速迭代，保持了极具竞争力的位密度成本。
1.3 HBF 的技术定位
HBF 旨在填补 DRAM 与传统 SSD 之间的巨大的性能与容量鸿沟。它利用 HBM 类似的封装工艺（TSV 堆叠），将 NAND Flash 直接通过宽接口连接至 GPU，旨在提供 8-16 倍于 HBM 的容量，同时维持 TB/s 级 的读取带宽。这使得将整个万亿参数模型驻留在 GPU 本地显存体系成为可能，从而避免了跨节点的低速互连通信。
2. 接口语义与 GPU 直连架构
HBF 的核心挑战在于如何让 GPU 像访问内存一样访问本质上是块设备的 NAND Flash。这需要从物理层到指令集层面的全面重构。
2.1 寻址机制：物理 Page 与逻辑 CacheLine 的辩证统一
用户疑问：GPU 是否“字节/CacheLine 可寻址”直连 HBF？
调研结论： HBF 在物理层仍然遵循 NAND 的 Page（页）读写与 Block（块）擦除特性，但在逻辑层与接口语义层实现了“伪字节/CacheLine 可寻址”。
物理层的不可变性：
HBF 的存储介质是 3D NAND（如 BiCS8 TLC/QLC）。NAND 的物理读取单元是 Page，典型大小为 16KB 1。
物理上不支持真正的字节级随机读取。若直接读取一个字节，NAND 阵列必须感测（Sense）整页数据到页寄存器（Page Buffer）。
逻辑层的细粒度抽象（Logic Die 的作用）：
HBF 堆叠底部的 Logic Die（逻辑基底）扮演了极其关键的角色。它不仅仅是物理接口转换器，更是一个高性能的存内计算单元。
当 GPU 发起一个 64 Byte 的 CacheLine 读取请求时，Logic Die 会执行物理 Page 读取（如 16KB），但仅将请求的 64 Byte 通过 TSV 链路回传给 GPU。这种机制被称为 "Sub-Page Access" 或 "Fine-Grained Access"。
在 SCADA (Scaled Accelerated Data Access) 架构中，优化的访问粒度被定义为 512 Bytes。这是因为对于 AI 向量检索（Vector Search）和稀疏 MoE 访问，512B 往往能平衡总线开销与数据有效性 2。
软件视角的字节寻址：
通过 BaM (Big Accelerator Memory) 系统，GPU 线程可以直接通过指针解引用（Load 指令）访问 HBF 空间。
底层通过 GPU 内部的软件缓存（Software Cache）或硬件辅助的 TLB 机制，将 Load 指令转化为对 HBF 控制器的请求。对程序员而言，HBF 表现为一个巨大的、高延迟的字节寻址内存池。
2.2 Store 与 Atomic 操作的支持性分析
用户疑问：是否允许 store/atomic？
调研结论： 允许，但受限于 NAND 物理特性，必须通过**缓冲（Buffering）**机制实现。
Store（写入）操作：
直接覆盖的不可行性：NAND Flash 不支持原地覆盖（Overwrite），必须先擦除（Erase）后写入（Program）。直接对 HBF 执行 Store 指令会导致极高的写放大（Write Amplification）和延迟（tPROG > 600us）。
Log-Structured Write（日志结构写入）：在 BaM 和 SCADA 架构中，GPU 的 Store 操作实际上是写入到 HBM 中的写缓冲（Write Buffer） 或 Logic Die 内部的 SRAM 缓冲。
写回策略：数据在缓冲区聚合成完整的 Page 后，由 Logic Die 异步写入 NAND。
Atomic（原子）操作：
挑战：原子操作（如 atomicAdd）需要“读-改-写”的一致性保证。在 NAND 上直接执行原子操作会导致流水线严重停顿。
实现路径：原子操作不在 NAND 介质上执行，而是在 HBM 缓存层 或 Logic Die 的 SRAM 控制器 中完成。
一致性模型：Logic Die 维护一个原子操作单元，锁定相关的逻辑地址（LBA），在 SRAM 中完成计算，随后标记该页为脏页（Dirty Page）。这种机制依赖于 Logic Die 强大的算力与片上缓存 4。
2.3 写返回语义：写入缓冲即 ACK
用户疑问：写返回语义是“写入缓冲即 ACK”还是“program 完成才 ACK”？
调研结论： 几乎确定采用 “写入缓冲即 ACK”（Write-Back / Posted Write）。
性能约束：NAND 的物理编程时间（tPROG）对于 TLC 约为 600us - 1ms，对于 pSLC 约为 150us。如果 GPU 线程需要等待物理编程完成才收到 ACK，整个流水线将彻底停滞，吞吐量将跌至不可接受的水平（仅几千 IOPS）。
架构设计：
HBM 作为 Write Buffer：在 SanDisk 和 KAIST 的设计中，HBM 被明确用作 HBF 的前端缓存 5。GPU 写入数据到 HBM 瞬间即可获得 ACK，延迟为纳秒级。
Logic Die SRAM：Logic Die 内部集成了数百 MB 的 SRAM。对于绕过 HBM 的直接写入，数据进入 Logic Die SRAM 后即返回 ACK。
可靠性保障（Power Loss Protection）：为了防止掉电数据丢失，企业级 HBF 模组会在 Logic Die 或中介层配备电容，确保在断电瞬间能将 SRAM 中的脏数据刷入 NAND（或利用 pSLC 的快速写入特性进行紧急转储）。
3. HBF/HBM 划分与映射规则
HBF 不是 HBM 的替代者，而是扩展者。两者构成了一个异构的内存层级。
3.1 划分规则：基于频率与数据类型的混合策略
用户疑问：按地址范围？按 page/region？还是 runtime hot/cold？
调研结论： 采用 Runtime Hot/Cold（运行时冷热分级） 为主，地址范围（Address Range） 为辅的混合策略。
静态划分（按数据类型）：
HBF 区域（Cold/Warm）：存储模型权重（Model Weights）、优化器状态（Optimizer States）的历史快照。这些数据主要特点是只读（Read-Only）或低频写入，且占据了 90% 以上的存储空间。
HBM 区域（Hot）：存储 KV Cache（当前上下文）、激活值（Activations）、梯度（Gradients）。这些数据对带宽和延迟极其敏感，且读写频繁。
动态划分（按访问频率）：
KV Cache Offloading：随着上下文长度增加（如 1M+ tokens），KV Cache 会溢出 HBM。此时，系统根据 LRU（最近最少使用） 算法，将较旧的 Token 对应的 KV Block 驱逐（Evict）到 HBF，保留最近的 Token 在 HBM 中。
Joungho Kim 的缓存模型：KAIST 提出的架构明确将 HBM 视为 HBF 的 L4 Cache 5。这意味着 GPU 看到的物理地址空间主要是 HBF，而 HBM 作为一个对软件透明或半透明的缓存层存在。
3.2 映射粒度
用户疑问：映射粒度（cacheline/page/region）？
调研结论： 多粒度并存（Multi-Granularity Mapping）。

映射粒度
应用场景
优势
Region (2MB/1GB)
模型加载、权重预取
减少页表（TLB）开销，最大化顺序读取带宽。Logic Die 可开启激进的预取模式。
Page (4KB/16KB)
传统虚拟内存管理
兼容 OS 现有的分页机制，便于内存超卖（Oversubscription）。
Sub-Page (512B)
SCADA/BaM 核心特性
针对 MoE 的稀疏访问和向量数据库（Vector DB）查询。Logic Die 内部维护 512B 粒度的 L2P（逻辑物理地址映射）表，只传输有效数据，节省带宽 2。

4. Flash 组织参数与并行性约束
HBF 的高性能来源于对 NAND 内部并行性的极致挖掘。
4.1 物理组织架构
用户疑问：channels、dies、planes、(sub-)pagesize、blocksize？
基于 SanDisk BiCS8 及 HBF 披露资料的参数重构：
Stack Height (层叠数)：典型值为 16 Dies 6。
Die Tech：BiCS8 (218-layer 3D NAND) TLC/QLC 1。
Capacity：单 Die 容量 1Tb (128GB) 或 256Gb (32GB, pSLC 模式)。单 Stack 容量 512 GB - 1 TB。系统级（8 Stacks）可达 4 TB 6。
Planes (平面)：BiCS8 单 Die 拥有 4 个逻辑 Plane (8 个物理 Sub-Planes) 1。
Block Size (块大小)：每个 Plane 约 3,270 Blocks。单 Block 包含 2,748 Pages。物理 Block 大约 45-50 MB 1。
Page Size (页大小)：物理页大小为 16 KB 1。
4.2 并行与互斥约束
用户疑问：同 die 是否串行？multi-plane 是否支持？
Logic Die 对 Channel 的重定义：
传统 SSD 控制器受限于 PCB 走线，通道数有限（如 8-16 通道）。
HBF 通过 TSV 连接，通道数不再受限。Logic Die 可以为 每一个 Die 甚至 每一个 Plane 提供独立的读写通道。
结论：同 Die 的不同 Plane 完全支持 Multi-Plane Independent Operation（多平面独立操作）。Logic Die 可以同时向 Stack 中的 16 个 Die 发送指令，且每个 Die 的 4 个 Plane 也可以并行工作。理论最大并行度 = 16 Dies × 4 Planes = 64 并行流。
Sub-Array Parallelism（子阵列并行）：
SanDisk 披露 HBF 将 NAND 阵列进一步细分为“Sub-Arrays” 7。这暗示了 Logic Die 可能利用 CBA 技术，独立控制 Plane 内部的更小单元，或者通过交错（Interleaving）技术在 sub-plane 级别实现流水线操作，从而打破传统“同 Plane 串行”的限制。
5. 时序、带宽与队列深度分析
5.1 关键时序参数 (tREAD, tPROG, tERASE)
以下参数基于 BiCS8 及现代 3D NAND 的物理特性推演 1：
参数
典型值 (TLC)
优化值 (pSLC/HBF Mode)
分布/尾部延迟
说明
tREAD (物理读取)
$40 \mu s$
$20 \mu s$
尾部 (P99) 可达 $100 \mu s+$
受 ECC 解码复杂度影响，Logic Die 需硬解码加速。
tPROG (编程/写入)
$600 \mu s - 1000 \mu s$
$150 \mu s - 250 \mu s$
分布较宽
写入操作通常被 Buffer 隐藏，不直接暴露给 GPU。
tERASE (擦除)
$3 ms - 5 ms$
$1 ms - 2 ms$
-
完全在后台由 GC 线程执行。
接口传输延迟
$ns$ 级
$ns$ 级
-
Logic Die 到 GPU 的 TSV 链路延迟极低。

5.2 接口带宽与频率
用户疑问：接口带宽/频率？
单 Stack 带宽：目标 1.6 TB/s（对齐 HBM4 规格）8。
注：这是一个极具野心的目标。目前的 PCIe 原型机仅展示了 64 GB/s 10。要达到 1.6 TB/s，必须依赖 TSV 的超宽位宽。
接口频率：
NAND 侧（内部）：Toggle DDR 6.0 标准，速率可达 4.8 Gbps/pin 11。
GPU 侧（外部）：HBF 物理接口（PHY）设计兼容 HBM 接口，运行频率可能在 6.4 Gbps - 10 Gbps (PAM4) 之间 12。
5.3 队列深度与最大 Outstanding
用户疑问：每级队列深度、最大 outstanding？
为了用 $40 \mu s$ 的慢速介质填满 $1.6 TB/s$ 的带宽管道，根据 Little's Law ($Concurrency = Bandwidth \times Latency$)，必须维持惊人的并发量。
计算：
带宽 = $1.6 TB/s$
延迟 = $40 \mu s$
Data in Flight = $1.6 TB/s \times 40 \mu s = 64 MB$
若按 4KB Page 读取，需 Outstanding Requests = $64 MB / 4 KB = 16,384$。
若按 512B SCADA 读取，需 Outstanding Requests = 131,072。
SCADA/BaM 的队列设计：
传统 NVMe 队列深度通常为 64K，但在 CPU 侧往往成为瓶颈。
HBF 的 Logic Die 设计目标是支持 100 Million IOPS 2。这意味着其内部状态机和 SRAM 必须能追踪 10 万到 20 万 个并发指令。
每级队列：GPU 每个 SM（Streaming Multiprocessor）可能维护独立的 Submission Queue，整个 GPU 可能有数千个队列，总深度达到百万级。
6. 写放大、FTL 与 GC 假设
HBF 的管理逻辑从“设备黑盒”转向“主机（GPU）协同”。
6.1 FTL 映射方式：主机协同的混合映射
用户疑问：映射方式（page-mapping/block-mapping/hybrid）？
调研结论： 采用 Page-Mapping（页级映射），但在 Logic Die 中实现硬件加速，并可能向 GPU 暴露部分物理几何信息（Host-Managed FTL）。
L2P 表的位置：
由于 HBF 容量巨大（512GB+），4KB 粒度的 L2P 表大小约为 512MB。这张表必须常驻在 Logic Die 的 SRAM 或 HBM 的保留区域 中，以保证地址翻译不产生额外的 NAND 读取延迟。
对于 SCADA 512B 访问，可能采用多级映射表或哈希映射结构。
Host-Managed FTL (BaM)：
为了减少写放大和 GC 干扰，BaM 架构允许 GPU 软件栈感知 Block 的状态。GPU 在写入时，会尽量凑满一个 Block（Log-Structured Write），从而减少设备端 FTL 执行昂贵的数据搬运（Compaction）操作 14。
6.2 垃圾回收 (GC) 模型
用户疑问：GC 触发阈值/代价模型？
触发阈值：
AI 推理主要是读操作，写入（如 KV Cache 溢出）是顺序追加的。
GC 策略为 "Lazy GC"：仅在空闲时间或容量接近耗尽（如 90%）时触发。
Runtime 协同：GPU 驱动会通知 HBF 当前处于“推理核心窗口”，此时 Logic Die 会挂起所有后台 GC 操作，确保读取延迟的确定性（Deterministic Latency）。
代价模型：
HBF 可能采用 Multi-Stream Write 技术，将不同生命周期的数据（如权重的更新 vs 临时 KV Cache）写入不同的 Block，从而使得 KV Cache 的 Block 失效时可以直接整块擦除，无需数据搬运，将写放大系数（WAF）降至接近 1.0 15。
6.3 DRAM 缓冲与 Over-Provisioning
用户疑问：Over-provisioning、是否有 DRAM 缓冲？
DRAM 缓冲：
有。HBM 被明确用作 Write-Back Cache。
Logic Die SRAM：除了 L2P 表，Logic Die 内部有数百 MB 的 SRAM 用作数据 Buffer，用于汇聚小写入请求和预取读取数据 17。
Over-Provisioning (OP)：
HBF 独特配置：对于存储模型权重的“只读分区”，OP 可以设置得很低（如 0-7%），以最大化容量。对于存储 KV Cache 的“读写分区”，OP 可能设置较高（28%），以维持稳态写入性能和寿命 1。
7. 结论与展望
HBF 代表了 AI 存储架构的一次根本性重构。它不仅是介质的堆叠，更是计算与存储边界的消融。
接口变革：从 CPU 主导的文件 IO 转向 GPU 主导的内存语义访问（Load/Store），通过 SCADA/BaM 软件栈屏蔽了 NAND 的物理复杂性。
性能逻辑：利用 Logic Die 实现大规模子阵列并行，用极其恐怖的并发度（100K+ Outstanding）来“掩盖” NAND 的高延迟，从而换取 TB/s 级的带宽。
生态融合：HBF 不会独立存在，它将与 HBM 紧密耦合，形成 HBM (Hot) + HBF (Warm) + SSD (Cold) 的三级存储金字塔。
对于 GPU 架构师和 AI 基础设施开发者而言，这意味着未来的优化方向将从单纯的“算子优化”转向“数据流与显存层级调度优化”，充分利用 HBF 带来的海量近存空间。
详细技术报告正文
1. 内存墙危机与 HBF 的架构定位
1.1 大模型时代的存储悖论
当前 AI 发展的核心矛盾在于：模型参数量每 18 个月增长 40 倍，而 GPU 显存容量每 18 个月仅增长 2 倍。这种非对称增长导致了严重的“碎片化”问题——为了推理一个万亿参数模型，不得不动用数千张 GPU 进行模型并行（Model Parallelism），导致巨大的通信开销和极低的硬件利用率（MFU 往往低于 40%）。
HBM 虽然带宽极高，但其工艺依赖于 DRAM 晶圆与复杂的 TSV 堆叠，良率低且成本高昂。目前 HBM3E 24GB 的成本占据了 GPU 模组成本的极大比例。
1.2 HBF：用 NAND 重构显存
SanDisk 和 SK Hynix 提出的 HBF 方案，本质上是试图用 NAND Flash 的容量成本优势，去逼近 DRAM 的带宽性能。其核心假设是：AI 推理对延迟的容忍度高于带宽。 只要预取（Prefetching）做得足够好，且带宽足够大，NAND $40 \mu s$ 的延迟可以通过流水线掩盖。
1.3 Logic Die 的革命性意义
HBF 与传统 SSD 的根本区别在于堆叠底部的 Logic Die。
传统 SSD：控制器（Controller）是独立的芯片，通过 PCB 与 NAND 颗粒相连，受限于引脚数量和信号完整性，通道数很少（8-16）。
HBF：Logic Die 与 NAND 晶圆直接键合（CBA 技术）。Logic Die 也就是 HBF 的“控制器”，但它拥有与 NAND 阵列几乎 1:1 的连接密度。这意味着它可以并行驱动成百上千个 NAND 子阵列，这是传统 SSD 物理上无法实现的。
2. 深度解析：HBF 的物理与逻辑架构
2.1 3D NAND 堆叠参数详解
以 SanDisk/Kioxia 的 BiCS8 技术为基准，我们可以通过推算得出 HBF 的详细物理参数：
参数项
规格数值
技术解读
层数 (Layers)
218 层 (BiCS8)
随着技术演进，HBF Gen2/Gen3 将采用 300+ 层 NAND。
接口速率
3200 MT/s - 4800 MT/s
内部 NAND 接口速率。Logic Die 负责将其汇聚并转换。
平面数 (Planes)
4 (逻辑) / 8 (物理)
4 个 Plane 可独立执行读写指令，显著提升单 Die 吞吐。
Die 容量
1 Tb (TLC) / 256 Gb (pSLC)
HBF 为了性能可能牺牲容量，运行在 pSLC 模式。
堆叠高度
16-Hi (16层 Die)
通过 TSV 互连。总容量可达 512GB (pSLC) 或 2TB (TLC)。
TSV 密度
> 1024 IOs
模仿 HBM 的宽接口设计，而非 SSD 的窄接口（PCIe x4）。

2.2 Sub-Array Parallelism（子阵列并行机制）
这是 HBF 区别于 multi-plane SSD 的核心。在 Logic Die 的调度下，一个 NAND Die 不再被视为一个单一的存储单元，而是被视为一组（例如 16 个）独立的存储库（Bank）。
机制：Logic Die 通过独立的地址线和数据线连接到不同的子阵列。
效果：如果一个 Die 内有 16 个子阵列，每个子阵列提供 400 MB/s 的读取速度，那么单 Die 的内部聚合带宽可达 6.4 GB/s。16 个 Die 堆叠即可达到 ~100 GB/s。为了达到 1 TB/s 的目标，Logic Die 必须进一步利用 CBA 技术在更细粒度上进行交错读取。
3. 接口语义：GPU-Initiated Storage (SCADA/BaM)
HBF 的硬件能力需要全新的软件栈来释放。传统的 read() / write() 系统调用路径（User -> Kernel -> Driver -> PCIe -> NVMe）延迟高达 $10-20 \mu s$，这与 HBF 的介质延迟相当，等于将性能腰斩。因此，必须采用 Kernel-Bypass 和 GPU-Initiated 模式。
3.1 SCADA (Scaled Accelerated Data Access) 架构
Nvidia 提出的 SCADA 是专为“Storage-Next”设备（即 HBF 和高性能 SSD）设计的。
控制路径下沉：SCADA 将存储控制逻辑（NVMe Driver 的核心功能）下沉到 GPU 的 CUDA Kernel 中。GPU 线程直接构建 NVMe Command。
Doorbell 机制：GPU 写 Doorbell 寄存器通知 HBF Logic Die。由于 HBF 直连 GPU（可能通过 CXL 或私有 NVLink 类协议），这一步极快。
512B 极速访问：SCADA 特别针对 512 Byte 随机读进行了优化。这是为了适应图神经网络（GNN）和向量检索中“跳跃式”访问大数据的需求。Logic Die 收到 512B 请求后，虽然内部读取 16KB Page，但只通过总线回传 512B，极大节省了宝贵的显存带宽。
3.2 BaM (Big Accelerator Memory) 的软件抽象
BaM 系统在 GPU 显存中维护了一个 Software Cache。
CacheLine 可寻址的实现：
用户代码执行 data[i]（加载一个 CacheLine）。
BaM 库检查 Software Cache 标签（Tag）。
Hit：直接返回数据。
Miss：BaM 库分配一个 Slot，构建一个读取请求放入 Submission Queue。
GPU 线程挂起（或切换到其他 Warp）。
HBF 完成读取，数据写入 HBM 中的 Cache Slot，更新 Completion Queue。
BaM 库唤醒线程，重试访问。
这使得不支持字节寻址的 HBF 在程序员眼中表现为字节寻址。
4. 性能与时序的深度博弈
4.1 延迟掩盖数学模型
HBF 的设计哲学是用吞吐量（Throughput）换延迟（Latency）。
场景假设：GPU 需要读取 1GB 的模型权重。
HBM3E：带宽 1.2 TB/s，耗时 $0.83 ms$。
HBF (未优化)：如果是单通道串行读取，延迟 $40 \mu s$ 一次 16KB，吞吐量仅 400 MB/s，耗时 2.5 秒。不可接受。
HBF (全并行)：
Logic Die 同时向 2000 个子阵列发送读取指令。
$t=0 \to 40 \mu s$：所有阵列同时处于 Sensing 状态（Busy）。
$t=40 \mu s$：2000 个 16KB 数据块（共 32MB）同时准备就绪。
$t=40 \mu s \to \text{传输结束}$：Logic Die 以 1.6 TB/s 的速度将这 32MB 数据“喷射”给 GPU。
流水线：在传输这 32MB 的同时，Logic Die 已经发出了下一批 2000 个读取指令。
结果：只要请求足够连续或并发度足够高，HBF 可以维持 1.6 TB/s 的饱和带宽。
4.2 队列深度的挑战
为了维持上述流水线，必须有足够多的“在途请求”（In-flight Requests）。
最大 Outstanding：SCADA 演示中展示了 100 Million IOPS 的能力。这要求 GPU 能够维护极深的队列。
资源开销：在 GPU 侧维护如此深的队列会消耗大量寄存器和 SRAM。因此，HBF 的 Logic Die 必须承担一部分队列管理职责，即 GPU 只需要发送轻量级的 Command，复杂的队列调度和乱序执行（Out-of-Order Execution）由 Logic Die 完成。
5. 闪存管理与可靠性设计
5.1 Host-Managed FTL (主机管理 FTL)
传统的 SSD FTL 是黑盒，导致了写放大和性能抖动。HBF 倾向于开放部分物理细节给 GPU。
Zoned Namespaces (ZNS)：HBF 可能支持 ZNS 接口，将 NAND 块抽象为 Zone。GPU 保证写入是顺序的（Sequential Write Required），这消除了设备端的垃圾回收（GC）需求，因为 GPU 可以直接重置整个 Zone。
应用感知的数据放置：AI 框架（如 PyTorch）知道哪些 Tensor 是属于同一个 Layer 的，它会指示 HBF 将这些数据物理上存放在同一个 Die 或 Block 中，以便未来并行读取。
5.2 读干扰（Read Disturb）的防护
AI 推理是极端的读密集型负载（WORM - Write Once Read Many）。频繁读取同一个 Block 的某些 Page 会导致邻近 Page 的电荷泄漏（Read Disturb）。
Logic Die 的应对：
Read Counter：Logic Die 记录每个 Block 的读取次数。
动态刷新（Read Reclaim）：当读取次数达到阈值（如 100K 次），Logic Die 会在后台自动将该 Block 的数据复制到新的 Block，并擦除旧 Block。这一过程对 GPU 透明，利用 HBF 内部的空闲带宽完成。
6. 结论
HBF 是一项将存储介质“内存化”的系统工程。它通过 3D 堆叠互连（TSV） 解决了带宽问题，通过 Logic Die 解决了并行控制问题，通过 SCADA/BaM 软件栈 解决了接口语义问题。
对于用户提出的核心问题：HBF 是一个具有 HBM 接口物理形态、内部高度并行、支持 GPU 直连指令寻址、但物理上仍受限于 NAND 时序特性的异构内存设备。 它的出现将彻底改变 AI 大模型的部署方式，使得单机运行 PB 级参数模型成为可能。
Works cited
Western Digital SN8100 2 TB Specs | TechPowerUp SSD Database, accessed January 13, 2026, https://www.techpowerup.com/ssd-specs/western-digital-sn8100-2-tb.d2387
Nvidia SCADA offloads storage control path to the GPU - Blocks and Files, accessed January 13, 2026, https://blocksandfiles.com/2025/11/25/scada-nvidia/
SC25 performance breakthrough: 230M IOPS in a single server | Micron Technology Inc., accessed January 13, 2026, https://www.micron.com/about/blog/storage/ssd/sc25-performance-breakthrough-230m-iops-in-a-single-server
GPU-Initiated On-Demand High-Throughput Storage Access in the BaM System Architecture - arXiv, accessed January 13, 2026, https://arxiv.org/pdf/2203.04910
Memory's Growing Role in AI: A Conversation with HBM Pioneer Prof. Joungho Kim, accessed January 13, 2026, https://www.sandisk.com/company/newsroom/blogs/2025/memory-growing-role-ai-conversation-hbm-pioneer-prof-joungho-kim
SanDisk Develops HBM Killer: High-Bandwidth Flash (HBF) Allows 4 TB of VRAM for AI GPUs | TechPowerUp, accessed January 13, 2026, https://www.techpowerup.com/332516/sandisk-develops-hbm-killer-high-bandwidth-flash-hbf-allows-4-tb-of-vram-for-ai-gpus
SanDisk's new High Bandwidth Flash memory enables 4TB of VRAM on GPUs, matches HBM bandwidth at higher capacity | Tom's Hardware, accessed January 13, 2026, https://www.tomshardware.com/pc-components/dram/sandisks-new-hbf-memory-enables-up-to-4tb-of-vram-on-gpus-matches-hbm-bandwidth-at-higher-capacity
(→) SANDISK UNVEILS THE FUTURE OF MEMORY ..., accessed January 13, 2026, https://documents.sandisk.com/content/dam/asset-library/en_us/assets/public/sandisk/collateral/company/Sandisk-HBF-Fact-Sheet.pdf
Redefining the Five-Minute Rule for AI-Era Memory Hierarchies - arXiv, accessed January 13, 2026, https://arxiv.org/html/2511.03944v1
Kioxia's new 5TB, 64 GB/s flash module puts NAND toward the memory bus for AI GPUs — HBF prototype adopts familiar SSD form factor | Tom's Hardware, accessed January 13, 2026, https://www.tomshardware.com/pc-components/gpus/kioxias-new-5tb-64-gb-s-flash-module-puts-nand-toward-the-memory-bus-for-ai-gpus-hbf-prototype-adopts-familiar-ssd-form-factor
News Posts matching 'NAND flash' - TechPowerUp, accessed January 13, 2026, https://www.techpowerup.com/news-tags/NAND+flash?page=3
What is HBM (High Bandwidth Memory)? Deep Dive into Architecture, Packaging, and Applications - Wevolver, accessed January 13, 2026, https://www.wevolver.com/article/what-is-hbm-high-bandwidth-memory-deep-dive-into-architecture-packaging-and-applications
Nvidia and Kioxia target 100 million IOPS SSD in 2027 — AI server drives aim to deliver 33 times more performance | Tom's Hardware, accessed January 13, 2026, https://www.tomshardware.com/tech-industry/nvidia-and-kioxia-target-100-million-iops-ssd-in-2027-33-times-more-than-existing-drives-for-exclusive-use-in-ai-servers
BaM: A Case for Enabling Fine-grain High Throughput GPU-Orchestrated Access to Storage, accessed January 13, 2026, https://www.researchgate.net/publication/359130416_BaM_A_Case_for_Enabling_Fine-grain_High_Throughput_GPU-Orchestrated_Access_to_Storage
Page 28 – Blocks and Files, accessed January 13, 2026, https://blocksandfiles.com/page/28/?p=contatti%2F1000
Remap-Based Inter-Partition Copy for Arrayed Solid-State Drives | Request PDF, accessed January 13, 2026, https://www.researchgate.net/publication/353469840_Remap-based_Inter-Partition_Copy_for_Arrayed_Solid-State_Drives
반도체기술 로드맵 2026, accessed January 13, 2026, https://www.theise.org/wp-content/uploads/2025/12/%EB%B0%98%EB%8F%84%EC%B2%B4%EA%B3%B5%ED%95%99%ED%9A%8C-%EB%B0%98%EB%8F%84%EC%B2%B4%EA%B8%B0%EC%88%A0%EB%A1%9C%EB%93%9C%EB%A7%B52026%EB%B0%9C%ED%91%9C%EC%9E%90%EB%A3%8C.pdf
