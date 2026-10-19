# 路线图（Roadmap）

> 这是方向性计划，不是承诺。优先级会随 issue、真机反馈和安全报告调整。
> 想推动某一项，请在 Discussions/Issue 里说明场景与验收标准。

## v0.8.0 — Technical Preview（当前）

- M1–M9 全链路：启动/中断/调度/堆/消息/potfs/MCP/以太网(QEMU)/观测/SMP。
- 2026-10-19 独立审计 P0/P1/P2/P3 全收口。
- QEMU 69/69 两连绿；真机 74 例 72 PASS/0 FAIL/2 SKIP。
- 目标：让内核玩家/嵌入式团队能在 QEMU 与普通 ESP32-S3 开发板上复现完整链路。

## v0.8.x — 预览期修补

- CI 稳定化（GitHub Actions Windows 构建 + QEMU 两轮门禁）。
- 文档/英文翻译补全；新贡献者 onboarding 反馈。
- 真机兼容性矩阵（不同板卡/Flash/PSRAM/串口芯片）。
- 修复预览期报告的 bug；不引入不兼容的 API 变更。

## v0.9 — 长稳与网络

- 48h soak、10⁷ 次上下文切换、SMP 跨核 heap/fs/msg 长稳压测。
- UDP 远程 MCP 的 HMAC/token 鉴权与写配额；可选 TLS/DTLS 评估。
- 外挂以太网（W5500 / DM9051 类 SPI 模块）驱动与真机验证。
- CPU1 tick 抢占默认开（完成专项真机稳定性后）。
- Linux/macOS 原生构建与测试（当前固定工具链是 Windows x64）。
- 观测增强：per-CPU trace、更多健康规则、日志回放工具。

## v1.0 — 生产基线（候选目标）

- 稳定内核 API/ABI 与语义化版本承诺。
- 完整的压力/对抗/故障注入矩阵与公开报告。
- 安全审计与供应链加固（依赖签名/可复现构建）。
- 功耗管理、更多 SoC/板卡支持、OTA/恢复策略（视社区需求）。
- 生产级文档：移植指南、工具链矩阵、长期支持策略。

## 非目标（当前）

- POSIX/MMU 进程隔离（单地址空间是既定设计）。
- 桌面级网络协议栈/文件系统。
- 在 ESP32-S3 上硬做内部以太网 MAC（硅片事实，见 docs/ethernet.md）。
