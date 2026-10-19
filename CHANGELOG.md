# Changelog

本文件遵循 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)
与语义化版本。技术预览版可能包含不兼容调整。

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
