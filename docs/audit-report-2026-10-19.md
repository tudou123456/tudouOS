# tudouOS 内核独立审计报告

- 报告日期：2026-10-19
- 审计对象：`D:\公司软件\tudouOS`（tudouOS：面向 ESP32-S3 的自研嵌入式 RTOS）
- 代码基线：`main` @ `8aa1fa5`「第7批：真机串口 MCP + 真机 M1–M9 串口回归台 + 文档事实纠错」
- 工作树状态：干净（审计期间未修改任何被跟踪文件；本报告为新增未跟踪文件）
- 审计方式：静态代码审读 + 独立干净构建 + QEMU/真机全量回归复跑 + 文档与实现对照 + 三路并行只读评审（core / services / net）+ 逐条复核
- 审计环境：Windows x64 / Python 3.14.3 / xtensa-esp-elf-gcc 13.2.0（crosstool-NG `esp-13.2.0_20250707`）/ QEMU 9.2.2（乐鑫分支 `esp_develop_9.2.2_20260417`）/ esptool 5.4.0 / 板卡 ESP32-S3 rev v0.2（8MB PSRAM，COM3，MAC `28:84:85:46:0c:b4`）

---

## 0. 结论摘要

**总评：工程完成度与测试基建明显高于个人/小团队自研 RTOS 的通常水平，M1–M9 功能真实可运行，真机链路已打通；但当前 HEAD 仍存在 3 个确定的高危缺陷（其中 1 个为确定的 use-after-free），且"全绿"是可达结果却不是稳定门禁。建议按本报告 P0 → P1 顺序收口 2–4 周后再对外宣称稳定。**

| 维度 | 评分 | 依据 |
|---|---|---|
| 架构与设计 | 8.5/10 | 分层清晰、里程碑完整、真机 ROM 引导与全 RAM 布局成立、纯自研边界清晰 |
| 工程化与测试基建 | 8/10 | 干净构建、-Werror、镜像自检、烧录哈希、QEMU/真机双测试台；扣分项为 flaky 与假绿风险 |
| 代码正确性 | 6.5/10 | 3 个高危 + 若干中危；默认配置（preempt 关、SMP 手动、单机网络）掩盖了部分缺陷 |
| 文档与自省 | 9/10 | `docs/defect-report.md` 1066 行、32 条缺陷五段式、判定实验 E-01/02/03；扣分项为 README 数字/文件名漂移 |
| 真机可用性 | 7.5/10 | 冷启动、命令面、串口 MCP、M9 双核链路实测通过；SMP 并发、网络对抗、长稳未验证 |
| **综合** | **≈7.7/10** | 属于"接近可交付"：先修 P0/P1 与测试稳定性，可达产品级基线 |

**Top 5 风险**

1. MCP 字符串 id 的 use-after-free（`kernel/mcp/mcp.c:451-466` 等）——所有字符串 id 请求的响应 id 读取已释放请求树。
2. 调度器 syscall 恢复不判 `frame_kind`（`kernel/sched/sched.c:272-279`）——`preempt on` 时被 IRQ 抢占过的任务可能跳到陈旧 `a0`。
3. 跨核临界区是"本核 rsil"假锁（`kernel/include/intc.h:70`）——`smp start` 后堆/消息/potfs/trace 会被两核并发修改。
4. potfs 读侧无锁且写侧释放旧数据（`kernel/fs/potfs.c:91-94` vs `100-111`）——并发读已释放内存。
5. UDP 远程 MCP 无认证（`kernel/net/netmcp.c:628-632/687-696/708-715`）——同网段可接管会话并调用 `fs.write` 等工具。

---

## 1. 审计范围与方法

### 1.1 范围

- 内核：`kernel/` 全部 C/汇编/链接脚本（约 11,194 行；net 2421、kernel 目录 2228、mcp 1078、include 1063、sched 954、trace 812、smp 561、time 456、intc 448、mem 277、msg 239、boot 226、linker 158、fs 149、driver 73、pal 51）。
- 测试：`test/` 约 5,249 行（QEMU 16 模块 67 用例；真机 10 模块 72 用例；QMP/serial 基础设施）。
- 工具：`tools/` 约 1,808 行（build/check_env/envlib/serial_mcp/hw_monitor/qprobe/ai_analysis）。
- 文档：`docs/` 约 2,381 行（architecture / defect-report / ethernet / serial-mcp / smp / observability / qemu-testing / hardware-bringup / mcp-bus-design）。
- 仓库：16 个提交（2026-10-06 → 2026-10-19），117 个被跟踪文件，工作树干净。

### 1.2 方法

1. **独立复跑**：从零构建 QEMU 镜像与真机镜像；QEMU 全量 4 轮、真机全量 2 轮；记录每条失败用例的共同根因。
2. **静态审计**：逐行复核调度/上下文切换/中断、MCP/JSON、堆、potfs、netmcp、SMP 等关键路径；对评审结论做二次验证（能证伪的证伪、能降级的降级）。
3. **并行评审**：三个只读子评审分别负责 core / services / network + 测试可信度，Lead 逐条对照源码复核，剔除误报。
4. **文档对照**：README、architecture、defect-report、qemu-testing、smp、hardware-bringup 的关键声称与代码/实测对齐。

### 1.3 限制与声明

- 仅一块 ESP32-S3 开发板；未跑 48h 长稳 soak、未做 10⁷ 次上下文切换压力、未做网络对抗测试。
- 本报告不评价产品商业定位与 AI 交互体验，只评价内核工程质量与正确性。
- RTOS 单地址空间、无 MMU 进程隔离属既定设计，不作为缺陷。
- 审计期间本机 `third_party/`（gitignored 工具链）被误删并按固定版本恢复，源码树未受影响；详见 §10。

---

## 2. 验证结果（独立复跑）

### 2.1 构建

| 项目 | 命令 | 结果 |
|---|---|---|
| QEMU 干净重建 | `py -3.14 tools/build.py --clean` | 34/34 目标重编，`-Werror` 通过；IROM 83276B @0x10020 逐字节自检 OK；4MB flash 镜像生成；产物 `build/kernel.elf`、`build/potato_boot.bin`、`build/flash_image.bin` |
| 真机干净重建 | `py -3.14 tools/build.py --platform hw --clean` | 34/34；`build/kernel_hw.elf` + `build/potato_boot_hw.bin`（**85952B**，与文档一致） |
| 真机烧录 | `py -3.14 -m esptool --chip esp32s3 --port COM3 --baud 921600 write-flash 0x0 build/potato_boot_hw.bin` | 85952B，`Hash of data verified`，固件 build `Oct 19 2026 21:28:31` |
| 环境体检 | `py -3.14 tools/check_env.py` | 交叉编译器 / esptool / QEMU 全部 OK，版本与基线一致 |

##### 构建期非致命告警（建议清理）

- `objcopy: warning: empty loadable segment detected at vaddr=0x42000020`（每次构建出现，不影响产物）。
- `build.py` 误报 `警告: QEMU 非乐鑫分支构建`：`tools/build.py:124-143` 只检查可执行文件父目录名是否含 `esp-develop`/`esp-release`，而本机安装路径为 `third_party/qemu/qemu/bin`（无版本目录），实际 QEMU `--version` 为乐鑫分支 9.2.2。建议改为解析 `--version` 输出。

### 2.2 QEMU 全量回归（67 用例，4 轮）

| 轮次 | 环境 | 结果 | 失败用例 |
|---|---|---|---|
| R1（干净构建后） | 与静态评审并行 | 66/67 | `test_msg_trace`（消息事件缺失） |
| R2（审计主轮） | 安静 | 65/67 | `test_msg_trace`、`test_mem_trace`（内存事件缺失） |
| R3（复跑） | 安静 | **67/67** | — |
| R4（复跑） | 安静 | **67/67** | — |

**失败根因（已定位，属测试设计脆弱，非内核数据损坏）**

- 追踪环 `POT_TRACE_CAPACITY=256`，而 tick 为 1kHz 且每个 tick 记 `IRQ_ENTER`+`IRQ_EXIT` 两条事件 ⇒ 环仅覆盖约 128ms。
- `trace 200` 的导出不是快照：`trace_push` 持续写、导出按实时 `head` 计算索引，打印 200 行期间环已整体滑动，早期事件被覆盖。
- 因此 `test_msg_trace`（`test/cases/test_m35.py:108-132`）与 `test_mem_trace`（`test/cases/test_m4.py:95-114`）依赖"流量结束后立刻 dump"，在主机时序抖动时必然抓不到事件；这两条用例是结构性 flaky。
- 真机侧同类用例已改用 `trace meta` 计数差分（`test/hw_cases/test_hw_m3.py:174-190`），QEMU 侧尚未迁移。
- 说明：旧"67/67 全绿"日志（`build/_final_qemu_runner3.log`，mtime 20:27:32）早于 HEAD 提交（20:36:11），并未覆盖当前 HEAD。

### 2.3 真机全量回归（72 用例，2 轮）

| 轮次 | 结果 | 说明 |
|---|---|---|
| H1 | PASS=69 / FAIL=0 / TIMEOUT=1 / SKIP=2 | 超时用例 `m3/preempt_toggle`；串口 transcript 显示固件已正确输出 `preempt enabled` 与 `preempt=1`，超时发生在 harness 采集侧 |
| H2（复跑） | **PASS=70 / FAIL=0 / TIMEOUT=0 / SKIP=2** | 与 README 声称一致；`m3/preempt_toggle` 复跑 0.44s PASS |

两条 SKIP 均属真机平台限制且有替代覆盖：`m35/disconnect_heal`（QMP stop/cont 是 QEMU 专有手段，真机 RTS 复位语义不等价）、`m5/xip_not_applicable`（真机默认全 RAM 构建，无 flash XIP 取指）。

**结论：文档的真机结论（70 PASS / 0 FAIL / 2 SKIP）可复现；QEMU 的"67 全绿"在安静环境可复现，但测试集存在结构性 flaky，不能作为稳定发布门禁。**

### 2.4 稳定性结论

- 4 轮 QEMU：2 次失败、2 次全绿，失败**全部**集中在两个 trace dump 用例；非 trace 用例 65 条在 4 轮中 100% 通过。
- 2 轮真机：1 次 1 例超时（采集侧 flake）、1 次完全通过。
- 建议：CI/发布门禁连续跑两轮全绿；或先修 trace 用例（改用 `trace meta` 计数），否则"全绿"会持续被误判为代码回归。

---

## 3. 架构与工程亮点

1. **完整度罕见**：单仓库覆盖 ROM 引导 → 中断/tick → 三分类调度 → TLSF 类堆 → 有界消息 QoS → potfs → MCP JSON-RPC → 以太网 ARP/ICMP/UDP → 观测/注入 → SMP 双核，且每层都有命令面与测试锚点。
2. **真机工程判断在自我修正**：主动证伪并纠正"ESP32-S3 有内部 EMAC"的早期规划（S3 硅片只有 Wi-Fi/BT，无 MAC），改为串口 MCP + 明确标注以太网仅 QEMU open_eth；真机改用全 RAM 单镜像规避 S3 的 0x42 flash 取指问题——这两条都是容易走错且代价高的判断。
3. **测试基建是核心资产**：QEMU 每用例独立 VM、动态端口、`-S` 冻结后先连 QMP/串口再放行、串口/QMP 全量落盘；真机 per-case 复位、串口协议、SKIP 写明理由。对比 FreeRTOS/Zephyr 的公开基线也不逊色。
4. **缺陷自省文化**：`docs/defect-report.md` 32 条缺陷采用"位置→证据→根因→修复→验收"五段式，含 E-01/02/03 判定实验（loop 指令、rfe 恢复、MMU 语义）与 D-01..D-32 的修复记录；K1/K2 从真机观测反推修复。这在个人项目中非常少见。
5. **可观测性**：trace 环 + 重要事件副环 + JSONL 导出 + `trace meta` 计数 + 故障注入（hang/leak/panic）+ 调度漂移/health 快照，形成了"可回放、可定位"的闭环。
6. **工程纪律**：`-Werror`、增量构建（mtime）、镜像 IROM 逐字节自检、烧录哈希校验、工具链版本 pin、`.gitignore` 干净（`build/`、`third_party/`、日志均不入库），D-29 已把误入仓库的二进制 `-` 移出。

---

## 4. 缺陷清单

> 级别定义：**P0** = 内存安全/崩溃/安全边界，且一旦对应特性开启即触发；**P1** = 并发/协议健壮性缺陷，正常主路径不易触发；**P2** = 稳定性和移植风险；**P3** = 卫生与可维护性。
> 证据来源：Lead = 本次逐行复核；评审 = 子评审提出且 Lead 复核了机制（未逐行复核全部分支）。

### 4.1 P0（建议立即修复）

#### P0-1 MCP 字符串 id use-after-free

- **位置**：`kernel/mcp/mcp.c:451-466`（`jid.str = jid_node->str`，结构体注释自述"指向请求树内字符串，生命周期随请求树"）；释放点与使用点：`:480`（initialize）、`:489-490`（tools/list）、`:516-517`（tools/call）、`:528-534`（旧直连工具）、`:539-542`（method not found）。
- **现象**：所有请求分支都先 `pot_json_free(req)`，随后 `send_*` 仍通过 `jid.str` 渲染响应 id。
- **影响**：带字符串 id 的 JSON-RPC 请求（含标准 MCP `initialize`）响应 id 可能损坏；`pot_json_free` 后该内存可被工具执行中的 JSON 分配复用（`tools/call` 路径），进一步造成非法读取或响应错配。属确定的 UAF。
- **修复建议**：在 `jid_t` 中增加固定 64B 缓冲，解析 id 时复制；或把 `pot_json_free(req)` 移到响应生成之后（注意 `params` 仍被工具使用，需按分支处理）。
- **回归**：新增 QEMU/真机用例：`initialize`/`tools/list`/`tools/call` 分别带字符串 id（含长 id、转义 id），断言响应 id 原样回显。

#### P0-2 调度器 syscall 恢复不判 `frame_kind`

- **位置**：`kernel/sched/sched.c:272-279`（所有 syscall 切换对 `next` 一律 `pot_do_resume()`，无 `frame_kind` 判断）；对照 `kernel/sched/sched.c:308-311`（IRQ 抢占路径有 `if (next->frame_kind == POT_FRAME_IRQ) pot_do_resume_irq(next);`）；`kernel/sched/switch.S:70-112`（`pot_do_resume` 固定 `wsr.epc1` = 帧[0]，即 a0 槽）与 `switch.S:118-133`（`pot_do_resume_irq` 用 +76/+80）。
- **现象**：任务 A 被 tick 从 IRQ 抢占（帧类型 IRQ，续体 PC 在 +76），随后任务 B 通过 syscall（yield/sleep/exit）切走，调度器挑中 A 时走 syscall 恢复函数 ⇒ 以 A 的陈旧 `a0` 作为 PC 执行。
- **影响**：`preempt on` 时可能跳转到非法地址/错误上下文（真机崩溃或静默错乱）。当前默认 `preempt` 关闭（QEMU 因模型缺陷默认关；真机代码同样恒为 0），因此默认路径未触发，但文档宣称的"真机默认开抢占"一旦落地即可触发。
- **修复建议**：与 `sched.c:308-311` 同构，按 `next->frame_kind` 分派；并加断言"IRQ 帧只能由 pot_do_resume_irq 恢复"。
- **回归**：`preempt on` + 两任务交替 yield + 高压 tick，校验 tick 计数连续、任务计数正确；QEMU 模型缺陷可能导致不可复现，真机为最终判据。

#### P0-3 跨核"锁"实为本核 `rsil`（SMP 下失效）

- **位置**：`kernel/include/intc.h:60-82`（`irq_save()` = `rsil %0, 1`，仅屏蔽本核 level-1）；被当互斥使用：`kernel/mem/heap.c:197`、`:230`；`kernel/msg/msg.c:171`、`:193`、`:222`、`:239`、`:254`；`kernel/fs/potfs.c:61`、`:121`；`kernel/trace/trace.c:69-79`、`:99`（trace_lock 同为 rsil 1）。
- **现象**：`smp start` 后，CPU0/CPU1 可同时进入上述"临界区"（PS.INTLEVEL 是每核私有的），对堆空闲链表/位图、消息环头尾、potfs 表、trace 环的复合更新没有跨核互斥。
- **影响**：双核并发分配/释放或收发消息时静默损坏内核数据结构；单核安全。
- **修复建议**：为这些子系统引入 S32C1I 自旋锁（可先只保护跨核入口）；或在设计上限定这些子系统只允许 CPU0 访问，并在 SMP 启动/任务迁移处加断言与文档约束。
- **回归**：双核并发 `memtest churn` + `msgdemo flood` + `fs write/read` + trace 导出压力测试（当前 M9 用例只覆盖调度/IPI/迁移，未覆盖该组合）。

#### P0-4 potfs 读侧无锁 + 写侧释放旧数据

- **位置**：`kernel/fs/potfs.c:61-98`（写路径 `irq_save`，`:91-94` 在临界区内 `pot_heap_free(f->data); f->data = nd`）；`potfs_read` `:100-111`、`potfs_size` `:113-117` 无锁直接解引用 `f->data`。
- **现象/影响**：控制台 `fs cat`、MCP `fs.read` 与并发写（UART level-2 中断上下文或另一核）交错时，读已释放内存（UAF/垃圾内容）。
- **修复建议**：读侧同域加锁并复制数据；SMP 下配合 P0-3 的真锁；更彻底可做版本号/引用计数。
- **回归**：并发 `fs.write` + `fs.read` 洪泛（串口 MCP + 控制台同时发起）校验无故障计数且内容完整。

#### P0-5 UDP 远程 MCP 无认证、信封可伪造

- **位置**：`kernel/net/netmcp.c:687-696`（任意源地址的 UDP 包被学习为 peer 并覆盖 `g_peer_ip/g_peer_port`）、`:708-715`（`epoch > g_epoch` 即接受并重置状态）、`:628-632`（`ack > g_ack` 即推进并丢弃重放槽，无纪元/上界校验）；可被调用的工具表见 `kernel/mcp/mcp.c:232-246`（含 `fs.write`）。
- **影响**：UDP 通道一旦暴露在不可信网络，任意主机可接管会话、伪造 ack 清空重放保护、重放/重复执行工具调用；串口通道因物理隔离不受影响。这与"内核原生 MCP 消息总线"的产品定位直接相关，需在文档中明确信任边界或补鉴权。
- **修复建议**（按成本递增）：① 文档明确"UDP MCP 仅限可信直连网段"，默认关闭 UDP 通道；② peer 固定配置，不学习任意源；③ ack 校验 `epoch == g_epoch` 且 `ack <= g_tx_seq`；④ 共享密钥 HMAC/token 鉴权。
- **回归**：伪造源地址 ack、跨纪元包、重放包、超大 seq/epoch 的负路径用例。

### 4.2 P1（高，建议下批修复）

| 编号 | 位置 | 问题 | 影响 | 建议 |
|---|---|---|---|---|
| P1-1 | `kernel/msg/msg.c:74-98` | 去重/回执窗口固定 8 槽且跨发送方共享，仅按 `(sender,id)` 配对；调用方复用 id（含 0）会误判幂等/误命中旧 ack | 静默丢消息或重复投递（工具二次执行） | ack 按入队 seq 配对、按发送方分桶、窗口扩容；只对 QoS1/2 记账（D-17） |
| P1-2 | `kernel/trace/trace.c:69-99` + `kernel/intc/intc.c:83/101` | `trace_push` 临界区只屏蔽 level-1，但 level-2 ISR（UART/EMAC）也调用 `trace_push` | 环记录撕裂、seq 重号；导出分析被污染 | level-2 事件走副环/无锁计数；或环写限定单写者并加断言 |
| P1-3 | `kernel/sched/sched.c:122-128` | 就绪表 16 槽满时 `enqueue_ready` 直接丢弃，但调用方已把任务状态置 READY（抢占路径丢的正是刚被中断的运行任务） | 任务永久失踪（仅诊断计数可见） | 返回失败由调用方回滚状态；或改可增长的容器 |
| P1-4 | `kernel/sched/sched.c:655`（`pick_best` 在 `pot_sched_should_preempt_cpu` 中无锁调用） | 就绪队列正被对核并发修改时读半更新状态 | SMP 下误判抢占、越界读风险 | 加锁或先做快照 |
| P1-5 | `kernel/smp/smp.c:447-451` | IPI 处理 `pend = g_ipi_pending[me]; g_ipi_pending[me] = 0; mmio_write(...,0)`：read/clear 之间新置的位与 MMIO 线一起被清 | IPI 丢失且无重试 | 先清 MMIO 源再原子读清（S32C1I），或发送侧重发兜底 |
| P1-6 | `kernel/mcp/mcp.c:232-246`（工具表） | UDP 与 UART 同入口，无会话概念/无写配额；任意入口可 `fs.write` | 网络暴露时等于任意写；串口场景为本地可信 | 会话 token + 写配额，或文档明确信任边界（与 P0-5 一并） |

另：堆 canary 使用固定派生值（`kernel/mem/heap.c:56/251-271`），可被伪造；`free` 先 `ptr-8` 读 magic 再校验范围，建议补地址范围/对齐校验并在 footer 不符时直接墓碑隔离（现状部分已修，仍有残留）。

### 4.3 P2（中，需真机/专项验证）

| 编号 | 位置 | 问题 | 说明 |
|---|---|---|---|
| P2-1 | `kernel/intc/vectors.S:106-122` | QEMU 构建的 IRQ 帧 PS 取自 `SR194`（binutils 名 eps2），而 level-1 异常的 PS 硬件存入 EPS1 | 若 QEMU 行为如评审推测（SR194 残留 level-2 现场），可能是"genex 切换 + CCOMPARE 丢 tick"的模型级缺陷真因；需真机确认 EPS1 编号后统一。**待实测** |
| P2-2 | `kernel/smp/cpu1_entry.S:42-54` | `movi a1, pot_cpu1_stack_top` 在 `wsr windowbase 0` 之前执行 | 若 ROM `callx8` 入口的 WINDOWBASE≠0，换栈可能写到旋转后的物理寄存器；QEMU 与真机均能启动，需读 WINDOWBASE 实测。**待核** |
| P2-3 | `kernel/sched/task.c:84-127` + `tools/build.py:184`（`-fno-stack-protector`） | 任务栈无 guard/canary，仅事后水印；`sched.c:91-94` 已记录 idle 1KB 栈被中断帧击穿的实证 | 建议在 `stk_lo` 放哨兵并在切换时校验 |
| P2-4 | `kernel/time/timg_timer.c:39/45` | 写 `T0_UPDATE=0`，但该位应为 bit31；锁存从未发生（被 32 位读掩盖） | TIMG 路径当前未启用，属移植隐患 |
| P2-5 | `kernel/intc/intc.c:86` + `kernel/intc/vectors.S:46` | 只处理 level-2；level-3 向量不清源即 `rfi 3` | 一旦启用 int22 类中断即风暴；当前未启用 |
| P2-6 | `kernel/kernel/main.c:52-58` | 先开全局中断再初始化 sched/smp（`g_cur[0]==NULL`、SCOMPARE1 未设） | 当前依赖初始化顺序侥幸安全，建议移开中断前完成 |
| P2-7 | `kernel/sched/sched.c:392-410` | `g_bg_used` 两核 tick 无保护 RMW | CPU1 后台消耗会扣 CPU0 预算；SMP 下统计失真 |
| P2-8 | `kernel/smp/smp.c:302`、`kernel/sched/task.c:35-47` | CPU1 上线即共享 UART printf；`pot_task_register` 无锁且被 CPU1 调用 | 行输出可能交错、注册表竞争（低概率） |

### 4.4 P3（卫生与可维护性）

- 死代码：`pot_sched_resched_from_irq`（`kernel/sched/sched.c:723-737`，声明于 `kernel/include/smp.h:95`，全仓库无调用者；真正抢占路径由 `intc.c:125` 与 `smp.c:332` 调 `pot_sched_preempt_from_irq_cpu`）。
- 注释与实现不符：`kernel/include/task.h:28`、`kernel/sched/sched.c:258` 仍写"ret.n 恢复"，实际统一 rfe（v5 已改）。
- 构建误报与告警：`tools/build.py:124-143` 乐鑫分支判定（见 §2.1）；objcopy empty segment 警告。
- 无 CI：仓库无 `.github/workflows`；"全绿"依赖人工跑并留日志，存在旧日志早于 HEAD 的假绿风险。
- `docs/defect-report.md` 遗留：D-16（QoS2 drop_oldest 保护）、D-17（窗口分桶）、D-26（时基运行时标定）、D-28（死代码清理）、D-30（CPENABLE/%d UB）、D-03 真机默认开抢占。
- 48h soak / 10⁷ 次上下文切换等文档承诺指标尚未跑（文档自认）。

---

## 5. 文档与声称一致性核对

| # | 声称 | 位置 | 实测/代码 | 判定 |
|---|---|---|---|---|
| 1 | QEMU 测试台 67 用例全绿 | `README.md:14`、`:95` | 4 轮：66/67、65/67、67/67、67/67；失败均为 trace dump 用例 | **可达但非稳定**；需修测试并加两轮门禁 |
| 2 | 真机 70 PASS / 0 FAIL / 2 SKIP | `README.md:17-19`、`:108` | 首轮 69 PASS/0 FAIL/1 TIMEOUT/2 SKIP（采集 flake，transcript 显示设备已正确响应）；复跑 70/0/0/2 | **基本一致**，1 例 flake 需加固 |
| 3 | M7/M8/M9 用例见 `test_m7*.py`、`test_m8*.py`、`test_m9*.py`、`test_build_hw_platform.py` | `README.md:97-98` | 实际无 `test_m8*.py`、无 `test_build_hw_platform.py`（M8 在 `test_obs.py`；构建门禁为 `test_build_hw.py`） | **需修正文件名** |
| 4 | 1kHz tick 与真实时间对齐（±20%） | `README.md:117` | `docs/defect-report.md:912` 自述容差实为 ±9.1%，且 tick 与 `now_us` 同源于硬编码 40MHz ccount，属自证循环 | **口径需改**（改为"ccount 与 40MHz 假设自洽"，另立真实节拍指标） |
| 5 | 真机默认应开抢占 | `docs/architecture.md:128`、`docs/qemu-testing.md:93` | `kernel/sched/sched.c:638` `g_preempt_enabled = 0` 硬编码，无平台分叉；仅命令可手动开 | **未实现**（D-03 收尾项） |
| 6 | CPU1 tick 抢占默认关、未做真机稳定性验证 | `docs/smp.md:73/155/301`、`docs/hardware-bringup.md:157` | 与代码一致（`smp.c:63` 默认 0，命令可开） | **一致** |
| 7 | 仓库无二进制垃圾、构建产物不入库 | `.gitignore`、D-29 | 根目录干净、117 tracked files、`git status` 干净 | **一致** |

---

## 6. 修复优先级与路线图

### 阶段 A：立即（0–1 天，ROI 最高）

1. **P0-1** MCP 字符串 id UAF（`mcp.c`）：复制 id 或调整释放顺序；补字符串 id 的 MCP 用例。
2. **P0-2** `sched.c` syscall 恢复按 `frame_kind` 分派；并用 `preempt on` 场景回归（QEMU 若受模型缺陷限制，以真机为准）。
3. 修正 README 的测试文件名与数字口径（§5 #3/#4），避免后续误判。

### 阶段 B：一周内

4. **P0-3** SMP 真锁（或"只允许 CPU0"的强制约束 + 断言）；**P0-4** potfs 读侧临界区。
5. **P0-5** netmcp：peer 固定 + ack 上界 + epoch 校验；若短期不做鉴权，至少在 README 明确"UDP MCP 仅可信直连网络"。
6. **P1-1/P1-2** 消息窗口与 trace 环写者收敛；**P1-3/P1-4/P1-5** 就绪表溢出回滚、`pick_best` 加锁、IPI 竞态修复。
7. 测试稳定化：`test_msg_trace`/`test_mem_trace` 改用 `trace meta` 计数差分（真机侧已有实现可复用）；`m3/preempt_toggle` 增加重试/更稳的锚点；CI/发布门禁连续两轮全绿。

### 阶段 C：2–4 周（发布前收口）

8. P2 项专项验证：EPS1/EPS2 槽位定论、CPU1 窗口/栈实测、栈 guard、时基标定、初始化顺序。
9. D-03 真机默认开抢占（依赖 P0-2）；D-16/D-17/D-26/D-28/D-30 清理。
10. 补齐压力与对抗测试：SMP 跨核堆/消息/fs 压测、MCP 负路径（伪造 ack/epoch/seq、超长帧、坏 CRC）、48h soak。
11. 引入 CI（至少"构建 + QEMU 两轮全绿"门禁），消除人工日志假绿。

---

## 7. 复现命令与证据索引

```bash
# 环境（一次性）
py -3.14 tools/check_env.py

# QEMU：干净构建 + 全量回归
cmd /c "cd /d D:\tudouOS && py -3.14 tools/build.py --clean"
cmd /c "cd /d D:\tudouOS && py -3.14 -m test.runner"

# 真机：干净构建 + 烧录 + 全量回归
cmd /c "cd /d D:\tudouOS && py -3.14 tools/build.py --platform hw --clean"
py -3.14 -m esptool --chip esp32s3 --port COM3 --baud 921600 write-flash 0x0 build/potato_boot_hw.bin
py -3.14 -m test.hw_runner --port COM3 --keep-going
```

审计证据（均为 gitignored 构建/日志目录，未入库）：

- `build/_audit_qemu_build.log`：干净构建日志（34/34，含告警）。
- `build/_audit_qemu_runner.log`：QEMU 主轮（65/67）。
- `build/_audit_qemu_rerun1.log`、`build/_audit_qemu_rerun2.log`：复跑（67/67、67/67）。
- `build/_audit_hw_build.log`、`build/_audit_hw_flash.log`：真机构建与烧录（Hash verified）。
- `build/_audit_hw_runner.log`：真机首轮（69 PASS + 1 TIMEOUT + 2 SKIP）。
- `build/_audit_hw_rerun.log`：真机复跑（70 PASS / 0 FAIL / 0 TIMEOUT / 2 SKIP）。
- `test/logs/`、`test/logs/hw/`：用例级串口与 QMP 日志。

---

## 8. 审计过程说明（透明记录）

- 审计初期工作目录被 Windows 文件权限阻挡（workspace 根目录缺少 WRITE_OWNER），按内置流程修复并验证（`D:\公司软件\tudouOS-acl-recovery\` 保留备份与回滚脚本）。
- 审计中途清理临时 worktree 时，误删了本机 `third_party/`（工具链 + QEMU，均为 gitignored 本地依赖，不影响源码树与 git 状态）；随后按 `tools/envlib.py` 固定的版本从乐鑫镜像（`dl.espressif.cn/github_assets/...`）重新下载并恢复，版本指纹与删除前一致；本报告所有构建/测试结果均为恢复后重新跑出的。
- 该事件提醒：**不要对包含 junction 的 worktree 直接做递归删除**；建议仓库 README 增加一条"清理 worktree 前先删除 `third_party` 联接"的说明。

---

## 9. 声明

- 本报告基于 2026-10-19 的代码与运行环境，不构成对未审计代码（含未来提交）的保证。
- 缺陷行号以审计基线 `8aa1fa5` 为准，后续修改后请以最新代码复核。
- 报告不改变仓库任何被跟踪文件；如需，可基于本报告的 P0/P1 清单提交修复补丁系列（每条带 QEMU/真机回归用例）。
