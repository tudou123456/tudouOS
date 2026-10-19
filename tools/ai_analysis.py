#!/usr/bin/env python3
"""土豆OS M8 日志分析流水线（AI/脚本共用的根因定位器）。

输入：串口/测试台导出的原始日志（含内核打印的所有行；非 [POTATO] 行忽略）。
输出：结构化 JSON 报告（--json）+ 人类可读报告；任一 finding 达到 --fail-on
      级别时退出码为 1，可直接作为 CI 判据。

消费的内核契约（见 kernel/include/obs.h）：
    [POTATO][TRJ] {"seq":N,"t":N,"ev":"NAME","a":N,"b":N}   事件（JSONL）
    [POTATO][TRJ] {... ,"imp":1}                             重要事件副环（可靠历史）
    [POTATO][HEALTH] {...}                                   健康快照
    [POTATO][PANIC] {...}                                    崩溃诊断
辅助锚点（非必需，用于关联注入动作）：
    [POTATO][INJ] inject <mode> arg=<n> started / mode=<m> done arg=<n>
    [POTATO][MEM] ... / [POTATO][OBS] ... / [POTATO][TRJ] exported=N

检测规则（rule id / 级别 / 触发条件 / 根因方向）：
    R_LEAK            high    健康快照 heap.used 持续上升且 alloc-free 差值增长
    R_IRQ_STALL       high    内核实测 TICK_GAP 事件（a=停摆us）≥阈值；事件流空洞/
                              jitmax 突增为旁证（事件间隔仅在导出无缺口时采信）
    R_IRQ_UNBALANCED  medium  IRQ_ENTER 与 IRQ_EXIT 计数不配对
    R_TASK_STARVE     high    任务 last_run_tick 落后 now 超过阈值且非 sleeping
    R_CPU_HOG         medium  单任务占用 tick 份额超过阈值
    R_MSG_LOSS        medium  通道 drop_*/retry 计数或 MSG_DROP 事件
    R_MEM_FAULT       high    MEM_FAULT 事件或 heap.fault 增长
    R_EXCEPTION       high    EXCEPTION 事件（a=EXCCAUSE, b=EPC）
    R_TRACE_LOSS      medium  trace.dropped/lost 增长（观测完整性受损）
    R_SCHED_DROP      high    就绪/睡眠表溢出（rdrop/sdrop）
    R_SCHED_LAG       high    调度时钟（pot_sched_now）落后 tick 计数（D-08 类）
    R_NET_LOSS        medium  NET_DROP 事件（M7）
    R_DATA_GAP        low     日志中 JSON 行解析失败（截断/交织）

AI 流水线用法：把 --json 报告喂给模型做归因复述，或直接读 report["findings"][i]
的 hypothesis/evidence 字段；报告含 parse 统计与原始锚点，模型无需再看原始日志。
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional

EV_PREFIX = "[POTATO][TRJ] "
HEALTH_PREFIX = "[POTATO][HEALTH] "
PANIC_PREFIX = "[POTATO][PANIC] "

CC_FREQ_HZ = 40_000_000  # time_ticks_raw(): ccount，40MHz 假设（见 trace.c 注释）

RE_INJ_START = re.compile(r"\[POTATO\]\[INJ\] inject (\w+) arg=(\d+) started")
RE_INJ_DONE = re.compile(r"\[POTATO\]\[INJ\] mode=(\d+) done arg=(\d+)")
RE_TRJ_EXPORTED = re.compile(r"\[POTATO\]\[TRJ\] exported=(\d+)")

SEVERITY_ORDER = {"low": 0, "medium": 1, "high": 2, "critical": 3}


@dataclass
class Event:
    seq: int
    t: int  # ccount 原始值
    ev: str
    a: int
    b: int
    imp: int = 0  # M8：来自重要事件副环（可靠历史，未被调度噪声冲刷）

    @property
    def t_us(self) -> int:
        return self.t // (CC_FREQ_HZ // 1_000_000)


@dataclass
class Finding:
    rule: str
    severity: str
    summary: str
    hypothesis: str
    evidence: List[str] = field(default_factory=list)
    metrics: Dict[str, Any] = field(default_factory=dict)

    def to_dict(self) -> Dict[str, Any]:
        return {
            "rule": self.rule,
            "severity": self.severity,
            "summary": self.summary,
            "hypothesis": self.hypothesis,
            "evidence": self.evidence,
            "metrics": self.metrics,
        }


@dataclass
class LogModel:
    events: List[Event] = field(default_factory=list)
    health: List[Dict[str, Any]] = field(default_factory=list)
    panics: List[Dict[str, Any]] = field(default_factory=list)
    injects: List[Dict[str, Any]] = field(default_factory=list)
    lines_total: int = 0
    lines_unparsed: int = 0
    bad_json_lines: List[str] = field(default_factory=list)
    exported_counts: List[int] = field(default_factory=list)

    def events_of(self, *names: str) -> List[Event]:
        want = set(names)
        return [e for e in self.events if e.ev in want]


def _loads(line: str) -> Optional[Dict[str, Any]]:
    try:
        obj = json.loads(line)
    except (ValueError, TypeError):
        return None
    return obj if isinstance(obj, dict) else None


def parse_log(text: str) -> LogModel:
    m = LogModel()
    for raw in text.splitlines():
        line = raw.rstrip("\r\n")
        if not line:
            continue
        m.lines_total += 1
        stripped = line.strip()
        # 内核打印可能带 CRLF 与行首空白；允许前缀前有杂音（串口交织）
        idx = stripped.find("[POTATO][")
        if idx < 0:
            continue
        stripped = stripped[idx:]

        if stripped.startswith(EV_PREFIX):
            obj = _loads(stripped[len(EV_PREFIX):].strip())
            if obj is None:
                m.lines_unparsed += 1
                m.bad_json_lines.append(stripped[:120])
                continue
            try:
                m.events.append(
                    Event(
                        seq=int(obj["seq"]),
                        t=int(obj["t"]),
                        ev=str(obj["ev"]),
                        a=int(obj["a"]),
                        b=int(obj["b"]),
                        imp=1 if obj.get("imp") else 0,
                    )
                )
            except (KeyError, TypeError, ValueError):
                m.lines_unparsed += 1
                m.bad_json_lines.append(stripped[:120])
            continue

        if stripped.startswith(HEALTH_PREFIX):
            obj = _loads(stripped[len(HEALTH_PREFIX):].strip())
            if obj is None:
                m.lines_unparsed += 1
                m.bad_json_lines.append(stripped[:120])
            else:
                m.health.append(obj)
            continue

        if stripped.startswith(PANIC_PREFIX):
            obj = _loads(stripped[len(PANIC_PREFIX):].strip())
            if obj is None:
                m.lines_unparsed += 1
                m.bad_json_lines.append(stripped[:120])
            else:
                m.panics.append(obj)
            continue

        mm = RE_INJ_START.search(stripped)
        if mm:
            m.injects.append({"kind": "start", "mode": mm.group(1), "arg": int(mm.group(2))})
            continue
        mm = RE_INJ_DONE.search(stripped)
        if mm:
            m.injects.append({"kind": "done", "mode": int(mm.group(1)), "arg": int(mm.group(2))})
            continue
        mm = RE_TRJ_EXPORTED.search(stripped)
        if mm:
            m.exported_counts.append(int(mm.group(1)))
            continue
    return m


# --------------------------------------------------------------------------
# 规则
# --------------------------------------------------------------------------

def _cc_delta_ms(a: int, b: int) -> float:
    """ccount 差分（容忍 32 位回绕），单位 ms。"""
    d = (b - a) & 0xFFFFFFFF
    return d / (CC_FREQ_HZ / 1000.0)


def _heap_series(m: LogModel) -> List[tuple]:
    out = []
    for h in m.health:
        heap = h.get("heap") or {}
        out.append((int(h.get("now", 0)), int(heap.get("used", 0)),
                    int(heap.get("alloc", 0)), int(heap.get("free_n", 0))))
    return out


def rule_leak(m: LogModel, opt) -> List[Finding]:
    ser = _heap_series(m)
    if len(ser) < 3:
        return []
    used = [s[1] for s in ser]
    rising = all(used[i + 1] >= used[i] for i in range(len(used) - 1))
    growth = used[-1] - used[0]
    t0, t1 = ser[0][0], ser[-1][0]
    span_s = max((t1 - t0) / 1000.0, 1e-3)
    slope = growth / span_s
    d_alloc = (ser[-1][2] - ser[-1][3]) - (ser[0][2] - ser[0][3])
    if not rising or growth <= 0:
        return []
    if slope < opt.leak_bytes_per_s and d_alloc < opt.leak_min_blocks:
        return []
    allocs = m.events_of("MEM_ALLOC")
    frees = m.events_of("MEM_FREE")
    tags: Dict[int, int] = {}
    for e in allocs:
        tags[e.b] = tags.get(e.b, 0) + 1
    ev = [
        f"heap.used {used[0]} -> {used[-1]} 字节（+{growth}B / {span_s:.2f}s，斜率 {slope:.0f}B/s）",
        f"alloc-free 差值增长 {d_alloc}（未回收块数增加）",
        f"MEM_ALLOC={len(allocs)} MEM_FREE={len(frees)}",
    ]
    if tags:
        top = sorted(tags.items(), key=lambda kv: -kv[1])[:3]
        ev.append("分配标签 Top: " + ", ".join(f"0x{k:x}×{v}" for k, v in top))
    return [
        Finding(
            rule="R_LEAK",
            severity="high",
            summary=f"堆使用量单调上升 +{growth} 字节（{slope:.0f}B/s），疑似内存泄漏",
            hypothesis=(
                "存在分配后未释放的路径：健康快照 heap.used 只升不降，"
                "且 alloc-free 差值持续增大。若伴随 inject leak 锚点，则确认为注入泄漏；"
                "否则检查最近新增的分配点（追踪环中 MEM_ALLOC 多于 MEM_FREE 的 tag）。"
            ),
            evidence=ev,
            metrics={
                "used_first": used[0],
                "used_last": used[-1],
                "growth_bytes": growth,
                "slope_bytes_per_s": round(slope, 1),
                "alloc_minus_free_delta": d_alloc,
            },
        )
    ]


def _export_complete(m: LogModel) -> bool:
    """流式导出是否完整（无环覆盖缺口）——只有完整时才可用事件间隔判空洞。"""
    if not m.health:
        return False
    for h in m.health:
        tr = h.get("trace") or {}
        imp = h.get("imp") or {}
        if int(tr.get("lost", 0)) != 0 or int(imp.get("lost", 0)) != 0:
            return False
    return True


def rule_irq_stall(m: LogModel, opt) -> List[Finding]:
    findings: List[Finding] = []
    # 主证据：内核 tick 实测停摆事件（TR_TICK_GAP，重要副环，a=间隔us）
    tick_gaps = sorted(m.events_of("TICK_GAP"), key=lambda e: e.a, reverse=True)
    max_gap_ms = (tick_gaps[0].a / 1000.0) if tick_gaps else 0.0
    complete = _export_complete(m)

    gaps = []
    if complete:  # 导出无缺口时，事件流间隔才代表真实空洞
        marks = m.events_of("IRQ_ENTER", "SCHED_SWITCH", "INJECT_BEGIN", "INJECT_END")
        for prev, cur in zip(marks, marks[1:]):
            d = _cc_delta_ms(prev.t, cur.t)
            if d >= opt.irq_gap_ms:
                gaps.append((prev, cur, d))

    jit_peaks = []
    for h in m.health:
        irq = h.get("irq") or {}
        j = int(irq.get("jitmax", 0))
        if j >= opt.jit_us:
            jit_peaks.append((int(h.get("now", 0)), j))

    if not tick_gaps and not gaps and not jit_peaks:
        return findings

    ev = []
    for e in tick_gaps[:5]:
        ev.append(f"内核实测 tick 停摆 {e.a / 1000.0:.1f}ms（tick={e.b}，seq={e.seq}）")
    for prev, cur, d in gaps[:5]:
        ev.append(f"事件间隔 {d:.1f}ms：seq {prev.seq}({prev.ev}) -> seq {cur.seq}({cur.ev})")
    for now, j in jit_peaks[:5]:
        ev.append(f"t={now}ms 健康快照 tick 抖动峰值 jitmax={j}us")
    if not complete and m.health:
        ev.append("注意：流式导出存在环覆盖缺口（trace.lost>0），事件间隔不作为空洞判据")
    inj = [i for i in m.injects if i.get("mode") in ("hang", 2)]
    if inj:
        ev.append(f"注入锚点：{inj}")

    severity = "high" if max_gap_ms >= opt.tick_gap_ms else "medium"
    return [
        Finding(
            rule="R_IRQ_STALL",
            severity=severity,
            summary=(
                f"内核实测 tick 停摆 {len(tick_gaps)} 次，最长 {max_gap_ms:.1f}ms"
                + (f"；事件流空洞 {len(gaps)} 处" if gaps else "")
                + (f"；jitmax 峰值 {len(jit_peaks)} 次" if jit_peaks else "")
            ),
            hypothesis=(
                "存在长时间屏蔽中断（INTLEVEL≥1）或不让出的执行区段（自旋/长临界区）："
                "CCOMPARE0 中断被推迟，tick 补记后单次间隔显著超过 1ms。"
                "结合停摆窗口内的 INJECT_BEGIN/健康快照当前任务定位责任代码。"
            ),
            evidence=ev,
            metrics={
                "tick_gap_count": len(tick_gaps),
                "max_tick_gap_ms": round(max_gap_ms, 1),
                "gap_count": len(gaps),
                "max_gap_ms": round(max((g[2] for g in gaps), default=0.0), 1),
                "jit_peaks": len(jit_peaks),
                "export_complete": complete,
            },
        )
    ]


def rule_irq_unbalanced(m: LogModel, opt) -> List[Finding]:
    ent = len(m.events_of("IRQ_ENTER"))
    ext = len(m.events_of("IRQ_EXIT"))
    if ent == 0:
        return []
    if abs(ent - ext) <= max(2, ent // 20):
        return []
    return [
        Finding(
            rule="R_IRQ_UNBALANCED",
            severity="medium",
            summary=f"IRQ_ENTER({ent}) 与 IRQ_EXIT({ext}) 不配对",
            hypothesis=(
                "中断路径提前返回、嵌套层级记账错误，或事件被追踪环覆盖"
                "（对照 trace.dropped）。若 dropped=0 则指向中断处理路径本身。"
            ),
            evidence=[f"IRQ_ENTER={ent}", f"IRQ_EXIT={ext}", f"差值={ent - ext}"],
            metrics={"enter": ent, "exit": ext},
        )
    ]


def rule_task_starve(m: LogModel, opt) -> List[Finding]:
    if not m.health:
        return []
    out: List[Finding] = []
    last = m.health[-1]
    now = int(last.get("now", 0))
    for t in last.get("tasks") or []:
        state = str(t.get("state", ""))
        lag = now - int(t.get("last", 0))
        if state == "sleep":
            continue
        if state in ("ready", "run") and lag >= opt.starve_ms and t.get("name") != "idle":
            out.append(
                Finding(
                    rule="R_TASK_STARVE",
                    severity="high" if lag >= 2 * opt.starve_ms else "medium",
                    summary=f"任务 {t.get('name')} 就绪但 {lag}ms 未被调度",
                    hypothesis=(
                        "同优先级或更高优先级任务长时间不让出（自旋/长临界区），"
                        "或调度器就绪表/老化逻辑异常。对照该任务 switches 与"
                        "健康快照中 CPU 占用最高的任务定位霸占者。"
                    ),
                    evidence=[
                        f"tasks[].name={t.get('name')} state={state} last={t.get('last')} now={now}",
                        f"switches={t.get('sw')} cpu_ticks={t.get('cpu')}",
                    ],
                    metrics={"lag_ms": lag, "task": t.get("name")},
                )
            )
    return out


def rule_cpu_hog(m: LogModel, opt) -> List[Finding]:
    if len(m.health) < 2:
        return []
    first, last = m.health[0], m.health[-1]
    fa = {str(t.get("name")): int(t.get("cpu", 0)) for t in (first.get("tasks") or [])}
    la = {str(t.get("name")): int(t.get("cpu", 0)) for t in (last.get("tasks") or [])}
    deltas = {n: la[n] - fa.get(n, 0) for n in la if la[n] - fa.get(n, 0) > 0}
    total = sum(deltas.values())
    if total <= 0:
        return []
    out = []
    for name, d in sorted(deltas.items(), key=lambda kv: -kv[1]):
        share = 100.0 * d / total
        if name == "idle" or share < opt.hog_share_pct:
            continue
        out.append(
            Finding(
                rule="R_CPU_HOG",
                severity="medium" if share < 90 else "high",
                summary=f"任务 {name} 占用 {share:.1f}% 的 CPU tick 份额",
                hypothesis=(
                    "该任务在观测窗口内几乎独占 CPU：检查其是否有不让出的自旋循环、"
                    "过长临界区或未按预算限速的后台任务（对照 inject spin 锚点）。"
                ),
                evidence=[f"cpu_ticks 增量 {d} / 总增量 {total}（{share:.1f}%）"],
                metrics={"task": name, "share_pct": round(share, 1), "delta": d},
            )
        )
    return out


def rule_msg_loss(m: LogModel, opt) -> List[Finding]:
    out: List[Finding] = []
    if m.health:
        for ch in (m.health[-1].get("ch") or []):
            lost = int(ch.get("drop_full", 0)) + int(ch.get("drop_dl", 0)) + int(
                ch.get("drop_dup", 0)
            )
            if lost <= 0 and int(ch.get("retry", 0)) <= 0:
                continue
            out.append(
                Finding(
                    rule="R_MSG_LOSS",
                    severity="medium",
                    summary=(
                        f"通道 {ch.get('name')} 丢弃={lost}（满 {ch.get('drop_full')}/"
                        f"超时 {ch.get('drop_dl')}/去重 {ch.get('drop_dup')}）"
                        f" 重发={ch.get('retry')}"
                    ),
                    hypothesis=(
                        "接收方处理速率低于到达速率（drop_full）或等待预算过短"
                        "（drop_dl）；去重命中说明发送方在重发，检查 QoS1/2 回执链路。"
                    ),
                    evidence=[json.dumps(ch, ensure_ascii=False)],
                    metrics={"channel": ch.get("name"), "lost": lost},
                )
            )
    drops = m.events_of("MSG_DROP")
    if drops and not out:
        reasons = {}
        for e in drops:
            reasons[e.b] = reasons.get(e.b, 0) + 1
        out.append(
            Finding(
                rule="R_MSG_LOSS",
                severity="medium",
                summary=f"MSG_DROP 事件 {len(drops)} 条",
                hypothesis="按 b 字段：1=队列满 2=超时 3=去重；对照发送方速率与 timeout。",
                evidence=[f"原因分布 {reasons}"],
                metrics={"count": len(drops), "reasons": reasons},
            )
        )
    return out


def rule_mem_fault(m: LogModel, opt) -> List[Finding]:
    ev = m.events_of("MEM_FAULT")
    faults = 0
    if m.health:
        faults = int((m.health[-1].get("heap") or {}).get("fault", 0))
    if not ev and faults == 0:
        return []
    reasons = {}
    for e in ev:
        reasons[e.a] = reasons.get(e.a, 0) + 1
    return [
        Finding(
            rule="R_MEM_FAULT",
            severity="high",
            summary=f"堆护栏捕获 {len(ev)} 次（heap.fault={faults}）",
            hypothesis=(
                "按 a 字段：1=越界写坏 canary 2=非法/二次释放。"
                "结合 b（tag）与 MEM_ALLOC 序列定位越界来源；护栏不崩溃，"
                "但被隔离的块永久泄漏，需修复源头。"
            ),
            evidence=[f"原因分布 {reasons}", f"heap.fault={faults}"],
            metrics={"events": len(ev), "reasons": reasons, "heap_fault": faults},
        )
    ]


def rule_exception(m: LogModel, opt) -> List[Finding]:
    ev = m.events_of("EXCEPTION")
    panics = m.panics
    if not ev and not panics:
        return []
    ev_lines = [f"EXCEPTION a(EXCCAUSE)=0x{e.a:x} b(EPC)=0x{e.b:x}" for e in ev[:8]]
    return [
        Finding(
            rule="R_EXCEPTION",
            severity="critical" if ev or panics else "high",
            summary=f"检出 {len(ev)} 次异常事件" + (f"、{len(panics)} 条崩溃诊断" if panics else ""),
            hypothesis=(
                "EXCCAUSE 定位异常类型（28=取指 MMU 未映射、0=非法指令、"
                "3=硬件异常入口），EPC 为触发地址；对照 [POTATO][PANIC] 快照中的"
                "当前任务与最近事件序列还原现场。"
            ),
            evidence=ev_lines + [json.dumps(p, ensure_ascii=False) for p in panics[:2]],
            metrics={"exception_count": len(ev), "panic_count": len(panics)},
        )
    ]


def rule_trace_loss(m: LogModel, opt) -> List[Finding]:
    if not m.health:
        return []
    out = []
    last = m.health[-1]
    tr = last.get("trace") or {}
    if int(tr.get("dropped", 0)) > 0 or int(tr.get("lost", 0)) > 0:
        out.append(
            Finding(
                rule="R_TRACE_LOSS",
                severity="medium",
                summary=(
                    f"追踪环覆盖事件 dropped={tr.get('dropped')}、导出缺口 lost={tr.get('lost')}"
                ),
                hypothesis=(
                    "事件产生速率超过环容量/导出速率：分析结论可能建立在残缺序列上，"
                    "应先提高导出频率（trace stream on 的 pump 周期）或缩短观测窗口。"
                ),
                evidence=[json.dumps(tr, ensure_ascii=False)],
                metrics={"dropped": tr.get("dropped"), "lost": tr.get("lost")},
            )
        )
    return out


def rule_sched_lag(m: LogModel, opt) -> List[Finding]:
    """D-08 类缺陷：tick 计数在走、调度器时钟停摆（sleep 到点不唤醒）。"""
    if len(m.health) < 2:
        return []
    lag = []
    for h in m.health:
        irq = h.get("irq") or {}
        if "lag" not in irq:
            return []  # 内核未提供该字段（旧固件）：不作为判据
        lag.append((int(h.get("now", 0)), int(irq["lag"])))
    worst_now, worst = max(lag, key=lambda kv: kv[1])
    if worst < opt.sched_lag_ms or worst <= lag[0][1]:
        return []
    sw = [int((h.get("sched") or {}).get("sw", 0)) for h in m.health]
    span_ms = int(m.health[-1].get("t", 0)) - int(m.health[0].get("t", 0))
    # 切换速率塌陷（<5 次/秒）视为冻结：健康系统在 1ms tick 下远高于此
    frozen = span_ms > 1000 and (sw[-1] - sw[0]) * 1000 < span_ms * 5
    ev = [f"t={now}ms 调度时钟滞后 {l}ms（tick 计数 - pot_sched_now）" for now, l in lag[-5:]]
    if frozen:
        ev.append(f"上下文切换计数冻结：sw={sw[0]} 从头到尾未增长")
    return [
        Finding(
            rule="R_SCHED_LAG",
            severity="high",
            summary=(
                f"调度时钟落后 tick 计数最多 {worst}ms"
                + ("，且上下文切换计数冻结" if frozen else "")
            ),
            hypothesis=(
                "存在未驱动调度器的 tick 补账路径：tick 计数（time_tick_count）"
                "在推进而 pot_sched_now() 停滞，sleep 到期不唤醒、老化/时间片失效。"
                "检查所有补记 tick 的旁路（看门狗/轮询/异常路径）是否都调用了 "
                "pot_sched_tick()；健康快照的 irq.wd 计数可确认看门狗路径活跃度。"
            ),
            evidence=ev,
            metrics={"max_lag_ms": worst, "sw_start": sw[0] if sw else None,
                     "sw_end": sw[-1] if sw else None, "sw_frozen": frozen},
        )
    ]


def rule_sched_drop(m: LogModel, opt) -> List[Finding]:
    if not m.health:
        return []
    last = m.health[-1]
    sd = last.get("sched") or {}
    rdrop, sdrop = int(sd.get("rdrop", 0)), int(sd.get("sdrop", 0))
    if rdrop == 0 and sdrop == 0:
        return []
    return [
        Finding(
            rule="R_SCHED_DROP",
            severity="high",
            summary=f"调度表溢出 rdrop={rdrop} sdrop={sdrop}",
            hypothesis=(
                "就绪/睡眠链表槽位不足导致任务丢失调度登记：属于致命调度器缺陷，"
                "需扩大槽位或修正入队去重。"
            ),
            evidence=[json.dumps(sd, ensure_ascii=False)],
            metrics={"rdrop": rdrop, "sdrop": sdrop},
        )
    ]


def rule_net_loss(m: LogModel, opt) -> List[Finding]:
    ev = m.events_of("NET_DROP")
    if not ev:
        return []
    reasons = {}
    for e in ev:
        reasons[e.a] = reasons.get(e.a, 0) + 1
    return [
        Finding(
            rule="R_NET_LOSS",
            severity="medium",
            summary=f"网络丢弃 {len(ev)} 帧",
            hypothesis=(
                "按 a 字段区分驱动层原因（无描述符/环满/链路断开）；"
                "对照 NET_TX/NET_RX 与远端重传计数确认丢包方向。"
            ),
            evidence=[f"原因分布 {reasons}"],
            metrics={"count": len(ev), "reasons": reasons},
        )
    ]


def rule_data_gap(m: LogModel, opt) -> List[Finding]:
    if m.lines_unparsed == 0:
        return []
    return [
        Finding(
            rule="R_DATA_GAP",
            severity="low",
            summary=f"{m.lines_unparsed} 行 JSON 解析失败",
            hypothesis="串口交织/截断导致，或导出侧缓冲区过小（pot_sb_t 截断）。",
            evidence=m.bad_json_lines[:5],
            metrics={"unparsed": m.lines_unparsed, "lines_total": m.lines_total},
        )
    ]


RULES = [
    rule_exception,
    rule_mem_fault,
    rule_sched_drop,
    rule_sched_lag,
    rule_leak,
    rule_irq_stall,
    rule_task_starve,
    rule_cpu_hog,
    rule_msg_loss,
    rule_net_loss,
    rule_irq_unbalanced,
    rule_trace_loss,
    rule_data_gap,
]


def analyze(model: LogModel, opt) -> List[Finding]:
    findings: List[Finding] = []
    for rule in RULES:
        findings.extend(rule(model, opt))
    findings.sort(key=lambda f: (-SEVERITY_ORDER.get(f.severity, 0), f.rule))
    return findings


# --------------------------------------------------------------------------
# 报告
# --------------------------------------------------------------------------

def render_text(model: LogModel, findings: List[Finding], opt) -> str:
    lines = []
    lines.append("=== 土豆OS 日志分析报告 ===")
    lines.append(
        f"输入行={model.lines_total} 事件={len(model.events)} "
        f"健康快照={len(model.health)} 崩溃={len(model.panics)} "
        f"注入锚点={len(model.injects)} 解析失败={model.lines_unparsed}"
    )
    if model.events:
        span = _cc_delta_ms(model.events[0].t, model.events[-1].t)
        by_name: Dict[str, int] = {}
        for e in model.events:
            by_name[e.ev] = by_name.get(e.ev, 0) + 1
        top = ", ".join(f"{k}={v}" for k, v in sorted(by_name.items(), key=lambda kv: -kv[1])[:10])
        lines.append(f"事件跨度={span:.1f}ms 类型分布: {top}")
    if not findings:
        lines.append("结论: 未检出异常（阈值内）")
        return "\n".join(lines)
    lines.append(f"结论: {len(findings)} 项发现")
    for f in findings:
        lines.append("")
        lines.append(f"[{f.severity.upper()}] {f.rule}: {f.summary}")
        lines.append(f"  根因方向: {f.hypothesis}")
        for e in f.evidence:
            lines.append(f"  证据: {e}")
    return "\n".join(lines)


def build_report(model: LogModel, findings: List[Finding], opt) -> Dict[str, Any]:
    return {
        "schema": "potato.obs.report/1",
        "source": {
            "lines_total": model.lines_total,
            "events": len(model.events),
            "health_snapshots": len(model.health),
            "panics": len(model.panics),
            "inject_markers": model.injects,
            "unparsed_lines": model.lines_unparsed,
            "exported_counts": model.exported_counts,
        },
        "thresholds": {
            "irq_gap_ms": opt.irq_gap_ms,
            "tick_gap_ms": opt.tick_gap_ms,
            "sched_lag_ms": opt.sched_lag_ms,
            "jit_us": opt.jit_us,
            "leak_bytes_per_s": opt.leak_bytes_per_s,
            "leak_min_blocks": opt.leak_min_blocks,
            "starve_ms": opt.starve_ms,
            "hog_share_pct": opt.hog_share_pct,
        },
        "findings": [f.to_dict() for f in findings],
        "summary": {
            "highest_severity": findings[0].severity if findings else "none",
            "rules": sorted({f.rule for f in findings}),
        },
    }


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="土豆OS M8 日志分析（根因定位）")
    ap.add_argument("logs", nargs="+", help="日志文件（- 表示标准输入）")
    ap.add_argument("--json", dest="json_out", help="结构化报告输出路径")
    ap.add_argument("--fail-on", default="high",
                    choices=["none", "low", "medium", "high", "critical"],
                    help="达到该级别时退出码 1（默认 high）")
    ap.add_argument("--irq-gap-ms", type=float, default=50.0)
    ap.add_argument("--jit-us", type=int, default=20000,
                    help="tick 抖动峰值告警门限（us；QEMU 空闲基线约 10ms）")
    ap.add_argument("--tick-gap-ms", type=int, default=50,
                    help="内核实测 tick 停摆判为 high 的门限（ms；内核记录门限为 20ms）")
    ap.add_argument("--sched-lag-ms", type=int, default=200,
                    help="调度时钟（pot_sched_now）落后 tick 计数的告警门限（ms）")
    ap.add_argument("--leak-bytes-per-s", type=float, default=256.0)
    ap.add_argument("--leak-min-blocks", type=int, default=8)
    ap.add_argument("--starve-ms", type=int, default=300)
    ap.add_argument("--hog-share-pct", type=float, default=90.0)
    ap.add_argument("--quiet", action="store_true", help="只输出 JSON 报告")
    opt = ap.parse_args(argv)

    text = ""
    for path in opt.logs:
        if path == "-":
            text += sys.stdin.read()
        else:
            with open(path, "r", encoding="utf-8", errors="replace") as fh:
                text += fh.read()

    model = parse_log(text)
    findings = analyze(model, opt)
    report = build_report(model, findings, opt)

    if opt.json_out:
        Path(opt.json_out).write_text(
            json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    if opt.quiet:
        print(json.dumps(report, ensure_ascii=False))
    else:
        print(render_text(model, findings, opt))
        if opt.json_out:
            print(f"\nJSON 报告: {opt.json_out}")

    if opt.fail_on != "none" and findings:
        worst = max(SEVERITY_ORDER.get(f.severity, 0) for f in findings)
        if worst >= SEVERITY_ORDER[opt.fail_on]:
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
