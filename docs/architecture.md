# tudouOS 架构蓝图

## 一、产品定位

面向 **AI Agent 交互、仿真自动化、智能终端设备** 的自研嵌入式实时操作系统：
自研土豆 RTOS 内核 + 完整自研 OS 上层生态。

不做通用工业 RTOS。专注解决传统 RTOS 在「AI 联动、仿真调试、智能终端交互」
场景的短板，在细分领域对标并超越 FreeRTOS / Zephyr。

## 二、四大核心支柱

### 支柱 1：内核原生 MCP AI 消息总线（最大差异化）

传统 RTOS 只有裸数据队列：无消息语义、无协议、无溯源、无 AI 适配能力，
AI 交互全靠上层二次封装，结构混乱、难调试。

tudouOS 的落地形态（工程分层，详见 [mcp-bus-design.md](mcp-bus-design.md)）：

- **内核层**提供**消息基座**：消息 ID、时序号、回执（ACK）、超时管控、
  有界队列 + 丢弃/重发策略、逐消息链路追踪标签——这些是实时语义，必须在内核。
- **系统服务层**提供 **MCP 协议适配器**（JSON-RPC 编解码、传输绑定），
  把内核消息基座桥接到本地 AI / 远程 Agent / 模拟器。
- 设备终端、本地 AI、远程 Agent、QEMU 之间一套统一通信语义。

> 设计纪律：**实时语义进内核，协议解析不进内核**。JSON 解析放在内存与
> 时序都可控的系统服务层，内核基座保持有界、确定性、可测试——这既是
> 工程稳健性要求，也让"内核原生 AI 消息能力"的说法站得住。

### 支柱 2：仿真/硬件双端 100% 同源

- 自研 **PAL 平台抽象层**：一套内核源码同时跑 QEMU（esp32s3）与真机。
- 任务调度、消息机制、内存逻辑、事件顺序双端一致。
- QMP 自动化控制模拟器：AI 全自动压力测试、回归测试、复位/掉电注入。

> 一致性的诚实边界：**功能与顺序**（调度次序、消息流水、内存布局、
> 事件序列日志）双端可完全一致并可自动比对；**硬实时抖动数值**
> （微秒级中断延迟）QEMU 与硅片天然不同，抖动预算以真机实测为准、
> QEMU 验证行为正确性。测试台提供双端事件日志 diff 用例固化这一点。

> 外设与接口的诚实边界：**内核/调度/消息语义**双端同源，但**外设集合不同构**——
> QEMU 的 open_eth 是 esp32s3 机器模型自建的测试外设（MMIO 映射 0x600CD000），
> ESP32-S3 硅片没有内部以太网 MAC，因此 ARP/ping/UDP 功能只在 QEMU 验证；
> 真机网络当前走串口 MCP 传输（见 [ethernet.md](ethernet.md) §8、
> [serial-mcp.md](serial-mcp.md)）。

### 支柱 3：内核原生可观测、可追踪

FreeRTOS 追踪组件臃肿、开销大、闭源、日志简陋，AI 无法自动分析。

tudouOS 内置轻量内核追踪系统（常驻 RAM 有界，单事件开销极小）：

- RAM 环形缓冲记录：任务切换、中断进出、阻塞原因、CPU 占用、
  消息流水（enqueue/dequeue/drop/retry）、内存分配释放。
- 事件导出为 JSONL，直接喂给 AI 做卡死/卡顿/内存泄漏/任务异常分析。
- 系统健康快照（任务水位、堆水位、队列水位、最长阻塞）随时经
  串口/QMP 读取，供 AI 自动运维与自我诊断。

### 支柱 4：AI 交互专用低抖动调度器

通用 RTOS 调度器不区分任务性质，导致屏幕卡顿、指令响应忽快忽慢。

tudouOS 的差异化调度：

- 任务三分类：**AI 通信类**（响应稳定低延迟）、**UI 交互类**（顺滑、
  保帧节奏）、**后台运算类**（限速率、可被压制、不让道于抖动源）。
- 抖动控制手段：分类优先级域 + 后台任务速率限制 + 防饥饿老化 +
  关键路径关中断窗口最小化；抖动用 systimer 时间戳写入追踪环，
  用数据证明"低抖动"而不是口号。

## 三、总体形态

```
┌──────────────────────────────────────────────────────────┐
│ 应用 / 串口 Shell / AI Agent 交互界面                     │
├──────────────────────────────────────────────────────────┤
│ 系统服务层                                                │
│  MCP 协议适配器(JSON-RPC/传输)  文件系统  网络栈  驱动框架 │
├──────────────────────────────────────────────────────────┤
│ 土豆 RTOS 内核                                            │
│  消息基座(MCP语义:ID/回执/超时/追踪/QoS)   ← 支柱1        │
│  三分类低抖动调度器  任务/IPC/Mutex/Sem     ← 支柱4        │
│  可观测追踪环(事件/水位/健康快照)           ← 支柱3        │
│  轻量内存管理(TLSF类/碎片治理/护栏)                        │
│  中断/异常/定时器                                           │
│  启动、UART 控制台、mini libc                ← M1 已交付   │
├──────────────────────────────────────────────────────────┤
│ PAL 平台抽象层 ← 支柱2                                    │
├──────────────┬───────────────────────────────────────────┤
│ QEMU esp32s3 │ ESP32-S3 真机（同源构建、hw 全 RAM 镜像）    │
│  QMP 测试台  │                                            │
└──────────────┴───────────────────────────────────────────┘
```

## 四、M1 已交付（最小闭环）

- **引导链路**：片上 ROM → flash 0x0 处的 ESP 镜像（`kernel.elf` 经
  esptool `elf2image` 生成，不依赖 ESP-IDF；0x0 是 S3 的二级引导偏移）→ `_start`。
- **内核全 RAM 运行**：IRAM 0x40380000 起（代码），DRAM 0x3FC88000 起
  （数据/栈/堆）。LMA==VMA，`.data` 由 ROM 搬运就位，`.bss` 由启动代码清零。
- **call0 ABI**：M1 全内核用 `-mabi=call0` 编译，不旋转寄存器窗口，无需
  窗口异常向量；自带极小 libc 与软除法，不链接任何预编译库。
- **单核默认运行**（S3 双核：boot 后 CPU1 不上电，`smp start` 显式拉起；单核行为
  与 M1 基线零变化，SMP 细节见 [smp.md](smp.md)）。
- **基础外设**：UART0 轮询驱动 + mini printf + 串口命令循环（`ping/echo/info/help`）；
  启动即关闭 TIMG0/TIMG1/RTC 三处看门狗。
- **研发检验闭环**：`python tools/build.py` 构建 → `python -m test.runner`
  QEMU 自动验收 4/4（详见 [qemu-testing.md](qemu-testing.md)）。

## 五、关键设计决策记录

| 决策 | 理由 |
| --- | --- |
| 不用 ESP-IDF/FreeRTOS 任何代码 | 100% 自研；ROM 是芯片内置，无法也无需绕开 |
| 内核作为"二级引导镜像"放在 flash 0x0 | ROM 直接加载 RAM 段并跳入口，免去自写 flash XIP；M5 内核变大后再引入自研 bootloader + XIP |
| QEMU 与真机同源构建、镜像布局按平台分化 | QEMU 走 flash XIP；真机走全 RAM 单镜像（ROM 直接搬段，规避 0x42 取指问题），`esptool write-flash` 直接上板（支柱2的根基） |
| QMP + UART 全走 TCP | 测试台纯 Python 跨平台；QEMU 可跑在本机或 WSL2/远程 |
| MCP：实时语义进内核、协议解析进服务层 | 内核保持有界确定性；AI 适配能力不打折（支柱1的落地纪律） |
| 追踪环静态定容、宁可丢旧不扩容 | 可观测性不能以破坏实时性/内存确定性为代价 |
| 启动横幅作为测试锚点 | `[POTATO]` 前缀行既是日志也是断言点 |

## 六、路线图（对齐四大支柱与五大强化重点）

| 里程碑 | 内容 | 支柱/强化对应 | 验收方式（QMP 测试台用例） |
| --- | --- | --- | --- |
| M1 ✅ | 启动 + UART 控制台 + 测试台闭环 | 支柱2雏形 | 横幅/ping-pong/QMP 复位/停止恢复 4/4 通过 |
| M2 ✅ | 中断与异常：VECBASE 向量表、UART RX 中断（level2 向量路径）、1kHz tick（核内 CCOMPARE0，level1 通用异常路径+RFE）、致命异常现场 dump、最小追踪环（IRQ/EXC 事件）。时钟双路线：QEMU Windows 用 ccount（TIMG 计数器在该构建上启用即崩，见 qemu-testing.md 已知问题），真机/WSL 可切 TIMG | 强化1（根基）、支柱3起步 | M1 4 用例 + tick 存活/RX 中断不丢/异常 dump+复位恢复/追踪 dump/抖动冒烟 共 9/9 |
| M3 ✅ | RTOS 核心：任务模型（静态栈 TCB 注册表）、syscall 统一上下文切换（call0，88 字节帧）、**三分类调度**（AI通信/UI/后台 + 轮转 + 5tick 时间片 + 200ms 老化防饿死【同优先级老化者优先决胜】+ 后台 1000tick 窗口 700tick 预算限速）、任务 sleep/yield/exit、idle 空闲任务、SCHED_SWITCH 追踪事件、taskstat/taskdemo 观测命令。**v5（2026-10-16 缺陷修复）**：tick 改 int6 中断驱动（时间推进独立于检查点，v4"忙烧冻结时间基准"病理消除，抖动 11892µs→~2.7ms）；tick 抢占切换完整接线（IRQ 帧 + rfe，E-02 实证路线），QEMU 模型在 genex 上下文切走时随机丢 CCOMPARE（见已知问题表），QEMU 默认检查点切换（`preempt on|off`），真机默认应开 | 支柱4、强化1 | 缺陷修复后含"无检查点忙烧期间 tick 持续推进"等判别性用例（测试台合计 48/48 两连绿） |
| M3.5 ✅ | **内核消息基座**：有界通道（静态容量环形+满丢/阻塞策略）、消息 ID/seq/回执/超时、QoS0/1/2（重发指数退避+上限、去重窗口、回执窗口）、水位/丢弃/重发计数健康快照、MSG_ENQ/DEQ/DROP/RETRY/ACK 追踪事件、msgdemo 五模式压测（flood/ping/lazy/timeout/heal）。等待语义为切片睡眠轮询（QEMU 限制），阻塞原语 pot_sched_sleep_until/wake 已就位供真机阶段恢复事件驱动 | 支柱1、强化2 | 洪泛 12.5×容量不 OOM 且回收准确、QoS1 全程回执零丢失、延迟回执触发重发+去重恰一次投递、超时失败上报、QMP 冻结自愈无丢无重、消息事件可回放——共 6 用例 |
| M4 ✅ | 轻量内存管理：TLSF 类分配器（15 桶分级空闲表+位图定位+首适配，大块分裂/邻块即时双向合并）、护栏（块尾 canary+回指，free 校验越界即捕获计数不崩溃）、度量（used/free/largest/最大水位/alloc/free/fail/fault，memstat 命令）、MEM_ALLOC/FREE/FAULT 追踪事件、memtest 五模式（selftest/holes/fault/exhaust/churn soak）。soak 场景 8s wall ≈ 27 万次随机分配/校验/释放零损坏零泄漏；48h 长测入口已备（churn+批量复位脚本） | 强化4、支柱3 | 6 用例：分配回归/碎片合并恢复/越界捕获存活/耗尽恢复/churn soak 零泄漏/追踪可回放 |
| M5 ✅ | **自研二级映射引导 + flash XIP**：boot.S 在进入 C 前配置外部内存 MMU（表@0x600C5000，表项=物理页号|invalid|type）映射 flash 页到 0x42 窗口 + 使能 icache（ROM 无映射能力实测，IROM 段从引导 ELF 剥离、LMA 落 flash 0x10020，镜像经 objcopy+merge-bin 双段布局）；C 代码+只读数据全部 XIP（摆脱 RAM 容量限制，IRAM 仅剩向量+汇编 <1KB）；**PAL 成形**（pal.h 双后端接口：qemu/hw，时基模式/时钟查询，TUDOU_PLATFORM 编译期选择）；**驱动框架**（链接期 .pot_drivers 段收集 + 优先级排序初始化 + drivers 枚举，uart 首个注册） | 支柱2、强化3 | 5 用例：XIP 布局（irom@0x42 窗口/IRAM≤4KB）/ROM 仅搬 RAM 段/PAL 查询/驱动注册枚举/XIP 全子系统功能——共 31/31 |
| M6 ✅ | **MCP 协议适配器**（支柱1 收官；D-20 对齐 2025-06-18 规范核心）：迷你 JSON 解析/序列化（递归下降+完整 \uXXXX/代理对转义+迭代化释放）、initialize/notifications/initialized 握手、tools/list 规范 schema、tools/call（content/isError 形态）、id 三态回显、notification 不回包；UART 行传输绑定（'{' 开头行→消息基座通道）、代理任务（AI 类）分发（-32700/-32601/-32602/-32600 错误路径，超长行显式报错）；工具含 truncated 语义；**potfs**（RAM 扁平命名 blob 表，写时复制，临界区保护）；**串口 Shell**。消息内联负载 256B | 支柱1、强化2 | 9 用例（含多请求/握手/unicode/行长回归）——测试台合计 48/48 两连绿 |
| M7 ✅ | 以太网（QEMU）：open_eth 测试外设 ↔ 协议栈；远程 MCP 通道（UDP 信封：纪元/序号/回执/去重/重同步）、UART tap 调试通路、`tools/net_peer.py` 主机侧对端、`docs/ethernet.md`。**真机 S3 无内部 EMAC**（硅片事实，旧 DW GMAC/外部 PHY 规划已推翻）；真机远程 MCP 走串口传输（[serial-mcp.md](serial-mcp.md)） | 支柱1/2 | ARP 应答、ping 回环、UDP 远程 MCP initialize/tools/list/去重/断线重连纪元重同步——6 用例（QEMU 6/6；nodev 无 EMAC 降级）；真机串口 MCP 与 M1–M9 命令面由真机串口回归台验收（70 PASS/0 FAIL/2 显式 SKIP，见 [test/logs/hw/summary.md](../test/logs/hw/summary.md)） |
| M8 ✅ | 可观测体系完整化：JSONL 导出（主环 256+/重要副环 64+/健康快照/流式泵）、注入（leak/hang/spin）、`tools/ai_analysis.py` 规则分析器（12 条规则）、`docs/observability.md`；D-08：tick 补账丢失导致时基/调度冻结（health `irq.sched_lag` 可观测，已修） | 支柱3、强化5 | 注入泄漏/卡死场景由 AI/脚本从导出日志定位根因——6 用例（含时基无漂移判别用例） |
| M9 ✅ | SMP：CPU1 启动（ROM 桩 `0x600C0004` 写入口，QEMU/真机同协议）、核间中断（IPI + 中断矩阵分核路由）、每核就绪队列/当前任务/时间片（THREADPTR 核号、S32C1I 全局调度锁）、任务亲和与跨核唤醒/迁移、CPU0 每 4 tick 负载均衡（20ms 冷却防抖）、`docs/smp.md` | 强化1 | 默认单核不变量、ROM 握手、IPI 双向、绑核执行、负载均衡迁移、混合压测稳定——6 用例（QEMU 6/6；真机 SMP 已验证：CPU1 经 `SYSTEM_CORE_1_CONTROL_0` 释放后上线，IPI/tick/绑核/负载均衡带板实测通过，见 `docs/hardware-bringup.md`） |

**真机带板状态（2026-10-19 更新）：** 2026-10-17 起 hw 平台为**默认全 RAM 布局**
（代码 IRAM、只读数据 DRAM，ROM 直接搬两段 RAM；`--xip` 仅保留 XIP 实验路径），
真机稳定启动、控制台命令面完整；CPU1 需先经 `SYSTEM_CORE_1_CONTROL_0` 释放复位，
释放后 SMP 全链路（自有 tick/IPI/绑核/负载均衡）带板实测通过；super watchdog 由
`RTC_CNTL_SWD_CONF` 关闭。**真机网络**：ESP32-S3 硅片没有内部以太网 MAC，
「DesignWare GMAC + 外部 PHY」的前提不成立（2026-10-19 事实核查，见
[ethernet.md](ethernet.md) §8）；QEMU 的 open_eth 只是机器模型测试外设，真机远程
MCP 改走串口 MCP 传输（[serial-mcp.md](serial-mcp.md)）；ARP/ping/UDP 协议栈目前
仅在 QEMU 验证，真机以太网需外挂 SPI 模块（W5500/DM9051 类，未实现/未到货）。
详见 [docs/hardware-bringup.md](hardware-bringup.md)。

**真机 M1–M9 全量回归（2026-10-19 晚）：** 串口自动回归台
（`test/hw_runner.py` + `test/hw_cases/`，72 用例）在普通 ESP32-S3 开发板上
跑通全部里程碑：**70 PASS / 0 FAIL / 0 TIMEOUT / 2 显式 SKIP**（QMP 断线冻结、
QEMU XIP 布局差异，均有替代覆盖）。报告与逐条日志见
[test/logs/hw/summary.md](../test/logs/hw/summary.md)；串口 MCP 工具
`tools/serial_mcp.py` 的 selftest/跨进程/重放/复位恢复/`--stdio` 桥均通过。
剩余已知限制：以太网需外挂 SPI 模块、CPU1 tick 抢占默认关未做真机稳定性验证、
**K1–K3**（启动 tick 统计瞬态/输出行锁加固建议/trace 环覆盖设计观察）。

**贯穿性纪律（每个里程碑都执行，不是最后一个里程碑才做）：**

1. **回归只增不减**：新功能用例进 `test/cases/`，M1 的用例永远在跑。
2. **双端一致性校准**（强化3）：关键里程碑加"事件序 diff"用例——同一
   输入在 QEMU 与真机产生的事件日志序列必须一致（时序数值除外）。
3. **挂机 soak**（强化1/4）：从 M3 起每版必跑长时压测脚本（任务风暴 +
   消息洪泛 + 内存翻搅），内存水位曲线必须水平。
4. **一切可观测**（强化5）：从 M2 起每个新机制落地时同步加追踪事件，
   追踪覆盖面是代码评审检查项。

## 七、五大强化重点 → 可测指标

| 强化重点 | 可测指标（验收口径） |
| --- | --- |
| 1 内核稳定性 | 48h soak 零 panic、零内存增长、零死锁；上下文切换 10⁷ 次无漂移 |
| 2 MCP 总线容错 | 洪泛不 OOM（有界队列）；断线自愈 < 指定超时；重发有去重；丢弃有计数可查 |
| 3 双端一致性 | 事件序 diff 用例双端通过；内存布局符号双端一致 |
| 4 内存管理 | 碎片率有上界；alloc/free 追踪可回放；越界即捕获并报告现场 |
| 5 可观测日志 | 卡死/泄漏注入场景，仅凭 JSONL 导出即可定位根因；健康快照随时可读 |

