"""D-02 回归用例：任务参数经上下文切换链路送达。

背景：pot_do_resume 曾借 a3 作 sar/lbeg/lend/lcount/PS 的暂存寄存器，
任务首启时收到的 arg 是 PS 值而非创建时传入的指针。argtest 命令创建
一个真正解引用参数的探测任务（解引用 PS 值会立即现形）。
"""

READY_LINE = r"\[POTATO\] READY"


def test_task_arg(ctx):
    """任务收到的参数 == 创建时传入的指针内容（魔数）。"""
    ctx.serial.expect(READY_LINE, timeout=20)
    ctx.serial.send("argtest")
    # \r 行尾锚点防半行早匹配
    m = ctx.serial.expect(r"\[POTATO\]\[ARG\] recv=0x([0-9a-f]+) "
                          r"expect=0x([0-9a-f]+)\r", timeout=30)
    assert m.group(1) == m.group(2) == "c0ffee42", \
        f"任务参数损坏: recv=0x{m.group(1)} expect=0x{m.group(2)}"
    ctx.serial.expect(r"\[POTATO\]\[ARG\] PASS\r", timeout=5)
