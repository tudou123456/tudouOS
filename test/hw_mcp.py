"""真机串口 MCP（netmcp serial-json transport）host 侧编解码 + 帧等待工具。

协议真源：kernel/net/netmcp.c（与 QEMU 侧 UDP 信封同一套字段）。

- host→device：先发 STX(0x02)，再发单行 ``@@MCP <crc16> <envelope>\\r\\n``。
  console.c 只认行首 STX，因此帧不会被回显/派发成本地命令，也不受 264B
  命令行上限约束。
- device→host：只发 ``@@MCP <crc16> <envelope>\\r\\n``（单行；若上行有残留
  半行——例如未换行的 ``potato> `` 提示符——netmcp 会先补 ``\\n`` 再发帧）。
- crc16 = CRC-16/CCITT-FALSE（poly 0x1021, init 0xFFFF, 不反射, 无终异或），
  覆盖 envelope JSON 原始字节（不含 ``@@MCP `` 前缀与行尾）。
- envelope = {"pot":1,"epoch":E,"seq":S,"ack":A,"type":"hello|req|resp|ack|resync",
  "body":"<转义后的内层 JSON-RPC 文本>"}

``test/hw_serial.py`` 已把 ``@@MCP `` 帧从匹配文本中剥离并按 ``mcp_frames``
保存（含 crc 与原始 payload 字符串）；本模块负责校验 CRC、构造请求、等待响应。
"""

from __future__ import annotations

import json
import time

from test.hw_serial import HwTimeoutError

MCP_PREFIX = "@@MCP "
STX = b"\x02"
CRC_POLY = 0x1021


# ---------------------------------------------------------------------- CRC 编解码
def crc16_ccitt_false(data) -> int:
    """CRC-16/CCITT-FALSE；data 可为 bytes 或 str（str 按 UTF-8 编码）。"""
    if isinstance(data, str):
        data = data.encode("utf-8")
    crc = 0xFFFF
    for byte in data:
        crc ^= (byte << 8) & 0xFFFF
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ CRC_POLY) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc & 0xFFFF


def build_envelope(typ: str, seq: int, body_json: str,
                   epoch: int = 1, ack: int = 0) -> str:
    """手工拼信封（字段顺序与固件一致，便于日志比对）。"""
    return ('{"pot":1,"epoch":%d,"seq":%d,"ack":%d,"type":"%s","body":%s}'
            % (epoch, seq, ack, typ, json.dumps(body_json, ensure_ascii=False)))


def build_frame(envelope: str, *, stx: bool = True, bad_crc: bool = False,
                corrupt_crc_delta: int = 1) -> bytes:
    """host→device 帧字节：STX + '@@MCP <hex4> <envelope>' + '\\r\\n'。"""
    crc = crc16_ccitt_false(envelope)
    if bad_crc:
        crc = (crc + corrupt_crc_delta) & 0xFFFF
    text = "%s%04x %s" % (MCP_PREFIX, crc, envelope)
    head = STX if stx else b""
    return head + text.encode("utf-8") + b"\r\n"


def build_device_line(envelope: str) -> bytes:
    """模拟 device→host 的单行（自测用，不发送）。"""
    return ("%s%04x %s\r\n" % (MCP_PREFIX, crc16_ccitt_false(envelope),
                               envelope)).encode("utf-8")


def check_frame_crc(frame: dict) -> dict:
    """校验 hw_serial 抓到的一帧：crc 字段必须覆盖 raw payload 的原始字节。"""
    raw = frame.get("raw", "")
    crc = frame.get("crc")
    if crc is None:
        raise AssertionError("串口 MCP 帧缺少 CRC 字段: %r" % (frame,))
    want = int(crc, 16)
    got = crc16_ccitt_false(raw)
    if want != got:
        raise AssertionError(
            "串口 MCP 帧 CRC 不符: crc=%s 实算=%04x payload=%r" % (crc, got, raw[:200]))
    payload = frame.get("json")
    if not isinstance(payload, dict):
        raise AssertionError("串口 MCP 帧 payload 不是 JSON 对象: %r" % (raw[:200],))
    if payload.get("pot") != 1:
        raise AssertionError("串口 MCP 信封缺 pot=1: %r" % (payload,))
    if "body" not in payload:
        raise AssertionError("串口 MCP 信封缺 body: %r" % (payload,))
    return payload


def wait_frame(hw, start: int, *, kind: str | None = None, timeout: float = 3.0,
               predicate=None, require_crc: bool = True) -> dict:
    """等待 start 索引之后的新帧（可按 type/predicate 过滤），返回信封 dict。

    hw 为 test.hw_serial.HwSerial（其 mcp_frames 已剥离文本，必须轮询列表）。
    """
    deadline = time.monotonic() + timeout
    last_len = -1
    while True:
        frames = hw.mcp_frames
        for frame in frames[start:]:
            env = frame.get("json")
            if not isinstance(env, dict):
                continue
            if require_crc:
                env = check_frame_crc(frame)
            if kind is not None and env.get("type") != kind:
                continue
            if predicate is not None and not predicate(env):
                continue
            return env
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            tail = "\n".join(hw.raw_lines[-12:])
            raise HwTimeoutError(
                "串口 MCP 帧(type=%r) 未在 %.1fs 内出现；最后原始输出:\n%s"
                % (kind, timeout, tail), timeout, tail)
        if len(frames) != last_len:
            last_len = len(frames)
        time.sleep(min(0.02, remaining))


def wait_frames(hw, start: int, n: int, *, kind: str | None = "resp",
                timeout: float = 3.0) -> list[dict]:
    """收集 start 之后至少 n 条帧。"""
    out: list[dict] = []
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline and len(out) < n:
        frames = hw.mcp_frames
        out = []
        for frame in frames[start:]:
            env = frame.get("json")
            if not isinstance(env, dict):
                continue
            if kind is not None and env.get("type") != kind:
                continue
            out.append(env)
        if len(out) >= n:
            return out
        time.sleep(0.02)
    return out


def inner_body(env: dict) -> str:
    """取出信封 body 字符串（内层 JSON-RPC 文本原文）。"""
    body = env.get("body")
    if not isinstance(body, str):
        raise AssertionError("信封 body 不是字符串: %r" % (env,))
    return body


def inner_json(env: dict):
    """把信封 body 解析成 JSON 对象（内层 JSON-RPC 响应）。"""
    body = inner_body(env)
    try:
        return json.loads(body)
    except ValueError as exc:
        raise AssertionError("信封 body 不是合法 JSON: %r (%s)" % (body[:200], exc)) from exc


def rpc(body: dict) -> str:
    """把内层 JSON-RPC 请求序列化成紧凑文本。"""
    return json.dumps(body, separators=(",", ":"), ensure_ascii=False)


def serial_rpc(ctx, body: dict, seq: int, *, epoch: int = 1, timeout: float = 3.0,
               ack: int = 0):
    """经串口 MCP 发一条 req，等 resp，返回 (resp 信封, 内层 JSON 对象)。"""
    hw = ctx.hw
    start = len(hw.mcp_frames)
    env = build_envelope("req", seq, rpc(body), epoch=epoch, ack=ack)
    hw.send_raw(build_frame(env))
    resp = wait_frame(hw, start, kind="resp", timeout=timeout)
    return resp, inner_json(resp)
