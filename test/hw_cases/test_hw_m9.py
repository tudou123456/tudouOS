"""M9 真机验收：双核 SMP（ESP32-S3 物理双核）。

断言照抄 test/cases/test_m9.py（QEMU）：smp stat 的 SUMMARY/CPU 行、selftest rc、
start 后 CPU1 在线且 ticks 增长、ipi 32/64、spawn 绑核/任意核 + work 计数、
迁移 balance、压力后 tasks/tickstat 自洽、preempt 开关。

真机差异（显式）：
- 每条用例前都复位（runner 默认行为）：CPU1 一旦 ``smp start`` 就持续在线到
  复位为止，测试顺序必须自适应；本文件末尾用 reset_restores_single 验证这条语义；
- QEMU 的 CPU 频率/时序是虚拟的；真机 tick=1kHz（M2 已单独验证时基），
  这里只断言「ticks>0 且递增」这一 QEMU 同款判据。
"""

from __future__ import annotations

import re
import time

from test.hw_runner import hw_case
from test.hw_util import memstat, poll, tickstat

SMP_SUMMARY = (r"\[POTATO\]\[SMP\] cpus=(\d+) online=(\d+) started=(\d+) "
               r"ipi_pending=(\d+)/(\d+)\r")
SMP_CPU_KEYS = ("online", "up", "ipi_tx", "ipi_rx", "resched", "ticksw", "ticks",
                "mig_in", "mig_out", "sw", "unk", "idle", "rq", "cur", "boot")
SMP_CPU = (r"\[POTATO\]\[SMP\] cpu(\d) online=(\d+) up=(\d+) ipi_tx=(\d+) "
           r"ipi_rx=(\d+) resched=(\d+) ticksw=(\d+) ticks=(\d+) mig_in=(\d+) "
           r"mig_out=(\d+) sw=(\d+) unk=(\d+) idle=(\d+) rq=0/(\d+) cur=(\S+) "
           r"boot=(\d+)ms\r")
# 真机这两行尾部还带诊断后缀（实测 "work n=0 cpu1_preempt=0 balance_seq=0
# diag=0/0/aff0/n0"）；语义字段与 QEMU 相同，尾部诊断字段允许存在。
SMP_WORK_SUM = r"\[POTATO\]\[SMP\] work n=(\d+) cpu1_preempt=(\d+) balance_seq=(\d+)[^\r\n]*\r"
SMP_WORK_CNT = r"\[POTATO\]\[SMP\] work(\d) count=(\d+)[^\r\n]*\r"
SMP_START = r"\[POTATO\]\[SMP\] start rc=(-?\d+) online=(\d+)\r"
SMP_SPAWN = r"\[POTATO\]\[SMP\] spawn aff=(-?\d+) n=(\d+) made=(-?\d+)\r"
SMP_IPI = r"\[POTATO\]\[SMP\] ipi sent=(\d+) to cpu1\r"
TASKS_SUM = (r"\[POTATO\]\[TS\] now=(\d+) switches=(\d+) bg_used=\d+/\d+ "
             r"diag wake=\d+ prem=\d+ sw=\d+ idle=\d+ drop ready=(\d+) "
             r"sleep=(\d+) reg=(\d+)(?: guard=(\d+))?\r")
XSTRESS = (r"\[POTATO\]\[SMP\] xstress run=(\d+) sent=(\d+) recv=(\d+) "
           r"drop=(\d+) msg_bad=(\d+) heap_ops=(\d+) heap_fail=(\d+) "
           r"heap_bad=(\d+) fs_ops=(\d+) fs_fail=(\d+) fs_bad=(\d+)\r")


def _smp_stat(ctx) -> dict:
    """发 ``smp stat`` 并解析 SUMMARY + 2 条 CPU 行 + WORK 汇总行（同 QEMU 助手）。"""
    ctx.send("smp stat")
    ms = ctx.expect(SMP_SUMMARY, 8.0)
    st = {
        "cpus": int(ms.group(1)),
        "online": int(ms.group(2)),
        "started": int(ms.group(3)),
        "ipi_pending": (int(ms.group(4)), int(ms.group(5))),
        "cpu": {},
    }
    while len(st["cpu"]) < 2:
        m = ctx.expect(SMP_CPU, 5.0)
        vals = list(m.groups())
        cid = int(vals[0])
        st["cpu"][cid] = {k: (v if k == "cur" else int(v))
                          for k, v in zip(SMP_CPU_KEYS, vals[1:])}
    w = ctx.expect(SMP_WORK_SUM, 5.0)
    st["work"] = {"n": int(w.group(1)),
                  "cpu1_preempt": int(w.group(2)),
                  "balance_seq": int(w.group(3))}
    return st


def _start_cpu1(ctx) -> dict:
    """``smp start`` 后等 CPU1 online/up 且 ticks>0（QEMU selftest_and_boot 判据）。"""
    line = ctx.cmd_line("smp start", SMP_START, 15.0)
    m = re.search(SMP_START, line)
    assert int(m.group(1)) == 0, "smp start rc=%s != 0" % m.group(1)
    assert int(m.group(2)) == 2, "smp start online=%s != 2" % m.group(2)
    deadline = time.monotonic() + 3.0
    st = _smp_stat(ctx)
    while st["cpu"][1]["ticks"] == 0 and time.monotonic() < deadline:
        time.sleep(0.2)
        st = _smp_stat(ctx)
    assert st["online"] == 2, "CPU1 启动后 online=%d != 2" % st["online"]
    assert st["started"] == 1, "started=%d != 1" % st["started"]
    assert st["cpu"][1]["online"] == 1 and st["cpu"][1]["up"] == 1, st["cpu"][1]
    assert st["cpu"][1]["ticks"] > 0, "CPU1 ticks=0（未真正运行）"
    return st


def _smp_work(ctx):
    """发 ``smp work`` 并返回 [(cpu, count)] 列表（QEMU test_m9 的 workN count 行）。"""
    # 注意：``smp work`` 只打印 "work n=N" 与 workN count 行；WORK_SUM
    # (cpu1_preempt/balance_seq/diag) 是 ``smp stat`` 的输出，这里不能等它。
    ctx.send("smp work")
    text = poll(ctx, "smp work 计数",
                lambda t: len(re.findall(SMP_WORK_CNT, t)) >= 1, 8.0)
    return [(int(m.group(1)), int(m.group(2)))
            for m in re.finditer(SMP_WORK_CNT, text)]


def _spawn(ctx, where: str, n: int) -> int:
    pattern = r"\[POTATO\]\[SMP\] spawn aff=(-?\d+) n=(\d+) made=(-?\d+)\r"
    line = ctx.cmd_line("smp spawn %s %d" % (where, n), pattern, 10.0)
    m = re.search(pattern, line)
    made = int(m.group(3))
    assert int(m.group(2)) == n, "spawn n=%s != %d" % (m.group(2), n)
    return made


# ---------------------------------------------------------------------- 用例
@hw_case("m9/stat_default_single_core", timeout=25)
def test_stat_default_single_core(ctx):
    """默认 smp stat：cpus=2 online=1 started=0；CPU1 未启动；堆总量>=64K（QEMU 同源）。"""
    st = _smp_stat(ctx)
    assert st["cpus"] == 2, "cpus=%d != 2（ESP32-S3 应报双核）" % st["cpus"]
    assert st["online"] == 1 and st["started"] == 0, st
    assert st["cpu"][1]["online"] == 0 and st["cpu"][1]["ticks"] == 0, st["cpu"][1]
    assert st["cpu"][0]["online"] == 1 and st["cpu"][0]["up"] == 1, st["cpu"][0]
    ms = memstat(ctx)
    assert ms["total"] >= 64 * 1024, "memstat total=%d < 64K" % ms["total"]
    ctx.checks.append("cpus=2 online=1 started=0, heap total=%d" % ms["total"])


@hw_case("m9/selftest_and_start", timeout=40)
def test_selftest_and_start(ctx):
    """smp selftest rc=0；smp start 后 CPU1 在线且 ticks 持续增长（QEMU 同判据）。"""
    line = ctx.cmd_line("smp selftest", r"\[POTATO\]\[SMP\] selftest rc=(-?\d+)\r", 15.0)
    rc = int(re.search(r"rc=(-?\d+)", line).group(1))
    assert rc == 0, "smp selftest rc=%d != 0" % rc
    st = _start_cpu1(ctx)
    t1 = st["cpu"][1]["ticks"]
    time.sleep(0.25)
    st2 = _smp_stat(ctx)
    assert st2["cpu"][1]["ticks"] > t1, "CPU1 ticks 未增长: %d -> %d" % (
        t1, st2["cpu"][1]["ticks"])
    ctx.checks.append("selftest rc=0, CPU1 ticks %d→%d" % (t1, st2["cpu"][1]["ticks"]))


@hw_case("m9/ipi_path", timeout=40)
def test_ipi_path(ctx):
    """smp ipi 32：32 次 IPI 送达 CPU1（rx 增量>=1 且<=发送数、resched>=1、unk=0）。

    真机 IPI 计数归属与 QEMU 不同（实测 2026-10-19：发送方 CPU0 的 ipi_tx +32，
    CPU1 的 ipi_rx +32；QEMU 用例断言的是 cpu1.ipi_tx>=32）。改为语义等价判据：
    (cpu0.tx+cpu1.tx) 增量 >=32、cpu1.rx 增量 >=1 且 <= 发送总数。
    """
    _start_cpu1(ctx)
    before = _smp_stat(ctx)
    ctx.cmd("smp ipi 32", SMP_IPI, 10.0)
    time.sleep(0.3)
    after = _smp_stat(ctx)
    b0, b1 = before["cpu"][0], before["cpu"][1]
    a0, a1 = after["cpu"][0], after["cpu"][1]
    d_tx0, d_tx1 = a0["ipi_tx"] - b0["ipi_tx"], a1["ipi_tx"] - b1["ipi_tx"]
    d_total_tx = d_tx0 + d_tx1
    d_rx = a1["ipi_rx"] - b1["ipi_rx"]
    assert d_total_tx >= 32, "IPI 发送计数增量 %d < 32（cpu0 +%d cpu1 +%d）" % (
        d_total_tx, d_tx0, d_tx1)
    assert d_rx >= 1, "CPU1 ipi_rx 增量 %d < 1（IPI 未送达）" % d_rx
    assert d_rx <= d_total_tx, "CPU1 ipi_rx +%d > 发送 +%d" % (d_rx, d_total_tx)
    assert a1["resched"] >= b1["resched"] + 1, "CPU1 resched 未增长: %d -> %d" % (
        b1["resched"], a1["resched"])
    assert a1["unk"] == 0, "CPU1 unk=%d != 0" % a1["unk"]
    assert a1["sw"] >= b1["sw"], "CPU1 sw 回退"
    ctx.checks.append(
        "ipi tx +%d (cpu0=%d cpu1=%d) rx +%d resched +%d（真机归属 cpu0 tx/cpu1 rx）"
        % (d_total_tx, d_tx0, d_tx1, d_rx, a1["resched"] - b1["resched"]))


@hw_case("m9/work_pinned", timeout=40)
def test_work_pinned(ctx):
    """smp spawn 1 2 → made=2；CPU1 切换增长、无迁移、smp work 计数>0（QEMU 同源）。"""
    _start_cpu1(ctx)
    before = _smp_stat(ctx)
    made = _spawn(ctx, "1", 2)
    assert made == 2, "spawn 1 2 made=%d != 2" % made
    time.sleep(0.4)
    after = _smp_stat(ctx)
    assert after["cpu"][1]["sw"] > before["cpu"][1]["sw"], \
        "CPU1 sw 未增长: %d -> %d" % (before["cpu"][1]["sw"], after["cpu"][1]["sw"])
    assert after["cpu"][1]["mig_in"] == 0, "绑核运行出现 mig_in=%d" % after["cpu"][1]["mig_in"]
    assert after["cpu"][0]["mig_out"] == 0, "绑核运行出现 mig_out=%d" % after["cpu"][0]["mig_out"]
    counts = _smp_work(ctx)
    assert len(counts) >= 2, "smp work 计数行 %d < 2" % len(counts)
    assert all(v > 0 for _, v in counts), "work 计数含 0: %r" % counts
    ctx.checks.append("spawn 1 2 made=2, CPU1 sw %d→%d, work counts %r"
                      % (before["cpu"][1]["sw"], after["cpu"][1]["sw"], counts))


@hw_case("m9/balance", timeout=45)
def test_balance(ctx):
    """smp spawn any 4 → made=4；2s 内 CPU1 mig_in>=1 且 CPU0 mig_out>=1（QEMU 同源）。"""
    _start_cpu1(ctx)
    made = _spawn(ctx, "any", 4)
    assert made == 4, "spawn any 4 made=%d != 4" % made
    deadline = time.monotonic() + 2.0
    st = _smp_stat(ctx)
    while (st["cpu"][1]["mig_in"] < 1 or st["cpu"][0]["mig_out"] < 1) \
            and time.monotonic() < deadline:
        time.sleep(0.2)
        st = _smp_stat(ctx)
    assert st["cpu"][1]["mig_in"] >= 1, "2s 内 CPU1 mig_in=%d（未发生迁移）" % \
        st["cpu"][1]["mig_in"]
    assert st["cpu"][0]["mig_out"] >= 1, "2s 内 CPU0 mig_out=%d（未发生迁移）" % \
        st["cpu"][0]["mig_out"]
    assert st["cpu"][1]["sw"] > 0, "CPU1 sw=0"
    counts = _smp_work(ctx)
    assert len(counts) >= 1 and sum(v for _, v in counts) > 0, counts
    ctx.checks.append("spawn any 4 made=4, mig_in=%d mig_out=%d, work counts %r"
                      % (st["cpu"][1]["mig_in"], st["cpu"][0]["mig_out"], counts))


@hw_case("m9/stress_stability", timeout=50)
def test_stress_stability(ctx):
    """双核压力：spawn any 4 + ipi 64 后 online=2、ticks>0、unk=0、tasks 无丢就绪（QEMU）。"""
    _start_cpu1(ctx)
    made = _spawn(ctx, "any", 4)
    assert made == 4, "spawn any 4 made=%d != 4" % made
    ctx.cmd("smp ipi 64", r"\[POTATO\]\[SMP\] ipi sent=64 to cpu1\r", 10.0)
    time.sleep(1.0)
    st = _smp_stat(ctx)
    assert st["online"] == 2, "online=%d != 2" % st["online"]
    assert st["cpu"][1]["ticks"] > 0, "CPU1 ticks=0"
    assert st["cpu"][0]["unk"] == 0 and st["cpu"][1]["unk"] == 0, st["cpu"]
    line = ctx.cmd_line("tasks", TASKS_SUM, 10.0)
    m = re.search(TASKS_SUM, line)
    switches = int(m.group(2))
    drops = (int(m.group(3)), int(m.group(4)), int(m.group(5)))
    assert switches > 0, "tasks switches=%d" % switches
    assert drops == (0, 0, 0), "任务注册表丢项: drop ready/sleep/reg=%r" % (drops,)
    t = tickstat(ctx)
    assert t["ticks"] > 0
    ctx.checks.append("stress: online=2 CPU1 ticks=%d unk=0, switches=%d, drops=0"
                      % (st["cpu"][1]["ticks"], switches))


@hw_case("m9/xcore_stress", timeout=60)
def test_xcore_stress(ctx):
    """P0-3/P0-4 真机回归：双核并发 heap/fs/msg 压测，内容零损坏。"""
    _start_cpu1(ctx)
    ctx.send("smp stress on")
    m = ctx.expect(r"stress start rc=(-?\d+)\r", 8.0)
    assert int(m.group(1)) == 0, "stress start rc=%s" % m.group(1)
    m = ctx.expect(XSTRESS, 8.0)
    assert int(m.group(1)) == 1, "stress 未进入运行态: %s" % m.group(0)
    time.sleep(1.5)
    ctx.send("smp stress off")
    m = ctx.expect(XSTRESS, 20.0)
    (run, sent, recv, drop, msg_bad, hops, hfail, hbad,
     fops, ffail, fbad) = [int(x) for x in m.groups()]
    assert run == 0, "stress 未停止"
    assert sent > 0 and recv > 0, "跨核消息未流动: sent=%d recv=%d" % (sent, recv)
    assert msg_bad == 0, "跨核消息损坏 %d" % msg_bad
    assert hops > 0 and hbad == 0, "跨核堆损坏 ops=%d bad=%d" % (hops, hbad)
    assert fops > 0 and fbad == 0, "跨核文件损坏 ops=%d bad=%d" % (fops, fbad)
    ctx.checks.append("xstress sent=%d recv=%d heap=%d fs=%d，bad 全 0"
                      % (sent, recv, hops, fops))


@hw_case("m9/preempt_toggle", timeout=20)
def test_preempt_toggle(ctx):
    """smp preempt on/off 切 CPU1 抢占开关并回读（QEMU 命令面）。"""
    line = ctx.cmd_line("smp preempt on", r"cpu1_preempt=1[^\r\n]*\r", 8.0)
    assert "cpu1_preempt=1" in line, line
    line = ctx.cmd_line("smp preempt off", r"cpu1_preempt=0[^\r\n]*\r", 8.0)
    assert "cpu1_preempt=0" in line, line
    ctx.checks.append("smp preempt on→cpu1_preempt=1, off→0")


@hw_case("m9/reset_restores_single", timeout=40)
def test_reset_restores_single(ctx):
    """CPU1 起来后板级复位应回到 online=1/started=0（真机持续到复位的语义）。"""
    _start_cpu1(ctx)
    ctx.reboot()
    st = _smp_stat(ctx)
    assert st["online"] == 1, "复位后 online=%d != 1" % st["online"]
    assert st["started"] == 0, "复位后 started=%d != 0" % st["started"]
    assert st["cpu"][1]["ticks"] == 0, "复位后 CPU1 ticks=%d != 0" % st["cpu"][1]["ticks"]
    ctx.checks.append("reset 后回到单核默认态")
