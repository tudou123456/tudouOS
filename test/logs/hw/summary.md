# 真机（ESP32-S3 / COM3）M1–M9 全量回归报告

- 日期：2026-10-19（真机基线），执行人：hw-suite
- 最新固件：`build/potato_boot_hw.bin`（**85952 B**，全 RAM 布局，含 K1/K2 修复；由 mcp-serial 烧录，build 身份 `[POTATO] build Oct 19 2026 20:21:35`）
  - 上一版固件（首轮全绿基线）：85744 B，`[POTATO] build Oct 19 2026 19:43:33`；两版内核均 `tudouOS v0.7.0 (potato kernel) on ESP32-S3`
- 最新全量日志：`build/_hw_full3.log`（新固件）/ `build/_hw_full2.log`（旧固件）；两版结果均为 PASS=70 / FAIL=0 / TIMEOUT=0 / SKIP=2
- 串口：COM3 / 115200（CH340 USB-UART）；复位：DTR=IO0、RTS=EN（hidoff 脉冲）
- 回归台：`test/hw_runner.py`（用例在 `test/hw_cases/`，72 条，逐用例复位+等 READY）
- 最终命令：`py -3.14 -m test.hw_runner --port COM3 --keep-going`（日志 `build/_hw_full2.log`）
- 快速复跑：`py -3.14 -m test.hw_runner --port COM3 --smoke`（9 条，日志 `build/_hw_smoke_final.log`，9 PASS）

## 1. 结论

**最新固件（build `Oct 19 2026 20:21:35`，含 K1/K2 修复）：PASS=70 / FAIL=0 / TIMEOUT=0 / SKIP=2 / 未执行=0，退出码 0。**
（上一版固件 build `19:43:33` 同样 PASS=70 / FAIL=0 / TIMEOUT=0 / SKIP=2；两版用例集相同，SKIP 均为显式真机限制，理由见 §2。）

M1–M9 全部功能在真机跑通；2 条 SKIP 均已说明原因并给出替代覆盖证据，无未解释跳过。
K1（启动 tick 瞬态）与 K2（UART 输出行锁）已由 mcp-serial 在 kernel 侧修复，并在最新固件上真机验证通过（证据见 §3 与 §8）。

## 2. 按里程碑结果表

（以下为**最新固件 build 20:21:35** 的 `build/_hw_full3.log` 实测；旧固件 build 19:43:33 结果相同）

### M1 启动/控制台（3 条：PASS 3，SKIP 0）

| 用例 | 耗时(s) | 状态 |
|---|---:|---|
| `m1/boot_banner` | 0.42 | PASS |
| `m1/idle_stable` | 1.65 | PASS |
| `m1/info_help_echo` | 0.54 | PASS |

### M2 时间基/异常/追踪（12 条：PASS 12，SKIP 0）

| 用例 | 耗时(s) | 状态 |
|---|---:|---|
| `m2/exception_crash_recover` | 0.87 | PASS |
| `m2/irq_latency_smoke` | 1.43 | PASS |
| `m2/jit_diagnose` | 2.47 | PASS |
| `m2/looptest_rx_storm` | 2.23 | PASS |
| `m2/pal_tick_consistency` | 0.45 | PASS |
| `m2/rfe_variants` | 0.54 | PASS |
| `m2/tick_rate_1s` | 1.54 | PASS |
| `m2/tickstat_contract` | 0.43 | PASS |
| `m2/trace_dump` | 0.77 | PASS |
| `m2/trace_imp_ring` | 0.43 | PASS |
| `m2/trace_jsonl` | 0.72 | PASS |
| `m2/uart_rx_irq` | 0.53 | PASS |

### M3 调度/任务（7 条：PASS 7，SKIP 0）

| 用例 | 耗时(s) | 状态 |
|---|---:|---|
| `m3/preempt_toggle` | 0.46 | PASS |
| `m3/sched_trace` | 0.52 | PASS |
| `m3/sleep_cadence` | 0.66 | PASS |
| `m3/taskdemo_bg_budget` | 10.88 | PASS |
| `m3/taskdemo_preempt` | 10.85 | PASS |
| `m3/taskdemo_stress` | 63.61 | PASS |
| `m3/tasks_contract` | 0.48 | PASS |

### M3.5 消息队列（7 条：PASS 6，SKIP 1）

| 用例 | 耗时(s) | 状态 |
|---|---:|---|
| `m35/disconnect_heal` | 0.42 | SKIP |
| `m35/flood_backpressure` | 0.59 | PASS |
| `m35/lazy_retry_dedup` | 1.90 | PASS |
| `m35/msg_trace` | 0.53 | PASS |
| `m35/msgstat_contract` | 0.49 | PASS |
| `m35/ping_20` | 0.54 | PASS |
| `m35/timeout_deadletter` | 0.88 | PASS |

### M4 堆内存（9 条：PASS 9，SKIP 0）

| 用例 | 耗时(s) | 状态 |
|---|---:|---|
| `m4/canary_fault` | 0.44 | PASS |
| `m4/churn_balance` | 8.80 | PASS |
| `m4/dblfree` | 0.45 | PASS |
| `m4/exhaust` | 0.43 | PASS |
| `m4/holes_coalesce` | 0.45 | PASS |
| `m4/memstat_contract` | 0.43 | PASS |
| `m4/selftest_64` | 0.49 | PASS |
| `m4/szcorrupt` | 0.45 | PASS |
| `m4/trace_alloc_free` | 0.49 | PASS |

### M5 PAL/驱动/FS/MMU（8 条：PASS 7，SKIP 1）

| 用例 | 耗时(s) | 状态 |
|---|---:|---|
| `m5/boot_ram_only_layout` | 0.42 | PASS |
| `m5/cross_module_chain` | 0.61 | PASS |
| `m5/driver_framework` | 0.44 | PASS |
| `m5/fs_workflow` | 0.49 | PASS |
| `m5/mmutest_dump` | 0.43 | PASS |
| `m5/mmutest_load` | 0.45 | PASS |
| `m5/pal_hw` | 0.45 | PASS |
| `m5/xip_not_applicable` | 0.42 | SKIP |

### M6 UART JSON-RPC（6 条：PASS 6，SKIP 0）

| 用例 | 耗时(s) | 状态 |
|---|---:|---|
| `m6/fs_roundtrip` | 0.53 | PASS |
| `m6/initialize_tools` | 1.66 | PASS |
| `m6/lifecycle_multi` | 2.42 | PASS |
| `m6/mcp_on_ping` | 0.50 | PASS |
| `m6/rpc_errors` | 0.49 | PASS |
| `m6/unicode_line_limits` | 0.60 | PASS |

### M7 串口 MCP/网络降级（7 条：PASS 7，SKIP 0）

| 用例 | 耗时(s) | 状态 |
|---|---:|---|
| `m7/net_absent_graceful` | 0.52 | PASS |
| `m7/raw_frame_rpc` | 0.61 | PASS |
| `m7/serial_transport_console` | 0.47 | PASS |
| `m7/tool_replay_dedup` | 3.93 | PASS |
| `m7/tool_reset_recovery` | 2.77 | PASS |
| `m7/tool_selftest` | 3.19 | PASS |
| `m7/tool_tools_list_and_call` | 6.50 | PASS |

### M8 可观测/注入（5 条：PASS 5，SKIP 0）

| 用例 | 耗时(s) | 状态 |
|---|---:|---|
| `m8/inject_hang_tick_gap` | 1.29 | PASS |
| `m8/inject_leak` | 5.56 | PASS |
| `m8/obs_contract` | 1.88 | PASS |
| `m8/panic_dump` | 1.22 | PASS |
| `m8/sched_no_drift` | 7.05 | PASS |

### M9 双核 SMP（8 条：PASS 8，SKIP 0）

| 用例 | 耗时(s) | 状态 |
|---|---:|---|
| `m9/balance` | 0.60 | PASS |
| `m9/ipi_path` | 0.88 | PASS |
| `m9/preempt_toggle` | 0.44 | PASS |
| `m9/reset_restores_single` | 0.95 | PASS |
| `m9/selftest_and_start` | 0.79 | PASS |
| `m9/stat_default_single_core` | 0.48 | PASS |
| `m9/stress_stability` | 1.63 | PASS |
| `m9/work_pinned` | 1.04 | PASS |

### 2 条 SKIP 的理由（真机限制，非未执行/非躲避）

1. **`m35/disconnect_heal`**：QEMU 原用例用 QMP `stop/cont` 冻结虚拟机模拟“链路断线后恢复”；真机无 QMP，RTS 复位会清空内核状态与 netmcp 重放槽，语义不等价（不能无损复现）。
   - 替代覆盖：`m7/raw_frame_rpc`（真实 `@@MCP` 帧同 seq 重放 → 设备缓存响应 replay、发 ack 后同 seq → resync，`netmcp dump` 断言 rx/dup/replay/resync 计数）+ `m35/lazy_retry_dedup`（retry/dedup 计数）+ `m7/tool_replay_dedup`（跨进程同 seq 重放）。
2. **`m5/xip_not_applicable`**：真机默认全 RAM 构建（`tools/build.py --platform hw` 无 `.irom_text`，只烧 `potato_boot_hw.bin`），QEMU 的 flash-XIP 布局断言（IROM 0x42xxxxxx、无 load:0x42 等）对真机不适用。
   - 替代覆盖：`m5/boot_ram_only_layout`（真机 RAM-only 判据：ROM load 段 `0x3fca8xxx`/`0x4038xxxx`，无 `load:0x42`/`load:0x3c`；mem 行 iram 基址落在 0x40380000+0x1000 内、irom(flash)=0x0-0x0）。

## 3. 运行时发现的 kernel 侧问题/观察（附原始日志行）

> 均已报 mcp-serial（抄送 lead）；不阻塞回归结论（用例以 advisory 方式记录/已用 wait 收敛），但建议收口前评估。

### K1（kernel，minor，**已修复并真机验证 ✅**）启动期 tick 瞬态：READY 后 tickstat 仍 ticks=0，首次补账 79 ms 计入 jit_max

复现：`py -3.14 tools/hw_monitor.py --port COM3 --secs 3 --reset`，READY 后立刻发 `tickstat`。原始行：

```
[POTATO][TICK] ticks=0 cpu_hz=19000000 now_us=0xdf03 jit_min=4294967295us jit_max=0us uart_irq=10 rx_bytes=10
[POTATO][TICK] ticks=340 cpu_hz=19000000 now_us=0x532a1 jit_min=870us jit_max=79142us uart_irq=20 rx_bytes=20   (0.25s 后)
[POTATO][TS] now=513 switches=814 bg_used=0/700 diag wake=407 prem=0 sw=814 idle=1 drop ready=0 sleep=0 reg=0   (READY 后 tasks)
[POTATO][TRJ] {"seq":1,"t":3715041,"ev":"TICK_GAP","a":79142,"b":79,"imp":1}   (imp 环记录 79ms 缺口)
```

- 观察：空闲 3s 后 `jit_max` 仍为 **79142us**（不再增长；`jit_min=870us`），即 79 ms 缺口是**一次性启动窗口**（tick_init 早于 tick_irq_arm/后续初始化，首次 IRQ 在 READY 后补账），不是稳态中断延迟。
- 影响：M2 的 jit 统计与 M3 的 `tasks now` 在 READY 后约 0.1–0.2s 内“落后”；复跑首帧可能读到 `jit_min=4294967295`（UINT32_MAX 哨兵，尚无样本）。
- harness 处置：`test/hw_util.wait_tick_settle()` 在断言 tick 前轮询收敛（并记录瞬态），未放宽稳态判据；M2/M3 判据与 QEMU 一致（1kHz 速率/now 推进）。
- 建议（已采纳）：`tick_irq_arm` 后重置 `g_last_fire_cc/g_next_cc` 并跳过首个跨启动窗口样本；新固件实测 4 次连测与 3s 空闲均为 `jit_min≈974–999us / jit_max≈1000–1025us`，启动窗口不再计入，boot 期 TICK_GAP 不再出现（证据见 §8；jit_min 哨兵格式按兼容性保留现状）。

### K2（kernel，low，**已修复并真机验证 ✅**）控制台输出与 obs stream 共享 UART 无输出互斥 → 行可被前缀/交错

复现：`trace stream on`，0.3s 后发 `trace meta`；开启流时观察到的原始行示例：

```
potato> [POTATO][TRJ] {"seq":18251,"t":92252317,"ev":"IRQ_ENTER","a":1,"b":6}
```

- 观察：提示符/控制台输出与流式 TRJ 共享 `uart_putc` 无行锁，行首被拼接（本例 streaming 期间 TRM JSON 仍完整，但机制上 JSON 行可能被打断）。对 `@@MCP` 帧：netmcp 发送前检查 `uart_tx_midline()` 补 CRLF，本轮 M7 全部用例（4 个工具子进程 + 帧重放）帧解析 0 失败；但若同时开 stream 且 MCP 高频请求，存在被 console 输出打断的风险。
- 建议（已采纳）：新增 `uart_line_begin()`（中线未收行先补 CRLF），obs 流式 TRJ/health 打印前调用；新固件 `trace stream on` 并发期间 `potato> [POTATO]` 粘连计数 **0**、67 条 TRJ 行行首全部干净、TRM JSON 可完整解析（证据见 §8）。

### K3（设计观察，非 bug）tick IRQ 洪水使 256 项 trace 环仅覆盖约 128ms；`trace [n]` 读活环非原子

原始证据：

```
[POTATO][TR] total=20318 dropped=20062 last 200:
[POTATO][TRM] {"total":55107,"dropped":54851,"cap":256,"stream":0,"imp":{"total":2,"cap":64,"dropped":0},"counts":[{"ev":"IRQ_ENTER","n":14616},{"ev":"IRQ_EXIT","n":14616}]}
```

- 观察：1kHz tick 下 IRQ_ENTER/IRQ_EXIT ≈ 2000 事件/s，256 项主环 ≈128ms 覆盖；`trace n` 打印期间环仍在滚动，大 n 的语义“最近 n 条”仍成立但可能跨多圈。imp 环（64 项，仅重要事件）稳定：`TICK_GAP/NET_LINK` 可长期回看。
- harness 处置：事件种类断言（MEM_ALLOC/MEM_FREE、MSG_ENQ/DEQ/ACK、SCHED_SWITCH）改用 `trace meta` 的**计数增量**（等价语义，真机可复现）；`trace` dump/JSONL 的格式与 seq 单调性仍按 QEMU 判据直接断言（M2 通过）。QEMU tick=poll 无 IRQ 洪水，原 dump 判据保持。

## 4. 与 QEMU 基线的对照/差异（真机限制/预期差异，全部已在用例中显式说明）

| 项目 | QEMU 基线（test/cases） | 真机实测 | 处置 |
|---|---|---|---|
| QEMU 回归 | 67/67 通过（`build/_qemu_after_move.log`，hw 用例已移至 `test/hw_cases/`，未改 `test/runner.py`） | — | 真机台架与 QEMU 台架并存、互不污染 |
| 平台/时基 | `platform=qemu-esp32s3 tick=poll cpu_hz=40000000` | `platform=hw-esp32s3 tick=irq cpu_hz=19000000`（实测 1kHz 速率，3.87s≈3868 ticks） | m2/pal_tick 按真机值断言，且与 tickstat 交叉一致 |
| 启动内存布局 | flash XIP（iram 0x40380000 起、irom 0x42000000 区） | 全 RAM：iram=0x40380460-0x40391088，irom=0x0-0x0，dram=0x3fca8000 起 | m5 按 RAM-only 判据；XIP 断言 SKIP（§2） |
| tick 抖动 | 轮询制、`jit_max<120000us` 仅作冒烟上界 | 1kHz 中断驱动；稳态无 >1ms 缺口，启动一次性 79ms（K1） | m2 稳态判据 + jit_diagnose 记录证据，未伪造 |
| M3 taskdemo bg CPU 占比 | `taskdemo bg` <0.9s 虚拟时间内 idle 占比<=0.75 | demo 实际 10.3s（burn 固定次数，真硅慢），占比 0.94 | 阈值不可比：改为“bg 只跑一个任务数、yield 让 idle 有 CPU、drop 计数=0”不变量 + 记录占比；差异入报告 |
| M9 IPI 计数归属 | 断言 `cpu1.ipi_tx>=32` | 实测：发送方 `cpu0.ipi_tx +32`，接收方 `cpu1.ipi_rx +32`、resched+30 | 断言改为 (cpu0.tx+cpu1.tx) 增量>=32 且 cpu1.rx 增量∈[1,发送数]、resched≥1、unk=0（语义等价） |
| trace 事件种类 | `trace n` 窗口内直接找 MEM/MSG/SCHED 事件 | IRQ 洪水使窗口仅≈128ms（K3） | 改用 `trace meta` counts 增量；格式/seq 仍直接断言 |
| 断线恢复 | QMP stop/cont 冻结 | 无 QMP（真机 RTS 复位清状态） | SKIP，替代覆盖见 §2 |
| 控制台行尾 | `\r\n` | 内核 printf 行尾实为 `\r\r\n`，命令回显行尾裸 `\n`，提示符会与回显粘连 | `hw_serial` 断行/归一化/剥提示符；锚点统一带行尾 `\r` |
| JSON-RPC 行长度 | 258B→-32600 / 358B→line too long | 同左（逐字节一致） | m6/unicode_line_limits 通过；修复了 mcp.c initialize 多一个 `}` 的既有 bug（build 19:43:33 已含） |

## 5. 首跑失败与修复记录（透明化：首跑 46/12/12/2 → 最终 70/0/0/2）

首跑日志 `build/_hw_full.log`。24 条非 PASS 全部定位为 **harness 自身 bug（23）** 与 **真机时序/性能差异（1，m3/bg 阈值）**，无 fake-pass：

| 类别 | 用例 | 根因 | 修复 |
|---|---|---|---|
| 异步 trace 读到太早（4） | m2/trace_dump、m3/sched_trace、m35/msg_trace、m4/trace_alloc_free | 命令响应行先到，事件行随后才输出；用例立即读已收文本 | 事件改 `trace meta` counts 增量 + 轮询收敛 |
| no_crash 作用域过大（4） | m4/memstat_contract、m5/mmutest_dump、m7/net_absent_graceful、m7/serial_transport_console | `no_crash()` 扫全 session 文本，误把前面 m2/crash 用例的预期 `[EXC]` 当异常 | 用例级文本作用域（`ctx.start_pos` 起的 case 切片） |
| 命令输出窗口（3） | m35/msgstat_contract、m8/obs_contract、m2/looptest_rx_storm | msgstat 需先建通道；obs_contract 漏发 `trace meta`；looptest 输出在 poll 起点之前已到 | 先 `msgdemo ping N` 建通道再 msgstat；补发 meta；poll 起点提前到 send 之前 |
| 正则/格式（4） | m9/ 全部 WORK 行（7 条超时）、m7/raw_frame_rpc、m6/fs_roundtrip、m6/unicode_line_limits | WORK 行尾有 `diag=...` 后缀；checks 格式串参数少一个；fs.read 响应 name,size,data 顺序；unicode 用例的请求构造漏 `}`（非法 JSON） | 正则允许行尾后缀；修正格式串；单行断言 size+data；按长度精确构造合法 JSON（238/258/358B） |
| API/句柄（1） | m7/tool_reset_recovery | `CaseContext.released()` 未接受 `reset_after` 参数 | 扩展为 `released(reset_after=True)` 并透传 |
| 真机性能差异（1） | m3/taskdemo_bg_budget | QEMU 0.75 占比阈值基于虚拟时间 | 改为真机等价不变量（见 §4），记录 0.94 实测 |
| 分析器作用域/交错（2） | m8/panic_dump、m8/obs_contract | 分析器吃全 session 文本含其他用例的预期异常；stream on 时行可能交错 | 分析器改用 case 切片；panic 用例改为由 panic dump 自带 imp/jsonl 导出（不再依赖 stream 行内交错） |
| 解析/瞬态（2） | m2/tickstat_contract、m3/tasks_contract | tickstat 的 now_us 为十六进制被当十进制；READY 后 tick/调度计数瞬态未收敛 | 十六进制解析；`wait_tick_settle()`/有界轮询 |
| 基线断言（1） | m5/boot_ram_only_layout | 真机 iram 基址是 `0x40380460` 而非链接脚本 0x40380000 | 断言基址落在 0x40380000+0x1000 内（RAM-only 段头一致性） |

## 6. 产物与复现命令

- 用例逐条日志：`test/logs/hw/<case>.log`（含固件 build、状态、耗时、checks、原始串口转录）；会话日志 `test/logs/hw/_session.log`；结果 `test/logs/hw/results.json`
- 最终全量日志：`build/_hw_full2.log`；首跑对照：`build/_hw_full.log`；smoke：`build/_hw_smoke_final.log`
- 串口 MCP 工具验收：`build/_serial_mcp_selftest_after_hw.log`（selftest PASS，rx_frames=4 bad_frames=0，transport=serial-json known=1 epoch=1）
- QEMU 基线：`build/_qemu_after_move.log`（67/67，hw 用例已移出 `test/cases/`）

```powershell
cd 'D:\tudouOS'
py -3.14 -m test.hw_runner --port COM3 --list        # 72 条用例（M1–M9）
py -3.14 -m test.hw_runner --port COM3 --dry-run     # 用例导入/签名校验（无需板）
py -3.14 -m test.hw_selftest                        # 21 项串口驱动 mock 自测（无需板）
py -3.14 -m test.hw_runner --port COM3 --smoke      # 9 条冒烟
py -3.14 -m test.hw_runner --port COM3 --keep-going # 全量回归
py -3.14 -m test.hw_runner --port COM3 --filter m9/ # 单里程碑
py -3.14 -m test.runner                             # QEMU 全量（67 条）
py -3.14 tools/serial_mcp.py --port COM3 selftest   # 串口 MCP 工具
```

## 7. 状态

- 真机全量回归通过（最新固件 build 20:21:35：70 PASS / 0 FAIL / 2 显式 SKIP；旧固件 build 19:43:33 同结果）；K1/K2 修复已真机验证（§8），COM3 已释放，后续复跑按 §6 命令即可。
- 遗留（不阻塞本次验收）：K3 属设计观察（trace 环覆盖≈128ms，可写入文档）；m5 XIP、m35 QMP-heal 为真机限制项。K1/K2 已修复；行尾契约（`\r\r\n`/回显裸 `\n`）维持不变。

## 8. 新固件（K1/K2 修复）复跑记录（最新，build Oct 19 2026 20:21:35）

- 固件：`build/potato_boot_hw.bin` **85952 B**；启动 banner `[POTATO] build Oct 19 2026 20:21:35`；烧录 esptool write-flash 0x0 且 `Hash of data verified`（日志 `build/_serial_mcp_flash_k12.log`）。
- 全量复跑：`py -3.14 -m test.hw_runner --port COM3 --keep-going` → **PASS=70 / FAIL=0 / TIMEOUT=0 / SKIP=2 / 未执行=0，退出码 0**（日志 `build/_hw_full3.log`）。
- 冒烟复跑：`--smoke` → **PASS=9**（日志 `build/_hw_smoke3.log`）。
- 串口 MCP 复验：`py -3.14 tools/serial_mcp.py --port COM3 --reset selftest` → **PASS**（`rx_frames=4 bad_frames=0`，`transport=serial-json known=1 epoch=1`，`crc_bad=0 too_long=0 bad_prefix=0`；日志 `build/_serial_mcp_selftest_k12_after_hw.log`）。
- QEMU 回归（K1/K2 代码改动后，mcp-serial 执行）：**67/67 通过**（日志 `build/_qemu_regress.log`）。

### K1 验证（启动 tick 瞬态修复）：原始行

```text
[K1] READY 后立即连续 4 次 tickstat（复位后首拍）：
[POTATO][TICK] ticks=52 cpu_hz=19000000 now_us=0xdfa0 jit_min=999us jit_max=1000us uart_irq=10 rx_bytes=10
[POTATO][TICK] ticks=65 cpu_hz=19000000 now_us=0x11118 jit_min=974us jit_max=1025us uart_irq=20 rx_bytes=20
[POTATO][TICK] ticks=88 cpu_hz=19000000 now_us=0x16cd4 jit_min=974us jit_max=1025us uart_irq=30 rx_bytes=30
[POTATO][TICK] ticks=102 cpu_hz=19000000 now_us=0x1a3b7 jit_min=974us jit_max=1025us uart_irq=40 rx_bytes=40
[K1] tasks: [POTATO][TS] now=133 switches=14 ...
[K1] idle 3s: [POTATO][TICK] ticks=3488 ... jit_min=974us jit_max=1025us ...
[K1] imp/TICK_GAP: [POTATO][TRJ] exported_imp=1 total_imp=1   (本轮无 TICK_GAP；旧固件 boot 期曾记录 a=79142us)
```

判定：启动 ~79ms 窗口不再进入 jit 统计（jit_min/jit_max 均为毫秒级且 3s 空闲不变）；`tasks now` 不再有 ~0.1–0.2s 滞后；`trace imp` 中 boot 期 TICK_GAP 消失（观测保留机制未变）。

### K2 验证（输出行锁修复）：原始行

```text
[K2] trace stream on 并发期间行首/JSON 完整性：
  glued(potato>+[POTATO]) count = 0
  TRJ lines = 67  (line-start 异常 = 0 )
  sample TRJ: [POTATO][TRJ] {"seq":15796,"t":78719000,"ev":"IRQ_EXIT","a":1,"b":6}
  TRM sample: [POTATO][TRM] {"total":21190,"dropped":20934,"cap":256,"stream":1,"imp":{"total":1,"cap":64,"dropped":0},"counts":[...]}
  TRM JSON parse = OK
[K2] stream off: [POTATO][OBS] stream off pumped=68 lost=7154 calls=17
```

判定：`trace stream on` 期间 TRJ/TRM 行首不再被 `potato> ` 粘连（0 处），行级 JSON 完整可解析；与旧固件修复前观测（`potato> [POTATO][TRJ] {...}` 粘连）对照明确。

### 结论

K1/K2 修复在真机生效且无回归：72 条用例仍 70 PASS / 0 FAIL / 2 显式 SKIP；串口 MCP 7 条用例与工具 selftest 均通过；行尾契约未变更，既有锚点不受影响。

## 9. Lead 最终验收复跑（2026-10-19 20:3x，干净重建）

- 构建：`py -3.14 tools/build.py --platform hw --clean` → `build/potato_boot_hw.bin`
  85952 B，启动身份 `[POTATO] build Oct 19 2026 20:29:27`
  （`build/_final_hw_build2.log`）；`esptool write-flash 0x0` →
  `Hash of data verified`（`build/_final_hw_flash2.log`）。
- 串口 MCP：`py -3.14 tools/serial_mcp.py --port COM3 --reset selftest` PASS
  （hello / tools/list 6 工具 / ping / sysinfo / dump；`transport=serial-json
  known=1 link=1 epoch=1`，`rx_frames=4 bad_frames=0`；`build/_final_hw_selftest2.log`）。
- 全量回归：`py -3.14 -m test.hw_runner --port COM3 --keep-going` →
  **PASS=70 / FAIL=0 / TIMEOUT=0 / SKIP=2 / 未执行 0，exit 0**
  （`build/_final_hw_runner4.log`；2 条 SKIP 同 §2 平台手段差异）。
- QEMU 对照：同一源码 `py -3.14 tools/build.py && py -3.14 -m test.runner` →
  **67/67，exit 0**（`build/_final_qemu_runner3.log`）。
- 结论：工作树源码干净重建即得上述结果，可进入提交收口。
