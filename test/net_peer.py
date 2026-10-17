"""远程 MCP 通道的对端（宿主侧 UDP peer，M7 验收用）。

为什么需要它：内核的远程 MCP 通道（kernel/net/netmcp.c）是「设备主动连
服务器」形态——QEMU SLIRP 的 hostfwd 只支持 TCP，所以通道由内核侧发起
（周期 hello 到 10.0.2.2:<port>），宿主侧本模块负责：

  * 绑定一个宿主 UDP 端口（测试台动态分配，端口号经串口告诉内核：
    `netmcp peer 10.0.2.2 <port>`）；
  * 收内核的 hello / resp 信封，记下「宿主视角的源地址」（SLIRP NAT 后的
    地址，回包必须发到它）；
  * 发 req（MCP JSON-RPC 行，如 tools/list、tools/call）与 ack；
  * 提供 epoch/seq/ack 与 body 的解析、去重校验与等待原语。

信封（内核 ↔ 对端，单行 JSON）::

    {"pot":1,"epoch":E,"seq":S,"ack":A,"type":"hello|req|resp|ack|resync",
     "body":"<JSON-RPC 单行>"}

语义：
  * epoch —— 链路代数；内核每次 down→up 递增。跨纪元残包被内核丢弃。
  * seq   —— 请求由对端编号（单调递增）；内核按 (epoch,seq) 去重，
             重复请求只重放缓存响应，不重复执行。
  * ack   —— 对端已确认的内核响应序号（累积），内核据此丢弃重放槽。

本模块只依赖标准库。用 fake 时钟不用；所有超时用 time.monotonic。
"""

import json
import socket
import time

MAGIC = 1
DEFAULT_GUEST_IP = "10.0.2.2"   # SLIRP 里的「宿主」地址


class Envelope:
    __slots__ = ("epoch", "seq", "ack", "type", "body", "raw", "addr")

    def __init__(self, obj, raw, addr):
        self.epoch = int(obj.get("epoch", 0))
        self.seq = int(obj.get("seq", 0))
        self.ack = int(obj.get("ack", 0))
        self.type = str(obj.get("type", ""))
        self.body = obj.get("body", "")
        self.raw = raw
        self.addr = addr

    def json(self):
        try:
            return json.loads(self.body)
        except Exception:  # noqa: BLE001 - body 非 JSON 时由调用方判失败
            return None

    def __repr__(self):
        return (f"Env(type={self.type!r}, epoch={self.epoch}, seq={self.seq}, "
                f"ack={self.ack}, body={self.body[:60]!r})")


class Peer:
    """宿主侧 UDP 对端：绑定端口、收内核信封、发请求。"""

    def __init__(self, bind_host="0.0.0.0", timeout=5.0):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind((bind_host, 0))
        self.sock.settimeout(0.2)
        self.port = self.sock.getsockname()[1]
        self.timeout = timeout
        self.guest_addr = None      # (ip, port)：内核在宿主视角的源地址
        self.received = []          # 收到的所有信封（按到达顺序）
        self.acks_sent = 0
        self.seq_next = 1
        self.epoch_next = 1
        self.log = []

    # ---- 收 ----
    def recv_one(self):
        try:
            data, addr = self.sock.recvfrom(4096)
        except socket.timeout:
            return None
        except OSError:
            return None
        try:
            obj = json.loads(data.decode("utf-8", "replace"))
        except Exception:  # noqa: BLE001
            self.log.append(("bad-json", data[:120]))
            return None
        env = Envelope(obj, data, addr)
        self.guest_addr = addr
        self.received.append(env)
        return env

    def wait_for(self, type_, timeout=None, predicate=None, body_contains=None):
        """等一个指定 type 的信封（可附加判定），超时抛 AssertionError。"""
        deadline = time.monotonic() + (self.timeout if timeout is None else timeout)
        while time.monotonic() < deadline:
            env = self.recv_one()
            if env is None:
                continue
            if type_ and env.type != type_:
                continue
            if predicate is not None and not predicate(env):
                continue
            if body_contains is not None and body_contains not in env.body:
                continue
            return env
        raise AssertionError(
            f"等待 type={type_!r} 超时；已收 {[e.type for e in self.received]}")

    def drain(self, seconds=0.5):
        """收一小段时间，返回这一段收到的信封列表。"""
        out = []
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            env = self.recv_one()
            if env is not None:
                out.append(env)
        return out

    def hello(self, timeout=None):
        """等内核的 hello（同时学习源地址）。"""
        return self.wait_for("hello", timeout=timeout)

    def epoch(self):
        """已见过的最大纪元（hello/req/resp/resync 都算）。"""
        e = 0
        for env in self.received:
            if env.type in ("hello", "req", "resp", "resync"):
                e = max(e, env.epoch)
        return e

    def of_type(self, type_):
        return [e for e in self.received if e.type == type_]

    # ---- 发 ----
    def _send(self, type_, body, seq=None, epoch=None, ack=None):
        if self.guest_addr is None:
            raise AssertionError("尚未收到内核任何报文：无法得知回包地址")
        obj = {
            "pot": MAGIC,
            "epoch": self.epoch_next if epoch is None else epoch,
            "seq": 0 if seq is None else seq,
            "ack": self.acks_sent if ack is None else ack,
            "type": type_,
            "body": body,
        }
        data = json.dumps(obj, separators=(",", ":")).encode("utf-8")
        self.sock.sendto(data, self.guest_addr)
        self.log.append(("sent", obj))
        return obj

    def send_req(self, line, seq=None, epoch=None):
        """发一条 MCP 请求行；返回 (seq, epoch)。seq 缺省则自增。"""
        if seq is None:
            seq = self.seq_next
            self.seq_next += 1
        self._send("req", line, seq=seq, epoch=epoch)
        return seq, (self.epoch_next if epoch is None else epoch)

    def send_ack(self, seq):
        self.acks_sent = max(self.acks_sent, int(seq))
        self._send("ack", "")

    def send_resync(self, epoch=None):
        self._send("resync", "", epoch=epoch)

    def set_epoch(self, epoch):
        """切换对端纪元（模拟对端重启/新会话）。"""
        self.epoch_next = int(epoch)
        self.seq_next = 1

    def call(self, line, timeout=None, epoch=None, seq=None, ack=True, recover=True):
        """发请求并等第一个 resp；返回 (Envelope, req_seq)。

        ack=False 时不发 ack —— 用于「同 seq 重放」测试：内核一旦收到 ack
        就会回收重放槽，之后再问同一个 seq 只能得到 resync（协议如此设计）。

        recover=True 时自动处理断线重连的**纪元重同步**：若等待期间收到
        更高纪元的 hello/resync（内核在链路重连后进入新会话，并会用 resync
        回绝旧纪元的包），对端切到新纪元并用同一 seq 重发本次请求——这正是
        M7 验收要覆盖的「断线重连后消息纪元重同步」路径。
        """
        s, _ = self.send_req(line, seq=seq, epoch=epoch)
        deadline = time.monotonic() + (self.timeout if timeout is None else timeout)
        while time.monotonic() < deadline:
            env = self.recv_one()
            if env is None:
                continue
            if env.type == "resp" and env.seq >= s:
                if ack:
                    self.send_ack(env.seq)
                return env, s
            if (recover and env.type in ("resync", "hello")
                    and env.epoch > self.epoch_next):
                self.epoch_next = env.epoch     # 跟随内核的新纪元
                self.log.append(("recover", env.epoch, s))
                self._send("req", line, seq=s, epoch=env.epoch)
        raise AssertionError(
            f"等待 type='resp' 超时（req_seq={s}，epoch={self.epoch_next}）；"
            f"已收 {[(e.type, e.epoch, e.seq) for e in self.received]}")

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def mcp_request(method, params=None, req_id=1):
    """构造一条 MCP JSON-RPC 请求行（紧凑无空白，内核按单行处理）。"""
    obj = {"jsonrpc": "2.0", "id": req_id, "method": method}
    if params is not None:
        obj["params"] = params
    return json.dumps(obj, separators=(",", ":"))
