"""QMP（QEMU Machine Protocol）最小客户端。

QMP 是 QEMU 的 JSON 控制协议：TCP 连上后先收到 greeting，必须先执行
qmp_capabilities 才能用其它命令。异步事件（RESET/SHUTDOWN 等）会插在
命令响应之间，这里统一缓存到 self.events 供测试断言/记录。
"""

import json
import socket
import time


class QMPError(RuntimeError):
    """QEMU 返回 error 响应"""


class QMPClient:
    def __init__(self, host="127.0.0.1", port=4444, timeout=10.0):
        self.host = host
        self.port = port
        self.timeout = timeout
        self.events = []
        self._sock = None
        self._buf = bytearray()

    def connect(self, wait_secs=20.0):
        """带重试的连接：QEMU 进程刚拉起时端口可能还没就绪。"""
        deadline = time.monotonic() + wait_secs
        last_err = None
        while time.monotonic() < deadline:
            try:
                self._sock = socket.create_connection((self.host, self.port), timeout=3.0)
                self._sock.settimeout(self.timeout)
                break
            except OSError as e:
                last_err = e
                time.sleep(0.2)
        else:
            raise ConnectionError(f"QMP {self.host}:{self.port} 连接失败: {last_err}")

        greeting = self._readmsg()
        if "QMP" not in greeting:
            raise QMPError(f"异常的 QMP greeting: {greeting!r}")
        self.command("qmp_capabilities")
        return self

    def _readmsg(self):
        while True:
            idx = self._buf.find(b"\n")
            if idx >= 0:
                break
            chunk = self._sock.recv(4096)
            if not chunk:
                raise ConnectionError("QMP 连接被对端关闭")
            self._buf.extend(chunk)
        line = bytes(self._buf[:idx])
        del self._buf[:idx + 1]
        return json.loads(line.decode("utf-8", errors="replace"))

    def command(self, name, args=None, timeout=None):
        """下发一条 QMP 命令，返回其 return 字段；error 则抛 QMPError。"""
        if timeout is not None:
            self._sock.settimeout(timeout)
        req = {"execute": name}
        if args is not None:
            req["arguments"] = args
        self._sock.sendall(json.dumps(req).encode("utf-8"))
        self._sock.sendall(b"\n")

        while True:
            msg = self._readmsg()
            if "return" in msg:
                return msg["return"]
            if "error" in msg:
                raise QMPError(f"{name}: {msg['error']}")
            if "event" in msg:
                self.events.append(msg)
                continue
            raise QMPError(f"无法识别的 QMP 消息: {msg!r}")

    def close(self):
        if self._sock is not None:
            try:
                self.command("quit", timeout=3.0)
            except Exception:  # noqa: BLE001
                pass
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None
