"""M3.5 验收用例：消息基座（有界通道/QoS/回执/重发去重/超时/冻结自愈）。

对应设计稿 docs/mcp-bus-design.md §7 测试清单。等待语义为切片睡眠轮询
（该 QEMU 限制），虚拟时间膨胀下断言均用宿主时钟+宽容差。
"""

import re
import time

READY_LINE = r"\[POTATO\] READY"
# 锚定到 dd0= 哨兵：确保 ack 数字完整（expect 可能在行传输中途匹配，
# 把 ack=20 截读成 ack=2——本轮排障实锤的测试台竞态）
STAT = (r"cnt=(\d+) hw=\d+ enq=(\d+) deq=(\d+) full=(\d+) dedup=(\d+) "
        r"dead=(\d+) retry=(\d+) ack=(\d+) dd0=")


def _stat(ctx):
    ctx.serial.send("msgstat")
    m = ctx.serial.expect(STAT, timeout=8)
    return {k: int(v) for k, v in zip(
        ["cnt", "enq", "deq", "full", "dedup", "dead", "retry", "ack"],
        m.groups())}


def _wait_both(ctx, timeout=90):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        t = ctx.serial.text
        if "PING-DONE" in t and "RECV-DONE" in t:
            return True
        time.sleep(0.6)
    return False


def test_msg_flood(ctx):
    """洪泛 12.5×容量：有界不 OOM、QoS0 丢新计数准确、系统存活。
    断言以内核通道统计为准（语义源），发送方返回值计数仅参考。"""
    ctx.serial.expect(READY_LINE, timeout=20)
    ctx.serial.send("msgdemo flood 400")
    ctx.serial.expect(r"FLOOD-DONE", timeout=60)
    st = _stat(ctx)
    assert st["cnt"] == 32, f"队列应恰好满容量: {st['cnt']}"
    assert st["enq"] == 32, f"入队应=容量: {st['enq']}"
    assert st["full"] >= 368, f"丢新计数 {st['full']} 不足"
    assert st["dead"] == 0, "QoS0 不应有超时丢弃"
    ctx.serial.send("taskdemo off")
    ctx.serial.send("ping")
    ctx.serial.expect(r"pong", timeout=10)


def test_msg_qos1_ping(ctx):
    """QoS1 收发回执：20 条全部送达且回执，零重发零丢失。"""
    ctx.serial.expect(READY_LINE, timeout=20)
    ctx.serial.send("msgdemo ping 20")
    assert _wait_both(ctx), "ping 20 未完成"
    m = re.findall(r"PING-DONE ok=(\d+) failed=(\d+)", ctx.serial.text)[-1]
    n = re.findall(r"RECV-DONE n=(\d+)", ctx.serial.text)[-1]
    assert m[0] == "20" and m[1] == "0", f"PING {m}"
    assert n == "20", f"RECV {n}"
    st = _stat(ctx)
    assert st["ack"] >= 20, f"ack {st['ack']}"
    assert st["dead"] == 0, f"意外丢失 {st['dead']}"
    ctx.serial.send("msgdemo off")


def test_msg_retry_dedup(ctx):
    """延迟回执触发重发：恰好一次投递（去重窗口吸收重发）。"""
    ctx.serial.expect(READY_LINE, timeout=20)
    ctx.serial.send("msgdemo lazy 5")
    assert _wait_both(ctx), "lazy 5 未完成"
    m = re.findall(r"PING-DONE ok=(\d+) failed=(\d+)", ctx.serial.text)[-1]
    n = re.findall(r"RECV-DONE n=(\d+)", ctx.serial.text)[-1]
    assert m[0] == "5" and m[1] == "0", f"PING {m}"
    assert n == "5", f"重发导致重复投递: RECV {n}"
    st = _stat(ctx)
    assert st["retry"] >= 1, f"重发未触发 {st['retry']}"
    assert st["dedup"] >= 1, f"去重未吸收重发 {st['dedup']}"
    ctx.serial.send("msgdemo off")


def test_msg_timeout(ctx):
    """无接收方 QoS1：重发耗尽后失败上报（不无限重发）。"""
    ctx.serial.expect(READY_LINE, timeout=20)
    ctx.serial.send("msgdemo timeout")
    m = ctx.serial.expect(r"TIMEOUT-RESULT r=(-?\d+) retry=(\d+) dead=(\d+)",
                          timeout=30)
    assert m.group(1) == "-1", f"应失败上报: {m.group(1)}"
    assert int(m.group(2)) == 2, f"重发次数应为上限 2: {m.group(2)}"
    assert int(m.group(3)) == 1, f"dead 计数 {m.group(3)}"


def test_msg_disconnect_heal(ctx):
    """QMP 冻结模拟断线：恢复后无丢失无重复（QoS1 回执语义保证）。"""
    ctx.serial.expect(READY_LINE, timeout=20)
    ctx.serial.send("msgdemo heal 6")
    time.sleep(1.5)  # 流量进行中冻结
    ctx.qmp.command("stop")
    time.sleep(1.0)
    ctx.qmp.command("cont")
    assert _wait_both(ctx), "heal 6 未完成"
    m = re.findall(r"PING-DONE ok=(\d+) failed=(\d+)", ctx.serial.text)[-1]
    n = re.findall(r"RECV-DONE n=(\d+)", ctx.serial.text)[-1]
    assert m == ("6", "0"), f"断线后丢失: {m}"
    assert n == "6", f"断线后重复: {n}"
    ctx.serial.send("msgdemo off")


def test_msg_trace(ctx):
    """追踪环含 MSG_ENQ/MSG_DEQ/MSG_ACK 事件（消息流水可回放）。
    流量刚结束须立刻 dump（256 项环会被切换事件刷掉）。"""
    ctx.serial.expect(READY_LINE, timeout=20)
    ctx.serial.send("msgdemo ping 5")
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        if "PING-DONE" in ctx.serial.text and "RECV-DONE" in ctx.serial.text:
            break
        time.sleep(0.05)
    ctx.serial.send("trace 200")
    m = ctx.serial.expect(r"\[POTATO\]\[TR\] total=\d+ dropped=\d+ last (\d+):",
                          timeout=5)
    n_printed = int(m.group(1))
    kinds = set()
    for _ in range(n_printed):
        try:
            m2 = ctx.serial.expect(
                r"\[POTATO\]\[TR\] #\d+ t=\d+ (MSG_ENQ|MSG_DEQ|MSG_DROP|MSG_RETRY|MSG_ACK|SCHED_SWITCH|IRQ_\w+)",
                timeout=3)
            if m2.group(1).startswith("MSG_"):
                kinds.add(m2.group(1))
        except Exception:
            break
    assert {"MSG_ENQ", "MSG_DEQ", "MSG_ACK"} <= kinds, f"消息事件缺失: {kinds}"
