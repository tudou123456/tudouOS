"""真机串口监控工具（带板批）。

用法:
  py -3.14 tools/hw_monitor.py --port COM3 --secs 15
  py -3.14 tools/hw_monitor.py --port COM3 --secs 20 --reset \
      --delay 3 --cmd "ping" --cmd "info"

功能:
  --reset     打开串口后先脉冲 EN（CH340 经 RTS，esptool 同款时序）复位重启
  --delay N   复位/打开后等待 N 秒再发送 --cmd 命令（等 banner）
  --cmd S     可重复；每条命令后等 --gap 秒（默认 1.5s）再发下一条
  --secs N    总采集时长，超时后原样打印全部输出（带时间戳）并退出
退出码 0 = 正常采集完成（不代表内核健康，看输出）。
"""

import argparse
import sys
import time

import serial


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--secs", type=float, default=10.0)
    ap.add_argument("--reset", action="store_true")
    ap.add_argument("--delay", type=float, default=2.0)
    ap.add_argument("--cmd", action="append", default=[])
    ap.add_argument("--gap", type=float, default=1.5)
    args = ap.parse_args()

    ser = serial.Serial(args.port, args.baud, timeout=0.1)
    if args.reset:
        # esptool 经典时序：DTR=IO0, RTS=EN（开发板经三极管反相）。
        # 复位重启：拉 RTS 断言 EN 100ms 后释放；DTR 全程无效（不进下载模式）。
        ser.dtr = False
        ser.rts = True
        time.sleep(0.1)
        ser.rts = False
        print(f"[monitor] {args.port} 已脉冲复位，采集 {args.secs}s ...")

    t0 = time.time()
    deadline = t0 + args.secs
    buf = bytearray()

    next_cmd_at = time.time() + args.delay
    cmds = list(args.cmd)

    while time.time() < deadline:
        chunk = ser.read(4096)
        if chunk:
            buf.extend(chunk)
        if cmds and time.time() >= next_cmd_at:
            c = cmds.pop(0)
            ser.write((c + "\r\n").encode())
            print(f"[monitor] >> {c}")
            next_cmd_at = time.time() + args.gap
        time.sleep(0.01)

    ser.close()
    text = buf.decode("utf-8", errors="replace")
    sys.stdout.write(text)
    if not text.endswith("\n"):
        sys.stdout.write("\n")
    print(f"[monitor] 采集完成 {len(buf)} 字节")
    return 0


if __name__ == "__main__":
    sys.exit(main())
