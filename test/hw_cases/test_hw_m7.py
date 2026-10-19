"""M7 真机验收：串口 MCP 远程通道（无 EMAC 网口 → netmcp serial-json transport）。

断言来源：
- test/cases/test_m7_nodev.py（无网口优雅降级：present=0、mcp 仍可用）；
- kernel/net/netmcp.c 的串口帧协议（@@MCP + CRC16/CCITT-FALSE + 信封）与
  tools/serial_mcp.py 客户端行为（selftest/tools-list/call/--seq 重放）。
真机差异：无（真机没有 EMAC，正是 M7「远程 MCP 走串口」的设计场景）：
- QEMU 的 UDP peer/链路 down-up 用例不适用于真机，由本文件的串口链路用例替代；
- 每个子进程用例用 ctx.released() 让出 COM3 给 tools/serial_mcp.py，
  退出后 runner 自动重开串口 + 复位，不影响后续用例。
"""

from __future__ import annotations

import json
import re
import subprocess
import sys
import time

from test.hw_mcp import build_envelope, build_frame, inner_json, rpc, wait_frame
from test.hw_runner import hw_case
from test.hw_util import ROOT, events_since, no_crash, poll

TOOL = str(ROOT / "tools" / "serial_mcp.py")

# 无 MAC 设备时 net_start 的降级输出（hw 分支 emac.c + net.c；QEMU nodev 同语义）
NET_ABSENT = (r"\[POTATO\]\[NET\] EMAC(?: probe: [^\r\n]* -> absent"
              r"|: ESP32-S3 has no internal MAC[^\r\n]*)")
NET_TRANSPORT = r"\[POTATO\]\[NETMCP\] transport=serial-json[^\r\n]*\r"
NETMCP_TRANSPORT = (r"\[POTATO\]\[NETMCP\] transport=serial-json known=(\d+) "
                    r"link=(\d+) epoch=(\d+)\r")
NETMCP_COUNTERS = (r"\[POTATO\]\[NETMCP\] rx=(\d+) tx=(\d+) req=(\d+) dup=(\d+) "
                   r"replay=(\d+) resync=(\d+) hello=(\d+) bad=(\d+) gaps=(\d+) "
                   r"ack=(\d+) cursor=(\d+) txseq=(\d+) pending=(\d+) trunc=(\d+)\r")
NETMCP_SERIAL = (r"\[POTATO\]\[NETMCP\] serial tx=(\d+) rx=(\d+) crc_bad=(\d+) "
                 r"too_long=(\d+) bad_prefix=(\d+) deferred=(\d+) "
                 r"pending_now=(-?\d+) maxframe=(\d+) maxbody=(\d+) maxdgram=(\d+)\r")
NETMCP_SELFTEST = r"\[POTATO\]\[NETMCP\] selftest (ok|FAIL)[^\r\n]*\r"


# ---------------------------------------------------------------------- 子进程工具
def _run_tool(port: str, *args: str, timeout: float = 150.0):
    cmd = [sys.executable, TOOL, "--port", port, "--timeout", "8"] + list(args)
    return subprocess.run(cmd, cwd=str(ROOT), capture_output=True, text=True,
                          encoding="utf-8", errors="replace", timeout=timeout)


def _json_line(proc) -> dict:
    for line in reversed((proc.stdout or "").splitlines()):
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            return json.loads(line)
        except ValueError:
            continue
    raise AssertionError("serial_mcp 工具 stdout 无 JSON 行: rc=%r stdout=%r stderr=%r"
                         % (proc.returncode, (proc.stdout or "")[-600:],
                            (proc.stderr or "")[-600:]))


def _tool_result(proc) -> dict:
    obj = _json_line(proc)
    assert "error" not in obj, "工具返回 JSON-RPC 错误: %r" % (obj,)
    result = obj.get("result") or {}
    content = result.get("content") or []
    if content and isinstance(content, list) and content[0].get("type") == "text":
        return json.loads(content[0]["text"])
    return result


# ---------------------------------------------------------------------- 用例
@hw_case("m7/net_absent_graceful", timeout=20, smoke=True)
def test_net_absent_graceful(ctx):
    """无 EMAC：boot 明确打出缺席 + net 驱动 skipped + ethernet disabled，但控制面可用。"""
    boot = ctx.hw.text()[ctx.start_pos:]
    assert re.search(NET_ABSENT, boot), "boot 日志缺 EMAC 缺席说明（hw 构建应为显式 skip）"
    assert "[POTATO][DRV] net init skipped" in boot, "net 驱动未记 skipped（应无硬件）"
    assert "[POTATO][NET] no EMAC device: ethernet disabled" in boot, \
        "缺 '[NET] no EMAC device: ethernet disabled'"
    assert re.search(NET_TRANSPORT, boot), "net_start 未启用串口 MCP 传输面"
    ctx.cmd("net", r"\[POTATO\]\[NET\] present=0[^\r\n]*\r", timeout=8.0)
    # QEMU test_m7_nodev 同款：无网口时 mcp 仍工作（JSON-RPC 直连）
    ctx.cmd("mcp on", r"\[POTATO\]\[MCP\] agent started[^\r]*\r", timeout=10.0)
    ctx.send('{"jsonrpc":"2.0","id":31,"method":"ping"}')
    ctx.expect(r'\{"jsonrpc":"2\.0","id":31,"result":\{"pong":true', timeout=10.0)
    no_crash(ctx.text_since(ctx.start_pos), "nodev")
    ctx.checks.append("present=0 + net skipped + serial-json transport + MCP ping ok")


@hw_case("m7/serial_transport_console", timeout=20)
def test_serial_transport_console(ctx):
    """netmcp dump 三行契约 + netmcp test 自检 ok（串口传输面真的启用）。"""
    line = ctx.cmd_line("netmcp dump", NETMCP_TRANSPORT, timeout=8.0)
    m = re.search(NETMCP_TRANSPORT, line)
    assert m.group(1) is not None
    ctx.expect(NETMCP_COUNTERS, timeout=6.0)
    ser_line = ctx.expect(NETMCP_SERIAL, timeout=6.0)
    maxframe = int(ser_line.group(8))
    maxbody = int(ser_line.group(9))
    assert maxframe >= 1024, "串口 MCP 帧上限 maxframe=%d 过小" % maxframe
    assert 0 < maxbody <= maxframe, "maxbody=%d 不合法（maxframe=%d）" % (maxbody, maxframe)
    line = ctx.cmd_line("netmcp test", NETMCP_SELFTEST, timeout=10.0)
    assert "selftest ok" in line, "netmcp test 未通过: %r" % line
    no_crash(ctx.text_since(ctx.start_pos), "netmcp")
    ctx.checks.append("netmcp dump 三行 + selftest ok (maxframe=%d maxbody=%d)"
                      % (maxframe, maxbody))


@hw_case("m7/raw_frame_rpc", timeout=30)
def test_raw_frame_rpc(ctx):
    """原始串口帧：CRC 校验、req/resp 配对、同 seq 重放去重、ack 后 resync。"""
    hw = ctx.hw
    body = rpc({"jsonrpc": "2.0", "id": 101, "method": "ping"})
    env = build_envelope("req", 11, body)
    frame = build_frame(env)

    start = len(hw.mcp_frames)
    hw.send_raw(frame)
    resp = wait_frame(hw, start, kind="resp", timeout=8.0)   # 内含 CRC 校验
    inner = inner_json(resp)
    assert inner.get("id") == 101, "响应 id 不配对: %r" % (inner,)
    assert inner.get("result", {}).get("pong") is True, "ping 响应异常: %r" % (inner,)
    ctx.expect(r"\[POTATO\]\[NETMCP\] serial peer learned[^\r\n]*\r", 5.0)

    # 同一 seq 再发：设备应重放缓存响应（不是重新执行 —— tick 值必须逐字一致）
    start2 = len(hw.mcp_frames)
    hw.send_raw(frame)
    resp2 = wait_frame(hw, start2, kind="resp", timeout=8.0)
    inner2 = inner_json(resp2)
    assert inner2 == inner, "同 seq 重放响应不一致（去重/重放失效）: %r vs %r" % (
        inner2, inner)

    # ack 回收重放槽后再发同 seq → resync
    ack = build_envelope("ack", 0, "", ack=11)
    start3 = len(hw.mcp_frames)
    hw.send_raw(build_frame(ack))
    time.sleep(0.05)
    hw.send_raw(frame)
    wait_frame(hw, start3, kind="resync", timeout=8.0)

    line = ctx.cmd_line("netmcp dump", NETMCP_COUNTERS, timeout=8.0)
    m = re.search(NETMCP_COUNTERS, line)
    rx, dup, replay, resync = int(m.group(1)), int(m.group(4)), int(m.group(5)), int(m.group(6))
    assert rx >= 4, "netmcp rx=%d（至少 4 帧：req/req/ack/req）" % rx
    assert dup >= 1, "dup=%d（未统计同 seq 重放）" % dup
    assert replay >= 1, "replay=%d（未重放缓存响应）" % replay
    assert resync >= 1, "resync=%d（ack 后旧 seq 应要求 resync）" % resync
    ctx.checks.append(
        "frame rpc id=101, replay 命中, ack→resync, rx=%d dup=%d replay=%d resync=%d"
        % (rx, dup, replay, resync))


@hw_case("m7/tool_selftest", timeout=120)
def test_tool_selftest(ctx):
    """tools/serial_mcp.py selftest：握手/tools:list/ping/sysinfo/netmcp dump 全通过（exit 0）。"""
    with ctx.released():
        proc = _run_tool(ctx.hw.port, "selftest")
    assert proc.returncode == 0, "selftest rc=%d\nstdout=%r\nstderr=%r" % (
        proc.returncode, (proc.stdout or "")[-1500:], (proc.stderr or "")[-1500:])
    out = proc.stdout or ""
    for token in ("hello ok", "tools/list ok", "ping ok", "sysinfo ok",
                  "selftest PASS", "transport=serial-json"):
        assert token in out, "selftest 输出缺 %r:\n%s" % (token, out[-1500:])
    assert "bad_frames=0" in out or "bad_frames=1" not in out, out[-500:]
    ctx.checks.append("serial_mcp selftest PASS（工具子进程 rc=0）")


@hw_case("m7/tool_tools_list_and_call", timeout=150)
def test_tool_tools_list_and_call(ctx):
    """工具 tools/list 暴露 6 个 MCP 工具；call fs.write/fs.read 真机往返。"""
    port = ctx.hw.port
    with ctx.released():
        proc = _run_tool(port, "tools/list")
        assert proc.returncode == 0, "tools/list rc=%d std=%r stderr=%r" % (
            proc.returncode, (proc.stdout or "")[-800:], (proc.stderr or "")[-800:])
        obj = _json_line(proc)
        tools = (obj.get("result") or {}).get("tools") or []
        names = {t.get("name") for t in tools}
        for want in ("ping", "echo", "sysinfo", "fs.list", "fs.read", "fs.write"):
            assert want in names, "tools/list 缺工具 %r（实际 %r）" % (want, sorted(names))

        proc = _run_tool(port, "call", "fs.write",
                         '{"name":"m7.txt","data":"hello-serial"}')
        assert proc.returncode == 0, "fs.write rc=%d std=%r" % (
            proc.returncode, (proc.stdout or "")[-800:])
        wrote = _tool_result(proc)
        assert wrote.get("written") == len("hello-serial"), \
            "fs.write written=%r（期望 %d）" % (wrote, len("hello-serial"))

        proc = _run_tool(port, "call", "fs.read", '{"name":"m7.txt"}')
        assert proc.returncode == 0, "fs.read rc=%d std=%r" % (
            proc.returncode, (proc.stdout or "")[-800:])
        got = _tool_result(proc)
        assert got.get("data") == "hello-serial", "fs.read data=%r" % (got.get("data"),)
        assert got.get("size") == len("hello-serial"), "fs.read size=%r" % (got.get("size"),)
    ctx.checks.append("tools/list 6 工具 + fs.write/read 往返 hello-serial")


@hw_case("m7/tool_replay_dedup", timeout=120)
def test_tool_replay_dedup(ctx):
    """工具 --seq/--no-ack 构造重放：同 seq 第二次响应与第一次逐字一致（设备重放缓存）。"""
    port = ctx.hw.port
    line = '{"jsonrpc":"2.0","id":7,"method":"ping"}'
    with ctx.released():
        p1 = _run_tool(port, "--no-ack", "--seq", "900", "raw", line)
        assert p1.returncode == 0, "no-ack raw rc=%d stderr=%r" % (
            p1.returncode, (p1.stderr or "")[-800:])
        a = _json_line(p1)
        p2 = _run_tool(port, "--seq", "900", "raw", line)
        assert p2.returncode == 0, "replay raw rc=%d stderr=%r" % (
            p2.returncode, (p2.stderr or "")[-800:])
        b = _json_line(p2)
    assert a == b, "同 seq 第二次没有重放缓存响应（可能重新执行了）: %r vs %r" % (a, b)
    assert (a.get("result") or {}).get("pong") is True, "首次响应非 ping 结果: %r" % (a,)
    ctx.checks.append("--no-ack 后同 seq 重放响应逐字一致（含 tick，证明未重执行）")


@hw_case("m7/tool_reset_recovery", timeout=120)
def test_tool_reset_recovery(ctx):
    """工具 --reset：复位板子后重新握手成功（串口通道在真机重启后自恢复）。"""
    port = ctx.hw.port
    with ctx.released(reset_after=False):
        proc = _run_tool(port, "--reset", "selftest")
        assert proc.returncode == 0, "--reset selftest rc=%d stdout=%r stderr=%r" % (
            proc.returncode, (proc.stdout or "")[-1200:], (proc.stderr or "")[-1200:])
        assert "selftest PASS" in (proc.stdout or ""), (proc.stdout or "")[-1200:]
    ctx.checks.append("--reset 后 selftest PASS（工具自动复位+握手恢复）")
