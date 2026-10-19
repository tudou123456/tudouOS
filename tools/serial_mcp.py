#!/usr/bin/env python
"""真机串口 MCP 客户端 / 桥（M7 串口传输，transport=serial-json）。

背景：ESP32-S3 硅片没有内部 EMAC，真机以太网只能等 SPI 扩展芯片
（W5500/DM9051）。在此之前，真机远程控制面走 UART0 上的 MCP 帧协议：
把与 UDP 通道完全相同的 netmcp 信封（epoch/seq/ack/去重/重放）架在单行
串口帧上，主机侧就是本工具（另一个实现见设备内核 kernel/net/netmcp.c）。

协议帧（UART0 115200 8N1，单行，最大 1600 字节）：
  device→host:  b"@@MCP <crc16> <json>\\r\\n"
  host→device:  b"\\x02" + b"@@MCP <crc16> <json>\\r\\n"
  <crc16>  = CRC-16/CCITT-FALSE（poly 0x1021, init 0xFFFF, 不反射, 无终异或），
             4 位小写十六进制，覆盖 <json> 原始字节。
  <json>   = netmcp 信封：
             {"pot":1,"epoch":E,"seq":S,"ack":A,"type":"hello|req|resp|ack|resync",
              "body":"<JSON-RPC 行，JSON 转义>"}
  host→device 的 STX(0x02) 让设备控制台把机器帧与人类命令行彻底分开；
  设备侧坏帧（前缀/CRC/JSON/超长）静默丢弃并计数，等对端重发。

用法:
  py -3.14 tools/serial_mcp.py --port COM3 selftest
  py -3.14 tools/serial_mcp.py --port COM3 tools/list
  py -3.14 tools/serial_mcp.py --port COM3 call ping
  py -3.14 tools/serial_mcp.py --port COM3 call fs.write '{"name":"n1","data":"hello"}'
  py -3.14 tools/serial_mcp.py --port COM3 call fs.read '{"name":"n1"}'
  py -3.14 tools/serial_mcp.py --port COM3 raw '{"jsonrpc":"2.0","id":1,"method":"ping"}'
  py -3.14 tools/serial_mcp.py --port COM3 dump
  py -3.14 tools/serial_mcp.py --port COM3 --stdio        # stdin JSON-RPC → 设备
  py -3.14 tools/serial_mcp.py --port COM3 --reset selftest

  --reset 按 esptool 同款时序脉冲 RTS(EN)，等设备重启后重新握手。
  stdout：tools/list / call / raw / --stdio 只输出 JSON-RPC 响应行（机器可读）；
          selftest、dump 输出人类摘要。诊断走 stderr 或 --verbose。
退出码：0 成功；2 传输/超时/握手失败；3 设备返回 MCP error 响应。
"""

import argparse
import json
import sys
import time

try:
    import serial
except ImportError:  # pragma: no cover - 环境缺依赖时的明确提示
    sys.stderr.write("需要 pyserial（py -3.14 -m pip install pyserial）\n")
    raise SystemExit(2)

PREFIX = b"@@MCP "
PREFIX_LEN = len(PREFIX)
STX = 0x02
MAX_FRAME = 1600          # 含 "@@MCP crc " 前缀的单行上限（设备同值）
MAX_ENVELOPE = 1400       # netmcp 信封上限（设备 NETMCP_DGRAM_MAX）
MAGIC = 1
DEFAULT_PORT = "COM3"
DEFAULT_BAUD = 115200


class SerialMcpError(Exception):
    """传输层错误（帧超长、串口异常等）。"""


class SerialMcpTimeout(SerialMcpError):
    """等响应超时。"""


def crc16_ccitt_false(data):
    """CRC-16/CCITT-FALSE：poly 0x1021, init 0xFFFF, 不反射, 无终异或。"""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def encode_frame(envelope_text):
    """一行串口帧（不含 host 侧 STX）。"""
    raw = envelope_text.encode("utf-8")
    if len(raw) > MAX_ENVELOPE:
        raise SerialMcpError(
            "信封 %d 字节超过设备上限 %d（body 上限 1200）" % (len(raw), MAX_ENVELOPE))
    frame = PREFIX + ("%04x " % crc16_ccitt_false(raw)).encode("ascii") + raw + b"\r\n"
    if len(frame) > MAX_FRAME:
        raise SerialMcpError("帧 %d 字节超过 %d" % (len(frame), MAX_FRAME))
    return frame


def decode_frame(line):
    """解析设备来的一行。返回 (dict, None) 或 (None, 错误原因)。"""
    if len(line) > MAX_FRAME:
        return None, "too_long"
    if not line.startswith(PREFIX):
        return None, "bad_prefix"
    if len(line) < PREFIX_LEN + 6:      # 前缀 + 4hex + 空格 + ≥1 字节
        return None, "short"
    if line[PREFIX_LEN + 4:PREFIX_LEN + 5] != b" ":
        return None, "bad_header"
    try:
        want = int(line[PREFIX_LEN:PREFIX_LEN + 4], 16)
    except ValueError:
        return None, "bad_crc_hex"
    body = line[PREFIX_LEN + 5:]
    if not body:
        return None, "empty"
    if crc16_ccitt_false(body) != want:
        return None, "crc"
    try:
        env = json.loads(body.decode("utf-8"))
    except (UnicodeDecodeError, ValueError):
        return None, "json"
    if not isinstance(env, dict):
        return None, "not_object"
    return env, None


def dumps(obj):
    return json.dumps(obj, ensure_ascii=False, separators=(",", ":"))


def jsonrpc_line(method, params=None, req_id=1):
    req = {"jsonrpc": "2.0", "id": req_id, "method": method}
    if params is not None:
        req["params"] = params
    return dumps(req)


class SerialPeer:
    """串口 netmcp 对端：帧编解码 + epoch 学习 + req/resp/ack 配对。"""

    def __init__(self, port, baud=DEFAULT_BAUD, timeout=5.0, reset=False,
                 verbose=False):
        self.port = port
        self.timeout = timeout
        self.verbose = verbose
        # 打开端口前先确定 DTR/RTS 无效：否则 CH340 板开端口瞬间会拉低 EN，
        # 板子复位（每次调用都重启，fs.write 后 fs.read 会丢文件）。
        self.ser = serial.Serial()
        self.ser.port = port
        self.ser.baudrate = baud
        self.ser.timeout = 0.05
        try:
            self.ser.dtr = False
            self.ser.rts = False
        except (serial.SerialException, OSError):
            pass
        self.ser.open()
        self._buf = bytearray()
        self.epoch = None       # 设备当前纪元（首次由 hello 学到）
        # 每次 CLI 进程从毫秒时间派生一个高起点序号：设备去重窗口只记最近 8 个
        # (epoch,seq)，若每次新进程都从 1 开始，会和上一个会话残留的 seq 撞车
        # 被误判成重复请求。高起点让不同会话的序号几乎不可能重叠。
        self.tx_seq = (int(time.time() * 1000) & 0x0FFFFFFF) + 1024
        self.dev_seq = 0        # 已收到的最大设备响应序号（回 ack 用）
        self.learned = False    # 收到过 hello
        self.rx_frames = 0
        self.bad_frames = 0
        self.orphan_lines = 0
        self.start_time = time.time()
        try:
            self.ser.dtr = False
            self.ser.rts = False
        except (serial.SerialException, OSError):
            pass
        if reset:
            self.pulse_reset()
        else:
            self.flush_input()

    # ---------- 底层 IO ----------
    def _log(self, msg):
        sys.stderr.write("[serial-mcp] %s\n" % msg)
        sys.stderr.flush()

    def _vlog(self, msg):
        if self.verbose:
            self._log(msg)

    def flush_input(self):
        try:
            self.ser.reset_input_buffer()
        except (serial.SerialException, OSError):
            pass
        self._buf.clear()

    def pulse_reset(self, hold=0.1):
        """esptool 同款复位时序：RTS=EN 断言 100ms 后释放，DTR 全程无效。"""
        try:
            self.ser.dtr = False
            self.ser.rts = True
            time.sleep(hold)
            self.ser.rts = False
            self.ser.dtr = False
        except (serial.SerialException, OSError) as exc:
            raise SerialMcpError("复位失败: %s" % exc)
        self.flush_input()
        self._log("%s 已脉冲复位，等待设备启动" % self.port)

    def close(self):
        try:
            self.ser.close()
        except (serial.SerialException, OSError):
            pass

    def _read_line(self, deadline):
        """读到一行完整输出；若是合法 MCP 帧则返回信封 dict，否则继续。"""
        while time.time() < deadline:
            chunk = self.ser.read(4096)
            if chunk:
                self._buf.extend(chunk)
            while True:
                idx = self._buf.find(b"\n")
                if idx < 0:
                    if len(self._buf) > 16384:
                        self._buf.clear()
                    break
                line = bytes(self._buf[:idx])
                del self._buf[:idx + 1]
                if line.endswith(b"\r"):
                    line = line[:-1]
                if not line:
                    continue
                if not line.startswith(PREFIX):
                    self.orphan_lines += 1
                    self._vlog("console: " + line.decode("utf-8", "replace"))
                    continue
                env, err = decode_frame(line)
                if env is None:
                    self.bad_frames += 1
                    self._log("坏帧丢弃（%s）: %r" % (err, line[:96]))
                    continue
                self.rx_frames += 1
                self._note(env)
                return env
        return None

    def _note(self, env):
        """更新学习到的会话状态（epoch 跟随设备；hello 标记已握手）。"""
        epoch = env.get("epoch")
        if isinstance(epoch, int) and epoch > 0:
            self.epoch = epoch
        if env.get("type") == "hello":
            self.learned = True
            body = env.get("body") or ""
            self._vlog("hello epoch=%s body=%s" % (self.epoch, body))

    def _write_envelope(self, env):
        env = dict(env)
        env["pot"] = MAGIC
        if self.epoch is None:
            self.epoch = 1      # 设备 epoch 从 1 起；未握手时按默认值先试
        env["epoch"] = self.epoch
        frame = encode_frame(dumps(env))
        try:
            self.ser.write(bytes([STX]) + frame)
            self.ser.flush()
        except (serial.SerialException, OSError) as exc:
            raise SerialMcpError("串口写失败: %s" % exc)
        return env

    # ---------- 协议动作 ----------
    def send_ack(self, force=False):
        """回 ack，让设备回收重放槽（累积 ack 语义；force 用于握手时结束 hello）。"""
        if self.dev_seq <= 0 and not force:
            return
        self._write_envelope({
            "seq": self.tx_seq,
            "ack": self.dev_seq,
            "type": "ack",
            "body": "",
        })

    def handshake(self, timeout=None):
        """等设备 hello（设备未学对端前每 500ms 周期发），学到 epoch。

        设备一旦学过本对端就不再发 hello（正常重连场景），因此等不到 hello 时
        不能一直阻塞：发一个 force ack 探测帧并乐观继续（epoch 默认 1，串口
        设备每个启动周期从 1 起；真断线时后续 request 会超时报 2）。
        """
        timeout = self.timeout if timeout is None else timeout
        deadline = time.time() + min(timeout, 2.0)
        while time.time() < deadline:
            env = self._read_line(deadline)
            if env is None:
                break
            if env.get("type") == "hello":
                self.learned = True
                self.send_ack()     # 顺带让设备学到对端、停止 hello 广播
                return True
        self.send_ack(force=True)   # 探测：合法帧即可让设备学对端
        self.learned = True
        self._vlog("handshake: 未见 hello，按已连接（epoch=%s）继续" % self.epoch)
        return True

    def notify(self, line):
        """发一条 JSON-RPC notification：设备对 notification 静默不回包，不等响应。"""
        if not self.learned:
            self.handshake(timeout=max(4.0, min(self.timeout, 8.0)))
        self.tx_seq += 1
        self._write_envelope({
            "seq": self.tx_seq,
            "ack": self.dev_seq,
            "type": "req",
            "body": line,
        })
        self._vlog("notification seq=%d 已发送（不等响应）" % self.tx_seq)

    def request(self, line, timeout=None, max_attempts=2, seq=None, ack=True):
        """发一条 JSON-RPC 行，返回设备 JSON-RPC 响应对象。

        seq：强制使用指定请求序号（重放/去重测试用；默认自增）。
        ack：收到响应后是否回 ack（False 时故意不回收设备重放槽，
             用于构造「同 seq 重发 → 设备重放」测试）。
        """
        timeout = self.timeout if timeout is None else timeout
        if not self.learned:
            # 复位后设备还在启动：给宽一点的握手窗口
            self.handshake(timeout=max(4.0, min(timeout, 8.0)))
        last_err = None
        for _ in range(max_attempts):
            if seq is None:
                self.tx_seq += 1
                use_seq = self.tx_seq
            else:
                use_seq = seq
                if seq > self.tx_seq:
                    self.tx_seq = seq
            self._write_envelope({
                "seq": use_seq,
                "ack": self.dev_seq,
                "type": "req",
                "body": line,
            })
            self._vlog("req seq=%d epoch=%s bytes=%d" %
                       (use_seq, self.epoch, len(line)))
            deadline = time.time() + timeout
            retry = False
            while time.time() < deadline:
                env = self._read_line(deadline)
                if env is None:
                    break
                etype = env.get("type")
                if etype == "resp":
                    dev_seq = int(env.get("seq") or 0)
                    if dev_seq > self.dev_seq:
                        self.dev_seq = dev_seq
                    if ack:
                        self.send_ack()
                    if int(env.get("ack") or 0) == use_seq:
                        body = env.get("body") or ""
                        if not body:
                            return None
                        try:
                            return json.loads(body)
                        except ValueError as exc:
                            raise SerialMcpError(
                                "响应 body 不是合法 JSON（%s）: %r" % (exc, body))
                    self._vlog("忽略非配对响应 dev_seq=%d ack=%s（等 seq=%d）" %
                               (dev_seq, env.get("ack"), use_seq))
                    continue
                if etype == "resync":
                    # 设备说纪元/游标过期：跟随设备纪元重发一次
                    self._vlog("resync epoch=%s（跟随重发）" % env.get("epoch"))
                    retry = True
                    break
                self._vlog("忽略帧 type=%s" % etype)
                last_err = None
            if retry:
                continue
            last_err = "请求超时（epoch=%s seq=%d）" % (self.epoch, use_seq)
        raise SerialMcpTimeout(last_err or "请求超时")

    def console_command(self, cmd, wait=1.0):
        """发一条本地控制台命令（非帧），返回采集到的文本。"""
        try:
            self.ser.write((cmd + "\r\n").encode("ascii"))
            self.ser.flush()
        except (serial.SerialException, OSError) as exc:
            raise SerialMcpError("串口写失败: %s" % exc)
        deadline = time.time() + wait
        out = []
        while time.time() < deadline:
            chunk = self.ser.read(4096)
            if chunk:
                out.append(chunk.decode("utf-8", "replace"))
            else:
                time.sleep(0.01)
        return "".join(out)

    def dump_netmcp(self, wait=1.2):
        text = self.console_command("netmcp dump", wait)
        # 控制台回显整行命令；把行内可能的 MCP 帧剔掉，保留人类可读部分
        lines = []
        for ln in text.replace("\r", "\n").split("\n"):
            if ln.startswith("@@MCP "):
                continue
            lines.append(ln)
        return "\n".join(ln for ln in lines if ln.strip())

    def drain_console(self, seconds=0.25):
        """抽空一串口输出（verbose 诊断用，例如设备打印的 dup/replay 行）。"""
        deadline = time.time() + seconds
        while time.time() < deadline:
            self._read_line(deadline)


# ---------- 子命令 ----------

def cmd_selftest(peer):
    """握手 → tools/list → ping → sysinfo → netmcp dump 传输自检。"""
    if not peer.handshake(timeout=max(4.0, peer.timeout)):
        print("[serial-mcp] FAIL 握手：未收到 hello 帧（设备未运行串口 MCP？）")
        return 2
    print("[serial-mcp] hello ok epoch=%s" % peer.epoch)

    resp = peer.request(jsonrpc_line("tools/list", None, 101))
    result = (resp or {}).get("result") or {}
    tools = result.get("tools") or []
    if not isinstance(tools, list) or not tools:
        print("[serial-mcp] FAIL tools/list: %s" % dumps(resp))
        return 3 if isinstance(resp, dict) and resp.get("error") else 2
    names = [t.get("name", "?") for t in tools]
    print("[serial-mcp] tools/list ok: %d 个工具 (%s)" %
          (len(tools), ", ".join(names)))

    resp = peer.request(jsonrpc_line("tools/call",
                                     {"name": "ping", "arguments": {}}, 102))
    if not isinstance(resp, dict) or resp.get("error") or not resp.get("result"):
        print("[serial-mcp] FAIL ping: %s" % dumps(resp))
        return 3 if isinstance(resp, dict) and resp.get("error") else 2
    print("[serial-mcp] ping ok")

    resp = peer.request(jsonrpc_line("tools/call",
                                     {"name": "sysinfo", "arguments": {}}, 103))
    if not isinstance(resp, dict) or resp.get("error") or not resp.get("result"):
        print("[serial-mcp] FAIL sysinfo: %s" % dumps(resp))
        return 3 if isinstance(resp, dict) and resp.get("error") else 2
    print("[serial-mcp] sysinfo ok: %s" % dumps(resp.get("result")))

    text = peer.dump_netmcp()
    if "transport=serial-json" not in text:
        print("[serial-mcp] FAIL netmcp dump 未显示 transport=serial-json")
        print(text)
        return 2
    for ln in text.split("\n"):
        if "transport=serial-json" in ln or ln.startswith("[POTATO][NETMCP] serial"):
            print("[serial-mcp] dump: %s" % ln)

    print("[serial-mcp] selftest PASS (rx_frames=%d bad_frames=%d)" %
          (peer.rx_frames, peer.bad_frames))
    return 0


def cmd_tools_list(peer):
    resp = peer.request(jsonrpc_line("tools/list", None, 1))
    print(dumps(resp))
    return 3 if isinstance(resp, dict) and resp.get("error") else 0


def cmd_call(peer, name, args_json, seq=None, ack=True):
    try:
        args = json.loads(args_json) if args_json else {}
    except ValueError as exc:
        sys.stderr.write("[serial-mcp] 参数不是合法 JSON: %s\n" % exc)
        return 2
    req = {"name": name, "arguments": args}
    resp = peer.request(jsonrpc_line("tools/call", req, 1), seq=seq, ack=ack)
    if peer.verbose:
        peer.drain_console(0.25)
    print(dumps(resp))
    return 3 if isinstance(resp, dict) and resp.get("error") else 0


def cmd_raw(peer, line, seq=None, ack=True):
    try:
        json.loads(line)
    except ValueError as exc:
        sys.stderr.write("[serial-mcp] raw 参数不是合法 JSON-RPC 行: %s\n" % exc)
        return 2
    resp = peer.request(line, seq=seq, ack=ack)
    if peer.verbose:
        peer.drain_console(0.25)
    print(dumps(resp))
    return 3 if isinstance(resp, dict) and resp.get("error") else 0


def cmd_dump(peer):
    if not peer.learned:
        peer.handshake(timeout=min(3.0, peer.timeout))
    text = peer.dump_netmcp()
    print(text)
    if "transport=serial-json" in text:
        return 0
    sys.stderr.write("[serial-mcp] 注意：dump 未出现 transport=serial-json\n")
    return 2


def stdio_loop(peer):
    """stdin 逐行 JSON-RPC → 设备；响应写 stdout（MCP 客户端桥）。"""
    if not peer.handshake(timeout=max(4.0, peer.timeout)):
        sys.stderr.write("[serial-mcp] --stdio 握手失败（无 hello）\n")
        return 2
    for raw in sys.stdin:
        line = raw.strip()
        if not line:
            continue
        try:
            obj = json.loads(line)
        except ValueError as exc:
            print(dumps({"jsonrpc": "2.0", "id": None,
                         "error": {"code": -32700,
                                   "message": "parse error: %s" % exc}}))
            sys.stdout.flush()
            continue
        # notification（无 id / notifications/*）设备静默不回包，不能等响应，
        # 否则 MCP 客户端握手（notifications/initialized）会超时挂起。
        is_notification = "id" not in obj or str(obj.get("method", "")).startswith(
            "notifications/")
        if is_notification:
            try:
                peer.notify(line)
            except SerialMcpError as exc:
                sys.stderr.write("[serial-mcp] notification 发送失败: %s\n" % exc)
                return 2
            continue
        try:
            resp = peer.request(line)
        except SerialMcpTimeout as exc:
            print(dumps({"jsonrpc": "2.0", "id": None,
                         "error": {"code": -32000, "message": str(exc)}}))
            sys.stdout.flush()
            return 2
        if resp is not None:
            print(dumps(resp))
            sys.stdout.flush()
    return 0


def build_parser():
    ap = argparse.ArgumentParser(
        description="真机串口 MCP 客户端（netmcp serial transport）")
    ap.add_argument("--port", default=DEFAULT_PORT, help="串口设备（默认 COM3）")
    ap.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    ap.add_argument("--timeout", type=float, default=5.0,
                    help="单次请求等待响应秒数（默认 5）")
    ap.add_argument("--reset", action="store_true", help="打开后脉冲复位设备")
    ap.add_argument("--verbose", action="store_true", help="打印帧级诊断（stderr）")
    ap.add_argument("--seq", type=int, default=None,
                    help="强制请求序号（重放/去重测试用，默认自增）")
    ap.add_argument("--no-ack", action="store_true",
                    help="收到响应后不回 ack（保留设备重放槽，构造重放测试）")
    ap.add_argument("--stdio", action="store_true",
                    help="stdin JSON-RPC 行 → 设备，响应写 stdout")
    ap.add_argument("cmd", nargs="?",
                    choices=["selftest", "tools/list", "call", "raw", "dump"],
                    help="子命令（--stdio 时省略）")
    ap.add_argument("rest", nargs="*", help="call <工具名> [参数JSON] / raw <JSON-RPC行>")
    return ap


def main(argv=None):
    args = build_parser().parse_args(argv)
    if not args.stdio and not args.cmd:
        build_parser().error("需要子命令（selftest|tools/list|call|raw|dump）或 --stdio")
    try:
        peer = SerialPeer(args.port, args.baud, args.timeout, args.reset,
                          args.verbose)
    except (serial.SerialException, OSError) as exc:
        sys.stderr.write("[serial-mcp] 打开串口 %s 失败: %s\n" % (args.port, exc))
        return 2
    try:
        if args.stdio:
            return stdio_loop(peer)
        if args.cmd == "selftest":
            return cmd_selftest(peer)
        if args.cmd == "tools/list":
            return cmd_tools_list(peer)
        if args.cmd == "call":
            if not args.rest:
                sys.stderr.write("[serial-mcp] 用法: call <工具名> [参数JSON]\n")
                return 2
            return cmd_call(peer, args.rest[0],
                            args.rest[1] if len(args.rest) > 1 else "",
                            seq=args.seq, ack=not args.no_ack)
        if args.cmd == "raw":
            if not args.rest:
                sys.stderr.write("[serial-mcp] 用法: raw '<JSON-RPC 行>'\n")
                return 2
            return cmd_raw(peer, args.rest[0], seq=args.seq, ack=not args.no_ack)
        if args.cmd == "dump":
            return cmd_dump(peer)
        return 2
    except SerialMcpTimeout as exc:
        sys.stderr.write("[serial-mcp] 超时: %s\n" % exc)
        return 2
    except SerialMcpError as exc:
        sys.stderr.write("[serial-mcp] 传输错误: %s\n" % exc)
        return 2
    finally:
        peer.close()


if __name__ == "__main__":
    sys.exit(main())
