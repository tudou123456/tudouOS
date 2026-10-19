# tudouOS v0.8.0 发布门禁报告（Technical Preview）

- 版本：**v0.8.0 Technical Preview**（2026-10-19）
- 源码：`release-v0.8.0` 分支，代码提交 `9e8c8ee`（发布准备）；
  tag `v0.8.0` 指向包含本证据的提交
- 门禁命令：`py -3.14 tools/release_gate.py --rounds 2 --hw-port COM3`
- 结果：**通过（exit 0）**——QEMU 连续两轮 69/69 + 真机 74 例
  72 PASS / 0 FAIL / 0 TIMEOUT / 2 SKIP
- 证据归档：`test/logs/gate-v0.8.0/`（含 `SHA256SUMS`，10/10 校验通过）
- 历史对照：`docs/release-gate-2026-10-19.md` 是审计修复批（v0.7.0）的
  门禁；本报告是 v0.8.0 发布门禁。

---

## 1. 环境与固件

| 项 | 值 |
| --- | --- |
| 平台 | Windows x64 |
| Python | 3.14.3 |
| QEMU | 9.2.2 (`esp_develop_9.2.2_20260417`) |
| 交叉编译器 | `xtensa-esp-elf-gcc 13.2.0` (`esp-13.2.0_20250707`) |
| esptool | 5.4.0 |
| 开发板 | ESP32-S3 rev v0.2（8MB PSRAM，COM3） |

**真机固件（本次重刷）**

| 项 | 值 |
| --- | --- |
| 文件 | `build/potato_boot_hw.bin` |
| 大小 | **91,376 B** |
| SHA256 | `90a58acb98333c189b1494a6acedeab901a7d224894cb50887cbf3c8c077e8e0` |
| 版本/构建身份 | `tudouOS v0.8.0` / `[POTATO] build Oct 19 2026 23:22:06` |
| 烧录 | `esptool ... write-flash 0x0` → `Wrote 91376 bytes` → **`Hash of data verified.`** |

**QEMU 镜像（门禁干净构建，34/34，IROM 88,708 B @0x10020 自检 OK）**

| 文件 | 大小 | SHA256 |
| --- | --- | --- |
| `build/potato_boot.bin` | 2,304 B | `b0c780525eedcfed530746201e30b6dbd46a9c2b5a802c288ce09b31ba461a67` |
| `build/flash_image.bin` | 4,194,304 B | `ef5b76a98a03ec1199190139be6b92b0fd8818088761473137bba6d6f78c3a20` |
| `build/kernel.elf` | 570,248 B | `68cca00459bd22475a995a1d68edc38f87bd33d425af4ca5185e5397e4ab77d6` |

---

## 2. 门禁结果

| 阶段 | 结果 | 原始日志 |
| --- | --- | --- |
| QEMU 全量第 1 轮 | **69/69 通过** | `_gate_qemu_run1.log` |
| QEMU 全量第 2 轮 | **69/69 通过** | `_gate_qemu_run2.log` |
| 真机全量（COM3） | **74 例：72 PASS / 0 FAIL / 0 TIMEOUT / 2 SKIP** | `_gate_hw.log` |
| 总门禁 | **通过，exit 0** | `_v080_gate.log` |

新增于 v0.8.0 的回归用例：`test_mcp_string_id_echo`、
`test_m9_xcore_locks_heap_fs_msg`（QEMU）；`m6/string_id_echo`、
`m9/xcore_stress`（真机）。

**两条真机 SKIP（平台手段差异，有替代覆盖）**

1. `m35/disconnect_heal`：需要 QMP `stop/cont` 冻结 CPU；真机 RTS 复位语义
   不等价。重放/去重由 `m7/raw_frame_rpc` 与 `m35/lazy_retry_dedup` 覆盖。
2. `m5/xip_not_applicable`：真机默认全 RAM 构建，无 flash XIP 取指；
   由 `m5/boot_ram_only_layout` 覆盖。

---

## 3. 归档清单

目录：`test/logs/gate-v0.8.0/`

| 文件 | 内容 | SHA256（前缀） |
| --- | --- | --- |
| `_gate_build.log` | QEMU 干净构建（34/34、IROM 自检） | `38a07fa4…` |
| `_gate_qemu_run1.log` | QEMU 第 1 轮 69/69 | `ff9c7009…` |
| `_gate_qemu_run2.log` | QEMU 第 2 轮 69/69 | `61c26cdd…` |
| `_gate_hw.log` | 真机 74 例原始输出 | `c7233ee1…` |
| `_v080_flash.log` | 91376B 重刷 + Hash verified | `b2171768…` |
| `_v080_gate.log` | 门禁完整 stdout | `a4d133b0…` |
| `summary.md` / `_session.log` | hw_runner 汇总/串口全集 | 见 `SHA256SUMS` |
| `firmware.txt` / `versions.txt` | 固件、版本、镜像哈希 | 见 `SHA256SUMS` |
| `SHA256SUMS` | 可机检清单 | `sha256sum -c` **10/10 OK** |
| `manifest.sha256` | 归档 + 构建产物哈希说明 | — |

---

## 4. 复现

```powershell
# 1) 依赖（固定 SHA256）
py -3.14 -m pip install -r tools/requirements.txt
py -3.14 tools/fetch_deps.py

# 2) 真机镜像与烧录
py -3.14 tools/build.py --platform hw --clean
py -3.14 -m esptool --chip esp32s3 --port COM3 --baud 921600 `
    write-flash 0x0 build/potato_boot_hw.bin

# 3) 完整门禁（QEMU 两轮 + 真机）
py -3.14 tools/release_gate.py --rounds 2 --hw-port COM3
```

预期末行：`发布门禁通过（QEMU 2 轮 + 真机 COM3）`，exit 0。
注意：`__DATE__/__TIME__` 会写入启动 banner，每次干净构建的 build 身份与
二进制哈希不同；归档的是本次发布的实际固件。

---

## 5. 已知限制（发布后跟踪）

- 48h soak、10⁷ 次上下文切换、SMP 长稳/缓存内存序尚未跑。
- UDP MCP 无 HMAC/共享密钥；只应部署在可信直连网段，远程写默认关闭。
- CPU1 tick 抢占仍默认关（`smp preempt on` 可开）。
- ESP32-S3 无内部 EMAC；真机以太网需外挂 SPI 模块（未实现）。
- 固定工具链/QEMU 当前为 Windows x64；Linux/macOS 需自行提供。
