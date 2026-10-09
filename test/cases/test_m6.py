"""M6 验收用例：MCP 协议适配器 + potfs 文件系统 + 串口 Shell。

支柱1 收官：Agent → 传输(UART 行) → 消息基座 → 工具 → 响应 JSON。
"""

import time

READY_LINE = r"\[POTATO\] READY"
RESULT = r'\{"jsonrpc":"2\.0","id":(\d+),"result":'


def _mcp(ctx, line, timeout=15):
    ctx.serial.send(line)
    m = ctx.serial.expect(r'\{"jsonrpc":"2\.0","id":\d+,(?:"result"|"error").*\}\r',
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
    assert '"name":"tudouOS"' in resp and '"version":"0.7.0"' in resp, resp
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
