"""M6 验收用例：MCP 协议适配器 + potfs 文件系统 + 串口 Shell。

支柱1 收官：Agent → 传输(UART 行) → 消息基座 → 工具 → 响应 JSON。
含缺陷修复回归：D-01 多请求、D-07 行长、D-19 unicode、D-20 协议语义。
"""

import re
import time

from test.serial_tap import SerialTimeoutError

READY_LINE = r"\[POTATO\] READY"
RESULT = r'\{"jsonrpc":"2\.0","id":(\d+),"result":'


def _mcp(ctx, line, timeout=15):
    """发送一行 JSON-RPC 并等待响应（id 支持数字/字符串/null 三态）。"""
    ctx.serial.send(line)
    m = ctx.serial.expect(
        r'\{"jsonrpc":"2\.0","id":(?:\d+|null|"[^"]*"),(?:"result"|"error").*\}\r',
        timeout=timeout)
    return m.group(0)


def test_mcp_ping(ctx):
    """ping 工具：pong+tick 回执。"""
    ctx.serial.expect(READY_LINE, timeout=20)
    ctx.serial.send("mcp on")
    ctx.serial.expect(r"\[POTATO\]\[MCP\] agent started", timeout=15)
    resp = _mcp(ctx, '{"jsonrpc":"2.0","id":1,"method":"ping"}')
    assert '"pong":true' in resp and '"tick":' in resp, resp


def test_mcp_echo_sysinfo(ctx):
    """echo 参数回显 + sysinfo 系统状态（支柱3 健康快照经 MCP 暴露）。"""
    ctx.serial.expect(READY_LINE, timeout=20)
    ctx.serial.send("mcp on")
    ctx.serial.expect(r"\[POTATO\]\[MCP\] agent started", timeout=15)
    resp = _mcp(ctx, '{"jsonrpc":"2.0","id":2,"method":"echo","params":{"text":"hello potato"}}')
    assert '"echo":"hello potato"' in resp, resp
    resp = _mcp(ctx, '{"jsonrpc":"2.0","id":3,"method":"sysinfo"}')
    assert '"name":"tudouOS"' in resp, resp
    assert re.search(r'"version":"\d+\.\d+\.\d+"', resp), resp  # 版本号格式断言（D-08 同款）
    assert '"heap_free":' in resp, resp


def test_mcp_error_paths(ctx):
    """错误路径：未知方法 -32601、畸形 JSON -32700。"""
    ctx.serial.expect(READY_LINE, timeout=20)
    ctx.serial.send("mcp on")
    ctx.serial.expect(r"\[POTATO\]\[MCP\] agent started", timeout=15)
    resp = _mcp(ctx, '{"jsonrpc":"2.0","id":5,"method":"nope"}')
    assert '"code":-32601' in resp and '"method not found"' in resp, resp
    resp = _mcp(ctx, '{"broken json!!!')
    assert '"code":-32700' in resp and '"parse error"' in resp, resp


def test_mcp_fs_roundtrip(ctx):
    """MCP 工具操作文件系统：fs.write → fs.read 数据回环。"""
    ctx.serial.expect(READY_LINE, timeout=20)
    ctx.serial.send("mcp on")
    ctx.serial.expect(r"\[POTATO\]\[MCP\] agent started", timeout=15)
    resp = _mcp(ctx, '{"jsonrpc":"2.0","id":7,"method":"fs.write","params":{"name":"b.txt","data":"via-mcp"}}')
    assert '"written":7' in resp, resp
    resp = _mcp(ctx, '{"jsonrpc":"2.0","id":8,"method":"fs.read","params":{"name":"b.txt"}}')
    assert '"data":"via-mcp"' in resp and '"size":7' in resp, resp


def test_fs_shell(ctx):
    """Shell 文件操作：write/cat/ls/rm 全流程。"""
    ctx.serial.expect(READY_LINE, timeout=20)
    ctx.serial.send("fs mkfs")
    ctx.serial.expect(r"\[POTATO\]\[FS\] formatted\r", timeout=8)
    ctx.serial.send("fs write test.txt hello-potfs")
    ctx.serial.expect(r"\[POTATO\]\[FS\] wrote test.txt 11 bytes\r", timeout=8)
    ctx.serial.send("fs cat test.txt")
    ctx.serial.expect(r"hello-potfs\r", timeout=8)
    ctx.serial.send("fs ls")
    ctx.serial.expect(r"\[POTATO\]\[FS\] test.txt 11\r", timeout=8)
    ctx.serial.send("fs rm test.txt")
    ctx.serial.expect(r"\[POTATO\]\[FS\] removed test.txt\r", timeout=8)
    ctx.serial.send("fs ls")
    ctx.serial.expect(r"\[POTATO\]\[FS\] 0 files, 0 bytes\r", timeout=8)


def test_mcp_agent_lifecycle(ctx):
    """代理生命周期：on/off/重复 on 幂等。"""
    ctx.serial.expect(READY_LINE, timeout=20)
    ctx.serial.send("mcp on")
    ctx.serial.expect(r"\[POTATO\]\[MCP\] agent started", timeout=15)
    ctx.serial.send("mcp on")
    ctx.serial.expect(r"already running", timeout=8)
    ctx.serial.send("mcp off")
    ctx.serial.expect(r"\[POTATO\]\[MCP\] stop-req\r", timeout=8)
    ctx.serial.expect(r"\[POTATO\]\[MCP\] agent stopped\r", timeout=15)
    ctx.serial.send("mcp on")  # 重启代理
    ctx.serial.expect(r"\[POTATO\]\[MCP\] agent started", timeout=15)
    resp = _mcp(ctx, '{"jsonrpc":"2.0","id":9,"method":"ping"}')
    assert '"pong":true' in resp, resp


def test_mcp_multi_request(ctx):
    """D-01 回归：连发多条 JSON-RPC，每条都有对应响应（去重不误杀）。"""
    ctx.serial.expect(READY_LINE, timeout=20)
    ctx.serial.send("mcp on")
    ctx.serial.expect(r"\[POTATO\]\[MCP\] agent started", timeout=15)
    for i in (11, 12, 13, 14):
        ctx.serial.send(f'{{"jsonrpc":"2.0","id":{i},"method":"ping"}}')
    for i in (11, 12, 13, 14):
        m = ctx.serial.expect(rf'\{{"jsonrpc":"2\.0","id":{i},"result":.*\}}\r',
                              timeout=15)
        assert '"pong":true' in m.group(0)


def test_mcp_initialize(ctx):
    """D-20：MCP 握手/列表/调用语义（2025-06-18 规范核心）。"""
    ctx.serial.expect(READY_LINE, timeout=20)
    ctx.serial.send("mcp on")
    ctx.serial.expect(r"\[POTATO\]\[MCP\] agent started", timeout=15)

    # initialize 握手
    resp = _mcp(ctx, '{"jsonrpc":"2.0","id":21,"method":"initialize",'
                     '"params":{"protocolVersion":"2025-06-18","capabilities":{},'
                     '"clientInfo":{"name":"t","version":"1"}}}')
    assert '"protocolVersion":"2025-06-18"' in resp, resp
    assert '"serverInfo":{"name":"tudouOS"' in resp, resp
    assert '"tools":{' in resp, resp

    # notifications/initialized：notification 无 id，不得回包
    ctx.serial.send('{"jsonrpc":"2.0","method":"notifications/initialized"}')
    try:
        ctx.serial.expect(r'"id":null,"result"', timeout=1.5)
        raise AssertionError("notification 不应产生响应")
    except SerialTimeoutError:
        pass

    # tools/list：规范 schema 形态
    resp = _mcp(ctx, '{"jsonrpc":"2.0","id":22,"method":"tools/list"}')
    assert '"tools":[{"name"' in resp, resp
    assert '"inputSchema":{"type":"object"' in resp, resp
    assert '"required":[' in resp, resp

    # tools/call：content 形态 + 参数对象
    resp = _mcp(ctx, '{"jsonrpc":"2.0","id":23,"method":"tools/call",'
                     '"params":{"name":"echo","arguments":{"text":"hi"}}}')
    assert '"content":[{"type":"text","text":' in resp, resp
    assert '\\"echo\\":\\"hi\\"' in resp, resp

    # tools/call 未知工具 → -32602
    resp = _mcp(ctx, '{"jsonrpc":"2.0","id":24,"method":"tools/call",'
                     '"params":{"name":"nope"}}')
    assert '"code":-32602' in resp and "unknown tool" in resp, resp

    # 字符串 id 原样回显
    resp = _mcp(ctx, '{"jsonrpc":"2.0","id":"sid","method":"ping"}')
    assert '"id":"sid"' in resp and '"pong":true' in resp, resp


def test_mcp_unicode(ctx):
    """D-19 回归：\\uXXXX 转义（含代理对 emoji）正确解析并 UTF-8 回传。"""
    ctx.serial.expect(READY_LINE, timeout=20)
    ctx.serial.send("mcp on")
    ctx.serial.expect(r"\[POTATO\]\[MCP\] agent started", timeout=15)
    resp = _mcp(ctx, '{"jsonrpc":"2.0","id":31,"method":"echo",'
                     '"params":{"text":"\\u4e2d\\u6587\\u6d4b\\u8bd5 \\ud83d\\ude80"}}')
    assert '"echo":"中文测试 🚀"' in resp, resp


def test_mcp_line_len(ctx):
    """D-07 回归：行长度三档——正常放行 / 超内联负载显式拒绝 / 超控制台缓冲显式丢弃。"""
    ctx.serial.expect(READY_LINE, timeout=20)
    ctx.serial.send("mcp on")
    ctx.serial.expect(r"\[POTATO\]\[MCP\] agent started", timeout=15)

    # ~238 字节：正常处理
    resp = _mcp(ctx, '{"jsonrpc":"2.0","id":41,"method":"echo","params":{"text":"'
                     + "x" * 180 + '"}}')
    assert '"echo":"' + "x" * 180 in resp, resp

    # 258 字节：过控制台（≤263）但超内联负载（>256）→ -32600 显式报错
    ctx.serial.send('{"jsonrpc":"2.0","id":42,"method":"echo","params":{"text":"'
                    + "y" * 200 + '"}}')
    m = ctx.serial.expect(r'"code":-32600.*max 256', timeout=10)
    assert m

    # 358 字节：超控制台缓冲 → 整行显式丢弃
    ctx.serial.send('{"jsonrpc":"2.0","id":43,"method":"echo","params":{"text":"'
                    + "z" * 300 + '"}}')
    ctx.serial.expect(r"line too long \(max 263\), dropped", timeout=10)
    # 丢弃后系统仍健康
    resp = _mcp(ctx, '{"jsonrpc":"2.0","id":44,"method":"ping"}')
    assert '"pong":true' in resp, resp
