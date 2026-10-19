# 真机（ESP32-S3 / COM3）M1–M9 全量回归报告

- 日期：2026-10-19（v0.8.0 Technical Preview 发布门禁）
- 固件：`build/potato_boot_hw.bin`，**91,376 B**，全 RAM 布局
  - SHA256：`47d4af9e5d59e41ae404ed3a04bb6764cd39c12574b3df70b3064c4dd19f8a1c`
  - 启动身份：`[POTATO] build Oct 19 2026 22:31:14` / `tudouOS v0.8.0`
  - 烧录：`esptool write-flash 0x0` → `Hash of data verified`
- 串口：COM3 / 115200（CH340 USB-UART）；复位：DTR=IO0、RTS=EN
- 回归台：`test/hw_runner.py` + `test/hw_cases/`，**74 条**，逐用例复位+等 READY
- 命令：`py -3.14 -m test.hw_runner --port COM3 --keep-going`
- 结果：**PASS=72 / FAIL=0 / TIMEOUT=0 / SKIP=2 / 未执行 0，exit 0**
- 原始输出：`build/_final_hw_run2.log`（本机）；完整门禁证据与哈希见
  [docs/release-gate-2026-10-19.md](../../docs/release-gate-2026-10-19.md)

## 1. 按里程碑

| 里程碑 | 用例数 | PASS | SKIP | 说明 |
| --- | ---: | ---: | ---: | --- |
| M1 启动/控制台 | 3 | 3 | 0 | ROM→内核、banner v0.8.0、info/help/echo |
| M2 时基/异常/追踪 | 12 | 12 | 0 | tick、UART IRQ、异常 dump、trace/JSONL/副环、抖动 |
| M3 调度 | 7 | 7 | 0 | tasks、taskdemo preempt/stress/bg、sleep、trace、preempt 开关 |
| M3.5 消息基座 | 7 | 6 | 1 | flood/ping/lazy/timeout/trace；`disconnect_heal` 见 §2 |
| M4 堆管理 | 9 | 9 | 0 | memstat、selftest、holes、canary、exhaust、churn、trace、dblfree、szcorrupt |
| M5 平台/驱动/文件 | 8 | 7 | 1 | 全 RAM 布局、PAL、驱动、fs、mmutest、跨模块；`xip` 见 §2 |
| M6 MCP/potfs | 7 | 7 | 0 | mcp on/ping、字符串 id、错误路径、握手、fs 往返、生命周期、unicode/行长 |
| M7 串口 MCP/网络 | 7 | 7 | 0 | 无 EMAC 降级、串口传输、原始帧 RPC、tool selftest/tools/list+call、重放、复位恢复 |
| M8 可观测 | 5 | 5 | 0 | obs 契约、注入 leak/hang、时钟无漂移、panic dump |
| M9 SMP | 9 | 9 | 0 | 单核默认、启动、IPI、绑核、均衡、混合压测、跨核锁压测、preempt 开关、复位恢复 |
| **合计** | **74** | **72** | **2** | |

## 2. 两条显式 SKIP（平台手段差异，均有替代覆盖）

1. `m35/disconnect_heal`：需要 QMP `stop/cont` 冻结 CPU；真机 RTS 复位会清内核
   状态与重放槽，语义不等价。重放/去重由 `m7/raw_frame_rpc` 与
   `m35/lazy_retry_dedup` 覆盖。
2. `m5/xip_not_applicable`：真机默认全 RAM 构建，无 flash XIP 取指；
   对应判据由 `m5/boot_ram_only_layout` 覆盖。

## 3. 环境

| 项 | 值 |
| --- | --- |
| 板卡 | ESP32-S3 rev v0.2，8MB PSRAM |
| 工具链 | xtensa-esp-elf-gcc 13.2.0（esp-13.2.0_20250707） |
| QEMU | 9.2.2（esp_develop_9.2.2_20260417，仅 QEMU 回归） |
| esptool | 5.4.0 |
| Python | 3.14.3 |

## 4. 说明

- 逐用例串口日志保存在本机 `test/logs/hw/`（gitignored，不入库）。
- 本次同时完成 QEMU 全量 **69/69 连续两轮**；两轮 QEMU + 真机的完整门禁证据
  （日志、固件 SHA256、`SHA256SUMS`）归档在
  `test/logs/gate-2026-10-19/`，摘要见
  [docs/release-gate-2026-10-19.md](../../docs/release-gate-2026-10-19.md)。
- 本次是 v0.8.0 Technical Preview 基线；长稳/网络对抗等未覆盖项见
  [docs/ROADMAP.md](../../docs/ROADMAP.md)。
