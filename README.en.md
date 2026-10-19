# tudouOS (土豆OS)

[![CI](https://github.com/tudou123456/tudouOS/actions/workflows/ci.yml/badge.svg)](https://github.com/tudou123456/tudouOS/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/tudou123456/tudouOS?include_prereleases&label=release)](https://github.com/tudou123456/tudouOS/releases)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

**English** | [简体中文](README.md)

> **v0.8.0 Technical Preview** — a from-scratch RTOS for ESP32-S3.
> It is not production-ready yet; APIs and behavior may change. See the
> [release notes](docs/release-notes-v0.8.0.md) and [roadmap](docs/ROADMAP.md).

tudouOS is a small, self-contained RTOS kernel written in C and Xtensa
assembly for the **ESP32-S3**. It runs the same source on the Espressif QEMU
fork and on real hardware, and exposes a **kernel-native MCP (Model Context
Protocol) control plane** so AI agents and test harnesses can inspect and
drive the device over UART or Ethernet.

## Highlights

- **Kernel-native MCP bus** — bounded message channels, JSON-RPC tools
  (`ping`, `echo`, `sysinfo`, `fs.list/read/write`), UART line transport and a
  serial frame transport (`@@MCP <crc16> <json>`).
- **QEMU + hardware from one source** — a Python/QMP test bench boots a fresh
  VM per case; a serial regression harness runs the same assertions on a real
  board (74 cases).
- **Observability** — 256-entry trace ring, reliable important-event ring,
  JSONL export, health snapshots, fault injection (`leak`/`hang`/`spin`) and a
  rule-based analysis tool.
- **Three-class scheduling** — AI / UI / background priority domains, 5-tick
  quantum, 200 ms aging anti-starvation, 700/1000-tick background budget.
- **SMP** — CPU1 bring-up, IPI via the interrupt matrix, per-core ready queues,
  affinity/migration and cross-core S32C1I locks for heap/fs/msg/trace.
- **Memory & storage** — TLSF-like heap with canaries/quarantine, RAM potfs
  with copy-on-write, bounded message QoS0/1/2 with retry/dedup/ack.

## Verified state

| Gate | Result |
| --- | --- |
| QEMU full regression (69 cases, two consecutive rounds) | **69/69, 69/69** |
| ESP32-S3 hardware regression (74 cases) | **72 PASS / 0 FAIL / 0 TIMEOUT / 2 SKIP** |
| Firmware | 91,376 B, SHA256 `90a58acb…e8e0`, `Hash of data verified` |

Evidence: [v0.8.0 release gate report](docs/release-gate-v0.8.0.md),
[independent audit](docs/audit-report-2026-10-19.md) and
[defect report](docs/defect-report.md). The two hardware SKIPs are platform
limitations (QMP freeze, QEMU-only XIP layout) with substitute coverage.

## Quick start (Windows x64)

```powershell
# 1) Python 3.14 + esptool
py -3.14 -m pip install -r tools/requirements.txt

# 2) Fetch the pinned toolchain + QEMU (SHA256 verified)
py -3.14 tools/fetch_deps.py

# 3) Health check, build and run the QEMU test bench
py -3.14 tools/check_env.py
py -3.14 tools/build.py --clean
py -3.14 -m test.runner

# 4) Release gate: clean build + two QEMU rounds (+ hardware if attached)
py -3.14 tools/release_gate.py --rounds 2
py -3.14 tools/release_gate.py --rounds 1 --hw-port COM3
```

Hardware image (full-RAM layout, no flash XIP):

```powershell
py -3.14 tools/build.py --platform hw
py -3.14 -m esptool --chip esp32s3 --port COM3 --baud 921600 `
    write-flash 0x0 build/potato_boot_hw.bin
py -3.14 -m test.hw_runner --port COM3 --keep-going
```

> The project path must be ASCII (binutils limitation on Windows). If your
> checkout is under a non-ASCII path, create an ASCII junction and build from
> there (see the Chinese README FAQ).

## MCP in one line

```json
{"jsonrpc":"2.0","id":"demo-1","method":"tools/call",
 "params":{"name":"sysinfo","arguments":{}}}
```

Send it over the UART console (lines starting with `{` are routed to the MCP
agent) or through the serial MCP frame transport. Responses echo the request
`id` verbatim and are single-line JSON.

## Repository layout

```
kernel/     土豆内核：boot/intc/sched/time/mem/msg/fs/net/mcp/trace/smp
test/       QEMU QMP runner + serial hardware runner + cases
tools/      build, env check, release gate, MCP bridges, AI log analysis
docs/       architecture, audit, observability, SMP, serial MCP, releases
qemu/       interactive QEMU launcher
```

## Documentation

- [Architecture](docs/architecture.md) (Chinese)
- [MCP bus design](docs/mcp-bus-design.md)
- [QEMU test bench](docs/qemu-testing.md)
- [Serial MCP protocol](docs/serial-mcp.md)
- [SMP design](docs/smp.md)
- [Observability](docs/observability.md)
- [Hardware bring-up](docs/hardware-bringup.md)
- [v0.8.0 release notes](docs/release-notes-v0.8.0.md) · [Roadmap](docs/ROADMAP.md)

## Contributing

Contributions are welcome — bug reports, hardware results, docs and code.
Please read [CONTRIBUTING.md](CONTRIBUTING.md) and the
[Code of Conduct](CODE_OF_CONDUCT.md). Security issues: see
[SECURITY.md](SECURITY.md); do not open a public issue for a vulnerability.

## License

[MIT](LICENSE). The kernel is written from scratch; no third-party RTOS code
is included. The pinned toolchain and QEMU are downloaded separately and keep
their own licenses.
