# tudouOS 土豆OS

面向 **ESP32-S3** 的自研嵌入式操作系统：自研土豆内核（C + Xtensa 汇编），
研发检验全程跑在 QEMU（乐鑫官方分支，`-machine esp32s3`），由 Python
QMP 测试台自动验收；`--platform hw` 产出全 RAM 单镜像，已用 esptool
在 ESP32-S3 开发板上带板验证（启动、命令面、M9 双核全流程）。

> 当前阶段：**M9 已完成**（M1~M9 全部交付）。最新增量：M7 远程 MCP 通道
> （UDP 信封：纪元/序号/回执/去重/重同步；以太网仅 QEMU open_eth——ESP32-S3
> 硅片没有内部以太网 MAC，真机远程 MCP 走串口传输，见
> `docs/serial-mcp.md`）、M8 可观测体系（JSONL 导出/健康快照/注入/
> `tools/ai_analysis.py`，`docs/observability.md`）、M9 双核 SMP（CPU1
> 启动/核间中断/负载均衡，`docs/smp.md`）。
> QEMU 测试台 **69 用例全绿**（M1~M9 + 缺陷修复回归；发布门禁要求连续两轮）。
> 真机带板验证已完成（2026-10-17：RAM-only 布局稳定启动、CPU1 上线、
> IPI/绑核/负载均衡实测通过，`docs/hardware-bringup.md`）。
> **真机 M1–M9 全量回归（2026-10-19 审计修复后）**：串口自动回归台
> `test/hw_runner.py`（74 用例，`test/hw_cases/`）在普通 ESP32-S3 开发板上
> **72 PASS / 0 FAIL / 2 显式 SKIP**；真机远程 MCP 走串口帧传输
> （`tools/serial_mcp.py` + `docs/serial-mcp.md`）。报告见
> [test/logs/hw/summary.md](test/logs/hw/summary.md)。
> QEMU 构建走 flash XIP 执行；真机构建默认全 RAM（代码 IRAM、只读数据 DRAM）；
> tick 中断驱动（v5，独立于任务检查点）。产品定位与四大核心支柱（内核原生 MCP
> 消息总线 / 仿真硬件双端同源 / 内核可观测 / AI 交互低抖动调度）见
> [docs/architecture.md](docs/architecture.md)，MCP 消息基座设计见
> [docs/mcp-bus-design.md](docs/mcp-bus-design.md)。

## 目录结构

```
kernel/          土豆内核（boot.S 启动、UART、mini printf、命令循环、看门狗）
  linker/tudou.ld  链接脚本（全 RAM 运行，IRAM/DRAM 布局见注释）
tools/           check_env.py 环境体检 / build.py 一键构建
qemu/            run_qemu.py 交互/测试台/gdbstub 三种模式拉起 QEMU
test/            QMP 测试台（runner + qmp_client + serial_tap + cases/）
                 真机串口回归台（hw_runner + hw_serial + hw_cases/）
docs/            架构蓝图、路线图、QMP 测试台手册
build/           构建产物（kernel.elf / potato_boot.bin / flash_image.bin）
third_party/     本地工具链与 QEMU（不入库）
```

## 环境准备（一次性）

```bash
python tools/check_env.py      # 体检：交叉编译器 / esptool / QEMU
python -m pip install esptool  # 缺 esptool 时
```

三件套的获取（check_env 缺什么会打印对应下载地址）：

1. **交叉编译器** `xtensa-esp-elf-gcc`：乐鑫 crosstool-NG 发布包
   （Windows x64 zip），解压到 `third_party/toolchain/` 任意子目录。
2. **QEMU** `qemu-system-xtensa`：乐鑫 QEMU fork 的 Windows 包
   （qemu-xtensa-softmmu-*-x86_64-w64-mingw32.tar.xz），解压到
   `third_party/qemu/` 任意子目录。
3. **esptool**：`pip install esptool`。

也可用环境变量 `TUDOU_CC` / `TUDOU_QEMU` 直接指定可执行文件路径。
若 QEMU 包不可用，WSL2 里跑 QEMU、Windows 跑测试台亦可（全走 TCP）。

## 快速开始

```bash
python tools/build.py                # QEMU 镜像：编译内核 → ESP 镜像 → 4MB flash 镜像
python tools/build.py --platform hw  # 真机镜像：全 RAM 单镜像 potato_boot_hw.bin
python qemu/run_qemu.py              # 交互运行（串口直连终端，可敲 ping/info/echo）
python -m test.runner                # QMP 自动验收（69 个用例 + 日志落盘）
py -3.14 tools/release_gate.py       # 发布门禁：干净构建 + QEMU 连续两轮全绿
```

## 构建真机镜像 / 烧录（ESP32-S3 开发板）

真机默认走**全 RAM 布局**：代码进 IRAM、只读数据进 DRAM，ROM 直接搬两段 RAM
后跳入内核，不依赖 flash XIP/icache（带板验证见 `docs/hardware-bringup.md`）。

```bash
python tools/build.py --platform hw     # → build/kernel_hw.elf + build/potato_boot_hw.bin
esptool --chip esp32s3 --port COMx --baud 921600 \
    write-flash 0x0 build/potato_boot_hw.bin
```

- 只需烧 **一段**（`0x0`，S3 二级引导偏移），约 89KB；构建结束会打印地址与大小。
- 串口 115200-8N1；启动后应看到 `banner v0.7.0` 和 `[POTATO] READY`。
- 调试启动阶段可在构建时加 `TUDOU_BOOT_TRACE=1`（串口打阶段字符
  `S/T/U/W/C/D/V` 与 `0..E`）；改该变量后需 `--clean`。
- `--platform hw --xip` 保留旧的 flash XIP 两段烧录实验路径
  （真机 0x42 取指目前不可用，仅供继续排查）。
- QEMU 镜像（`python tools/build.py` 默认）仍为 XIP 布局，供测试台使用。
- **真机网络**：ESP32-S3 硅片没有内部以太网 MAC（「DesignWare GMAC + 外部
  PHY」方案不成立，事实核查见 `docs/ethernet.md` §8）；`net`/`ping` 的以太网
  通道仅在 QEMU 有效，真机远程 MCP 走**串口传输**——协议细节见
  [docs/serial-mcp.md](docs/serial-mcp.md)，主机侧可用
  `py -3.14 tools/serial_mcp.py --port COM3 selftest` 做握手 + `tools/list` 自检；
  真机以太网需外挂 SPI 模块（W5500/DM9051 类），尚未实现/到货。

## 测试用例（QEMU 69 个全绿 + 真机 74 条串口回归）

下表为 M1~M6 核心用例（QEMU）；M7/M8/M9 用例见各自文档与
`test/cases/test_m7.py`、`test/cases/test_m7_nodev.py`、`test/cases/test_m9.py`、
`test/cases/test_obs.py`（M8）、`test/cases/test_build_hw.py`。

**真机串口回归台**（`test/hw_runner.py` + `test/hw_cases/`，74 条，M1–M9 全覆盖）：

```powershell
py -3.14 -m test.hw_runner --port COM3 --list        # 列出用例
py -3.14 -m test.hw_runner --port COM3 --smoke       # 9 条冒烟
py -3.14 -m test.hw_runner --port COM3 --keep-going  # 全量回归
```

2026-10-19 审计修复后验收结果 **72 PASS / 0 FAIL / 2 显式 SKIP**（QMP 断线冻结、QEMU XIP
布局差异，均有替代覆盖），报告见 [test/logs/hw/summary.md](test/logs/hw/summary.md)。

| 用例 | 验证 |
| --- | --- |
| test_boot_banner | ROM→内核完整启动链路、版本号、内存布局、READY 标记 |
| test_ping_pong | 串口命令交互（ping→pong、echo、info） |
| test_qmp_reset | QMP 复位后完整重启、RESET 事件、复位后功能正常 |
| test_qmp_stop_cont | QMP 冻结 CPU 期间串口静默，恢复后补处理命令 |
| test_tick_alive | tick 计数与 ccount 频率假设自洽（真机另由 systimer 标定 `cpu_hz`，见 `tickstat`） |
| test_uart_rx_irq | UART RX 中断 + 环形缓冲连收 20 行不丢 |
| test_exception_dump | 非法指令异常现场 dump，QMP 复位后完整恢复 |
| test_trace_dump | 追踪环事件序号递增、时间戳单调 |
| test_irq_latency_smoke | tick 触发间隔有界冒烟（硬预算以真机为准） |
| test_sched_checkpoint_preempt | 后台忙烧被 UI 睡醒任务在检查点抢占（心跳交织证据） |
| test_three_class_stress | 三分类严格优先级 + 老化防饿死（AI 先完成、UI/后台保底推进） |
| test_bg_budget | 后台预算限速（1000tick 窗口 ≤700tick） |
| test_sleep_cadence | 任务睡眠节拍（宿主时钟容差断言） |
| test_sched_trace | 追踪环含 SCHED_SWITCH 事件（调度可回放） |
| test_msg_flood | 消息洪泛 12.5×容量：有界不 OOM、丢新计数准确、系统存活 |
| test_msg_qos1_ping | QoS1 收发回执：20 条全部送达且回执，零丢失零重发 |
| test_msg_retry_dedup | 延迟回执触发重发：去重窗口保证恰一次投递 |
| test_msg_timeout | 无接收方 QoS1：重发耗尽后失败上报（不无限重发） |
| test_msg_disconnect_heal | QMP 冻结模拟断线：恢复后无丢失无重复 |
| test_msg_trace | 追踪环含 MSG_ENQ/DEQ/ACK 事件（消息流水可回放） |
| test_mem_basic | 64 块渐增尺寸分配校验释放回归，回收后堆完整 |
| test_mem_coalesce | 挖洞碎裂后大块可得，全释放 largest 恢复=total |
| test_mem_canary | 越界写捕获计数，系统存活不崩溃 |
| test_mem_exhaust | 耗尽失败计数，全释放后大块复得 |
| test_mem_churn_soak | 随机尺寸持续压测零损坏零泄漏（alloc/free 平衡） |
| test_mem_trace | 追踪环含 MEM_ALLOC/FREE 事件（分配流水可回放） |
| test_xip_memory_map | XIP 布局：irom@0x42 flash 窗口，IRAM≤4KB |
| test_xip_boot_chain | ROM 仅搬 RAM 段（引导链路证明） |
| test_pal_info | PAL 平台/时基模式查询（双端接口） |
| test_driver_framework | 驱动链接期注册/启动初始化/运行枚举 |
| test_xip_full_function | XIP 下调度/消息/内存全子系统烟测 |
| test_mcp_ping | MCP ping 工具回执（pong+tick） |
| test_mcp_echo_sysinfo | echo 参数回显 + sysinfo 健康快照（版本/堆/文件数） |
| test_mcp_error_paths | 未知方法 -32601 / 畸形 JSON -32700 |
| test_mcp_fs_roundtrip | MCP 工具操作文件系统写读回环 |
| test_fs_shell | Shell 文件全流程（mkfs/write/cat/ls/rm） |
| test_mcp_agent_lifecycle | 代理 on/off/重复 on 幂等 + 重启恢复 |

**调度观测命令**：`tasks`（各任务状态/CPU 占用/切换数/诊断计数）、
`taskdemo preempt|stress|bg|off`（调度演示与压测）、`sleep <ms>`、
`flood <n>`（串口压测）。

**消息基座命令**（M3.5）：`msgdemo flood|ping|lazy|timeout|heal [n]`
（洪泛/回执/延迟回执/超时/断线自愈压测）、`msgstat`（通道健康快照：
水位/丢弃/重发/回执计数）。

**内存管理命令**（M4）：`memstat`（堆健康快照：总量/占用/最大块/
水位/分配释放失败捕获计数/碎片率）、`memtest selftest|holes|fault|
exhaust|churn|off`（分配回归/碎片合并/越界注入/耗尽恢复/soak 压测）。

**平台/驱动命令**（M5）：`pal`（平台名/时基模式/CPU 频率）、
`drivers`（已注册驱动与初始化状态）。

**文件系统/MCP 命令**（M6）：`fs ls|write <n> <t>|cat <n>|rm <n>|mkfs|df`
（potfs 操作）、`mcp on|off`（MCP 代理生命周期）；**以 `{` 开头的输入行
自动作为 JSON-RPC 请求**投递消息基座处理并返回 JSON 响应——外部 Agent
（如测试台/AI）经 UART 即可调用 6 个内核工具。

**网络命令**（M7）：`net [arp|link down|up|ip <a.b.c.d> [gw]]`、
`ping <a.b.c.d>`（ICMP echo）、`netmcp [dump|test|port <p>|peer <a.b.c.d> <port>|rw on|off]`
（UDP 远程 MCP 通道：纪元/序号/回执/去重/重同步）。注意以太网侧仅在 **QEMU
open_eth** 下有效（S3 真机无内部 EMAC）；真机远程 MCP 走串口传输，见
[docs/serial-mcp.md](docs/serial-mcp.md)，以太网细节与限制见
[docs/ethernet.md](docs/ethernet.md)。

**可观测命令**（M8）：`trace [n]|jsonl [n]|imp [n]|stream on|off|pump|meta|health`、
`health`（单行健康快照）、`obs panic`、`inject leak|hang|spin|off [arg]`
（注入锚点 + AI 分析器 `tools/ai_analysis.py`，详见
[docs/observability.md](docs/observability.md)）。

**双核命令**（M9）：`smp [stat|start|ipi <n>|spawn <cpu|any> <n>|work|stress on|off|stat|preempt on|off|selftest]`
（默认单核，`smp start` 拉起 CPU1；`smp stress` 是双核 heap/potfs/消息并发压测；
详见 [docs/smp.md](docs/smp.md)）。

## 安全与信任边界（2026-10-19 审计修复）

- **UDP 远程 MCP 只接受固定 peer**（`netmcp peer <ip> <port>`），源地址/端口
  不匹配的报文直接丢弃（`netmcp dump` 的 `reject` 计数可见）；`ack` 必须与
  当前纪元一致且不超过已发响应上界，任意主机无法伪造 ack 清空重放保护。
- **远程写工具默认关闭**：UDP 通道默认只读，`netmcp rw on` 才允许 `fs.write`；
  串口 MCP 是物理隔离的可信控制面，不受此限。UDP 通道仍应仅部署在可信直连
  网段——本版本未实现共享密钥/HMAC 鉴权。
- **跨核数据结构**（堆/potfs/消息环/追踪环/调度容器）使用 S32C1I 自旋锁；
  `smp stress` 回归用例在双核上并发跑 5 万+ 次 heap/fs/msg 操作，内容损坏
  计数必须为 0。
- **发布门禁**：QEMU 全量 **连续两轮** 69/69 全绿 + 真机全量 72 PASS/0 FAIL/
  2 SKIP；完整门禁证据（固件 SHA256、两轮 QEMU、真机原始日志与清单）见
  [docs/release-gate-2026-10-19.md](docs/release-gate-2026-10-19.md)。测试用例
  不再依赖"流量结束后立刻 dump"的时序（trace 断言改用 `trace meta` 累计计数
  差分）。审计清单见
  [docs/audit-report-2026-10-19.md](docs/audit-report-2026-10-19.md) 与
  [docs/defect-report.md](docs/defect-report.md) 的 2026-10-19 修复批次。

## 常见问题

- **项目路径含中文导致构建报 `can't create xxx.o`**：工具链的 binutils
  不支持非 ASCII 路径（本项目位于 `D:\公司软件\tudouOS`）。已建目录联接
  `D:\tudouOS` → 本项目，构建时走联接路径即可：
  `cmd /c "cd /d D:\tudouOS && python tools\build.py"`。
  （QEMU 运行与测试台不受影响，两条路径都可用。）
- **测试台/交互模式无任何输出**：多半是构建产物不存在或工具链缺失，
  先 `python tools/check_env.py`。
- **想看内核反汇编/调试**：`build/kernel.elf` 带符号，
  `python qemu/run_qemu.py --gdb` + `xtensa-esp32s3-elf-gdb`。
- **QMP 怎么单独玩**：交互模式已开 QMP 4444 端口，可自行发送 JSON，
  例如 `{"execute":"system_reset"}`（需先 `qmp_capabilities`）。
