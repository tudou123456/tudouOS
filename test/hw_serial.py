"""tudouOS 真机串口控制台驱动（ESP32-S3, USB 串口 COM3 @ 115200）。

与 QEMU 台架 test/serial_tap.py 的 expect/wait_line 语义对齐，但面向真实 UART：

- 后台读线程：打开串口后持续收字节、增量 UTF-8 解码，按 '\n'（或裸 '\r'）断行；
  设备日志以 "\r\n" 结束，行文本中**保留行尾 '\r'**，因此用例锚点应以 '\r'
  结尾，避免半行误匹配（与 QEMU 回归台一致）。
- 过滤两类噪声：
  * 串口 MCP 帧行（`@@MCP <crc16> <json>`，新固件 netmcp serial 传输的输出），
    从匹配文本中剔除，同时解析后存进 self.mcp_frames 供取证；
  * 控制台提示符 `potato> `（它不带换行，会与下一条输出/命令回显拼在同一行），
    在行首出现时剥离。
- 全量原始行（含被过滤行）保留在 self.raw_lines / self.raw_text，用例失败时由
  runner 落盘到 test/logs/hw/<case>.log。
- 复位时序与 tools/hw_monitor.py 一致（DTR=IO0 / RTS=EN）：
      dtr=False; rts=True; sleep(0.1); rts=False
- 线程安全：expect/send/reset 可在读线程运行时由主线程调用；close() 幂等。
- 独占资源：M7 用例需要临时把 COM3 让给 tools/serial_mcp.py 子进程时用
  `with hw.released(): subprocess.run(...)`：上下文内驱动关闭端口，退出后
  重新打开、复位、等 READY。
"""

from __future__ import annotations

import codecs
import contextlib
import json
import re
import threading
import time

try:
    import serial  # pyserial 3.5
except ImportError as exc:  # pragma: no cover - 环境缺少 pyserial 时给出可读错误
    raise SystemExit("需要 pyserial：py -3.14 -m pip install pyserial") from exc


DEFAULT_PORT = "COM3"
DEFAULT_BAUD = 115200

PROMPT = "potato> "
READY_RE = r"\[POTATO\] READY\r"
MCP_FRAME_RE = re.compile(r"^@@MCP (?:([0-9a-fA-F]{4}) )?(.*)$")
# 真机 potato_printf 行尾是 "\r\r\n"（"\r\n" 被行尾翻译二次加 \r），归一化用
_CRLF_FIX_RE = re.compile(r"\r{2,}\n")

#: 复位脉冲参数（与 tools/hw_monitor.py 一致）
RESET_SETTLE = 0.15   # DTR=IO0 拉低的保持时间
RESET_PULSE = 0.10    # RTS=EN 拉低的保持时间


class HwSerialError(RuntimeError):
    """串口/驱动层错误（含串口被占用、读线程异常）。"""


class HwTimeoutError(HwSerialError):
    """expect/wait_line 超时；tail 保留超时前最后一段可匹配文本便于取证。"""

    def __init__(self, pattern: str, timeout: float, tail: str):
        self.pattern = pattern
        self.timeout = timeout
        self.tail = tail
        super().__init__(
            "超时 %.1fs 未匹配锚点 %r；最后输出：\n%s" % (timeout, pattern, tail)
        )


class HwSerial:
    r"""真机串口控制台。典型用法::

        hw = HwSerial("COM3")
        hw.open()
        hw.reset()                 # 复位 + 等 READY
        m = hw.send_line_and_wait("info", r"\[POTATO\] info: ")
        hw.close()
    """

    def __init__(
        self,
        port: str = DEFAULT_PORT,
        baud: int = DEFAULT_BAUD,
        log_path: str | None = None,
        serial_factory=None,
    ):
        self.port = port
        self.baud = baud
        self.log_path = log_path
        #: 串口类工厂（默认 pyserial.serial.Serial）；test/hw_selftest.py 注入伪设备
        #: 做无板自测，生产路径不传。
        self.serial_factory = serial_factory

        self._serial = None
        self._thread: threading.Thread | None = None
        self._stop = threading.Event()
        self._read_error: BaseException | None = None
        self._closed = True

        self._cond = threading.Condition()
        self._lines: list[str] = []          # 过滤后的行（保留行尾 '\r'）
        self._raw_lines: list[str] = []      # 全量行（含 @@MCP / 提示符噪声）
        self._tx_lines: list[str] = []
        self._mcp_frames: list[dict] = []    # 串口 MCP 帧（json 解析成功部分）
        self._buf = ""
        self._decoder = codecs.getincrementaldecoder("utf-8")("replace")
        self._pos = 0                        # expect 的游标（过滤文本字符偏移）
        self._rx_bytes = 0

    # ------------------------------------------------------------------ 生命周期
    @property
    def is_open(self) -> bool:
        return self._serial is not None and not self._closed

    def open(self, *, reset_on_open: bool = False) -> "HwSerial":
        if self.is_open:
            return self
        factory = self.serial_factory or serial.Serial
        try:
            ser = factory(self.port, self.baud, timeout=0.05, write_timeout=2.0)
        except Exception as exc:
            raise HwSerialError(
                "打开串口 %s 失败（可能被其它进程占用：烧录/串口工具/另一个回归实例）：%s"
                % (self.port, exc)
            ) from exc
        # 保持 IO0=高、EN=高：不触发复位，让板子正常跑；需要复位时显式 reset()
        with contextlib.suppress(Exception):
            ser.dtr = False
            ser.rts = False
        self._serial = ser
        self._closed = False
        self._stop.clear()
        self._read_error = None
        self._decoder.reset()
        self._buf = ""
        self._thread = threading.Thread(
            target=self._reader, name="hw-serial-rx", daemon=True
        )
        self._thread.start()
        return self

    def close(self) -> None:
        """线程安全关闭（幂等）。未关闭时 flush 完整日志到 log_path。"""
        if self._closed:
            return
        self._closed = True
        self._stop.set()
        with self._cond:
            self._cond.notify_all()
        t = self._thread
        if t is not None and t is not threading.current_thread():
            t.join(timeout=2.0)
        ser = self._serial
        self._serial = None
        if ser is not None:
            with contextlib.suppress(Exception):
                ser.close()
        self._thread = None
        if self.log_path:
            self.dump_log()

    @contextlib.contextmanager
    def released(self, *, reset_after: bool = True, ready_timeout: float = 8.0):
        """临时让出 COM3（给 tools/serial_mcp.py 子进程），退出后重开+复位+等 READY。"""
        self.close()
        try:
            yield
        finally:
            self.open()
            if reset_after:
                self.reset(ready_timeout=ready_timeout)

    def __enter__(self) -> "HwSerial":
        return self.open()

    def __exit__(self, *exc_info) -> None:
        self.close()

    # ------------------------------------------------------------------ 原始读线程
    def _reader(self) -> None:
        try:
            while not self._stop.is_set():
                ser = self._serial
                if ser is None:
                    break
                try:
                    n = ser.in_waiting
                except Exception:
                    n = 0
                try:
                    data = ser.read(n if n > 0 else 1)
                except Exception as exc:  # 拔线/端口被抢
                    if self._stop.is_set():
                        break
                    self._read_error = exc
                    with self._cond:
                        self._cond.notify_all()
                    break
                if not data:
                    continue
                self._rx_bytes += len(data)
                text = self._decoder.decode(data)
                if text:
                    self._feed(text)
        except Exception as exc:  # pragma: no cover - 防御性兜底
            self._read_error = exc
        finally:
            with self._cond:
                self._cond.notify_all()

    def _feed(self, text: str) -> None:
        self._buf += text
        # 真机 UART 输出：内核 potato_printf 的 "\r\n" 会被行尾翻译成
        # "\r" + "\r\n" = "\r\r\n"（真机实测）。把 2+ 个 '\r'
        # 后接 '\n' 归一化为单个 "\r\n"，让锚点统一用行尾 '\r'。
        if "\r\r" in self._buf:
            self._buf = _CRLF_FIX_RE.sub("\r\n", self._buf)
        while True:
            cut = self._find_line_cut()
            if cut is None:
                break
            end, term_len, keep_cr = cut
            if keep_cr:
                raw = self._buf[: end + 1]
            else:
                raw = self._buf[:end]
            self._buf = self._buf[end + term_len:]
            self._append_line(raw)
        with self._cond:
            self._cond.notify_all()

    def _find_line_cut(self):
        """返回 (终止符下标, 终止符长度, 是否保留 '\r')；None 表示尚无完整行。

        真机 UART 上有两种行尾：
          - ROM/回显行："...\\n"
          - 内核 potato_printf："...\\r\\r\\n"（"\r\n" 被行尾翻译二次加 '\r'）
        后者即使被 USB 分片成 "..\\r" / "\\r" / "\\n"，也要等齐第三个字节再断行，
        否则会把 "\\r\\r" 当成裸 CR 断行，丢掉行尾 '\r'（真机实测）。
        """
        buf = self._buf
        n = len(buf)
        i = 0
        while i < n:
            ch = buf[i]
            if ch == "\n":
                return (i, 1, False)          # 裸 '\n'（命令回显行）
            if ch == "\r":
                if i + 1 >= n:
                    return None               # 末尾孤立 '\r'，等后续字节
                if buf[i + 1] == "\n":
                    return (i, 2, True)       # "\r\n"：行文本保留 '\r'
                if buf[i + 1] == "\r":
                    if i + 2 >= n:
                        return None           # "\r\r" 在末尾：等第三个字节确认
                    if buf[i + 2] == "\n":
                        return (i, 3, True)   # "\r\r\n"：行文本保留第一个 '\r'
                return (i, 1, False)          # 裸 '\r' 断行
            i += 1
        return None

    def _append_line(self, raw: str) -> None:
        with self._cond:
            self._raw_lines.append(raw)
            line = raw
            if line.startswith(PROMPT):
                line = line[len(PROMPT):]
            if not line.strip():
                return
            m = MCP_FRAME_RE.match(line)
            if m:
                crc, payload = m.group(1), m.group(2).rstrip("\r")
                frame = {"crc": crc, "raw": payload}
                with contextlib.suppress(ValueError):
                    frame["json"] = json.loads(payload)
                self._mcp_frames.append(frame)
                return
            self._lines.append(line)

    # ------------------------------------------------------------------ 日志/快照
    @property
    def lines(self) -> list[str]:
        with self._cond:
            return list(self._lines)

    @property
    def raw_lines(self) -> list[str]:
        with self._cond:
            return list(self._raw_lines)

    @property
    def mcp_frames(self) -> list[dict]:
        with self._cond:
            return list(self._mcp_frames)

    @property
    def rx_bytes(self) -> int:
        return self._rx_bytes

    def text(self) -> str:
        """过滤后的匹配文本（行间以 '\\n' 连接，行内保留原始 '\\r'）。"""
        with self._cond:
            return "\n".join(self._lines)

    def raw_text(self) -> str:
        with self._cond:
            return "\n".join(self._raw_lines)

    def text_since(self, pos: int) -> str:
        return self.text()[pos:]

    def latest(self, n: int = 20) -> str:
        with self._cond:
            return "\n".join(self._lines[-n:])

    def mark(self) -> int:
        """返回当前匹配文本长度，作为后续 expect/证据切片的起点。"""
        with self._cond:
            return len("\n".join(self._lines))

    def dump_log(self) -> None:
        if not self.log_path:
            return
        try:
            with open(self.log_path, "w", encoding="utf-8", newline="") as fh:
                fh.write(self.raw_text())
                fh.write("\n")
        except OSError:
            pass

    # ------------------------------------------------------------------ 发送
    def _ensure_open(self) -> None:
        if not self.is_open:
            raise HwSerialError("串口未打开（先 open()/reset()）")
        if self._read_error is not None:
            raise HwSerialError("串口读线程异常：%r" % (self._read_error,))

    def send(self, line: str) -> None:
        """发送一行控制台命令（自动补 '\\r\\n'，与 tools/hw_monitor.py 一致）。"""
        self._ensure_open()
        data = (line + "\r\n").encode("utf-8")
        with contextlib.suppress(Exception):
            self._serial.write(data)
            self._serial.flush()
        with self._cond:
            self._tx_lines.append(line)
            self._cond.notify_all()

    def send_raw(self, data: bytes) -> None:
        """发送原始字节（M7 串口帧测试用：STX + '@@MCP ...' + '\\r\\n'）。"""
        self._ensure_open()
        with contextlib.suppress(Exception):
            self._serial.write(data)
            self._serial.flush()
        with self._cond:
            self._tx_lines.append("<raw %d bytes>" % len(data))
            self._cond.notify_all()

    # ------------------------------------------------------------------ 匹配
    def expect(self, pattern, timeout: float = 3.0, pos: int | None = None):
        """等待正则/子串出现，返回 re.Match；游标从 pos（默认当前游标）开始搜索。"""
        rx = re.compile(pattern, re.MULTILINE) if isinstance(pattern, str) else pattern
        deadline = time.monotonic() + timeout
        with self._cond:
            if pos is not None:
                self._pos = pos
            while True:
                if self._read_error is not None and not self._stop.is_set():
                    raise HwSerialError("串口读线程异常：%r" % (self._read_error,))
                text = "\n".join(self._lines)
                m = rx.search(text, self._pos)
                if m:
                    self._pos = m.end()
                    return m
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise HwTimeoutError(
                        pattern if isinstance(pattern, str) else pattern.pattern,
                        timeout,
                        text[-1800:],
                    )
                self._cond.wait(min(remaining, 0.1))

    def expect_line(self, pattern, timeout: float = 3.0) -> str:
        """等待并返回命中锚点所在的整行文本（保留行尾 '\\r'）。"""
        m = self.expect(pattern, timeout)
        text = self.text()
        start = text.rfind("\n", 0, m.start()) + 1
        end = text.find("\n", m.end())
        return text[start:] if end < 0 else text[start:end]

    # 语义别名（任务书用 wait_line 命名）
    wait_line = expect_line

    def send_line_and_wait(self, command: str, pattern, timeout: float = 3.0):
        """发送命令并从发送前位置等待锚点，返回 re.Match。"""
        pos = self.mark()
        self.send(command)
        return self.expect(pattern, timeout, pos=pos)

    # 语义别名（任务书用 send_line_and_wait 的简写）
    cmd = send_line_and_wait

    # ------------------------------------------------------------------ 复位/就绪
    def reset(self, *, wait: bool = True, ready_timeout: float = 8.0):
        """DTR/RTS 复位（DTR=IO0，RTS=EN），复位后从新位置等 READY。

        返回 READY 的 re.Match（wait=False 时返回 None）。
        """
        self._ensure_open()
        with contextlib.suppress(Exception):
            self._serial.reset_input_buffer()
        pos = self.mark()
        with contextlib.suppress(Exception):
            self._serial.dtr = False        # IO0 = 高（正常启动）
        time.sleep(RESET_SETTLE)
        with contextlib.suppress(Exception):
            self._serial.rts = True         # EN = 低 → 复位
        time.sleep(RESET_PULSE)
        with contextlib.suppress(Exception):
            self._serial.rts = False        # EN = 高 → 释放
        if not wait:
            return None
        return self.expect(READY_RE, ready_timeout, pos=pos)

    def wait_ready(self, timeout: float = 8.0):
        """等待 `[POTATO] READY` 行（必须带行尾 '\\r'，防半行误匹配）。"""
        return self.expect(READY_RE, timeout)

    def wait_boot(self, timeout: float = 8.0):
        """等 READING + banner：返回 (banner 行, build 行)。"""
        banner = self.expect_line(r"\[POTATO\] tudouOS v[^\r]*\r", timeout)
        build = self.expect_line(r"\[POTATO\] build [^\r]*\r", timeout)
        return banner, build
