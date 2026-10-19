# Changelog

本文件遵循 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)
与语义化版本。技术预览版可能包含不兼容调整，升级前请读对应 release notes。

## [0.8.0] - 2026-10-19 · Technical Preview

首个公开技术预览。完成 2026-10-19 独立审计的 P0/P1/P2/P3 全部收口，
并在 QEMU 连续两轮与真机全量回归上验证。

### Added

- **MCP 字符串 id 回归**：`test_mcp_string_id_echo`（QEMU）与
  `m6/string_id_echo`（真机），覆盖 initialize/tools/list/tools/call、
  长 id、转义 id、超长 id 拒绝。
- **跨核压测**：`smp stress` 命令 + `test_m9_xcore_locks_heap_fs_msg` /
  `m9/xcore_stress`，双核并发 heap/potfs/消息 5 万+ 次操作零损坏。
- **发布门禁**：`tools/release_gate.py`（干净构建 + QEMU 连续 N 轮全绿 +
  可选真机 `--hw-port`）。
- **依赖引导**：`tools/fetch_deps.py` 按固定 GitHub Release URL/SHA256
  下载并校验工具链与 QEMU。
- **开源工程文件**：LICENSE(MIT)、CONTRIBUTING、SECURITY、CODE_OF_CONDUCT、
  CHANGELOG、ROADMAP、issue/PR 模板、CI 工作流、英文 README。
- `netmcp rw on|off`：UDP 远程写工具显式授权；`netmcp dump` 新增
  `reject`/`rw` 计数。
- `tasks` 输出新增 `guard=` 栈哨兵计数。

### Changed

- **真机 CPU0 tick 抢占默认开启**（QEMU 仍默认关，模型丢 tick 缺陷规避）。
- 消息通道去重/回执窗口改为 4 发送方桶 × 4 槽，回执按入队 seq 配对；
  QoS0 不再占用窗口；`next_seq` 从 1 开始。
- `potfs` 读写删统一持跨核锁；写侧释放旧 data 与读复制同域。
- `heap/msg/potfs/trace/任务注册表` 从单核 `rsil 1` 临界区升级为
  S32C1I 跨核锁；`potato_printf` 按行跨核互斥。
- 初始化顺序：调度器/SMP 状态在开中断前就绪；任务常态 PS 显式清
  INTLEVEL/EXCM。
- 真机 level-1 异常恢复不再写陈旧的 EPS2（`rfe` 只依赖 EPC1）。
- `build.py` 用 QEMU `--version` 判定乐鑫分支；移除 objcopy 已知空段告警。
- `ai_analysis.py` JSON 输出改用 `pathlib`（静态扫描 CWE-22 收口）。

### Fixed

- **P0-1** MCP 字符串 id 在 `pot_json_free` 后回显导致 use-after-free。
- **P0-2** syscall 切换未按 `frame_kind` 分派，可能恢复到陈旧 `a0`。
- **P0-3** 跨核临界区实为本核 `rsil`，SMP 下堆/消息/potfs/trace 可并发损坏。
- **P0-4** potfs 读侧无锁，与写侧释放旧数据交错时读已释放内存。
- **P0-5** UDP MCP 可被任意源接管、伪造 ack 清空重放保护。
- **P1-1** 消息去重/回执窗口 8 槽共享、按 id 配对导致误判/漏配对。
- **P1-2** trace 环 level-2 写者与 level-1 临界区撕裂。
- **P1-3** 就绪/睡眠表满时任务被静默丢弃。
- **P1-4** `pick_best` 在抢占决策中无锁读半更新队列。
- **P1-5** IPI pending 读清竞态导致原因位丢失。
- **P1-6** 远程入口无写配额/信任边界。
- **P2** 栈哨兵、TIMG UPDATE bit31、level-3 源屏蔽、CPU1 窗口顺序、
  `g_bg_used` 跨核 RMW、注册表/printf 并发等。
- 测试稳定化：`test_msg_trace`/`test_mem_trace` 改用 `trace meta` 计数差分；
  真机 `m7/raw_frame_rpc` 的 ack 改为响应 seq。

### Security

- UDP 远程 MCP：固定 peer + ack 纪元/上界校验；远程写默认关闭。
- 堆 canary 键改为启动期从 ccount 派生；`free` 增加指针范围/对齐校验。
- `fetch_deps.py` 对工具链/QEMU 归档做 SHA256 校验与 zip-slip 防护。
- 明确信任边界：串口可信；UDP 仅限可信直连网段，无 HMAC。

### Verification

| 门禁 | 结果 |
| --- | --- |
| QEMU 全量（69 例，连续两轮） | 69/69、69/69 |
| 真机全量（74 例） | 72 PASS / 0 FAIL / 0 TIMEOUT / 2 SKIP |
| 固件 | 91,376 B，SHA256 `90a58acb…e8e0`（v0.8.0），烧录 Hash verified |

证据：[docs/release-gate-v0.8.0.md](docs/release-gate-v0.8.0.md)、
[独立审计](docs/audit-report-2026-10-19.md)；
历史审计修复批门禁见 [docs/release-gate-2026-10-19.md](docs/release-gate-2026-10-19.md)。

### Compatibility / upgrade notes

- `netmcp` 的 UDP 对端必须先用 `netmcp peer <ip> <port>` 固定；任意源学习已移除。
- UDP 远程 `fs.write` 默认被拒绝，需 `netmcp rw on`。
- QoS1/2 调用方应保证同一发送方的消息 `id` 在收到 ack 前唯一。
- `tasks` 摘要行末尾新增 `guard=` 字段；按行尾全匹配的脚本需放宽。
- 真机默认开启 tick 抢占；如遇旧硬件时序问题可用 `preempt off` 对照。

### Known issues

- 48h soak、10⁷ 次上下文切换压力、SMP 长稳/缓存内存序尚未跑。
- UDP MCP 无 HMAC/共享密钥；仅限可信直连网段。
- CPU1 tick 抢占仍默认关（`smp preempt on` 可开）。
- ESP32-S3 无内部 EMAC；真机以太网需外挂 W5500/DM9051 类模块（未实现）。
