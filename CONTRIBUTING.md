# 贡献指南（Contributing to tudouOS）

感谢你愿意参与土豆OS。bug 报告、真机兼容性结果、文档和代码都很欢迎。
开始前请先读 [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md)。

> 当前是 **v0.8.0 Technical Preview**：接口与行为仍可能调整。大改动建议
> 先开 issue 讨论设计，避免白做。

## 1. 开发环境

- Windows x64（当前固定工具链/QEMU 是 Windows 构建）
- Python 3.14（3.12+ 也可；测试台只用标准库；真机需要 `pyserial`）
- 项目路径必须是 **ASCII**。若检出在非 ASCII 路径下，请按 README
  「常见问题」建立目录联接，并从联接路径构建。

一次性准备：

```powershell
py -3.14 -m pip install -r tools/requirements.txt
py -3.14 tools/fetch_deps.py
py -3.14 tools/check_env.py
```

`fetch_deps.py` 的下载地址和 SHA256 固定在脚本内；升级工具链时请同时更新
`tools/envlib.py` 的 URL 和该文件的摘要。

## 2. 构建与测试

```powershell
# QEMU 镜像
py -3.14 tools/build.py --clean
py -3.14 -m test.runner

# 发布门禁：干净构建 + QEMU 连续两轮
py -3.14 tools/release_gate.py --rounds 2

# 真机镜像（全 RAM 布局）与串口回归
py -3.14 tools/build.py --platform hw --clean
py -3.14 -m esptool --chip esp32s3 --port COM3 --baud 921600 `
    write-flash 0x0 build/potato_boot_hw.bin
py -3.14 -m test.hw_runner --port COM3 --keep-going
```

提交 PR 的最低要求：

- 改内核/驱动/协议：QEMU 全量通过（建议连续两轮）。
- 改真机路径/串口/中断/时基：附真机回归结果（0 FAIL/0 TIMEOUT）。
- 改文档：`git diff --check` 通过；命令/输出与代码一致。

## 3. 代码风格与内核约束

- C11，`-Wall -Wextra -Werror`；不引入 libc/libgcc 依赖（裸机）。
- Xtensa 侧统一 **call0 ABI**；帧布局只在 `switch.S`/`vectors.S` 定义，
  改帧必须同步 `task.h`/注释与回归。
- 任务态非重入结构（堆/potfs/消息环/追踪环/调度容器）必须持锁：
  单核任务态用短临界区，SMP 可见结构用 `pot_lock_acquire/release`
  （见 `kernel/include/intc.h`）。持锁期间不得睡眠。
- 锁顺序：`fs → heap → trace`、`sched → trace`；不要新增反向嵌套。
- 中断上下文只做最小工作；`potato_printf` 已按行持跨核锁，禁止在 ISR 里打印。
- 新机制必须带可观测事件/计数或测试覆盖；真机可见差异写进 `docs/`。
- 注释解释"约束/为什么"，不要复述代码。

## 4. 提交与 PR

1. Fork 仓库，从 `main` 拉分支：`feat/...`、`fix/...`、`docs/...`。
2. 提交信息用一句话说明目的，正文列验证命令与结果；建议 `git commit -s`
   （DCO：你确认有权提交该代码）。
3. 不要提交：`build/`、`test/logs/`、`third_party/`、任何密钥/令牌/个人日志。
4. PR 描述请填写模板：改动、动机、验证、风险/兼容性。
5. 维护者会复跑 QEMU 门禁；涉及硬件的 PR 会要求真机结果。

## 5. 报告问题

- 普通 bug/功能请求：用 GitHub Issue 模板，附版本（`info` 输出）、构建命令、
  最小复现、串口日志片段。
- 安全漏洞：**不要开公开 issue**，见 [SECURITY.md](SECURITY.md)。
- 真机问题请注明板卡、供电、串口、烧录方式与 `tickstat`/`tasks`/`memstat`
  输出；能复现的日志比描述更有用。

## 6. 适合第一次参与的任务

- 文档：英文翻译、命令示例补全、已知限制核对。
- 测试：给现有驱动/协议补负路径用例（坏帧、超长、并发）。
- 平台：WSL2/Linux 构建脚本、非 Windows 串口工具。
- 观测：给新计数器补 `health`/`trace` 字段与规则。

从 issue 标签 `good first issue` 开始，或在 Discussions 里认领路线图条目。
