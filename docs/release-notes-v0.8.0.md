# tudouOS v0.8.0 — Technical Preview 发布说明

- 发布日期：2026-10-19
- 版本：`0.8.0`（技术预览；不建议直接用于生产）
- 许可证：MIT

---

## 这是什么版本

v0.8.0 是土豆OS的首个公开技术预览。它覆盖从 ROM 引导、中断/异常、
三分类调度、堆、消息基座、potfs、MCP 协议面、以太网（QEMU）、可观测到
双核 SMP 的完整链路，并提供 QEMU 与真机两套回归台。但它仍然是预览版：

- 内核 API、命令与协议可能在不兼容的方式下调整；
- 长稳、对抗网络、SMP 缓存/内存序尚未完成专项验证；
- 生产使用请等待 v1.0 或自行做完整评估。

## 亮点

### 1. 内核原生 MCP 控制面

- `initialize` / `tools/list` / `tools/call`，id 数字/字符串/notification 三态。
- 工具：`ping`、`echo`、`sysinfo`、`fs.list`、`fs.read`、`fs.write`。
- UART 行传输（`{` 开头自动路由）+ 真机串口帧
  `@@MCP <crc16> <json>`（CRC16/CCITT-FALSE）。
- UDP 远程通道（QEMU open_eth）：纪元/序号/回执/去重/重同步。

### 2. 双端同源验证

- QEMU：每个用例独立 VM、QMP 控制、动态端口、串口日志落盘。
- 真机：per-case 复位、串口协议、硬件回归台。
- 同一份内核源码与断言，QEMU 与真机共享。

### 3. 可观测与可回放

- 追踪环 + 重要事件副环；JSONL 导出、`trace meta` 计数、健康快照、
  注入（leak/hang/spin）、规则分析器 `tools/ai_analysis.py`。

### 4. 调度与 SMP

- AI/UI/后台三分类、时间片、老化、后台预算。
- CPU1 启动、IPI、绑核/迁移；堆/文件/消息/追踪使用跨核原子锁。
- 真机 CPU0 tick 抢占默认开启；QEMU 因模型差异默认关。

## 快速开始

```powershell
py -3.14 -m pip install -r tools/requirements.txt
py -3.14 tools/fetch_deps.py
py -3.14 tools/build.py --clean
py -3.14 -m test.runner
```

真机：

```powershell
py -3.14 tools/build.py --platform hw
py -3.14 -m esptool --chip esp32s3 --port COM3 --baud 921600 `
    write-flash 0x0 build/potato_boot_hw.bin
py -3.14 -m test.hw_runner --port COM3 --keep-going
```

## 兼容与升级注意

- **UDP peer 固定**：先 `netmcp peer <ip> <port>`；不再学习任意源。
- **远程写默认关闭**：UDP 的 `fs.write` 需 `netmcp rw on`；串口不受限。
- **tasks 输出**：摘要行包含 `guard=` 字段；按行尾全匹配的脚本需放宽。
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

MIT licensed. 欢迎反馈板卡、场景与需求。
