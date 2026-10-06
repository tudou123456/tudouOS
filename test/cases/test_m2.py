"""M2 验收用例：tick 存活 / UART RX 中断 / 异常现场与恢复 / 追踪环 / 抖动冒烟。

M2 时间方案背景（QEMU Windows 已知限制，详见 docs/qemu-testing.md）：
内核 tick 用核内 CCOMPARE0（level-1 通用异常路径），QEMU 的 ccount 按翻译块
大步推进会跳过比较点，内核按跨过的周期数补记——tick 计数与真实时间对齐，
但"触发间隔抖动"（jit）在 QEMU 中天然偏大（毫秒级），硬性抖动预算留待真机。
"""

import time

READY_LINE = r"\[POTATO\] READY"

TICK_LINE = (r"\[POTATO\]\[TICK\] ticks=(\d+) cpu_hz=(\d+) now_us=0x([0-9a-f]+) "
             r"jit_min=(\d+)us jit_max=(\d+)us uart_irq=(\d+) rx_bytes=(\d+)")


def _tickstat(ctx):
    ctx.serial.send("tickstat")
    return ctx.serial.expect(TICK_LINE, timeout=5)


def test_tick_alive(ctx):
    """1kHz tick 与真实时间对齐（跳拍补记后 ±20% 容差）。"""
    ctx.serial.expect(READY_LINE, timeout=20)
    t1 = _tickstat(ctx)
    time.sleep(1.1)
    t2 = _tickstat(ctx)
    d = int(t2.group(1)) - int(t1.group(1))
    assert 800 <= d <= 1200, f"1.1s 内 tick 增量 {d} 超出容差"
    assert int(t2.group(2)) == 40000000, f"cpu_hz 异常: {t2.group(2)}"
    assert int(t2.group(5)) > 0, "jit_max 应有观测值"


def test_uart_rx_irq(ctx):
    """RX 走中断+环形缓冲：连发 20 行命令全部应答且不丢。"""
    ctx.serial.expect(READY_LINE, timeout=20)
    for i in range(20):
        ctx.serial.send(f"echo line{i}")
        ctx.serial.expect(rf"line{i}", timeout=3)
    m = _tickstat(ctx)
    assert int(m.group(6)) >= 20, f"UART RX 中断计数 {m.group(6)} 应 ≥20"


def test_exception_dump(ctx):
    """crash 注入非法指令：异常现场完整打印，QMP 复位后可完整重启。"""
    ctx.serial.expect(READY_LINE, timeout=20)
    ctx.serial.send("crash")
    ctx.serial.expect(r"\[POTATO\]\[EXC\] EXCEPTION", timeout=5)
    m = ctx.serial.expect(
        r"\[POTATO\]\[EXC\] cause=(\d+)\(([A-Z_]+)\) epc=0x([0-9a-f]+) "
        r"vaddr=0x([0-9a-f]+) ps=0x([0-9a-f]+)", timeout=5)
    assert m.group(1) == "0" and m.group(2) == "ILLEGAL_INSTRUCTION"
    # 注：QEMU 不给通用异常回填 EPC1/EPS1（见 docs/qemu-testing.md 已知问题），
    # epc/vaddr/ps 在 QEMU 中可为 0，真机上有值；此处只断言 cause 与恢复语义。
    ctx.serial.expect(r"\[POTATO\]\[EXC\] HALTED", timeout=5)

    ctx.qmp.command("system_reset")
    ctx.serial.expect(r"ESP-ROM: ?esp32s3", timeout=20)
    ctx.serial.expect(READY_LINE, timeout=10)


def test_trace_dump(ctx):
    """追踪环：事件序号递增、时间戳单调不减。"""
    ctx.serial.expect(READY_LINE, timeout=20)
    time.sleep(0.3)  # 让 tick 事件积累
    ctx.serial.send("trace 8")
    ctx.serial.expect(r"\[POTATO\]\[TR\] total=\d+ dropped=\d+ last 8:", timeout=5)
    ts, seqs = [], []
    for _ in range(8):
        m = ctx.serial.expect(
            r"\[POTATO\]\[TR\] #(\d+) t=(\d+) (IRQ_ENTER|IRQ_EXIT|EXCEPTION)", timeout=3)
        seqs.append(int(m.group(1)))
        ts.append(int(m.group(2)))
    assert all(a <= b for a, b in zip(seqs, seqs[1:])), f"序号非递增: {seqs}"
    assert all(a <= b for a, b in zip(ts, ts[1:])), f"时间戳非单调: {ts}"


def test_irq_latency_smoke(ctx):
    """抖动冒烟（QEMU 版）：只断言有界（<10ms）防退化；硬预算以真机为准。"""
    ctx.serial.expect(READY_LINE, timeout=20)
    time.sleep(1.0)
    m = _tickstat(ctx)
    jit_max = int(m.group(5))
    assert jit_max < 10000, f"tick 触发最大间隔 {jit_max}us 超过 10ms 冒烟上限"
