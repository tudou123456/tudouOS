# tudouOS

[![Release](https://img.shields.io/github/v/release/tudou123456/tudouOS?include_prereleases&label=release)](https://github.com/tudou123456/tudouOS/releases)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

**English** | [简体中文](README.md)

> **v0.8.0 Technical Preview** — an RTOS for ESP32-S3. The kernel and hardware
> path work, but this is not production-ready yet; APIs and behavior may
> change. See the [release notes](docs/release-notes-v0.8.0.md) and
> [roadmap](docs/ROADMAP.md).

tudouOS is a small, from-scratch RTOS written in C and Xtensa assembly for the
**ESP32-S3** (call0 ABI, no libc/libgcc dependency). The same source runs on
the Espressif QEMU fork and on real boards. It puts an **MCP (Model Context
Protocol) control plane inside the kernel**, so AI agents, scripts and test
harnesses can inspect and drive the device over UART or Ethernet.

## Highlights

- **Kernel-native MCP** — bounded message channels, JSON-RPC tools (`ping`,
  `echo`, `sysinfo`, `fs.list/read/write`), UART line transport and a CRC16
  serial frame transport; `initialize` / `tools/list` / `tools/call` with
  numeric/string ids.
- **QEMU + hardware from one source** — a QMP test bench boots a fresh VM per
  case; a serial harness runs the same assertions on a board.
- **Three-class scheduling** — AI / UI / background domains, round-robin,
  aging anti-starvation, background budget; interrupt-driven tick with optional
  preemption.
- **SMP** — CPU1 bring-up, IPIs, affinity/migration; cross-core atomic locks
  for heap, fs, messages and trace.
- **Memory & storage** — TLSF-like heap with canaries/quarantine, RAM potfs
  with copy-on-write, bounded message QoS0/1/2 with retry/dedup/ack.
- **Observability** — trace ring + reliable event ring, JSONL export, health
  snapshots, fault injection, rule-based analysis (`tools/ai_analysis.py`).
- **Networking** — ARP/IPv4/ICMP/UDP and remote MCP over QEMU open_eth; real
  hardware uses the serial MCP transport (ESP32-S3 has no internal EMAC).

## Quick start (Windows x64)

```powershell
py -3.14 -m pip install -r tools/requirements.txt
py -3.14 tools/fetch_deps.py        # pinned toolchain + QEMU, SHA256 verified
py -3.14 tools/build.py --clean
py -3.14 -m test.runner             # QEMU test bench
```

Hardware image (full-RAM layout, no flash XIP):

```powershell
py -3.14 tools/build.py --platform hw
py -3.14 -m esptool --chip esp32s3 --port COM3 --baud 921600 `
    write-flash 0x0 build/potato_boot_hw.bin
py -3.14 -m test.hw_runner --port COM3 --keep-going
```

> The checkout path must be ASCII (Windows binutils limitation). If it is not,
> create an ASCII junction and build from there.

## MCP example

Lines starting with `{` on the console are routed to the MCP agent:

```json
{"jsonrpc":"2.0","id":"demo-1","method":"tools/call",
 "params":{"name":"sysinfo","arguments":{}}}
```

Responses are single-line JSON and echo the request `id`. See
[docs/serial-mcp.md](docs/serial-mcp.md) for the serial frame format.

## Layout

```
kernel/     boot, interrupts, scheduler, time, heap, msg, fs, net, mcp, trace, smp
test/       QEMU QMP runner + serial hardware runner + cases
tools/      build, env check, dependency fetch, release gate, MCP bridges, log analysis
docs/       architecture, MCP, SMP, serial protocol, observability, QEMU bench, roadmap
qemu/       interactive QEMU launcher
```

## Documentation

- [Architecture](docs/architecture.md) (Chinese)
- [MCP bus design](docs/mcp-bus-design.md)
- [Serial MCP protocol](docs/serial-mcp.md)
- [SMP design](docs/smp.md)
- [Observability](docs/observability.md)
- [Ethernet / networking](docs/ethernet.md)
- [QEMU test bench](docs/qemu-testing.md)
- [v0.8.0 release notes](docs/release-notes-v0.8.0.md) · [Roadmap](docs/ROADMAP.md)
- [Maintaining](docs/MAINTAINING.md)

## Contributing

Bug reports, hardware results, docs and code are welcome. Please read
[CONTRIBUTING.md](CONTRIBUTING.md) and the
[Code of Conduct](CODE_OF_CONDUCT.md). For a vulnerability, follow
[SECURITY.md](SECURITY.md); do not open a public issue.

## License

[MIT](LICENSE). The kernel is written from scratch; no third-party RTOS code
is included. The pinned toolchain and QEMU are downloaded separately and keep
their own licenses.
