"""按项目标准参数拉起 QEMU（esp32s3 机器）。

默认交互模式：串口直接接到当前终端，可直接敲内核命令（ping/info/...）。
  python qemu/run_qemu.py

--harness  测试台模式：CPU 先冻结(-S)，UART/QMP 走 TCP 端口，
           由 test/runner.py 连接后放行，保证启动日志零丢失。
--gdb      调试模式：附加 gdbstub(-s -S)，配合同目录工具链里的
           xtensa-esp32s3-elf-gdb 使用（target remote :1234）。
"""

import argparse
import os
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                                "tools"))
import envlib  # noqa: E402

ROOT = envlib.PROJECT_ROOT
DEFAULT_FLASH = os.path.join(ROOT, "build", "flash_image.bin")


def main():
    ap = argparse.ArgumentParser(description="tudouOS QEMU 启动器")
    ap.add_argument("--flash", default=DEFAULT_FLASH, help="flash 镜像路径")
    ap.add_argument("--harness", action="store_true", help="测试台模式（-S + TCP）")
    ap.add_argument("--gdb", action="store_true", help="gdbstub 模式（-s -S）")
    ap.add_argument("--qmp-port", type=int, default=4444)
    ap.add_argument("--serial-port", type=int, default=5555)
    args = ap.parse_args()

    qemu = envlib.find_qemu()
    if not qemu:
        print("未找到 qemu-system-xtensa，先运行 python tools/check_env.py 按提示准备环境。")
        return 1
    if not os.path.isfile(args.flash):
        print(f"未找到 flash 镜像 {args.flash}，先执行 python tools/build.py")
        return 1

    drive_arg = f"file={args.flash},if=mtd,format=raw"
    serial_tcp = f"tcp:127.0.0.1:{args.serial_port},server,nowait"
    qmp_tcp = f"tcp:127.0.0.1:{args.qmp_port},server,nowait"

    if args.harness:
        print(f"harness 模式: 串口 {args.serial_port} / QMP {args.qmp_port}（CPU 冻结待放行）")
        return subprocess.run([qemu, "-machine", "esp32s3", "-drive", drive_arg,
                               "-display", "none", "-monitor", "none", "-S",
                               "-serial", serial_tcp, "-qmp", qmp_tcp],
                              shell=False).returncode
    if args.gdb:
        print(f"gdbstub 模式: gdb 端口 1234 / QMP {args.qmp_port}（CPU 冻结待连接）")
        return subprocess.run([qemu, "-machine", "esp32s3", "-drive", drive_arg,
                               "-display", "none", "-monitor", "none", "-s", "-S",
                               "-serial", "stdio", "-qmp", qmp_tcp],
                              shell=False).returncode

    print(f"交互模式（QMP 端口 {args.qmp_port} 可用于程序化控制；Ctrl-C 退出）\n")
    return subprocess.run([qemu, "-machine", "esp32s3", "-drive", drive_arg,
                           "-display", "none", "-monitor", "none",
                           "-serial", "stdio", "-qmp", qmp_tcp],
                          shell=False).returncode


if __name__ == "__main__":
    sys.exit(main())
