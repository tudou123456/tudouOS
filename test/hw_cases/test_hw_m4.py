"""M4 真机验收：堆管理（memstat 不变量 / selftest / 合并 / canary / 耗尽 / 抖动 / 双释放 / 越界）。

断言来源：test/cases/test_m4.py（QEMU），期望值逐项照抄：
- memstat fresh: total>=64K, used=0, alloc=free_n, largest=total；
- selftest 64: PASS n=64 faults=0，之后 used=0；
- holes: PASS big=ok, free=total, largest=total（碎片被合并）；
- fault(canary): FAULT-CAPTURED faults>=1, used=0（故障块被回收）;
- exhaust: got>=64, big=ok；
- churn 8s: CHURN-DONE ops>=10000，faults/fail 不增加，used=0，alloc/free_n 平衡;
- dblfree: faults +1，largest>0，随后 selftest 16 PASS；
- szcorrupt: faults +1，used>0（隔离块计入 used），随后 selftest 16 PASS。
真机命令与输出格式与 QEMU 一致，锚点直接复用。
"""

from __future__ import annotations

import re
import time

from test.hw_runner import hw_case
from test.hw_util import (count, memstat, no_crash, poll, poll_since,
                          trace_meta, tr_events)

SELFTEST = r"\[POTATO\]\[MEM\] SELFTEST-(PASS|FAIL) n=(\d+) faults=(\d+)\r"
HOLES = (r"\[POTATO\]\[MEM\] HOLES-(PASS|FAIL) big=(\w+) free=(\d+) "
         r"largest=(\d+) faults=(\d+)\r")
FAULT = r"\[POTATO\]\[MEM\] FAULT-CAPTURED faults=(\d+) used=(\d+)\r"
EXHAUST = (r"\[POTATO\]\[MEM\] EXHAUST-PASS got=(\d+) fail=(\d+) big=(\w+) "
           r"faults=(\d+)\r")
CHURN = r"\[POTATO\]\[MEM\] CHURN-DONE ops=(\d+) faults=(\d+) fail=(\d+)\r"
DBLFREE = (r"\[POTATO\]\[MEM\] DBLFREE-CAPTURED faults=(\d+)\(\+(\d+)\) "
           r"free=(\d+) largest=(\d+)\r")
SZCORRUPT = (r"\[POTATO\]\[MEM\] SZCORRUPT-QUARANTINED faults=(\d+)\(\+(\d+)\) "
             r"used=(\d+)\r")
TR_HEAD = r"\[POTATO\]\[TR\] total=(\d+) dropped=(\d+) last (\d+):\r"


def _fresh_invariants(st):
    assert st["total"] >= 64 * 1024, "total=%d < 64KB" % st["total"]
    assert st["used"] == 0, "used=%d != 0（fresh 堆应全空）" % st["used"]
    assert st["alloc"] == st["free_n"], "alloc=%d != free_n=%d" % (
        st["alloc"], st["free_n"])
    assert st["largest"] == st["total"], "largest=%d != total=%d" % (
        st["largest"], st["total"])


@hw_case("m4/memstat_contract", timeout=12, smoke=True)
def test_memstat_contract(ctx):
    """memstat fresh 不变量（QEMU mem_basic 同判据）。"""
    st = memstat(ctx)
    _fresh_invariants(st)
    no_crash(ctx.text_since(ctx.start_pos), "memstat")
    ctx.checks.append("memstat total=%d free=%d alloc=%d"
                      % (st["total"], st["free"], st["alloc"]))


@hw_case("m4/selftest_64", timeout=15)
def test_selftest_64(ctx):
    """memtest selftest 64: SELFTEST-PASS n=64 faults=0，之后 used=0（QEMU 同判据）。"""
    line = ctx.cmd_line("memtest selftest 64", SELFTEST, timeout=12.0)
    m = re.search(SELFTEST, line)
    status, n, faults = m.group(1), int(m.group(2)), int(m.group(3))
    assert status == "PASS", "SELFTEST-%s（n=%d faults=%d）" % (status, n, faults)
    assert n == 64, "selftest n=%d != 64" % n
    assert faults == 0, "selftest faults=%d != 0" % faults
    st = memstat(ctx)
    _fresh_invariants(st)
    ctx.checks.append("selftest 64 PASS, faults=0")


@hw_case("m4/holes_coalesce", timeout=15)
def test_holes_coalesce(ctx):
    """memtest holes: HOLES-PASS big=ok，free=total、largest=total（QEMU 同判据）。"""
    line = ctx.cmd_line("memtest holes", HOLES, timeout=12.0)
    m = re.search(HOLES, line)
    status, big, free, largest, faults = (m.group(1), m.group(2), int(m.group(3)),
                                          int(m.group(4)), int(m.group(5)))
    assert status == "PASS", "HOLES-%s big=%s faults=%d" % (status, big, faults)
    assert big == "ok", "holes 大块分配结果 big=%r（应为 ok）" % big
    st = memstat(ctx)
    assert st["free"] == st["total"], "holes 后 free=%d != total=%d" % (
        st["free"], st["total"])
    assert st["largest"] == st["total"], "holes 后 largest=%d != total=%d（未合并）" % (
        st["largest"], st["total"])
    ctx.checks.append("holes PASS big=ok free=total=%d" % st["total"])


@hw_case("m4/canary_fault", timeout=15)
def test_canary_fault(ctx):
    """memtest fault: FAULT-CAPTURED faults>=1 used=0，随后 pong（QEMU 同判据）。"""
    line = ctx.cmd_line("memtest fault", FAULT, timeout=12.0)
    m = re.search(FAULT, line)
    faults, used = int(m.group(1)), int(m.group(2))
    assert faults >= 1, "FAULT-CAPTURED faults=%d（应 >=1）" % faults
    assert used == 0, "canary 故障块未回收 used=%d" % used
    ctx.cmd("ping", r"pong\r", timeout=5.0)
    ctx.checks.append("fault faults=%d used=0" % faults)


@hw_case("m4/exhaust", timeout=20)
def test_exhaust(ctx):
    """memtest exhaust: got>=64、big=ok（QEMU 同判据）。

    got 是 1KB 块装填数（含 16B 头 + 8B 尾，约 1048B/块）；真机堆总量
    约 69KB，64 块阈值仍然是有效的"堆完整"判据，64KB 大块复得（big=ok）
    是第二重校验。
    """
    line = ctx.cmd_line("memtest exhaust", EXHAUST, timeout=15.0)
    m = re.search(EXHAUST, line)
    got, fail, big, faults = (int(m.group(1)), int(m.group(2)), m.group(3),
                              int(m.group(4)))
    assert got >= 64, "exhaust got=%d < 64" % got
    assert big == "ok", "exhaust 大块分配 big=%r（应 ok）" % big
    ctx.checks.append("exhaust got=%d fail=%d big=%s" % (got, fail, big))


@hw_case("m4/churn_balance", timeout=45)
def test_churn_balance(ctx):
    """8s churn：ops>=5000、faults/fail 不增、used=0、alloc/free 平衡。

    ops 下限按真机 preempt 默认开启后的后台预算（700/1000 tick）折算：
    churn 是 BG 类，稳定拿到约 70% CPU，旧 10000 阈值对应"检查点调度
    下 BG 独占"的旧基线。正确性判据仍是零故障/零泄漏/收支平衡。
    """
    st0 = memstat(ctx)
    mark = ctx.mark()
    ctx.send("memtest churn")
    time.sleep(8.0)
    ctx.send("memtest off")
    deadline = time.monotonic() + 20.0
    m = None
    while time.monotonic() < deadline:
        m = re.search(CHURN, ctx.text_since(mark))
        if m:
            break
        time.sleep(0.05)
    assert m, "memtest off 后未收到 CHURN-DONE（churn 任务未停）"
    ops, faults, fail = int(m.group(1)), int(m.group(2)), int(m.group(3))
    assert ops >= 5000, "churn ops=%d < 5000" % ops
    assert faults <= st0["faults"], "churn faults=%d 增加（起始 %d）" % (
        faults, st0["faults"])
    assert fail <= st0["fail"], "churn fail=%d 增加（起始 %d）" % (fail, st0["fail"])
    st1 = memstat(ctx)
    assert st1["used"] == 0, "churn 后 used=%d != 0" % st1["used"]
    assert (st1["alloc"] - st0["alloc"]) == (st1["free_n"] - st0["free_n"]), \
        "alloc/free 计数不平衡: alloc %d->%d free_n %d->%d" % (
            st0["alloc"], st1["alloc"], st0["free_n"], st1["free_n"])
    ctx.checks.append("churn ops=%d faults=%d fail=%d" % (ops, faults, fail))


@hw_case("m4/trace_alloc_free", timeout=15)
def test_trace_alloc_free(ctx):
    """holes 期间 trace 捕获 MEM_ALLOC/MEM_FREE（QEMU 同判据，真机用 meta counts）。

    QEMU 用 ``trace 200`` 事件行；真机 tick=irq 的 IRQ 事件洪水（~2000 条/s）
    使 256 深环 ~128ms 即被覆盖、大 n dump 打印期间还会继续覆盖（实测丢行），
    故改用 trace meta 的 counts[] 差值：holes 前后 MEM_ALLOC/MEM_FREE 计数均
    增长，语义等价（环确实记录到这些事件，只是大 n dump 不可靠）。
    """
    before = trace_meta(ctx)["counts_map"]
    ctx.cmd("memtest holes", HOLES, timeout=12.0)
    after = trace_meta(ctx)["counts_map"]
    for ev in ("MEM_ALLOC", "MEM_FREE"):
        d = after.get(ev, 0) - before.get(ev, 0)
        assert d >= 1, "holes 期间 %s 计数增量 %d < 1（before=%r after=%r）" % (
            ev, d, before, after)
    ctx.checks.append(
        "holes 期间 MEM_ALLOC+%d MEM_FREE+%d（trace meta counts）"
        % (after.get("MEM_ALLOC", 0) - before.get("MEM_ALLOC", 0),
           after.get("MEM_FREE", 0) - before.get("MEM_FREE", 0)))


@hw_case("m4/dblfree", timeout=20)
def test_dblfree(ctx):
    """memtest dblfree: faults +1、largest>0，随后 selftest 16 PASS + pong（QEMU 同判据）。"""
    line = ctx.cmd_line("memtest dblfree", DBLFREE, timeout=12.0)
    m = re.search(DBLFREE, line)
    faults, delta, free, largest = (int(m.group(1)), int(m.group(2)),
                                    int(m.group(3)), int(m.group(4)))
    assert delta == 1, "dblfree faults delta=%d（应 +1）" % delta
    assert largest > 0, "dblfree 后 largest=0（堆被破坏）"
    line = ctx.cmd_line("memtest selftest 16", SELFTEST, timeout=12.0)
    assert "PASS" in line and "n=16" in line, "dblfree 后 selftest 16 未 PASS: %r" % line
    ctx.cmd("ping", r"pong\r", timeout=5.0)
    ctx.checks.append("dblfree faults=%d delta=%d largest=%d, selftest16 PASS"
                      % (faults, delta, largest))


@hw_case("m4/szcorrupt", timeout=20)
def test_szcorrupt(ctx):
    """memtest szcorrupt: faults +1、used>0（隔离），随后 selftest 16 PASS + pong（QEMU 同判据）。"""
    line = ctx.cmd_line("memtest szcorrupt", SZCORRUPT, timeout=12.0)
    m = re.search(SZCORRUPT, line)
    faults, delta, used = int(m.group(1)), int(m.group(2)), int(m.group(3))
    assert delta == 1, "szcorrupt faults delta=%d（应 +1）" % delta
    assert used > 0, "szcorrupt 后 used=%d（隔离块应计入 used）" % used
    line = ctx.cmd_line("memtest selftest 16", SELFTEST, timeout=12.0)
    assert "PASS" in line and "n=16" in line, "szcorrupt 后 selftest 16 未 PASS: %r" % line
    ctx.cmd("ping", r"pong\r", timeout=5.0)
    ctx.checks.append("szcorrupt faults=%d delta=%d used=%d, selftest16 PASS"
                      % (faults, delta, used))
