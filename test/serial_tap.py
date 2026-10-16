"""UART 采集器：连接 QEMU 的 TCP 串口，后台线程收字节，支持正则断言。

expect() 从上次匹配结束的位置向后搜索，避免跨用例/跨步骤重复匹配旧输出。
"""

import codecs
import re
import socket
import threading
import time


class SerialTimeoutError(TimeoutError):
    pass


class SerialTap:
    def __init__(self, host="127.0.0.1", port=5555):
        self.host = host
        self.port = port
        self.text = ""
        self.ts_lines = []  # [(monotonic_ts, line)] 行时间戳（时序断言用）
        self._pos = 0
        self._cv = threading.Condition()
        self._sock = None
        self._thread = None
        self._line_buf = ""
        # 增量 UTF-8 解码：多字节字符跨 TCP 分块边界不坏（D-19 验证依赖）
        self._decoder = codecs.getincrementaldecoder("utf-8")(errors="replace")

    def connect(self, wait_secs=20.0):
        deadline = time.monotonic() + wait_secs
        last_err = None
        while time.monotonic() < deadline:
            try:
                self._sock = socket.create_connection((self.host, self.port), timeout=3.0)
                self._sock.settimeout(None)
                break
            except OSError as e:
                last_err = e
                time.sleep(0.2)
        else:
            raise ConnectionError(f"串口 {self.host}:{self.port} 连接失败: {last_err}")

        self._thread = threading.Thread(target=self._reader, daemon=True)
        self._thread.start()
        return self

    def _reader(self):
        while True:
            try:
                data = self._sock.recv(4096)
            except OSError:
                break
            if not data:
                break
            now = time.monotonic()
            with self._cv:
                chunk = self._decoder.decode(data)
                self.text += chunk
                self._line_buf += chunk
                while "\n" in self._line_buf:
                    line, self._line_buf = self._line_buf.split("\n", 1)
                    self.ts_lines.append((now, line.rstrip("\r")))
                self._cv.notify_all()

    def ts_of(self, pattern, last_n=200):
        """返回最近匹配行的 (时间戳, match)，找不到返回 None"""
        import re
        rx = re.compile(pattern)
        for ts, line in reversed(self.ts_lines[-last_n:]):
            m = rx.search(line)
            if m is not None:
                return (ts, m)
        return None

    def expect(self, pattern, timeout=10.0):
        """等待正则出现并返回 Match；超时抛 SerialTimeoutError（附最近输出）。"""
        rx = re.compile(pattern, re.MULTILINE)
        deadline = time.monotonic() + timeout
        with self._cv:
            while True:
                m = rx.search(self.text, self._pos)
                if m is not None:
                    self._pos = m.end()
                    return m
                remain = deadline - time.monotonic()
                if remain <= 0:
                    tail = self.text[-800:]
                    raise SerialTimeoutError(
                        f"等待 {pattern!r} 超时 {timeout}s，最近输出:\n{tail}")
                self._cv.wait(remain)

    def send(self, line):
        self._sock.sendall((line + "\n").encode())

    def close(self):
        if self._sock is not None:
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None
