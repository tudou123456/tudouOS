# tudouOS 土豆OS

[![Release](https://img.shields.io/github/v/release/tudou123456/tudouOS?include_prereleases&label=release)](https://github.com/tudou123456/tudouOS/releases)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

**简体中文** | [English](README.en.md)

> **v0.8.0 Technical Preview（技术预览）**：内核与真机链路已跑通，但尚未达到
> 生产级稳定性；API、命令与协议可能随版本调整。升级注意与已知限制见
> [发布说明](docs/release-notes-v0.8.0.md) 和 [路线图](docs/ROADMAP.md)。

tudouOS 是面向 **ESP32-S3** 的自研嵌入式操作系统。内核从零实现（C + Xtensa
汇编，call0 ABI，不依赖 libc/libgcc），同一份源码可运行在 Espressif QEMU
分支和真实开发板上。它把 **MCP（Model Context Protocol）控制面做进内核**，
让 AI Agent、脚本和测试台可以通过串口或网络直接查询与驱动设备。

## 特性

- **内核原生 MCP**：消息基座 + JSON-RPC 工具（`ping`、`echo`、`sysinfo`、
  `fs.list/read/write`）；UART 行传输与带 CRC16 的串口帧传输；支持
  `initialize` / `tools/list` / `tools/call` 与数字/字符串 id。
- **双端同源**：QEMU 测试台（QMP 控制、每用例独立 VM）与真机串口回归台
  共用同一套内核与断言。
- **三分类调度**：AI / UI / 后台优先级域、时间片轮转、老化防饿死、
  后台预算；tick 中断驱动，可选 tick 抢占。
- **SMP**：CPU1 启动、核间中断、绑核与负载均衡；堆/文件/消息/追踪等
  共享结构使用跨核原子锁。
- **内存与存储**：TLSF 类堆（canary/隔离/合并）、RAM potfs（写时复制）、
  有界消息通道（QoS0/1/2、重发、去重、回执）。
- **可观测**：追踪环 + 重要事件副环、JSONL 导出、健康快照、故障注入、
  规则分析工具 `tools/ai_analysis.py`。
- **网络**：QEMU open_eth 上的 ARP/IPv4/ICMP/UDP 与远程 MCP 通道；
  真机远程控制走串口传输（ESP32-S3 无内部 EMAC）。

## 快速开始（Windows x64）

```powershell
# 1) Python 依赖（固定版本）
py -3.14 -m pip install -r tools/requirements.txt

# 2) 下载并校验固定版本工具链 + QEMU
py -3.14 tools/fetch_deps.py

# 3) 体检、构建、跑 QEMU 测试台
py -3.14 tools/check_env.py
py -3.14 tools/build.py --clean
py -3.14 -m test.runner
```

真机镜像（全 RAM 布局，不依赖 flash XIP）：

```powershell
py -3.14 tools/build.py --platform hw
py -3.14 -m esptool --chip esp32s3 --port COM3 --baud 921600 `
    write-flash 0x0 build/potato_boot_hw.bin
py -3.14 -m test.hw_runner --port COM3 --keep-going
```

> 项目路径需为 ASCII（Windows binutils 限制）。若检出在非 ASCII 路径下，
> 请建立 ASCII 目录联接并从联接路径构建（见下方常见问题）。

## MCP 示例

控制台中以 `{` 开头的行会自动进入 MCP 代理：

```json
{"jsonrpc":"2.0","id":"demo-1","method":"tools/call",
 "params":{"name":"sysinfo","arguments":{}}}
```

响应为单行 JSON，`id` 原样回显。真机串口帧格式与工具说明见
[docs/serial-mcp.md](docs/serial-mcp.md)。

## 目录结构

```
kernel/     内核：boot/intc/sched/time/mem/msg/fs/net/mcp/trace/smp
test/       QEMU QMP 测试台 + 真机串口回归台 + 用例
tools/      构建、环境体检、依赖下载、发布门禁、MCP 桥、日志分析
docs/       架构、MCP、SMP、串口协议、可观测、QEMU 测试台、路线图
qemu/       交互式 QEMU 启动器
```

## 文档

- [架构设计](docs/architecture.md)
- [MCP 消息基座](docs/mcp-bus-design.md)
- [串口 MCP 协议](docs/serial-mcp.md)
- [SMP 设计](docs/smp.md)
- [可观测体系](docs/observability.md)
- [以太网与网络栈](docs/ethernet.md)
- [QEMU 测试台](docs/qemu-testing.md)
- [v0.8.0 发布说明](docs/release-notes-v0.8.0.md) · [路线图](docs/ROADMAP.md)
- [维护与发布](docs/MAINTAINING.md)

## 参与

欢迎 bug 报告、真机兼容性结果、文档和代码贡献。开始前请读
[CONTRIBUTING.md](CONTRIBUTING.md) 与
[CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md)。安全漏洞请按
[SECURITY.md](SECURITY.md) 私密报告，不要开公开 issue。

## 常见问题

- **构建报 `can't create xxx.o`**：项目路径含非 ASCII 字符。Windows 上
  建立目录联接（如 `mklink /J D:\tudouOS "实际路径"`），从联接路径构建。
- **找不到工具链/QEMU**：运行 `py -3.14 tools/fetch_deps.py`；也可用
  `TUDOU_CC` / `TUDOU_QEMU` 指向本机安装。
- **串口权限/占用**：关闭其他串口工具；真机回归默认使用 115200-8N1。
- **想交互运行**：`py -3.14 qemu/run_qemu.py`，控制台可敲 `help`/`ping`/`info`。

## 许可证

[MIT](LICENSE)。内核为自研代码，不包含第三方 RTOS 源码；工具链与 QEMU
由 `tools/fetch_deps.py` 单独下载，遵循各自许可证。
