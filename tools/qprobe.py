"""QEMU 串口探针：拉起一台 QEMU，按脚本交互，落盘串口全文。

用途：现场取证与底层探针（寄存器/异常/MMU/循环/指令行为），
不动 test/ 正式用例。脚本格式（每行一条）：
  send <行内容>       发送一行（自动补 \r\n）
  expect <正则> [秒]  等待正则出现（默认 10 秒，超时打印已收文本并退出码 2）
  sleep <秒>          静默等待
  mark <文本>         在输出文本里插一段标记（便于分段阅读）

用法:
  python tools/qprobe.py probe_script.txt out.serial.log
  python tools/qprobe.py -          # 脚本从 stdin 读
"""

import argparse
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tools"))

import envlib  # noqa: E402
from test.qmp_client import QMPClient  # noqa: E402
from test.serial_tap import SerialTap  # noqa: E402


def free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def main():
    ap = argparse.ArgumentParser(description="tudouOS QEMU 串口探针")
    ap.add_argument("script", help="探针脚本路径（- 表示 stdin）")
    ap.add_argument("out", help="串口全文落盘路径")
    ap.add_argument("--flash", default=str(ROOT / "build" / "flash_image.bin"))
    args = ap.parse_args()

    lines = (sys.stdin if args.script == "-"
             else Path(args.script).open(encoding="utf-8")).read().splitlines()
    lines = [l.strip() for l in lines if l.strip() and not l.startswith("#")]

    qemu = envlib.find_qemu()
    if not qemu:
        print("未找到 qemu-system-xtensa")
        return 2

    qmp_port, serial_port = free_port(), free_port()
    proc = subprocess.Popen(
        [qemu, "-machine", "esp32s3",
         "-drive", f"file={args.flash},if=mtd,format=raw",
         "-display", "none", "-monitor", "none", "-S",
         "-serial", f"tcp:127.0.0.1:{serial_port},server,nowait",
         "-qmp", f"tcp:127.0.0.1:{qmp_port},server,nowait"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, shell=False)

    chunks = []

    def drain():
        while True:
            data = proc.stdout.read(4096)
            if not data:
                break
            chunks.append(data)

    t = threading.Thread(target=drain, daemon=True)
    t.start()

    qmp = QMPClient(port=qmp_port).connect()
    serial = SerialTap(port=serial_port).connect()
    qmp.command("cont")

    marks = []
    t0 = time.monotonic()
    try:
        for ln in lines:
            op, _, rest = ln.partition(" ")
            if op == "send":
                serial.send(rest)
            elif op == "expect":
                parts = rest.rsplit(" ", 1)
                pat = parts[0]
                tmo = float(parts[1]) if len(parts) == 2 and parts[1].isdigit() else 10.0
                serial.expect(pat, timeout=tmo)
            elif op == "sleep":
                time.sleep(float(rest))
            elif op == "mark":
                marks.append(f"[{time.monotonic() - t0:.1f}s] {rest}")
            else:
                print(f"未知指令: {ln}")
                return 2
        print("探针脚本执行完毕")
    except Exception as e:  # noqa: BLE001
        print(f"探针失败: {type(e).__name__}: {e}")
        rc = 2
    else:
        rc = 0
    finally:
        body = serial.text
        if marks:
            body += "\n\n===== PROBE MARKS =====\n" + "\n".join(marks) + "\n"
        Path(args.out).write_text(body, encoding="utf-8")
        qmp.close()
        serial.close()
        proc.kill()
        proc.wait()
        t.join(timeout=2)
    print(f"串口全文已写入 {args.out}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
