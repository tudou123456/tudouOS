# 维护手册（Maintaining tudouOS）

面向仓库维护者，说明分支、版本、发布门禁、CI 与 issue 运营。

## 分支与合并

- `main`：可发布基线；只接受通过门禁的 PR/合并。
- `feat/*`、`fix/*`、`docs/*`、`release/*`：短期分支，合并后删除。
- 大改动先开 issue/讨论；涉及 `switch.S`/`vectors.S`/中断矩阵/SMP 的 PR
  必须附 QEMU 连续两轮 + 真机全量结果。
- 建议开启 `main` 分支保护：至少 1 个 review、CI 通过、禁止强推。

## 版本与发布

版本定义在 `kernel/include/potato.h::POTATO_VERSION`（语义化版本）。
发布流程：

1. 从 `main` 拉 `release/vX.Y.Z`。
2. 更新 `POTATO_VERSION`、README/发布说明中的版本与数字；
   `test/hw_selftest.py` 的硬编码 banner 同步（或改为读取头文件）。
3. 更新 `CHANGELOG.md`（Added/Changed/Fixed/Security/Verification/
   Compatibility/Known issues）。
4. 干净构建 + 门禁：

   ```powershell
   py -3.14 tools/build.py --clean
   py -3.14 tools/release_gate.py --rounds 2 --hw-port COM3
   ```

5. 真机重刷当前 `potato_boot_hw.bin`，记录 SHA256 与 `Hash of data verified`。
6. 把门禁日志归档到 `test/logs/gate-YYYY-MM-DD/`，生成 `SHA256SUMS`；
   在 `docs/release-gate-YYYY-MM-DD.md` 写摘要（固件哈希、两轮 QEMU、
   真机结果、SKIP 原因）。
7. 合并到 `main`（fast-forward/merge），打签名 tag `vX.Y.Z`，在 GitHub
   Releases 粘贴发布说明；技术预览勾选 **Set as a pre-release**。
8. 不改历史 tag；修复走 patch 版本。

## CI

- `.github/workflows/ci.yml`
  - `static`：Python 语法/依赖白名单/`git diff --check`。
  - `qemu-gate`（Windows）：`fetch_deps.py` → `check_env` → 干净构建 →
    `release_gate.py --rounds 2 --skip-build`；失败上传门禁日志。
- `.github/workflows/hw-gate.yml`：手动触发，`self-hosted` 且带板 runner，
  跑真机全量。不要把带板机放到公开 PR 触发。
- CI 首次运行需下载固定工具链/QEMU（约 317MB）并解压；`actions/cache`
  缓存 `third_party/`（约 1GB）。缓存键绑 `tools/envlib.py` +
  `tools/fetch_deps.py` 的哈希。

## Issue / PR 运营

建议标签：`bug`、`feature`、`docs`、`hardware`、`qemu`、`security`、
`good first issue`、`needs-repro`、`blocked`。

- 新 issue 24–72 小时内分类；缺版本/日志的请求 `needs-repro` 并给出模板。
- 安全报告转到 GitHub Security Advisories，不在公开 issue 讨论细节。
- 合并前确认：测试、文档、无日志/密钥/third_party、提交信息可读。
- 发布后把 release notes 链接到相关 issue，关闭已修复项。

## 真机回归

- 板卡：ESP32-S3 + USB 串口；当前固定 Windows 工具链。
- 刷写前确认 COM 口与固件哈希；`test.hw_runner` 默认 per-case 复位。
- 两条 SKIP 是平台手段差异，不要为了"全绿"硬改判据；如需覆盖，新增
  等价用例并在 SKIP 原因里引用。
- 新增硬件用例：在 `test/hw_cases/test_hw_*.py` 用 `@hw_case` 注册，
  锚点以 `\r` 结尾，避免半行早匹配。

## 安全与供应链

- 漏洞走私密报告；修复后写 `CHANGELOG` 的 Security 段并在 release notes
  致谢（经报告者同意）。
- `tools/fetch_deps.py` 的 URL/SHA256 是供应链边界；升级工具链必须同时
  更新 URL、大小、SHA256，并在 PR 中贴出上游 Release 链接。
- 不把 `build/`、`test/logs/`、`third_party/`、板卡 MAC、个人路径提交到仓库。

## 社区

- Discussions 用于问答/路线图提案；issue 用于可复现 bug 与具体任务。
- 每月整理一次 `good first issue`；发布后在 Discussions 发一条变更摘要。
- 欢迎真机兼容性报告（板卡型号、Flash/PSRAM、串口芯片、结果）。
