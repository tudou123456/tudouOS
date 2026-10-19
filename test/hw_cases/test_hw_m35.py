"""M3.5 真机验收：消息子系统（QoS1 / 重试 / 去重 / 队列水位 / 消息 trace）。

断言来源：test/cases/test_m35.py（QEMU），期望值照抄：
- flood 400: cnt==32 enq==32 full>=368 dead==0（32 槽满 + 368 背压丢弃）；
- ping 20: ok=20 failed=0、RECV n=20、ack>=20 dead==0；
- lazy 5: ok=5 failed=0、RECV n=5、retry>=1 dedup>=1；
- timeout: r=-1 retry=2 dead=1；
- trace 200 内出现 MSG_ENQ/MSG_DEQ/MSG_ACK。

真机差异（显式 SKIP，不算通过也不隐藏）：
- ``msgdemo heal``（QEMU 用 QMP stop/cont 冻结整机再恢复）在真机没有无损冻结手段：
  RTS 复位会清空内核状态/重放槽，语义不等价，故该子项 SKIP 并在此说明。
  重放/去重语义的真机证据由 M7 ``m7/replay_dedup``（原始 @@MCP 帧重放）覆盖。
"""

from __future__ import annotations

import re
import time

from test.hw_runner import HwSkip, hw_case
from test.hw_util import count, poll, poll_since, trace_meta, tr_events

MSGSTAT = (r"\[POTATO\]\[M\] ch=\S+ cap=\d+ cnt=(\d+) hw=\d+ enq=(\d+) "
           r"deq=(\d+) full=(\d+) dedup=(\d+) dead=(\d+) retry=(\d+) "
           r"ack=(\d+) dd0=")
FLOOD_DONE = r"\[POTATO\]\[M\] FLOOD-DONE[^\r\n]*\r"
PING_DONE = r"\[POTATO\]\[M\] PING-DONE ok=(\d+) failed=(\d+)"
RECV_DONE = r"\[POTATO\]\[M\] RECV-DONE n=(\d+)"
TIMEOUT_RESULT = r"\[POTATO\]\[M\] TIMEOUT-RESULT r=(-?\d+) retry=(\d+) dead=(\d+)"
FF = r"\[POTATO\]\[D\] demo stop-req\r"


def msgstat(ctx):
    line = ctx.cmd_line("msgstat", MSGSTAT, timeout=6.0)
    m = re.search(MSGSTAT, line)
    return {
        "cnt": int(m.group(1)), "enq": int(m.group(2)), "deq": int(m.group(3)),
        "full": int(m.group(4)), "dedup": int(m.group(5)), "dead": int(m.group(6)),
        "retry": int(m.group(7)), "ack": int(m.group(8)),
    }


def _demo_off(ctx):
    ctx.cmd("taskdemo off", FF, timeout=6.0)


@hw_case("m35/msgstat_contract", timeout=25)
def test_msgstat_contract(ctx):
    """msgstat 行字段齐全、计数器自洽（先建通道：真机在通道创建前打印
    'msg channel not created yet'，与 QEMU 不同，先跑一次 ping 建出 demo 通道）。"""
    mark = ctx.mark()
    ctx.send("msgdemo ping 3")
    poll(ctx, "ping3 完成", lambda t: (
        re.search(PING_DONE, t) is not None
        and re.search(RECV_DONE, t) is not None), timeout=20.0)
    s = msgstat(ctx)
    assert s["cnt"] == 0, "ping 结束后队列应清空 cnt=%d" % s["cnt"]
    assert s["enq"] == 3, "enq=%d != 3（3 条 ping 应全部入队）" % s["enq"]
    assert s["deq"] == 3, "deq=%d != 3（3 条应全部出队）" % s["deq"]
    assert s["ack"] >= 3, "ack=%d < 3（QoS1 ack 未完成）" % s["ack"]
    assert s["dead"] == 0 and s["full"] == 0, s
    _demo_off(ctx)
    ctx.checks.append("msgstat enq=3 deq=3 ack=%d cnt=0 dead=0" % s["ack"])


@hw_case("m35/flood_backpressure", timeout=30)
def test_flood_backpressure(ctx):
    """msgdemo flood 400：FLOOD-DONE + cnt=32/enq=32/full>=368/dead=0（QEMU 同判据）。"""
    ctx.send("msgdemo flood 400")
    ctx.expect(FLOOD_DONE, timeout=25.0)
    s = msgstat(ctx)
    assert s["cnt"] == 32, "flood 后 cnt=%d != 32（32 槽应满）" % s["cnt"]
    assert s["enq"] == 32, "flood 后 enq=%d != 32（满后不再入队）" % s["enq"]
    assert s["full"] >= 368, "full=%d < 368（400-32 应背压丢弃）" % s["full"]
    assert s["dead"] == 0, "dead=%d != 0（背压不应判死信）" % s["dead"]
    _demo_off(ctx)
    ctx.cmd("ping", r"pong\r", timeout=5.0)
    ctx.checks.append("flood cnt=%d enq=%d full=%d dead=0"
                      % (s["cnt"], s["enq"], s["full"]))


@hw_case("m35/ping_20", timeout=40)
def test_ping_20(ctx):
    """msgdemo ping 20：ok=20/failed=0、RECV n=20、ack>=20、dead=0（QEMU 同判据）。"""
    mark = ctx.mark()
    ctx.send("msgdemo ping 20")
    text = poll(ctx, "PING-DONE & RECV-DONE", lambda t: (
        re.search(PING_DONE, t) is not None
        and re.search(RECV_DONE, t) is not None), timeout=30.0)
    pm = re.search(PING_DONE, text)
    ok, failed = int(pm.group(1)), int(pm.group(2))
    assert ok == 20 and failed == 0, "ping 结果 ok=%d failed=%d（期望 20/0）" % (ok, failed)
    rm = re.search(RECV_DONE, text)
    assert int(rm.group(1)) == 20, "RECV-DONE n=%s != 20" % rm.group(1)
    s = msgstat(ctx)
    assert s["ack"] >= 20, "ack=%d < 20" % s["ack"]
    assert s["dead"] == 0, "dead=%d != 0" % s["dead"]
    _demo_off(ctx)
    ctx.checks.append("ping ok=20 n=20 ack=%d" % s["ack"])


@hw_case("m35/lazy_retry_dedup", timeout=40)
def test_lazy_retry_dedup(ctx):
    """msgdemo lazy 5：ok=5/failed=0、RECV n=5、retry>=1、dedup>=1（QEMU 同判据）。"""
    mark = ctx.mark()
    ctx.send("msgdemo lazy 5")
    text = poll(ctx, "PING-DONE & RECV-DONE", lambda t: (
        re.search(PING_DONE, t) is not None
        and re.search(RECV_DONE, t) is not None), timeout=30.0)
    pm = re.search(PING_DONE, text)
    ok, failed = int(pm.group(1)), int(pm.group(2))
    assert ok == 5 and failed == 0, "lazy 结果 ok=%d failed=%d（期望 5/0）" % (ok, failed)
    assert int(re.search(RECV_DONE, text).group(1)) == 5
    s = msgstat(ctx)
    assert s["retry"] >= 1, "retry=%d < 1（未触发重传）" % s["retry"]
    assert s["dedup"] >= 1, "dedup=%d < 1（未触发去重）" % s["dedup"]
    assert s["dead"] == 0, "dead=%d != 0" % s["dead"]
    _demo_off(ctx)
    ctx.checks.append("lazy ok=5 retry=%d dedup=%d" % (s["retry"], s["dedup"]))


@hw_case("m35/timeout_deadletter", timeout=30)
def test_timeout_deadletter(ctx):
    """msgdemo timeout：TIMEOUT-RESULT r=-1 retry=2 dead=1（QEMU 固定期望值）。"""
    mark = ctx.mark()
    ctx.send("msgdemo timeout")
    m = None
    text = poll(ctx, "TIMEOUT-RESULT", lambda t: re.search(TIMEOUT_RESULT, t) is not None,
                timeout=25.0)
    m = re.search(TIMEOUT_RESULT, text)
    r, retry, dead = int(m.group(1)), int(m.group(2)), int(m.group(3))
    assert (r, retry, dead) == (-1, 2, 1), \
        "TIMEOUT-RESULT r=%d retry=%d dead=%d（QEMU 期望 -1/2/1）" % (r, retry, dead)
    _demo_off(ctx)
    ctx.checks.append("timeout r=-1 retry=2 dead=1")


@hw_case("m35/disconnect_heal", timeout=10)
def test_disconnect_heal(ctx):
    """QEMU heal 用 QMP stop/cont 冻结整机；真机无等价无损冻结，显式 SKIP 并说明替代证据。"""
    raise HwSkip(
        "QMP stop/cont 冻结是 QEMU 专有手段；真机 RTS 复位会清内核状态与重放槽，"
        "语义不等价，heal 无法在真机无损复现。重放/去重由 m7/replay_dedup 用真实 "
        "@@MCP 帧重放覆盖；msgdemo lazy 覆盖 retry/dedup 计数。")


@hw_case("m35/msg_trace", timeout=40)
def test_msg_trace(ctx):
    """ping 5 期间 trace 捕获 MSG_ENQ/MSG_DEQ/MSG_ACK（QEMU 同判据，真机用 meta counts）。"""
    before = trace_meta(ctx)["counts_map"]
    ctx.send("msgdemo ping 5")
    poll(ctx, "ping5 完成", lambda t: (
        re.search(PING_DONE, t) is not None
        and re.search(RECV_DONE, t) is not None), timeout=30.0)
    _demo_off(ctx)
    after = trace_meta(ctx)["counts_map"]
    deltas = {}
    for ev in ("MSG_ENQ", "MSG_DEQ", "MSG_ACK"):
        d = after.get(ev, 0) - before.get(ev, 0)
        deltas[ev] = d
        assert d >= 1, "ping5 期间 %s 计数增量 %d < 1（before=%r after=%r）" % (
            ev, d, before, after)
    ctx.checks.append("msg trace 计数增量 %r（trace meta counts）" % deltas)