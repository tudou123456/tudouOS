"""无板自测：用伪串口（FakeSerial）验证 test/hw_serial.py 的收发/断行/过滤/复位逻辑。

不接真机、不占 COM3，可在任何机器上跑::

    py -3.14 -m test.hw_selftest

覆盖点：
1. 后台读线程 + 增量 UTF-8 解码 + "\\r\\n" 行保留行尾 '\\r'、裸 '\\n' 回显行；
2. 提示符 "potato> " 剥离；
3. `@@MCP <crc> <json>` 帧行进 mcp_frames、不出现在匹配文本；
4. expect 游标语义（从上次匹配之后继续、可显式 pos 回看）、超时抛 HwTimeoutError；
5. send 自动追加 "\\r\\n"、send_line_and_wait 从发送前位置匹配；
6. reset() 的 DTR/RTS 时序 + 从复位位置等 READY（旧文本不会误匹配）；
7. released() 让出后重开（伪设备计数）；
8. 完整原始日志 raw_text 保留噪声行。
"""

from __future__ import annotations

import re
import sys
import threading
import time

from test.hw_serial import HwSerial, HwTimeoutError

BOOT = (
    "ESP-ROM:esp32s3-20210327\r\n"
    "rst:0x1 (POWERON),boot:0x8 (SPI_FAST_FLASH_BOOT)\r\n"
    "entry 0x40380460\r\n"
    # 真机实测：potato_printf 行尾是 "\r\r\n"（行尾翻译二次加 \r）
    "[POTATO] hw rtc-cntl: swd-conf=0x80000000 (want bit30)\r\r\n"
    "[POTATO] hw timebase calibrated: cpu=19000000Hz tick_period=19000cc\r\r\n"
    "\r\r\n"
    "[POTATO] tudouOS v0.8.0 (potato kernel) on ESP32-S3\r\r\n"
    "[POTATO] build Oct 19 2026 19:29:00\r\r\n"
    "[POTATO] mem iram=0x40380000-0x40388000 irom(flash)=0x42000020-0x42000000 "
    "data=0x3fca8000-0x3fcb0000 bss=0x3fcb0000-0x3fcc0000\r\r\n"
    "[POTATO] intc+tick(1ms)+trace ready\r\r\n"
    "[POTATO] READY\r\r\n"
    "[POTATO] type 'help' for commands\r\r\n"
    "@@MCP 1a2b {\"pot\":1,\"epoch\":1,\"seq\":0,\"ack\":0,\"type\":\"hello\","
    "\"body\":\"\"}\r\n"
    "potato> "
)


class FakeSerial:
    """最小 pyserial 兼容替身：设备响应用线程异步写入接收缓冲。"""

    def __init__(self, port=None, baudrate=None, timeout=0.05, write_timeout=None):
        self.port = port
        self.baudrate = baudrate
        self.timeout = timeout
        self.dtr = None
        self.rts = None
        self._in = bytearray()
        self._cond = threading.Condition()
        self._open = True
        self._reset_pulses = 0
        self.tx = bytearray()
        self.open_count = 0
        self.close_count = 0

    # -- pyserial API --
    def open(self):
        self._open = True
        self.open_count += 1

    def close(self):
        self._open = False
        self.close_count += 1

    def reset_input_buffer(self):
        with self._cond:
            self._in.clear()

    def flush(self):
        pass

    @property
    def in_waiting(self):
        with self._cond:
            return len(self._in)

    def read(self, n=1):
        with self._cond:
            if not self._in:
                self._cond.wait(self.timeout or 0.05)
            if not self._in:
                return b""
            take = bytes(self._in[:n])
            del self._in[:n]
            return take

    def write(self, data):
        self.tx.extend(data)
        return len(data)

    # -- 伪设备行为 --
    def feed(self, text: str):
        with self._cond:
            self._in.extend(text.encode("utf-8"))
            self._cond.notify_all()

    @property
    def rts(self):  # noqa: F811 - 属性语义：写 False 视为复位脉冲
        return self._rts

    @rts.setter
    def rts(self, value):
        prev = getattr(self, "_rts", None)
        self._rts = value
        if prev is False and value is True:
            self._reset_pulses += 1
            self.feed(BOOT)
            # 模拟命令回显 + 应答
        if prev is True and value is False:
            pass

    dtr = None


def _pump_commands(ser: FakeSerial, stop: threading.Event):
    """伪设备命令循环：把收到的行回显+应答，覆盖 hw_selftest 用到的命令。"""
    seen = 0
    while not stop.is_set():
        data = bytes(ser.tx)
        if len(data) == seen:
            time.sleep(0.01)
            continue
        chunk = data[seen:]
        seen = len(data)
        for line in chunk.decode("utf-8", "replace").split("\r\n"):
            if not line:
                continue
            ser.feed("potato> " + line + "\n")
            reply = _reply(line)
            if reply:
                ser.feed(reply)


def _reply(line: str) -> str:
    if line == "ping":
        return "pong\r\n"
    if line == "info":
        return ("[POTATO] info: tudouOS v0.8.0 kernel=potato target=ESP32-S3\r\n")
    if line == "tickstat":
        return ("[POTATO][TICK] ticks=42 cpu_hz=19000000 now_us=0x2a "
                "jit_min=843us jit_max=45169us uart_irq=7 rx_bytes=99\r\n")
    if line == "help":
        return ("commands: help, ping, echo <text>, info, tickstat, trace, "
                "taskdemo, msgstat, memstat, pal, drivers, fs, mcp, smp\r\n")
    return ""


CHECKS: list[str] = []


def check(cond, msg):
    if not cond:
        raise AssertionError(msg)
    CHECKS.append(msg)


def main() -> int:
    holders: dict[str, FakeSerial] = {}

    def factory(port, baud, timeout=None, write_timeout=None):
        ser = FakeSerial(port, baud, timeout=timeout, write_timeout=write_timeout)
        holders["serial"] = ser
        return ser

    hw = HwSerial("FAKE", baud=115200, serial_factory=factory)
    stop = threading.Event()
    hw.open()
    ser = holders["serial"]
    pump = threading.Thread(target=_pump_commands, args=(ser, stop), daemon=True)
    pump.start()

    # 1) 复位时序 + READY（READY 必须来自本次复位后的新启动日志）
    ready_before = hw.text().count("[POTATO] READY")
    hw.reset(ready_timeout=5.0)
    check(ser._reset_pulses == 1, "reset() 产生一次 RTS 复位脉冲")
    check("[POTATO] READY\r" in hw.text(), "复位后匹配到带 \\r 的 READY 行")
    check(hw.text().count("[POTATO] READY") == ready_before + 1,
          "READY 只来自本次复位的新启动日志")

    # 2) 提示符剥离 + 行尾 \r 保留 + 裸 \n 回显行
    check(re.search(r"\[POTATO\] tudouOS v(\d+\.\d+\.\d+) \(potato kernel\) "
                    r"on ESP32-S3\r", hw.text()) is not None,
          "banner 行以 \\r 结尾可被锚点命中")
    check(not any(x.startswith("potato>") for x in hw.lines),
          "匹配文本中不应残留 'potato> ' 提示符")
    check(any(x == "[POTATO] READY\r" for x in hw.lines),
          "CRLF 行保留行尾 \\r（防半行误匹配）")

    # 3) @@MCP 帧被过滤进 mcp_frames
    check(len(hw.mcp_frames) >= 1, "收到 @@MCP 帧")
    frame = hw.mcp_frames[0]
    check(frame["json"]["type"] == "hello", "MCP 帧 JSON 可解析（hello）")
    check(all(not line.startswith("@@MCP ") for line in hw.lines),
          "@@MCP 帧不出现在匹配文本里")
    check(any(line.startswith("@@MCP ") for line in hw.raw_lines),
          "raw_lines 保留 @@MCP 原始行（失败取证用）")

    # 4) send + send_line_and_wait + expect 游标
    ser.feed("potato> ")
    hw.send("ping")
    check(ser.tx.endswith(b"ping\r\n"), "send 自动追加 \\r\\n")
    hw.send_line_and_wait("info", r"\[POTATO\] info: [^\r]*\r")
    check("target=ESP32-S3" in hw.text(), "info 应答整行可读")

    # 5) 游标从上次匹配之后继续（旧文本不重复命中）+ 显式 pos 回看
    hw.send_line_and_wait("ping", r"pong\r")
    count_after = len(re.findall(r"pong\r", hw.text()))
    hw.send_line_and_wait("ping", r"pong\r")
    check(len(re.findall(r"pong\r", hw.text())) == count_after + 1,
          "expect 游标推进后只命中新 pong")
    hw.expect(r"\[POTATO\] READY\r", 1.0, pos=0)          # 显式回看启动日志
    check(True, "expect(pos=0) 可回看历史")

    # 6) tickstat 解析（真机用例共享锚点）
    hw.send_line_and_wait("tickstat", r"\[POTATO\]\[TICK\][^\r]*\r", 3.0)
    check("[POTATO][TICK] ticks=42 cpu_hz=19000000" in hw.text(),
          "tickstat 行字段齐全")

    # 7) 超时抛 HwTimeoutError（含 pattern/tail）
    try:
        hw.expect(r"\[THIS_WILL_NEVER_APPEAR\]", 0.3)
    except HwTimeoutError as exc:
        check("THIS_WILL_NEVER_APPEAR" in str(exc.pattern), "超时异常含锚点")
        check(isinstance(exc.timeout, float), "超时异常含 timeout")
    else:
        raise AssertionError("expect 未抛 HwTimeoutError")

    # 8) released()：端口让出后重开 + 复位
    with_hw_closed = {"ok": False}

    with hw.released():
        with_hw_closed["ok"] = not hw.is_open
        check(hw.is_open is False, "released() 内端口已关闭（让给子进程）")
    check(with_hw_closed["ok"] and hw.is_open, "released() 退出后重开端口")
    ser2 = holders["serial"]
    check(ser2 is not ser and ser2._reset_pulses == 1,
          "released() 重开新端口并自动复位一次")

    hw.close()
    stop.set()
    check(hw.raw_text().count("[POTATO] READY") >= 2, "完整原始日志记录多次启动")

    print("hw_selftest: PASS (%d checks)" % len(CHECKS))
    for c in CHECKS:
        print("  - %s" % c)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as exc:
        print("hw_selftest: FAIL - %s" % exc)
        sys.exit(1)
