"""UART 采集器：连接 QEMU 的 TCP 串口，后台线程收字节，支持正则断言。

expect() 从上次匹配结束的位置向后搜索，避免跨用例/跨步骤重复匹配旧输出。
"""

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
        self._pos = 0
        self._cv = threading.Condition()
        self._sock = None
        self._thread = None

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
            with self._cv:
                self.text += data.decode("utf-8", errors="replace")
                self._cv.notify_all()

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
