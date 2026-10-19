"""M7 无网卡降级用例：没有 EMAC 设备时以太网不应初始化，系统照常可用。

QEMU 的 esp32s3 机器默认会给一块 user 网卡（open_eth 模型），所以必须用
模块级 `-nic none` 才能构造「无网卡」场景；runner 会把模块级 QEMU_ARGS
追加到该模块每个用例的 QEMU 命令行。

诚信性要求：设备缺席必须是**显式**的（探测判缺席 → 驱动 skipped → 协议层
明确的 disabled 行），而不是静默失败或拖垮启动。
"""

READY_LINE = r"\[POTATO\] READY"
QEMU_ARGS = ["-nic", "none"]


def test_m7_nodev_absent_is_explicit_and_harmless(ctx):
    """无 -nic：探测缺席、net 驱动 skipped、显式禁用；net present=0；控制台/MCP 仍正常。

    顺序：pot_drivers_init() 的驱动 skipped 行与 net_start() 的禁用行都在
    启动横幅与 READY 之前打印。
    """
    # 驱动框架：探测读回毒值（0xdeadbeef）→ EMAC 判缺席 → net 驱动 init 返回 1
    m = ctx.serial.expect(r"\[POTATO\]\[NET\] EMAC probe: [^\r\n]* -> absent", timeout=20)
    assert "absent" in m.group(0), f"EMAC 探测未判缺席: {m.group(0)}"
    ctx.serial.expect(r"\[POTATO\]\[DRV\] net init skipped", timeout=10)
    # 协议层：一行明确的禁用打印（不是静默跳过）
    m = ctx.serial.expect(
        r"\[POTATO\]\[NET\] no EMAC device: ethernet disabled\r", timeout=10)
    assert "ethernet disabled" in m.group(0), f"未显式禁用: {m.group(0)}"
    ctx.serial.expect(READY_LINE, timeout=10)

    ctx.serial.send("net")
    ctx.serial.expect(r"\[POTATO\]\[NET\] present=0", timeout=10)

    # 系统其余部分不受影响：MCP 代理可起、ping 工具可用
    ctx.serial.send("mcp on")
    ctx.serial.expect(r"\[POTATO\]\[MCP\] agent started", timeout=15)
    ctx.serial.send('{"jsonrpc":"2.0","id":31,"method":"ping"}')
    m = ctx.serial.expect(r'\{"jsonrpc":"2\.0","id":31,"result":\{"pong":true[^\r\n]*',
                          timeout=10)
    assert '"pong":true' in m.group(0)
