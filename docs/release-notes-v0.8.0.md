# tudouOS v0.8.0 — Technical Preview 发布说明

- 发布日期：2026-10-19
- 版本：`0.8.0`（技术预览；不建议直接用于生产）
- 基线：`main` 分支提交 `53d3038`（第 8 批：独立审计收口）
- 证据：[发布门禁报告](release-gate-2026-10-19.md) ·
  [独立审计报告](audit-report-2026-10-19.md) ·
  [缺陷报告 §12](defect-report.md#12-2026-10-19-独立审计修复批次第-8-批)

---

## 这是什么版本

v0.8.0 是土豆OS的首个公开技术预览。它不是一个"玩具 demo"：从 ROM 引导、
中断/异常、三分类调度、堆、消息基座、potfs、MCP 协议面、以太网（QEMU）、
可观测到双核 SMP 全部可运行，并有 QEMU 与真机双回归台。但它仍然是预览版：

- 内核 API/命令/协议可能在不兼容的方式下调整；
- 长稳、对抗网络、SMP 缓存/内存序尚未完成专项验证；
- 生产使用请等待 v1.0 或自行做完整评估。

## 亮点

### 1. 内核原生 MCP 控制面

- `initialize` / `tools/list` / `tools/call`，id 数字/字符串/notification 三态。
- 工具：`ping`、`echo`、`sysinfo`、`fs.list`、`fs.read`、`fs.write`。
- UART 行传输（`{` 开头自动路由）+ 真机串口帧
  `@@MCP <crc16> <json>`（带 CRC16/CCITT-FALSE）。
- UDP 远程通道（QEMU open_eth）：纪元/序号/回执/去重/重同步。

### 2. 双端同源验证

- QEMU：每个用例独立 VM、QMP 控制、动态端口、串口全量落盘。
- 真机：per-case 复位、串口协议、74 条回归；本次 72 PASS / 0 FAIL /
  2 显式 SKIP（QMP 冻结与 QEMU XIP 布局，均有替代覆盖）。

### 3. 可观测与可回放

- 256 项追踪环 + 64 项重要事件副环；JSONL 导出、`trace meta` 计数、
  健康快照、注入（leak/hang/spin）、规则分析器 `tools/ai_analysis.py`。

### 4. 调度与 SMP

- AI/UI/后台三分类、5 tick 时间片、200ms 老化、700/1000 tick 后台预算。
- CPU1 启动、IPI、绑核/迁移；heap/potfs/msg/trace 使用 S32C1I 跨核锁。
- 真机 CPU0 tick 抢占默认开启；QEMU 因模型缺陷默认关。

## 本次审计修复（摘要）

独立审计发现的 3 个 P0（MCP 字符串 id UAF、syscall 恢复不判 frame_kind、
SMP 假锁）与 potfs 读侧 UAF、UDP 无鉴权等全部修复；另有 P1/P2/P3 共
二十余项并发/协议/卫生问题收口。完整映射见
[defect-report.md §12](defect-report.md#12-2026-10-19-独立审计修复批次第-8-批)。

## 验证

| 项 | 结果 |
| --- | --- |
| QEMU 全量（69 例，连续两轮） | **69/69、69/69** |
| 真机全量（74 例，COM3） | **72 PASS / 0 FAIL / 0 TIMEOUT / 2 SKIP** |
| 真机固件 | 91,376 B，SHA256 `47d4af9e5d59e41ae404ed3a04bb6764cd39c12574b3df70b3064c4dd19f8a1c` |
| 烧录校验 | `Hash of data verified` |
| 工具链 | xtensa-esp-elf-gcc 13.2.0 / QEMU esp_develop_9.2.2 / esptool 5.4.0 |

复现：`py -3.14 tools/release_gate.py --rounds 2 --hw-port COM3`。

## 升级/兼容注意

- **UDP peer 固定**：不再学习任意源；先 `netmcp peer <ip> <port>`。
- **远程写默认关闭**：UDP 的 `fs.write` 需 `netmcp rw on`；串口不受限。
- **任务 PS/启动顺序**：真机 tick 抢占默认开；旧硬件异常可用
  `preempt off` 对照。
- **tasks 输出**：摘要行末尾新增 `guard=`，按行尾全匹配的脚本需放宽。
- **消息 id**：QoS1/2 的发送方应保证 ack 前 id 唯一；回执按入队 seq 配对。

## 已知限制

- 48h soak、10⁷ 次上下文切换、SMP 长稳/缓存内存序未跑。
- UDP MCP 无 HMAC/共享密钥，只应部署在可信直连网段。
- CPU1 tick 抢占默认关。
- ESP32-S3 无内部 EMAC；真机以太网需外挂 SPI 模块。
- 固定工具链/QEMU 当前为 Windows x64；Linux/macOS 需自行提供。

## 如何参与

- 复现/测试：按 README 跑 QEMU 与真机回归，提交 issue 时附版本与日志。
- 代码/文档：见 [CONTRIBUTING.md](../CONTRIBUTING.md)。
- 安全：见 [SECURITY.md](../SECURITY.md)，不要开公开漏洞 issue。
- 路线：见 [ROADMAP.md](ROADMAP.md)。

感谢所有参与审计、带板验证与文档校对的人。技术预览的下一步取决于
社区反馈——欢迎在 Discussions 里告诉我们你的板卡、场景与需求。
