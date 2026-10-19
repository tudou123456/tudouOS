# tudouOS 发布门禁验收报告（2026-10-19）

- 验收对象：`D:\公司软件\tudouOS`，分支 **`audit-fixes-2026-10-19`**，
  提交 **`53d3038c487af124355c5a7394666739fce566a4`**（第 8 批，44 files，
  1858 insertions / 362 deletions）
- 验收命令：`py -3.14 tools/release_gate.py --rounds 2 --hw-port COM3`
- 门禁结果：**通过（exit 0）**——QEMU 连续两轮 69/69 全绿 + 真机 74 例
  72 PASS / 0 FAIL / 0 TIMEOUT / 2 SKIP
- 证据归档：`test/logs/gate-2026-10-19/`（含 SHA256 清单 `manifest.sha256`
  与可机检 `SHA256SUMS`）
- 说明：`test/logs/` 与 `build/` 为 gitignored；本报告把关键哈希与结果内联，
  原始日志保存在归档目录，便于本机复核。Mimosa 深度静态扫描（23:04）
  `findings=0`；扫描覆盖状态为 partial，不构成完整安全保证。

---

## 1. 环境与镜像

| 项 | 值 |
| --- | --- |
| 平台 | Windows x64 |
| Python | 3.14.3 |
| QEMU | 9.2.2 (`esp_develop_9.2.2_20260417`) |
| 交叉编译器 | `xtensa-esp-elf-gcc 13.2.0` (`esp-13.2.0_20250707`) |
| esptool | 5.4.0 |
| 开发板 | ESP32-S3 rev v0.2（8MB PSRAM，COM3，MAC `28:84:85:46:0c:b4`） |

**真机固件（本次重刷）**

| 项 | 值 |
| --- | --- |
| 文件 | `build/potato_boot_hw.bin` |
| 大小 | **91,376 B** |
| SHA256 | `47d4af9e5d59e41ae404ed3a04bb6764cd39c12574b3df70b3064c4dd19f8a1c` |
| 构建身份 | `[POTATO] build Oct 19 2026 22:31:14` |
| 烧录 | `esptool ... write-flash 0x0 build/potato_boot_hw.bin` → `Wrote 91376 bytes` → **`Hash of data verified.`** |

**QEMU 镜像（门禁干净构建）**

| 文件 | 大小 | SHA256 |
| --- | --- | --- |
| `build/potato_boot.bin` | 2,304 B | `b0c780525eedcfed530746201e30b6dbd46a9c2b5a802c288ce09b31ba461a67` |
| `build/flash_image.bin` | 4,194,304 B | `63efc2bbeef4d81d50532c10299c90cb39d033ee809de108d59051e6c828ef82` |
| `build/kernel.elf` | 570,248 B | `0114459c52f9bed70eac7d41a5b5e9dc8cd9e96df48836fd40a49d225e089761` |

干净构建：**34/34 目标重编**，`-Werror` 通过，IROM 段 **88,708 B @0x10020**
逐字节自检 OK（`_gate_build.log`）。

---

## 2. 门禁结果

| 阶段 | 结果 | 证据 |
| --- | --- | --- |
| QEMU 全量第 1 轮 | **69/69 通过** | `_gate_qemu_run1.log` |
| QEMU 全量第 2 轮 | **69/69 通过** | `_gate_qemu_run2.log` |
| 真机全量（COM3） | **74 例：72 PASS / 0 FAIL / 0 TIMEOUT / 2 SKIP / 未执行 0** | `_gate_hw.log`、`summary.md` |
| 总门禁 | **通过，exit 0** | `_gate_full.log` |

门禁脚本在任一轮非全绿、或真机 `FAIL/TIMEOUT` 非零时返回非零；本次两轮
QEMU 与真机全部满足。QEMU 用例数从审计前 67 增至 **69**（新增
`test_mcp_string_id_echo`、`test_m9_xcore_locks_heap_fs_msg`）；真机用例数从
72 增至 **74**（新增 `m6/string_id_echo`、`m9/xcore_stress`）。

**两条真机 SKIP（平台手段差异，均有替代覆盖）**

1. `m35/disconnect_heal`：需要 QMP `stop/cont` 冻结 CPU；真机 RTS 复位语义不等价，
   断线自愈无法无损复现。重放/去重由 `m7/raw_frame_rpc` 与 `msgdemo lazy` 覆盖。
2. `m5/xip_not_applicable`：真机默认全 RAM 构建，无 flash XIP 取指；对应判据由
   `m5/boot_ram_only_layout` 覆盖。

---

## 3. 归档证据

目录：`test/logs/gate-2026-10-19/`

| 文件 | 内容 | SHA256 |
| --- | --- | --- |
| `_gate_build.log` | QEMU 干净构建（34/34、IROM 自检） | `38a07fa4…a13315` |
| `_gate_qemu_run1.log` | QEMU 第 1 轮 69/69 | `8d4c7b4a…faec06` |
| `_gate_qemu_run2.log` | QEMU 第 2 轮 69/69 | `72619353…b4e5b1` |
| `_gate_hw.log` | 真机 74 例原始输出 | `d279d15d…d6a37d` |
| `_gate_flash.log` | 91376B 重刷 + Hash verified | `f5340202…51cc5a` |
| `_gate_full.log` | 门禁完整 stdout（含 exit 语义） | `a4d133b0…cbc40f` |
| `summary.md` / `_session.log` | hw_runner 汇总与串口全集 | 见 `manifest.sha256` |
| `firmware.txt` / `versions.txt` | 固件、版本与镜像哈希 | 见 `manifest.sha256` |
| `SHA256SUMS` | 归档日志与元数据的可机检哈希清单 | `cd test/logs/gate-2026-10-19 && sha256sum -c SHA256SUMS` |
| `manifest.sha256` | 本目录全部文件 + 构建产物哈希清单 | — |

完整 SHA256 见归档目录的 `manifest.sha256`；关键值已在上文内联。
`SHA256SUMS` 已用 `sha256sum -c` 逐项复核，**10/10 OK**。

---

## 4. 复现

```powershell
# 1) 真机镜像（当前 91376B）
cmd /c "cd /d D:\tudouOS && py -3.14 tools/build.py --platform hw"
py -3.14 -m esptool --chip esp32s3 --port COM3 --baud 921600 `
    write-flash 0x0 build/potato_boot_hw.bin

# 2) 完整门禁（干净构建 QEMU + 两轮全量 + 真机全量）
cmd /c "cd /d D:\tudouOS && py -3.14 tools/release_gate.py --rounds 2 --hw-port COM3"
```

预期末行：`发布门禁通过（QEMU 2 轮 + 真机 COM3）`，exit 0。

---

## 5. 结论与残留项

本次门禁证明：审计修复后的工作树在 QEMU 上连续两轮 69/69 稳定全绿，在同一份
源码构建的 91,376B 真机固件上 74 例回归 0 FAIL / 0 TIMEOUT。审计报告的
P0/P1/P2/P3 阻塞项已全部关闭，可作为当前发布基线。

仍未覆盖（发布后跟踪，不阻塞本基线）：

- 48h 长稳 soak、10⁷ 次上下文切换压力；SMP 长稳/缓存内存序专项。
- UDP MCP 未实现 HMAC/共享密钥：仅应部署在可信直连网段；远程写默认关闭，
  需 `netmcp rw on` 显式授权。
- CPU1 tick 抢占仍默认关（`smp preempt on` 可开）；CPU0 在真机默认开、
  QEMU 默认关（模型缺陷规避）。
- 本报告对应提交 `53d3038`；归档日志（`test/logs/gate-2026-10-19/`）为本机
  gitignored 证据，可用 `SHA256SUMS` 逐项复核。
