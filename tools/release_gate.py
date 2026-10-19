"""发布门禁：干净构建 + QEMU 全量回归连续两轮全绿（可选真机回归）。

"全绿"必须是稳定门禁而不是偶然结果：任一轮有失败/超时即非零退出，
日志落在 build/_gate_*.log。

在项目根目录运行（Windows；项目可能位于非 ASCII 路径）::

    py -3.14 tools/release_gate.py                 # QEMU 两轮
    py -3.14 tools/release_gate.py --rounds 3      # 三轮
    py -3.14 tools/release_gate.py --skip-build    # 复用当前构建
    py -3.14 tools/release_gate.py --hw-port COM3  # 另跑真机全量（板子已烧当前固件）

工具链的 binutils 不支持非 ASCII 路径；若当前目录不是 ASCII，请在
README 所述的 ASCII 目录联接（如 D:\\tudouOS）下运行。
真机刷写不在此脚本内自动执行（避免误擦板）；先按 README 烧录再用
``--hw-port`` 跑回归。
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(os.path.abspath(os.getcwd()))


def require_ascii_root() -> Path:
    if str(ROOT).isascii():
        return ROOT
    print("错误: 项目路径含非 ASCII 字符，工具链无法创建目标文件。")
    print("     请按 README 建立目录联接后从联接路径运行:")
    print("     mklink /J D:\\tudouOS \"%s\"" % ROOT)
    sys.exit(2)


def run(cmd: list[str], cwd: Path, log: Path) -> int:
    print("+", " ".join(cmd))
    proc = subprocess.run(cmd, cwd=str(cwd), capture_output=True, text=True,
                          shell=False)
    text = (proc.stdout or "") + (proc.stderr or "")
    log.write_text(text, encoding="utf-8", errors="replace")
    for line in text.splitlines()[-6:]:
        print("  " + line)
    return proc.returncode


def main() -> int:
    ap = argparse.ArgumentParser(description="tudouOS 发布门禁")
    ap.add_argument("--rounds", type=int, default=2,
                    help="QEMU 全量连续轮数（默认 2）")
    ap.add_argument("--skip-build", action="store_true",
                    help="跳过干净构建，复用现有产物")
    ap.add_argument("--hw-port", default="",
                    help="真机串口号（如 COM3）；给出时追加一轮真机全量")
    args = ap.parse_args()
    if args.rounds < 1:
        ap.error("--rounds 必须 >= 1")

    root = require_ascii_root()
    if not (root / "kernel").is_dir() or not (root / "tools" / "build.py").is_file():
        print("错误: 请在项目根目录运行本脚本（当前 %s）" % root)
        return 2
    py = sys.executable
    build_dir = root / "build"
    build_dir.mkdir(exist_ok=True)

    if not args.skip_build:
        rc = run([py, "tools/build.py", "--clean"], root,
                 build_dir / "_gate_build.log")
        if rc != 0:
            print("门禁失败: 干净构建失败")
            return 1

    pat = re.compile(r"结果:\s*(\d+)/(\d+)\s*通过")
    for i in range(1, args.rounds + 1):
        log = build_dir / ("_gate_qemu_run%d.log" % i)
        rc = run([py, "-m", "test.runner"], root, log)
        text = log.read_text(encoding="utf-8", errors="replace")
        m = pat.findall(text)
        if rc != 0 or not m or m[-1][0] != m[-1][1]:
            print("门禁失败: QEMU 第 %d 轮未全绿 (%s)" %
                  (i, m[-1] if m else "无汇总行"))
            return 1
        print("门禁: QEMU 第 %d 轮 %s/%s 通过" % (i, m[-1][0], m[-1][1]))

    if args.hw_port:
        log = build_dir / "_gate_hw.log"
        rc = run([py, "-m", "test.hw_runner", "--port", args.hw_port,
                  "--keep-going"], root, log)
        text = log.read_text(encoding="utf-8", errors="replace")
        m = re.search(r"PASS=(\d+) FAIL=(\d+) TIMEOUT=(\d+)", text)
        if rc != 0 or not m or int(m.group(2)) != 0 or int(m.group(3)) != 0:
            print("门禁失败: 真机回归 (%s)" % (m.group(0) if m else "无汇总"))
            return 1
        print("门禁: 真机 %s" % m.group(0))

    print("发布门禁通过（QEMU %d 轮 + %s）" %
          (args.rounds, ("真机 " + args.hw_port) if args.hw_port else "未跑真机"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
