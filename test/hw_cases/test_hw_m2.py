"""M2 真机验收：tick/时间基、UART 中断、异常转储、trace 环、延迟实测。

断言来源与期望值照抄 test/cases/test_m2.py（QEMU），真机差异显式标注：

- QEMU 用例断言 ``cpu_hz == 40000000``（QEMU 固定值）；真机 cpu_hz 由
  systimer 校准（旧固件基线 19,000,000Hz），故改为：>0 且与 pal/tickstat
  自洽，并记录实测值；
- QEMU 用例的 1.1s tick 增量 800..1200 保持不变（tick=1kHz，真机同样适用，
  若校准错误这里就会失败——这是真实判据，不放宽）；
- QEMU 异常里 epc/vaddr 允许为 0（QEMU 限制）；真机 epc 必须非 0；
- irq_latency_smoke 的 jit_max<120000us 是 QEMU poll 模式的冒烟上限；真机
  用同一上限（旧固件基线 45169us，仍应通过），另加 jit_diagnose 如实记录。
"""

from __future__ import annotations

import re
import time

from test.hw_runner import hw_case
from test.hw_util import (READY, TICK, TR_EVENT, collect_tr, count, events_since,
                          no_crash, poll_since,
                          parse_tick, poll, tickstat, tr_events,
                          wait_tick_settle)

PAL = r"\[POTATO\]\[PAL\] platform=(\S+) tick=(\w+) cpu_hz=(\d+)\r"
EXC_BANNER = r"\[POTATO\]\[EXC\] EXCEPTION\r"
EXC_CAUSE = (r"\[POTATO\]\[EXC\] cause=(\d+)\(([A-Z_]+)\) epc=0x([0-9a-f]+) "
             r"vaddr=0x([0-9a-f]+) ps=0x([0-9a-f]+)")
EXC_TASK = r"\[POTATO\]\[EXC\] task=[^\r\n]*\r"
EXC_HALT = r"\[POTATO\]\[EXC\] HALTED[^\r\n]*\r"
TR_HEAD = r"\[POTATO\]\[TR\] total=(\d+) dropped=(\d+) last (\d+):\r"
LOOP_DONE = r"\[POTATO\]\[E1\] looptest done rounds=(\d+) mismatches=(\d+)\r"
E2_H = r"\[POTATO\]\[E2\]\[H\] cause=0\(ILLEGAL_INSTRUCTION\)[^\r\n]*\r"
E2_PASS = r"\[POTATO\]\[E2\] PASS[^\r\n]*\r"


@hw_case("m2/tickstat_contract", timeout=12, smoke=True)
def test_tickstat_contract(ctx):
    """tickstat 行字段齐全且自洽（QEMU test_m2 tick_alive 的字段判据）。

    真机启动瞬态（实测）：READY 后第一次 tickstat
    可能是 ticks=0、now_us>0、jit_min=0xFFFFFFFF（tick_init 早于 tick_irq_arm，
    首个 IRQ 补账之前）。先 wait_tick_settle 等补账结束，再做 QEMU 同源字段
    判据；瞬态本身由 m2/jit_diagnose 如实记录并上报，不放宽判据。
    """
    t = wait_tick_settle(ctx)
    assert t["cpu_hz"] > 0, "cpu_hz 非正"
    assert t["ticks"] > 0, "ticks 非正"
    assert t["now_us"] > 0, "now_us 非正"
    assert t["jit_min"] > 0, "jit_min 应为正（有 tick 后开始统计）"
    assert t["jit_max"] > 0, "jit_max 应为正"
    assert t["jit_min"] <= t["jit_max"], "jit_min=%d > jit_max=%d" % (
        t["jit_min"], t["jit_max"])
    ctx.checks.append("tickstat ticks=%d cpu_hz=%d jit=%d..%dus now_us=0x%x"
                      % (t["ticks"], t["cpu_hz"], t["jit_min"], t["jit_max"],
                         t["now_us"]))


@hw_case("m2/tick_rate_1s", timeout=12)
def test_tick_rate_1s(ctx):
    """1.1s 墙钟内 tick 增量 800..1200（@1kHz），QEMU test_m2 同一判据。"""
    t1 = wait_tick_settle(ctx)   # 跳过 READY 后首次 IRQ 补账的启动瞬态
    time.sleep(1.1)
    t2 = tickstat(ctx)
    delta = t2["ticks"] - t1["ticks"]
    assert 800 <= delta <= 1200, (
        "1.1s tick 增量 %d 不在 800..1200（tick 时基/校准错误？cpu_hz=%d，"
        "now_us 0x%x->0x%x）" % (delta, t2["cpu_hz"], t1["now_us"], t2["now_us"]))
    assert t2["cpu_hz"] == t1["cpu_hz"], "cpu_hz 两次不一致"
    assert t2["now_us"] > t1["now_us"], "now_us 未推进"
    ctx.checks.append("1.1s ticks +%d, cpu_hz=%d" % (delta, t2["cpu_hz"]))


@hw_case("m2/pal_tick_consistency", timeout=10)
def test_pal_tick_consistency(ctx):
    """真机平台名/中断 tick/校准频率与 tickstat 自洽（QEMU 断言 qemu-esp32s3/poll/40M）。"""
    line = ctx.cmd_line("pal", PAL)
    m = re.search(PAL, line)
    assert m, "pal 行格式不符: %r" % line
    platform, tick, cpu_hz = m.group(1), m.group(2), int(m.group(3))
    assert platform == "hw-esp32s3", "platform=%r（真机应为 hw-esp32s3）" % platform
    assert tick == "irq", "tick=%r（真机为 irq 中断 tick）" % tick
    t = tickstat(ctx)
    assert cpu_hz == t["cpu_hz"], "pal cpu_hz=%d != tickstat cpu_hz=%d" % (
        cpu_hz, t["cpu_hz"])
    assert 1_000_000 <= cpu_hz <= 500_000_000, "cpu_hz 超出合理范围: %d" % cpu_hz
    ctx.checks.append("pal platform=%s tick=%s cpu_hz=%d" % (platform, tick, cpu_hz))


@hw_case("m2/uart_rx_irq", timeout=20)
def test_uart_rx_irq(ctx):
    """20 条 echo 走 UART RX 中断，uart_irq 计数至少 +20（QEMU test_m2 同判据）。"""
    before = tickstat(ctx)["uart_irq"]
    for i in range(20):
        ctx.cmd("echo line%d" % i, r"line%d\r" % i, timeout=3.0)
    after = tickstat(ctx)["uart_irq"]
    delta = after - before
    assert after >= 20, "uart_irq=%d < 20" % after
    assert delta >= 20, "20 条命令后 uart_irq 仅 +%d（应为每行至少一次 RX 中断）" % delta
    ctx.checks.append("20 行 echo → uart_irq +%d (%d→%d)" % (delta, before, after))


@hw_case("m2/exception_crash_recover", timeout=25)
def test_exception_crash_recover(ctx):
    """crash → EXC cause=0(ILLEGAL_INSTRUCTION)/epc 非 0/HALTED；复位后恢复（QEMU 同源）。"""
    ctx.send("crash")
    ctx.expect(EXC_BANNER, 8.0)
    m = ctx.expect(EXC_CAUSE, 8.0)
    cause, name = int(m.group(1)), m.group(2)
    epc, vaddr, ps = int(m.group(3), 16), int(m.group(4), 16), int(m.group(5), 16)
    assert cause == 0 and name == "ILLEGAL_INSTRUCTION", \
        "异常原因 cause=%d(%s) 非 ILLEGAL_INSTRUCTION" % (cause, name)
    assert epc != 0, "真机异常现场 epc 不应为 0（QEMU 限制才允许 0）"
    ctx.expect(EXC_TASK, 5.0)
    ctx.expect(EXC_HALT, 5.0)
    assert "[POTATO][EXC] HALTED" in ctx.hw.text()
    # 真机恢复方式：RTS 复位（QEMU 用 QMP system_reset）
    ctx.reboot()
    ctx.cmd("ping", r"pong\r", timeout=6.0)
    ctx.checks.append("EXC cause=0 epc=0x%x vaddr=0x%x ps=0x%x, reset 后 pong"
                      % (epc, vaddr, ps))


@hw_case("m2/trace_dump", timeout=12)
def test_trace_dump(ctx):
    """trace 8：头部字段 + 8 条事件行，seq/t 非递减（QEMU test_m2 trace_dump 同源）。"""
    time.sleep(0.3)                 # 让环里至少攒够 8 个事件
    mark = ctx.mark()
    line = ctx.cmd_line("trace 8", TR_HEAD, timeout=5.0)
    m = re.search(TR_HEAD, line)
    total, dropped, last = int(m.group(1)), int(m.group(2)), int(m.group(3))
    assert last == 8, "trace 8 头部 last=%d != 8" % last
    text = collect_tr(ctx, mark, 8, timeout=6.0)
    assert len(text) >= 8, "trace 8 只输出 %d 条事件行" % len(text)
    evs = text[-8:]
    seqs = [e["seq"] for e in evs]
    ts = [e["t"] for e in evs]
    assert seqs == sorted(seqs), "trace seq 非递增: %r" % seqs
    assert ts == sorted(ts), "trace t 非递减失败: %r" % ts
    for e in evs:
        assert re.fullmatch(r"[A-Z][A-Z0-9_]*", e["ev"]), "事件名异常: %r" % e["ev"]
        assert e["t"] > 0, "事件时间戳为 0: %r" % e
    ctx.checks.append("trace total=%d dropped=%d, 8 events %s..%s"
                      % (total, dropped, seqs[0], seqs[-1]))


@hw_case("m2/trace_jsonl", timeout=12)
def test_trace_jsonl(ctx):
    """trace jsonl 16：导出行数、字段、seq 唯一递增（QEMU test_m2 同源）。"""
    time.sleep(0.2)
    mark = ctx.mark()
    line = ctx.cmd_line("trace jsonl 16", r"\[POTATO\]\[TRJ\] exported=(\d+)\r")
    exported = int(re.search(r"exported=(\d+)", line).group(1))
    assert exported == 16, "exported=%d != 16" % exported
    evs = events_since(ctx, mark)
    assert len(evs) >= 16, "JSONL 行只收到 %d 条" % len(evs)
    evs = evs[-16:]
    for e in evs:
        for key in ("seq", "t", "ev", "a", "b"):
            assert key in e, "JSONL 事件缺字段 %r: %r" % (key, e)
    seqs = [int(e["seq"]) for e in evs]
    assert len(set(seqs)) == 16, "JSONL seq 有重复: %r" % seqs
    assert seqs == sorted(seqs), "JSONL seq 非递增: %r" % seqs
    ctx.checks.append("jsonl exported=16, seq %d..%d" % (seqs[0], seqs[-1]))


@hw_case("m2/trace_imp_ring", timeout=12)
def test_trace_imp_ring(ctx):
    """trace imp 0：重要事件副环导出数一致、total>=exported（QEMU 同源）。"""
    mark = ctx.mark()
    line = ctx.cmd_line("trace imp 0",
                        r"\[POTATO\]\[TRJ\] exported_imp=(\d+) total_imp=(\d+)\r")
    m = re.search(r"exported_imp=(\d+) total_imp=(\d+)", line)
    exported, total_imp = int(m.group(1)), int(m.group(2))
    evs = [e for e in events_since(ctx, mark) if e.get("imp") == 1]
    assert total_imp >= exported, "total_imp=%d < exported=%d" % (total_imp, exported)
    assert len(evs) >= exported, "副环 JSONL 行 %d < exported %d" % (len(evs), exported)
    ctx.checks.append("imp exported=%d total=%d" % (exported, total_imp))


@hw_case("m2/irq_latency_smoke", timeout=12)
def test_irq_latency_smoke(ctx):
    """空闲 1s 后 jit_max < 120000us（QEMU smoke 上限；真机基线 45169us 应通过）。"""
    time.sleep(1.0)
    t = tickstat(ctx)
    assert t["jit_max"] < 120000, "jit_max=%dus >= 120000us（QEMU smoke 上限）" % t["jit_max"]
    ctx.checks.append("jit_min=%dus jit_max=%dus" % (t["jit_min"], t["jit_max"]))


@hw_case("m2/jit_diagnose", timeout=15)
def test_jit_diagnose(ctx):
    """如实记录真机 tick 抖动与墙钟关系（不伪造通过；为 45ms 异常定位提供证据）。"""
    t0 = tickstat(ctx)
    time.sleep(1.0)
    t1 = tickstat(ctx)
    time.sleep(1.0)
    t2 = tickstat(ctx)
    d1 = t1["ticks"] - t0["ticks"]
    d2 = t2["ticks"] - t1["ticks"]
    assert 300 <= d1 <= 3000 and 300 <= d2 <= 3000, \
        "1s tick 增量异常: %d / %d" % (d1, d2)
    assert t1["now_us"] >= t0["now_us"] and t2["now_us"] >= t1["now_us"], \
        "now_us 回退"
    mark = ctx.mark()
    ctx.send("trace imp 0")
    ctx.expect(r"\[POTATO\]\[TRJ\] exported_imp=\d+ total_imp=\d+\r", 5.0)
    evs = [e for e in events_since(ctx, mark) if e.get("imp") == 1]
    gaps = [e for e in evs if e.get("ev") == "TICK_GAP"]
    max_gap_ms = max([int(e.get("a", 0)) / 1000.0 for e in gaps] or [0.0])
    ctx.checks.append(
        "wall1s ticks=%d/%d cpu_hz=%d jit_min=%dus jit_max=%dus imp=%d "
        "TICK_GAP=%d max_gap=%.1fms"
        % (d1, d2, t2["cpu_hz"], t2["jit_min"], t2["jit_max"], len(evs),
           len(gaps), max_gap_ms))
    if t2["jit_max"] >= 20000:
        # 旧固件基线 jit_max=45169us；若副环被 boot 事件挤掉也属正常，不硬判 pass/fail，
        # 但要求至少能把证据写进日志供 task-5 报告归类。
        ctx.checks.append("警告: jit_max>=20ms，已记录 TICK_GAP 证据 %d 条" % len(gaps))


@hw_case("m2/looptest_rx_storm", timeout=60)
def test_looptest_rx_storm(ctx):
    """looptest 200 + 运行中 6 条空行（RX 中断风暴）→ rounds=200 mismatches=0（QEMU E-01）。"""
    mark = ctx.mark()          # poll 起点必须在发送前取：整个 looptest 在 ~1s 内结束
    ctx.send("looptest 200")
    for _ in range(6):
        time.sleep(0.3)
        ctx.send("")
    text = poll_since(ctx, mark, "looptest 完成",
                      lambda t: re.search(LOOP_DONE, t) is not None, timeout=50.0)
    m = re.search(LOOP_DONE, text)
    rounds, mismatches = int(m.group(1)), int(m.group(2))
    assert rounds == 200, "looptest rounds=%d != 200" % rounds
    assert mismatches == 0, "looptest mismatches=%d != 0（UART RX 路径有错）" % mismatches
    ctx.cmd("ping", r"pong\r", timeout=6.0)
    ctx.checks.append("looptest rounds=200 mismatches=0（RX 风暴中）")


@hw_case("m2/rfe_variants", timeout=30)
def test_rfe_variants(ctx):
    """rfetest 161/1611/194：每个变体 cause=0(ILLEGAL_INSTRUCTION) 后 PASS + pong（QEMU E-02）。"""
    for sr in ("161", "1611", "194"):
        ctx.send("rfetest %s" % sr)
        ctx.expect(E2_H, timeout=10.0)
        ctx.expect(E2_PASS, timeout=10.0)
        ctx.cmd("ping", r"pong\r", timeout=6.0)
    ctx.checks.append("rfetest 161/1611/194 全部 PASS")
