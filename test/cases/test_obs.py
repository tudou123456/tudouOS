"""M8 验收用例：可观测体系（JSONL 导出 / 健康快照 / 重要事件副环 / AI 分析流水线）。

验收判据（docs/architecture.md M8 行）：注入卡死/泄漏场景后，脚本能从导出
日志定位根因。本文件把该判据落成端到端用例：
    1) 契约用例：TRJ/HEALTH/TRM 三类行的字段与单调性
    2) 泄漏：注入 leak → 健康快照 heap 单调上升 → 分析器给 R_LEAK(high)
    3) 卡死：注入 hang → 内核实测 TICK_GAP 事件 → 分析器给 R_IRQ_STALL(high)
    4) 崩溃诊断：obs panic → PANIC 快照 + 重要事件副环 JSONL
"""

import json
import os
import re
import subprocess
import sys
import time

from test.serial_tap import SerialTimeoutError

READY_LINE = r"\[POTATO\] READY"
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
RE_TRJ = re.compile(r'\[POTATO\]\[TRJ\] (\{[^\r\n]*\})\r')


def _boot(ctx):
    ctx.serial.expect(READY_LINE, timeout=20)


def _health(ctx, timeout=15):
    """发送 health 并解析单行快照（要求整行到达：以 \\r 结尾）。"""
    ctx.serial.send("health")
    m = ctx.serial.expect(r"\[POTATO\]\[HEALTH\] (\{[^\r\n]*\})\r", timeout=timeout)
    return json.loads(m.group(1))


def _events(text):
    out = []
    for m in RE_TRJ.finditer(text):
        try:
            out.append(json.loads(m.group(1)))
        except ValueError:
            out.append(None)
    return [e for e in out if e]


def _analyze(text, name):
    """把日志喂给 M8 分析流水线，返回 (report, proc)。"""
    log = os.path.join(ROOT, "build", "_obs_%s.log" % name)
    rep = os.path.join(ROOT, "build", "_obs_%s.report.json" % name)
    with open(log, "w", encoding="utf-8", errors="replace") as f:
        f.write(text)
    proc = subprocess.run(
        [sys.executable, os.path.join(ROOT, "tools", "ai_analysis.py"), log,
         "--json", rep, "--quiet"],
        capture_output=True, text=True, timeout=180)
    with open(rep, encoding="utf-8") as f:
        return json.load(f), proc


def _rules(report):
    return {f["rule"]: f["severity"] for f in report["findings"]}


def test_obs_contract_trace_and_health(ctx):
    """契约：JSONL 事件字段/序号单调、健康快照字段齐全、meta 含副环统计。"""
    _boot(ctx)
    ctx.serial.send("trace stream on")
    ctx.serial.expect(r"\[POTATO\]\[OBS\] stream on", timeout=15)
    time.sleep(1.2)
    streamed = ctx.serial.text  # jsonl dump 会重放更早的 seq，单调性只看流式段
    ctx.serial.send("trace meta")
    meta_line = ctx.serial.expect(r"\[POTATO\]\[TRM\] (\{[^\r\n]*\})\r", timeout=15)
    meta = json.loads(meta_line.group(1))
    h = _health(ctx)
    ctx.serial.send("trace jsonl 16")
    ctx.serial.expect(r"\[POTATO\]\[TRJ\] exported=\d+", timeout=15)

    evs = _events(streamed)
    assert len(evs) >= 20, "流式事件行过少: %d" % len(evs)
    for e in evs:
        assert set(("seq", "t", "ev", "a", "b")) <= set(e), e
    seqs = [e["seq"] for e in evs if not e.get("imp")]
    assert seqs == sorted(seqs), "普通环序号非单调"
    assert len(set(seqs)) == len(seqs), "普通环序号重复"
    assert _events(ctx.serial.text), "trace jsonl 未产出可解析事件"

    for key in ("trace", "imp", "heap", "sched", "irq", "tasks", "ch", "inject"):
        assert key in h, "健康快照缺字段 %s: %s" % (key, h)
    assert h["imp"]["cap"] == 64, h["imp"]
    for key in ("total", "oldest", "dropped", "lost", "pumped"):
        assert key in h["trace"], h["trace"]
    assert isinstance(h["tasks"], list) and h["tasks"], h
    assert {"name", "cls", "state", "cpu", "sw", "stk", "last"} <= set(h["tasks"][0])
    assert {"name", "enq", "deq", "drop_full", "drop_dl", "drop_dup", "retry",
            "ack", "hw", "count"} <= set(h["ch"][0])
    assert meta["cap"] == 256 and "imp" in meta and meta["imp"]["cap"] == 64, meta
    assert h["trace"]["total"] >= meta["total"], (meta, h["trace"])


def test_obs_inject_leak_root_cause(ctx):
    """泄漏注入：heap 单调上升 → AI 分析流水线给出 R_LEAK(high)。"""
    _boot(ctx)
    ctx.serial.send("inject leak 64")
    ctx.serial.expect(r"\[POTATO\]\[INJ\] inject leak arg=64 started", timeout=15)
    snaps = []
    for _ in range(4):
        time.sleep(1.0)
        snaps.append(_health(ctx))
    time.sleep(0.6)  # 等泄漏循环跑完（64 块 × 50ms）
    assert "[POTATO][INJ] leak done blocks=64" in ctx.serial.text, \
        "泄漏注入未正常收尾"
    assert "[POTATO][INJ] heap used=" in ctx.serial.text, "缺少注入收尾堆统计"

    used = [s["heap"]["used"] for s in snaps]
    assert used[-1] > used[0], "注入泄漏后 heap.used 未增长: %s" % used
    assert all(b >= a for a, b in zip(used, used[1:])), used

    report, proc = _analyze(ctx.serial.text, "leak")
    assert proc.returncode in (0, 1), proc.stderr[-400:]
    rules = _rules(report)
    assert "R_LEAK" in rules, "分析器未检出泄漏: %s" % rules
    assert rules["R_LEAK"] == "high", rules
    assert report["source"]["health_snapshots"] >= 3, report["source"]
    leak = [f for f in report["findings"] if f["rule"] == "R_LEAK"][0]
    assert leak["metrics"]["growth_bytes"] >= 2048, leak["metrics"]


def test_obs_inject_hang_tick_gap(ctx):
    """卡死注入：内核 tick 实测停摆事件 → AI 分析流水线给出 R_IRQ_STALL(high)。"""
    _boot(ctx)
    ctx.serial.send("trace stream on")
    ctx.serial.expect(r"\[POTATO\]\[OBS\] stream on", timeout=15)
    ctx.serial.send("inject hang 300")
    ctx.serial.expect(r"\[POTATO\]\[INJ\] inject hang arg=300 started", timeout=15)
    ctx.serial.expect(r"\[POTATO\]\[INJ\] mode=2 done arg=300", timeout=20)
    ctx.serial.send("trace imp 0")  # 重要事件副环：完整可靠历史
    ctx.serial.expect(r"\[POTATO\]\[TRJ\] exported_imp=\d+", timeout=15)
    time.sleep(0.3)

    evs = _events(ctx.serial.text)
    gaps = [e for e in evs if e["ev"] == "TICK_GAP"]
    assert gaps, "未记录到内核实测 tick 停摆事件（副环）"
    assert all(e.get("imp") == 1 for e in gaps), "TICK_GAP 应来自重要事件副环"
    max_gap_ms = max(e["a"] for e in gaps) / 1000.0
    assert max_gap_ms >= 150.0, "tick 停摆时长不足: %.1fms" % max_gap_ms

    report, proc = _analyze(ctx.serial.text, "hang")
    assert proc.returncode in (0, 1), proc.stderr[-400:]
    rules = _rules(report)
    assert "R_IRQ_STALL" in rules, "分析器未检出停摆: %s" % rules
    assert rules["R_IRQ_STALL"] == "high", rules
    stall = [f for f in report["findings"] if f["rule"] == "R_IRQ_STALL"][0]
    assert stall["metrics"]["max_tick_gap_ms"] >= 150.0, stall


def test_obs_sched_clock_no_drift(ctx):
    """D-08 回归：调度时钟必须跟随 tick 计数（看门狗补账也要驱动调度器）。

    缺陷现场（修复前实测）：空闲运行 ~2.5s 后 health.irq.lag 单调增长到 3.6s，
    sched.sw/wake 冻结——sleep 到期不唤醒、时间片/老化失效。本用例把它变成
    持续 5 秒的不变量断言，并在同一份日志上要求分析器不再报 R_SCHED_LAG。
    """
    _boot(ctx)
    snaps = []
    for _ in range(6):
        time.sleep(1.0)
        snaps.append(_health(ctx))
    lags = [s["irq"]["lag"] for s in snaps]
    sw = [s["sched"]["sw"] for s in snaps]
    assert all("wd" in s["irq"] for s in snaps), "健康快照缺 irq.wd 计数"
    assert max(lags) <= 100, "调度时钟漂移: lag 序列 %s" % lags
    growth = [b - a for a, b in zip(sw, sw[1:])]
    assert all(g > 100 for g in growth), "上下文切换停滞: sw=%s" % sw

    report, proc = _analyze(ctx.serial.text, "drift")
    assert proc.returncode in (0, 1), proc.stderr[-400:]
    assert "R_SCHED_LAG" not in _rules(report), _rules(report)


def test_obs_sched_lag_rule_detects(ctx):
    """分析器规则契约：合成的漂移日志必须被判为 R_SCHED_LAG(high)。"""
    def snap(n, t, now, lag, sw):
        return ('[POTATO][HEALTH] {"health_n":%d,"t":%d,"now":%d,"stream":0,'
                '"trace":{"total":100,"oldest":0,"dropped":0,"lost":0,"pumped":0},'
                '"imp":{"total":0,"cap":64,"pumped":0,"lost":0},'
                '"heap":{"total":80000,"used":0,"free":80000,"largest":80000,'
                '"maxused":0,"alloc":0,"free_n":0,"fail":0,"fault":0},'
                '"sched":{"wake":%d,"preempt":0,"sw":%d,"idle":1,"sw_total":%d,'
                '"bg_used":0,"rdrop":0,"sdrop":0,"regover":0},'
                '"irq":{"rx":0,"rxb":0,"jitmin":30,"jitmax":4000,"wd":%d,"lag":%d},'
                '"tasks":[],"ch":[],"inject":0}'
                % (n, t, now, sw, sw, sw, n * 3, lag))

    log = "\n".join([
        "[POTATO] READY",
        snap(0, 1000, 1000, 0, 700),
        snap(1, 2000, 1400, 600, 702),
        snap(2, 3000, 1500, 1500, 702),
        snap(3, 4000, 1560, 2440, 703),
    ])
    report, proc = _analyze(log, "lag_rule")
    assert proc.returncode in (0, 1), proc.stderr[-400:]
    rules = _rules(report)
    assert rules.get("R_SCHED_LAG") == "high", rules
    f = [x for x in report["findings"] if x["rule"] == "R_SCHED_LAG"][0]
    assert f["metrics"]["max_lag_ms"] == 2440, f["metrics"]
    assert f["metrics"]["sw_frozen"] is True, f["metrics"]


def test_obs_panic_dump(ctx):
    """崩溃诊断：PANIC 快照 + 重要事件副环 JSONL（根因现场可导出）。"""
    _boot(ctx)
    ctx.serial.send("trace stream on")
    ctx.serial.expect(r"\[POTATO\]\[OBS\] stream on", timeout=15)
    ctx.serial.send("inject spin 120")
    ctx.serial.expect(r"\[POTATO\]\[INJ\] inject spin arg=120 started", timeout=15)
    ctx.serial.send("obs panic")
    m = ctx.serial.expect(r"\[POTATO\]\[PANIC\] (\{[^\r\n]*\})\r", timeout=15)
    panic = json.loads(m.group(1))
    for key in ("reason", "t", "now", "cur", "heap_used", "trace_total", "inject"):
        assert key in panic, panic
    assert panic["reason"] == "panic", panic
    time.sleep(0.5)

    evs = _events(ctx.serial.text)
    assert any(e.get("imp") == 1 for e in evs), "PANIC 之后未导出重要事件副环"
    inj = [e for e in evs if e["ev"] in ("INJECT_BEGIN", "INJECT_END")]
    assert len(inj) >= 1, "副环未保留注入锚点"

    report, proc = _analyze(ctx.serial.text, "panic")
    assert proc.returncode in (0, 1), proc.stderr[-400:]
    assert report["source"]["unparsed_lines"] == 0, report["source"]
    assert report["source"]["panics"] >= 1, report["source"]
