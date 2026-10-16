"""测试台运行器。

每个用例独立拉起一次 QEMU（-S 冻结启动 → 连 QMP/串口 → 放行 CPU），
用例函数通过 ctx.serial / ctx.qmp 与虚拟机交互；结束后把串口全记录、
QEMU 输出和 QMP 事件落盘到 test/logs/，并打印汇总。

用法:
  python -m test.runner                 # 跑全部用例
  python -m test.runner --filter reset  # 只跑名字含 reset 的用例
  python -m test.runner --list
"""

import argparse
import glob
import importlib.util
import json
import os
import socket
import subprocess
import sys
import threading
import time
import traceback
from pathlib import Path

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
sys.path.insert(0, os.path.join(ROOT, "tools"))

import envlib  # noqa: E402
from test.qmp_client import QMPClient  # noqa: E402
from test.serial_tap import SerialTap  # noqa: E402

LOGDIR = Path(ROOT) / "test" / "logs"
DEFAULT_FLASH = os.path.join(ROOT, "build", "flash_image.bin")


def log_path(case_name, suffix):
    """用例名净化为纯文件名成分，日志只落在 test/logs/ 内。"""
    clean = "".join(ch if (ch.isalnum() or ch in "_-") else "_" for ch in case_name)
    fname = os.path.basename(clean + suffix)
    if fname != clean + suffix:
        raise ValueError("非法日志文件名: " + repr(case_name))
    return LOGDIR / fname


def free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class _PipeDrain(threading.Thread):
    """后台线程：持续读 QEMU 的 stdout/stderr，攒到内存待落盘。"""

    def __init__(self, pipe):
        super().__init__(daemon=True)
        self.pipe = pipe
        self.chunks = []

    def run(self):
        while True:
            data = self.pipe.read(4096)
            if not data:
                break
            self.chunks.append(data)

    def bytes(self):
        return b"".join(self.chunks)


class VMInstance:
    """一台专属 QEMU 虚拟机：动态端口，日志齐全，用完即毁。"""

    def __init__(self, qemu, flash, case_name):
        self.qemu = qemu
        self.flash = flash
        self.qmp_port = free_port()
        self.serial_port = free_port()
        self.proc = None
        self.qmp = None
        self.serial = None
        self.drain = None
        self.qemu_log_path = log_path(case_name, ".qemu.log")

    def start(self):
        drive_arg = f"file={self.flash},if=mtd,format=raw"
        serial_tcp = f"tcp:127.0.0.1:{self.serial_port},server,nowait"
        qmp_tcp = f"tcp:127.0.0.1:{self.qmp_port},server,nowait"
        self.proc = subprocess.Popen(
            [self.qemu, "-machine", "esp32s3", "-drive", drive_arg,
             "-display", "none", "-monitor", "none", "-S",
             "-serial", serial_tcp, "-qmp", qmp_tcp],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, shell=False)
        self.drain = _PipeDrain(self.proc.stdout)
        self.drain.start()

        # 先连 QMP 与串口，再放行 CPU，保证启动日志零丢失
        self.qmp = QMPClient(port=self.qmp_port).connect()
        self.serial = SerialTap(port=self.serial_port).connect()
        self.qmp.command("cont")

    def stop(self):
        if self.qmp is not None:
            self.qmp.close()
            self.qmp = None
        if self.serial is not None:
            self.serial.close()
            self.serial = None
        if self.proc is not None:
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
            self.proc = None
        if self.drain is not None:
            self.drain.join(timeout=2)
            self.qemu_log_path.write_bytes(self.drain.bytes())
            self.drain = None


class CaseContext:
    def __init__(self, serial, qmp, vm):
        self.serial = serial
        self.qmp = qmp
        self.vm = vm


def discover_cases():
    cases = []
    pattern = os.path.join(ROOT, "test", "cases", "test_*.py")
    for path in sorted(glob.glob(pattern)):
        stem = os.path.splitext(os.path.basename(path))[0]
        spec = importlib.util.spec_from_file_location("cases." + stem, path)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        for attr in dir(mod):
            if attr.startswith("test_"):
                fn = getattr(mod, attr)
                if callable(fn):
                    cases.append(fn)
    return cases


def dump_logs(vm, case_name, error, tb=None):
    if vm.serial is not None:
        log_path(case_name, ".serial.log").write_text(vm.serial.text, encoding="utf-8")
    events = vm.qmp.events if vm.qmp is not None else []
    lines = "".join(json.dumps(e, ensure_ascii=False) + "\n" for e in events)
    log_path(case_name, ".qmp-events.json").write_text(lines, encoding="utf-8")
    if error is not None:
        # traceback 必须在 except 块里抓（事后 format_exc 只剩 NoneType: None）
        log_path(case_name, ".error.log").write_text(error + "\n" + (tb or ""),
                                                     encoding="utf-8")


def main():
    ap = argparse.ArgumentParser(description="tudouOS QMP 测试台")
    ap.add_argument("--filter", default="", help="只跑名字包含该子串的用例")
    ap.add_argument("--list", action="store_true", help="仅列出用例")
    ap.add_argument("--flash", default=DEFAULT_FLASH)
    args = ap.parse_args()

    cases = [fn for fn in discover_cases() if args.filter in fn.__name__]
    if args.list:
        for fn in cases:
            print(fn.__name__)
        return 0

    qemu = envlib.find_qemu()
    if not qemu:
        print("未找到 qemu-system-xtensa，先运行 python tools/check_env.py。")
        return 2
    if not os.path.isfile(args.flash):
        print(f"未找到 flash 镜像 {args.flash}，先执行 python tools/build.py")
        return 2

    LOGDIR.mkdir(parents=True, exist_ok=True)
    print(f"测试台: {len(cases)} 个用例，日志目录 test/logs/\n")

    failed = 0
    for fn in cases:
        name = fn.__name__
        print(f"--- RUN  {name}")
        vm = VMInstance(qemu, args.flash, name)
        t0 = time.monotonic()
        error = None
        tb = None
        try:
            vm.start()
            fn(CaseContext(vm.serial, vm.qmp, vm))
        except Exception as e:  # noqa: BLE001
            error = f"{type(e).__name__}: {e}"
            tb = traceback.format_exc()
        finally:
            dump_logs(vm, name, error, tb)
            vm.stop()
        secs = time.monotonic() - t0
        if error is None:
            print(f"--- PASS {name}  ({secs:.1f}s)\n")
        else:
            failed += 1
            print(f"--- FAIL {name}  ({secs:.1f}s)\n      {error}\n")

    print("=" * 50)
    total = len(cases)
    print(f"结果: {total - failed}/{total} 通过" + ("" if failed == 0 else f"，{failed} 失败"))
    if failed == 0:
        print("全部通过！日志见 test/logs/")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
