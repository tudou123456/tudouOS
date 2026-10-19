"""M8 真机验收：可观测体系（JSONL / 健康快照 / 重要事件副环 / AI 分析流水线）。

断言照抄 test/cases/test_obs.py（QEMU），并把 QEMU 台架读取全文改为真机串口文本：
- 契约：TRJ/HEALTH/TRM 三类行字段与单调性；
- 泄漏：inject leak → health.heap.used 单调上升 → 分析器 R_LEAK(high)；
- 卡死：inject hang → 内核 TICK_GAP 副环事件 → 分析器 R_IRQ_STALL(high)；
- 崩溃：obs panic → PANIC 快照 + 重要事件副环（INJECT_BEGIN/END）；
- 调度时钟不漂移：连续 1s 健康快照 irq.lag 不增长、sched.sw 持续增长。
"""

from __future__ import annotations

import json
import re
import subprocess
import sys
import time

from test.hw_runner import hw_case
from test.hw_util import ROOT, events, expect_json, health, no_crash, poll_since

OBS_ON = r"\[POTATO\]\[OBS\] stream on[^\r\n]*\r"
OBS_OFF = r"\[POTATO\]\[OBS\] stream off[^\r\n]*\r"
TRM = r"\[POTATO\]\[TRM\] (\{[^\r\n]*\})\r"
HEALTH = r"\[POTATO\]\[HEALTH\] (\{[^\r\n]*\})\r"
TRJ_EXPORTED = r"\[POTATO\]\[TRJ\] exported=\d+\r"
TRJ_IMP = r"\[POTATO\]\[TRJ\] exported_imp=\d+ total_imp=\d+\r"
LEAK_START = r"\[POTATO\]\[INJ\] inject leak arg=64 started\r"
HANG_START = r"\[POTATO\]\[INJ\] inject hang arg=300 started\r"
HANG_DONE = r"\[POTATO\]\[INJ\] mode=2 done arg=300\r"
SPIN_START = r"\[POTATO\]\[INJ\] inject spin arg=120 started\r"
PANIC = r"\[POTATO\]\[PANIC\] (\{[^\r\n]*\})\r"


def _analyze(ctx, name: str):
    """把本用例日志喂给 M8 分析流水线（tools/ai_analysis.py），返回 (report, proc)。"""
    log = ROOT / "build" / ("_hw_obs_%s.log" % name)
    rep = ROOT / "build" / ("_hw_obs_%s.report.json" % name)
    log.parent.mkdir(parents=True, exist_ok=True)
    # 只分析本用例范围的日志：避免其他用例（如 crash 用例）的转储混进来
    log.write_text(ctx.text_since(ctx.start_pos), encoding="utf-8", errors="replace")
    proc = subprocess.run(
        [sys.executable, str(ROOT / "tools" / "ai_analysis.py"), str(log),
         "--json", str(rep), "--quiet"],
        capture_output=True, text=True, timeout=180)
    report = {}
    if rep.exists():
        report = json.loads(rep.read_text(encoding="utf-8"))
    return report, proc


def _rules(report):
    return {f["rule"]: f["severity"] for f in report.get("findings", [])}


def _case_text(ctx) -> str:
    return ctx.hw.text()[ctx.start_pos:]


# ---------------------------------------------------------------------- 契约
@hw_case("m8/obs_contract", timeout=30)
def test_obs_contract(ctx):
    """TRJ/HEALTH/TRM 契约：字段齐全、普通环 seq 单调唯一、meta 统计自洽（QEMU 同判据）。"""
    mark = ctx.mark()
    ctx.cmd("trace stream on", OBS_ON, 10.0)
    time.sleep(1.2)
    streamed = poll_since(ctx, mark, "流式事件 >=20",
                          lambda t: len(events(t)) >= 20, 8.0)
    # 真机 UART 没有输出互斥：流式 TRJ 与 console 输出可能字节级交错，先把
    # 流关掉再取 meta/health/jsonl，保证结构化行完整（交错证据见 task-5 报告）。
    ctx.cmd("trace stream off", OBS_OFF, 10.0)
    ctx.send("trace meta")
    meta = expect_json(ctx, TRM, 8.0)[1]
    h = health(ctx)
    ctx.cmd("trace jsonl 16", TRJ_EXPORTED, 8.0)

    evs = events(streamed)
    assert len(evs) >= 20, "流式事件行过少: %d（1.2s 内应持续泵出）" % len(evs)
    for e in evs:
        assert set(("seq", "t", "ev", "a", "b")) <= set(e), e
    seqs = [e["seq"] for e in evs if not e.get("imp")]
    assert seqs == sorted(seqs), "普通环序号非单调"
    assert len(set(seqs)) == len(seqs), "普通环序号重复"
    assert events(ctx.hw.text()[ctx.start_pos:]), "trace jsonl 未产出可解析事件"

    for key in ("trace", "imp", "heap", "sched", "irq", "tasks", "ch", "inject"):
        assert key in h, "健康快照缺字段 %s: %s" % (key, h)
    assert h["imp"]["cap"] == 64, h["imp"]
    for key in ("total", "oldest", "dropped", "lost", "pumped"):
        assert key in h["trace"], h["trace"]
    assert isinstance(h["tasks"], list) and h["tasks"], h
    assert {"name", "cls", "state", "cpu", "sw", "stk", "last"} <= set(h["tasks"][0])
    assert {"name", "enq", "deq", "drop_full", "drop_dl", "drop_dup", "retry",
            "ack", "hw", "count"} <= set(h["ch"][0]), h["ch"]
    assert meta["cap"] == 256 and "imp" in meta and meta["imp"]["cap"] == 64, meta
    assert h["trace"]["total"] >= meta["total"], (meta, h["trace"])
    no_crash(_case_text(ctx), "obs_contract")
    ctx.checks.append("流式 %d 事件, health/TRM 契约字段齐全, cap 256/imp 64"
                      % len(evs))


# ---------------------------------------------------------------------- 泄漏
@hw_case("m8/inject_leak", timeout=90)
def test_inject_leak(ctx):
    """注入泄漏：heap.used 单调上升 → 分析器 R_LEAK(high) 且 growth>=2048（QEMU 同判据）。"""
    ctx.cmd("inject leak 64", LEAK_START, 10.0)
    snaps = []
    for _ in range(4):
        time.sleep(1.0)
        snaps.append(health(ctx))
    time.sleep(0.6)
    text = _case_text(ctx)
    assert "[POTATO][INJ] leak done blocks=64" in text, "泄漏注入未正常收尾"
    assert "[POTATO][INJ] heap used=" in text, "缺少注入收尾堆统计"
    used = [s["heap"]["used"] for s in snaps]
    assert used[-1] > used[0], "注入泄漏后 heap.used 未增长: %s" % used
    assert all(b >= a for a, b in zip(used, used[1:])), used

    report, proc = _analyze(ctx, "leak")
    assert proc.returncode in (0, 1), (proc.stderr or "")[-400:]
    rules = _rules(report)
    assert "R_LEAK" in rules, "分析器未检出泄漏: %s" % rules
    assert rules["R_LEAK"] == "high", rules
    assert report["source"]["health_snapshots"] >= 3, report.get("source")
    leak = [f for f in report["findings"] if f["rule"] == "R_LEAK"][0]
    assert leak["metrics"]["growth_bytes"] >= 2048, leak["metrics"]
    ctx.checks.append("heap used %s → R_LEAK high growth=%d"
                      % (used, leak["metrics"]["growth_bytes"]))


# ---------------------------------------------------------------------- 卡死
@hw_case("m8/inject_hang_tick_gap", timeout=90)
def test_inject_hang_tick_gap(ctx):
    """注入 300ms 关中断：TICK_GAP 副环事件 a>=150ms → 分析器 R_IRQ_STALL(high)。"""
    ctx.cmd("trace stream on", OBS_ON, 10.0)
    ctx.cmd("inject hang 300", HANG_START, 10.0)
    ctx.expect(HANG_DONE, 20.0)
    ctx.cmd("trace imp 0", TRJ_IMP, 10.0)
    time.sleep(0.3)

    evs = events(_case_text(ctx))
    gaps = [e for e in evs if e["ev"] == "TICK_GAP"]
    assert gaps, "未记录到内核实测 tick 停摆事件（重要事件副环）"
    assert all(e.get("imp") == 1 for e in gaps), "TICK_GAP 应来自重要事件副环"
    max_gap_ms = max(e["a"] for e in gaps) / 1000.0
    assert max_gap_ms >= 150.0, "tick 停摆时长不足: %.1fms" % max_gap_ms

    report, proc = _analyze(ctx, "hang")
    assert proc.returncode in (0, 1), (proc.stderr or "")[-400:]
    rules = _rules(report)
    assert "R_IRQ_STALL" in rules, "分析器未检出停摆: %s" % rules
    assert rules["R_IRQ_STALL"] == "high", rules
    stall = [f for f in report["findings"] if f["rule"] == "R_IRQ_STALL"][0]
    assert stall["metrics"]["max_tick_gap_ms"] >= 150.0, stall
    ctx.checks.append("TICK_GAP %d 条 max=%.1fms → R_IRQ_STALL high"
                      % (len(gaps), max_gap_ms))


# ---------------------------------------------------------------------- 调度时钟
@hw_case("m8/sched_no_drift", timeout=90)
def test_sched_no_drift(ctx):
    """D-08 回归：连续 6×1s 健康快照 irq.lag<=100、sched.sw 持续增长、无 R_SCHED_LAG。"""
    snaps = []
    for _ in range(6):
        time.sleep(1.0)
        snaps.append(health(ctx))
    lags = [s["irq"]["lag"] for s in snaps]
    sw = [s["sched"]["sw"] for s in snaps]
    assert all("wd" in s["irq"] for s in snaps), "健康快照缺 irq.wd 计数"
    assert max(lags) <= 100, "调度时钟漂移: lag 序列 %s" % lags
    growth = [b - a for a, b in zip(sw, sw[1:])]
    assert all(g > 100 for g in growth), "上下文切换停滞: sw=%s" % sw

    report, proc = _analyze(ctx, "drift")
    assert proc.returncode in (0, 1), (proc.stderr or "")[-400:]
    assert "R_SCHED_LAG" not in _rules(report), _rules(report)
    ctx.checks.append("lag max=%d, sw growth=%s, R_SCHED_LAG 未触发"
                      % (max(lags), growth))


# ---------------------------------------------------------------------- 崩溃诊断
@hw_case("m8/panic_dump", timeout=60)
def test_panic_dump(ctx):
    """obs panic：PANIC 快照字段齐全 + 副环保留注入锚点 + 分析器 panics>=1。"""
    # 真机上不要开 trace stream：panic 会自己导出重要事件副环 + JSONL，
    # 而流式 TRJ 与 console 输共享 UART 无互斥，交错会破坏 PANIC JSON 行。
    ctx.cmd("inject spin 120", SPIN_START, 10.0)
    ctx.send("obs panic")
    m = ctx.expect(PANIC, 15.0)
    panic = json.loads(m.group(1))
    for key in ("reason", "t", "now", "cur", "heap_used", "trace_total", "inject"):
        assert key in panic, panic
    assert panic["reason"] == "panic", panic
    time.sleep(0.5)

    evs = events(_case_text(ctx))
    assert any(e.get("imp") == 1 for e in evs), "PANIC 之后未导出重要事件副环"
    inj = [e for e in evs if e["ev"] in ("INJECT_BEGIN", "INJECT_END")]
    assert len(inj) >= 1, "副环未保留注入锚点"

    report, proc = _analyze(ctx, "panic")
    assert proc.returncode in (0, 1), (proc.stderr or "")[-400:]
    assert report["source"]["unparsed_lines"] == 0, report.get("source")
    assert report["source"]["panics"] >= 1, report.get("source")
    ctx.checks.append("PANIC reason=panic, 副环注入锚点 %d 条, 分析器 panics>=1"
                      % len(inj))
