# Changelog

本文件遵循 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)
与语义化版本。技术预览版可能包含不兼容调整。

## [Unreleased]

### Added

- **UDP 远程 MCP 鉴权**：32B 共享密钥的 HMAC-SHA256 信封认证
  （`netmcp key <64hex>`）、严格字段顺序解析；缺 mac/被篡改的报文在
  状态变更前丢弃。串口传输不强制（物理隔离入口）。
- **远程写配额**：UDP `fs.write` 默认 8 次/60s（`netmcp quota <n>`，
  0=不限制）；超限返回 `isError:true` + `write quota exceeded` 且不落盘。
  未配密钥时 `netmcp rw on` 被拒绝。
- **`tools/soak.py`**：单实例长稳框架（QEMU/真机），周期施加堆/消息/
  文件/调度负载，采样 `health`/`tasks`，判定复位、tick 停摆、堆泄漏、
  看门狗、栈哨兵；输出 JSONL + summary。

### Changed

- net 任务栈保持 640 words（2.5KB）：MCP `send_*` 改为流式输出 id/转义
  body 并加 `noinline`，降低 tools/call 栈占用；避免扩栈挤占 m4 门禁的
  64KB 堆。
- **UART TX 改为 2KB ring + 1kHz tick 泵送**：`potato_printf` 只入队，
  不再持 `rsil15` 忙等 TX FIFO；长控制台行（health/tasks 1KB+）不再
  屏蔽 tick。ring 满（flood/trace dump）才回退同步 drain。
- `netmcp` 响应重放槽 8→6（请求去重窗口仍 8），为 TX ring 腾 BSS；
  m4 64KB 堆门禁保持通过。

### Fixed

- 远程 `tools/call` 响应在 net 栈不足时无输出（表现为请求已执行但
  对端超时）；流式输出 + noinline 后响应稳定，栈维持 2.5KB。
- **真机时基标定偏快 5.266%**：400k ccount 短窗口把 systimer 锁存/读
  的约 1ms 固定开销算进 `dsys`，把实际 20MHz 标成 19MHz，导致
  tick/`now_us` 快 5.266%（≈20/19）。改为双窗口（每窗 1M ccount）
  斜率标定，抵消固定读开销；新增 `clockdiag` 诊断命令。修复后 10s
  墙钟 ratio 0.99975，`cpu_hz=20000000`。
- **控制台长输出导致 tick 丢失/wd**：`potato_printf` 持打印锁
  （`rsil 15`）期间 `uart_putc` 按 FIFO 忙等，115200 下 health 一行
  ~1.2KB 会屏蔽中断 100ms 级；soak 的 health/tasks 采样因此自扰动。
  TX ring 后 health 仍占 84ms 串口时间，但 `jitmax` 从 ~72ms 降到
  ~5.5ms。
- 异常/panic 收尾在 `halt_forever()` 前调用 `uart_tx_flush()`，同步
  发完 TX ring；否则 CPU 停机后 tick 泵不再运行，异常现场会留在
  ring 里（CI `test_exception_dump` 捕获）。

## [0.8.0] - 2026-10-19 · Technical Preview

首个公开技术预览：ESP32-S3 自研内核 + 内核原生 MCP + QEMU/真机双测试台。

### Added

- **内核**：ROM 引导、Xtensa 异常/中断、1kHz tick、三分类调度、任务
  sleep/yield/exit、TLSF 类堆、RAM potfs、有界消息通道（QoS0/1/2）、
  追踪环/健康快照、故障注入、双核 SMP。
- **MCP**：`initialize` / `tools/list` / `tools/call`，数字/字符串 id，
  UART 行传输与 CRC16 串口帧传输；工具 `ping`、`echo`、`sysinfo`、
  `fs.list`、`fs.read`、`fs.write`。
- **网络**：QEMU open_eth 上的 ARP/IPv4/ICMP/UDP 与远程 MCP 通道
  （纪元/序号/回执/去重/重同步）。
- **工具**：`tools/build.py`、`check_env.py`、`fetch_deps.py`（固定
  URL+SHA256）、`release_gate.py`、`serial_mcp.py`、`ai_analysis.py`。
- **测试台**：QMP 驱动的 QEMU 全量回归（每用例独立 VM）与真机串口回归台。
- **工程**：MIT 许可证、英文 README、贡献/安全/行为准则、Issue/PR 模板、
  CI（静态检查 + Windows QEMU 门禁）、自托管真机门禁示例。

### Changed

- 版本号 0.8.0；模块、文档与示例统一为当前接口。

### Known limitations

- 技术预览：API/命令/协议仍可能调整；不建议直接用于生产。
- 长稳（48h soak、10⁷ 次上下文切换）与网络对抗测试尚未完成。
- UDP MCP 无 HMAC/共享密钥，仅限可信直连网段；远程写默认关闭。
- CPU1 tick 抢占默认关。
- ESP32-S3 无内部以太网 MAC；真机以太网需外挂 SPI 模块（未实现）。
- 固定工具链/QEMU 当前为 Windows x64；Linux/macOS 需自行提供。

### License

MIT。内核为自研代码；工具链/QEMU 为独立下载的第三方产物，遵循各自许可证。
