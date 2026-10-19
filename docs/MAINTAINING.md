# 维护与发布

面向仓库维护者，说明分支、版本、测试门禁与 issue 运营。

## 分支模型

- `main`：可发布基线；建议开启分支保护（至少 1 review + CI 通过）。
- `feat/*`、`fix/*`、`docs/*`、`release/*`：短期分支，合并后删除。
- 大改动先开 issue/Discussions；涉及中断、帧布局、SMP 的 PR 需要额外审查。

## 版本

- 版本定义在 `kernel/include/potato.h::POTATO_VERSION`（语义化版本）。
- 技术预览阶段（0.x）允许不兼容调整，但必须在 `CHANGELOG.md` 和 release
  notes 中写清升级注意。
- 发布走 patch/minor 版本；不改历史 tag。

## 发布流程

1. 从 `main` 拉 `release/vX.Y.Z`。
2. 更新 `POTATO_VERSION`、README/发布说明中的版本。
3. 更新 `CHANGELOG.md`（Added/Changed/Fixed/Known limitations）。
4. 干净构建并跑 QEMU 全量；涉及硬件路径时跑真机回归：

   ```powershell
   py -3.14 tools/build.py --clean
   py -3.14 -m test.runner
   py -3.14 tools/release_gate.py --rounds 2
   ```

5. 真机镜像构建/烧录后跑 `py -3.14 -m test.hw_runner --port COM3 --keep-going`
   （SKIP 必须写明平台原因）。
6. 合并到 `main`，打 tag `vX.Y.Z`，在 GitHub Releases 粘贴发布说明；
   技术预览勾选 **Set as a pre-release**。
7. 发布后把相关 issue 关闭/标注，收集板卡与使用反馈。

## CI

- `.github/workflows/ci.yml`
  - `static`：Python 语法、版本一致性、`git diff --check`、依赖 pin 校验。
  - `qemu-gate`（Windows）：`fetch_deps.py` → `check_env` → 干净构建 →
    QEMU 全量（`release_gate.py --rounds 2 --skip-build`）。
- `.github/workflows/hw-gate.yml`：手动触发，自托管 Windows runner + 板卡；
  只对维护者开放，不要在公开 PR 上自动跑硬件。
- 依赖更新采用月度分组 PR；重大版本升级必须重新跑门禁并更新 pin。

## Issue / PR

建议标签：`bug`、`enhancement`、`documentation`、`hardware`、`qemu`、
`security`、`needs-repro`、`good first issue`、`dependencies`。

- 新 issue 24–72 小时内分类；缺版本/复现的请求 `needs-repro`。
- 安全报告转到 GitHub Security Advisories，不在公开 issue 讨论细节。
- 合并前确认：测试、文档、无日志/密钥/third_party、提交信息可读。
- 发布后在 Discussions 发一条变更摘要；每月整理 `good first issue`。

## 安全与供应链

- 漏洞走私密报告；修复后写 `CHANGELOG` 并在 release notes 致谢。
- `tools/fetch_deps.py` 的 URL/SHA256 是供应链边界；升级必须同时更新
  URL、大小、SHA256，并附上游 Release 链接。
- 不提交 `build/`、`test/logs/`、`third_party/`、板卡 MAC、个人路径。
