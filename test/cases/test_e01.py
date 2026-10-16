"""E-01 判定实验回归用例：零开销 loop 指令在切换/中断下正确。

背景：docs/qemu-testing.md 历史结论"QEMU 不支持 loop 指令"已被
2026-10-16 判定实验证伪。本用例固化该结论：looptest 命令内联手写
loopnez 长循环 + volatile 参照实现逐轮比对，测试期间持续发送串口
字节制造 level-2 RX 中断打断循环体。mismatches 必须为 0。
"""

READY_LINE = r"\[POTATO\] READY"


def test_loop_instruction(ctx):
    """loop 指令正确性：切换 + RX 中断风暴下 200 轮零失配。"""
    ctx.serial.expect(READY_LINE, timeout=20)
    ctx.serial.send("looptest 200")
    for _ in range(6):  # 中断源：测试运行期间持续回车（每字节一次 RX IRQ）
        ctx.serial.send("")
        import time
        time.sleep(0.3)
    m = ctx.serial.expect(r"\[POTATO\]\[E1\] looptest done rounds=(\d+) "
                          r"mismatches=(\d+)", timeout=60)
    assert int(m.group(1)) == 200, f"轮数异常: {m.group(1)}"
    assert int(m.group(2)) == 0, f"loop 指令失配 {m.group(2)} 次（E-01 复发）"
