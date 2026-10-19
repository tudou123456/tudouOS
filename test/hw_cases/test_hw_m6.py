"""M6 真机验收：UART JSON-RPC MCP（``{`` 开头的行）+ fs 命令 + 生命周期。

断言来源与期望字符串照抄 test/cases/test_m6.py（QEMU），真机差异：
- QEMU 用 ``-serial tcp`` 台架；真机同样走 USB 串口 115200，MCP 行协议不变；
- M7 新增的 ``@@MCP`` 帧通道由 HwSerial 过滤，不干扰本文件的行级 MCP 断言；
- 文件系统在真机是 RAM potfs（掉电丢失），用例内自行 mkfs 建立干净盘。
"""

from __future__ import annotations

import json
import re
import time

from test.hw_runner import hw_case
from test.hw_util import no_crash, poll

MCP_STARTED = r"\[POTATO\]\[MCP\] agent started[^\r]*\r"
MCP_STOPREQ = r"\[POTATO\]\[MCP\] stop-req[^\r\n]*\r"
MCP_STOPPED = r"\[POTATO\]\[MCP\] agent stopped\r"
RESP = r'\{"jsonrpc":"2\.0","id":(?:\d+|null|"[^"]*"),(?:"result"|"error").*\}\r'


def _line(obj) -> str:
    return json.dumps(obj, separators=(",", ":"), ensure_ascii=False)


def _send_rpc(ctx, obj, pattern=None, timeout=10.0):
    ctx.send(_line(obj))
    return ctx.expect(pattern or RESP, timeout)


def _mcp_on(ctx):
    ctx.cmd("mcp on", MCP_STARTED, 10.0)


# ---------------------------------------------------------------------- M6.1 行级 MCP
@hw_case("m6/mcp_on_ping", timeout=20, smoke=True)
def test_mcp_on_ping(ctx):
    """mcp on → ping/echo/sysinfo 三个核心方法（QEMU test_m6 同款请求）。"""
    _mcp_on(ctx)
    ctx.send(_line({"jsonrpc": "2.0", "id": 1, "method": "ping"}))
    line = ctx.wait_line(r'\{"jsonrpc":"2\.0","id":1,"result":\{', 10.0)
    assert '"pong":true' in line, "ping 结果缺 pong: %r" % line
    assert '"tick"' in line, "ping 结果缺 tick 字段: %r" % line

    _send_rpc(ctx, {"jsonrpc": "2.0", "id": 2, "method": "sysinfo"},
              r'\{"jsonrpc":"2\.0","id":2,"result":\{[^\r\n]*"name":"tudouOS"')
    _send_rpc(ctx, {"jsonrpc": "2.0", "id": 3, "method": "echo",
                    "params": {"text": "hello potato"}},
              r'\{"jsonrpc":"2\.0","id":3,"result":\{[^\r\n]*"echo":"hello potato"')
    ctx.checks.append("mcp on + ping/echo/sysinfo ok（id 1/2/3 配对）")


@hw_case("m6/rpc_errors", timeout=20)
def test_rpc_errors(ctx):
    """错误路径：未知方法 -32601 / 非法 JSON -32700（QEMU 同锚点），之后仍能服务。"""
    _mcp_on(ctx)
    _send_rpc(ctx, {"jsonrpc": "2.0", "id": 5, "method": "nope"},
              r'\{"jsonrpc":"2\.0","id":5,"error":\{[^\r\n]*"code":-32601[^\r\n]*"method not found"')
    ctx.send('{"broken json!!!')
    ctx.expect(r'\{"jsonrpc":"2\.0","id":null,"error":\{[^\r\n]*"code":-32700'
               r'[^\r\n]*"parse error"', 8.0)
    _send_rpc(ctx, {"jsonrpc": "2.0", "id": 6, "method": "ping"},
              r'\{"jsonrpc":"2\.0","id":6,"result":\{"pong":true')
    ctx.checks.append("不存在的 method→-32601，坏 JSON→-32700，之后 ping 正常")


@hw_case("m6/initialize_tools", timeout=30)
def test_initialize_tools(ctx):
    """initialize / tools:list / tools:call / 未知工具 / 通知不回复 / 字符串 id（QEMU 同源）。"""
    _mcp_on(ctx)
    _send_rpc(ctx, {"jsonrpc": "2.0", "id": 21, "method": "initialize",
                    "params": {"protocolVersion": "2025-06-18",
                               "capabilities": {},
                               "clientInfo": {"name": "hw_runner", "version": "1.0"}}},
              r'\{"jsonrpc":"2\.0","id":21,"result":\{[^\r\n]*'
              r'"protocolVersion":"2025-06-18"')
    _send_rpc(ctx, {"jsonrpc": "2.0", "id": 22, "method": "tools/list"},
              r'\{"jsonrpc":"2\.0","id":22,"result":\{"tools":\[\{"name"')
    _send_rpc(ctx, {"jsonrpc": "2.0", "id": 23, "method": "tools/call",
                    "params": {"name": "echo", "arguments": {"text": "hi"}}},
              r'"content":\[\{"type":"text","text":')
    ctx.expect(r'\\"echo\\":\\"hi\\"', 5.0)
    _send_rpc(ctx, {"jsonrpc": "2.0", "id": 24, "method": "tools/call",
                    "params": {"name": "nope", "arguments": {}}},
              r'\{"jsonrpc":"2\.0","id":24,"error":\{[^\r\n]*"code":-32602'
              r'[^\r\n]*"unknown tool"')

    # notification（无 id）必须静默：随后 ping id=25 的响应之前不得出现 id:null 结果
    mark = ctx.mark()
    ctx.send(_line({"jsonrpc": "2.0", "method": "notifications/initialized"}))
    time.sleep(1.0)
    assert '"id":null,"result"' not in ctx.text_since(mark), \
        "notification 不应产生响应，但收到了 id:null result"
    _send_rpc(ctx, {"jsonrpc": "2.0", "id": 25, "method": "ping"},
              r'\{"jsonrpc":"2\.0","id":25,"result":\{"pong":true')

    # 字符串 id 原样回显
    _send_rpc(ctx, {"jsonrpc": "2.0", "id": "sid", "method": "ping"},
              r'\{"jsonrpc":"2\.0","id":"sid","result":\{"pong":true')
    ctx.checks.append("initialize/tools:list/tools:call/unknown tool/notification/字符串 id 全覆盖")


@hw_case("m6/fs_roundtrip", timeout=30)
def test_fs_roundtrip(ctx):
    """fs.write/read 经 MCP + fs 控制台命令 mkfs/write/cat/ls/rm/ls（QEMU 同锚点）。"""
    _mcp_on(ctx)
    _send_rpc(ctx, {"jsonrpc": "2.0", "id": 7, "method": "fs.write",
                    "params": {"name": "b.txt", "data": "via-mcp"}},
              r'\{"jsonrpc":"2\.0","id":7,"result":\{[^\r\n]*"written":7')
    line = ctx.cmd_line(_line({"jsonrpc": "2.0", "id": 8, "method": "fs.read",
                               "params": {"name": "b.txt"}}),
                        r'\{"jsonrpc":"2\.0","id":8,"result":\{[^\r\n]*\}\r',
                        timeout=8.0)
    assert '"data":"via-mcp"' in line, "fs.read 响应缺 data: %r" % line
    assert '"size":7' in line, "fs.read 响应缺 size: %r" % line

    ctx.cmd("fs mkfs", r"\[POTATO\]\[FS\] formatted\r", 8.0)
    ctx.cmd("fs write test.txt hello-potfs",
            r"\[POTATO\]\[FS\] wrote test\.txt 11 bytes\r", 8.0)
    ctx.cmd("fs cat test.txt", r"hello-potfs\r", 8.0)
    ctx.cmd("fs ls", r"\[POTATO\]\[FS\] test\.txt 11\r", 8.0)
    ctx.cmd("fs rm test.txt", r"\[POTATO\]\[FS\] removed test\.txt\r", 8.0)
    ctx.cmd("fs ls", r"\[POTATO\]\[FS\] 0 files, 0 bytes\r", 8.0)
    no_crash(ctx.text_since(ctx.start_pos), "m6/fs")
    ctx.checks.append("fs.write/read via MCP + mkfs/write/cat/ls/rm/ls")


@hw_case("m6/lifecycle_multi", timeout=30)
def test_lifecycle_multi(ctx):
    """mcp on/重复 on/off/再 on + 多请求 id 11..14（QEMU test_m6 生命周期 + multi_request）。"""
    _mcp_on(ctx)
    mark = ctx.mark()
    ctx.send("mcp on")
    poll(ctx, "重复 mcp on 提示", lambda t: "already running" in t, 6.0)
    assert "already running" in ctx.text_since(mark), "重复 mcp on 未报 already running"
    ctx.cmd("mcp off", MCP_STOPREQ, 8.0)
    ctx.expect(MCP_STOPPED, 8.0)
    _mcp_on(ctx)
    for i in (11, 12, 13, 14):
        _send_rpc(ctx, {"jsonrpc": "2.0", "id": i, "method": "ping"},
                  r'\{"jsonrpc":"2\.0","id":%d,"result":\{"pong":true' % i)
    ctx.checks.append("mcp 生命周期 on/already/off/on，多请求 id 11–14 全部配对")


@hw_case("m6/unicode_line_limits", timeout=30)
def test_unicode_line_limits(ctx):
    """UTF-8 中文/emoji echo + MCP 行长度限制（QEMU test_m6 同判据）。"""
    _mcp_on(ctx)
    _send_rpc(ctx, {"jsonrpc": "2.0", "id": 31, "method": "echo",
                    "params": {"text": "中文测试 🚀"}},
              r'"echo":"中文测试 🚀"')

    def padded(n, c="y", rid=42):
        head = ('{"jsonrpc":"2.0","id":%d,"method":"echo","params":{"text":"'
                % rid)
        tail = '"}}'
        return head + c * (n - len(head) - len(tail)) + tail

    normal = padded(238, rid=41)
    assert len(normal.encode("utf-8")) == 238, len(normal.encode("utf-8"))
    ctx.send(normal)
    ctx.expect(r'\{"jsonrpc":"2\.0","id":41,"result"', 8.0)

    big = padded(258, rid=42)
    assert len(big.encode("utf-8")) == 258
    ctx.send(big)
    ctx.expect(r'"code":-32600[^\r\n]*max 256', 8.0)

    huge = padded(358, rid=43)
    assert len(huge.encode("utf-8")) == 358
    ctx.send(huge)
    ctx.expect(r"line too long \(max 263\), dropped\r", 8.0)

    _send_rpc(ctx, {"jsonrpc": "2.0", "id": 44, "method": "ping"},
              r'\{"jsonrpc":"2\.0","id":44,"result":\{"pong":true')
    ctx.checks.append("unicode echo 中文测试 🚀；238B ok / 258B -32600 / 358B line-too-long")
