"""M4 验收用例：轻量内存管理（TLSF 类分配器/护栏/度量/追踪）。

对应强化重点4 可测指标：反复分配释放回归、越界即捕获并报告、
碎片率有上界（回收后 largest 恢复）、分配流水可回放。
"""

import json
import re
import time

READY_LINE = r"\[POTATO\] READY"
MEMSTAT = (r"total=(\d+) used=(\d+) free=(\d+) largest=(\d+) "
           r"maxused=(\d+) alloc=(\d+) free_n=(\d+) fail=(\d+) faults=(\d+)")


def _memstat(ctx):
    ctx.serial.send("memstat")
    m = ctx.serial.expect(MEMSTAT, timeout=8)
    return {k: int(v) for k, v in zip(
        ["total", "used", "free", "largest", "maxused", "alloc",
         "free_n", "fail", "faults"], m.groups())}


def test_mem_basic(ctx):
    """基本分配回归：64 块渐增尺寸写入校验全过，释放后完全回收。"""
    ctx.serial.expect(READY_LINE, timeout=20)
    ctx.serial.send("memtest selftest 64")
    m = ctx.serial.expect(r"\[POTATO\]\[MEM\] SELFTEST-(PASS|FAIL) n=(\d+) faults=(\d+)",
                          timeout=30)
    assert m.group(1) == "PASS", "花样校验失败"
    assert m.group(2) == "64"
    st = _memstat(ctx)
    assert st["total"] >= 64 * 1024, f"堆过小: {st['total']}"
    assert st["used"] == 0, f"释放后应归零: {st['used']}"
    assert st["alloc"] == st["free_n"], "alloc/free 不平衡"
    assert st["largest"] == st["total"], "全释放后 largest 应=total（合并有效）"


def test_mem_coalesce(ctx):
    """碎片与合并：挖洞/碎裂后 64KB 大块可得，全释放后堆完整回收。"""
    ctx.serial.expect(READY_LINE, timeout=20)
    ctx.serial.send("memtest holes")
    m = ctx.serial.expect(r"\[POTATO\]\[MEM\] HOLES-(PASS|FAIL) big=(\w+) "
                          r"free=(\d+) largest=(\d+) faults=(\d+)", timeout=30)
    assert m.group(1) == "PASS" and m.group(2) == "ok", "碎片后大块分配失败"
    free, largest = int(m.group(3)), int(m.group(4))
    st = _memstat(ctx)
    assert free == st["total"], f"全释放后 free 应=total: {free} vs {st['total']}"
    assert largest == st["total"], "合并未恢复最大块"


def test_mem_canary(ctx):
    """越界捕获：故意写穿块尾，free 时 canary 校验捕获并计数，系统存活。"""
    ctx.serial.expect(READY_LINE, timeout=20)
    ctx.serial.send("memtest fault")
    m = ctx.serial.expect(r"\[POTATO\]\[MEM\] FAULT-CAPTURED faults=(\d+) used=(\d+)",
                          timeout=15)
    assert int(m.group(1)) >= 1, "越界未捕获"
    assert int(m.group(2)) == 0, "故障块回收后 used 应归零"
    ctx.serial.send("ping")
    ctx.serial.expect(r"pong", timeout=10)  # 系统存活


def test_mem_exhaust(ctx):
    """耗尽与恢复：分配到失败（fail 计数），全释放后大块复得。"""
    ctx.serial.expect(READY_LINE, timeout=20)
    ctx.serial.send("memtest exhaust")
    m = ctx.serial.expect(r"\[POTATO\]\[MEM\] EXHAUST-PASS got=(\d+) fail=(\d+) "
                          r"big=(\w+) faults=(\d+)", timeout=30)
    got = int(m.group(1))
    assert got >= 64, f"装填块数异常: {got}"
    assert m.group(3) == "ok", "耗尽释放后 64KB 大块失败"


def test_mem_churn_soak(ctx):
    """churn soak：后台随机尺寸分配/校验/释放持续压测，无损坏无泄漏。"""
    ctx.serial.expect(READY_LINE, timeout=20)
    st0 = _memstat(ctx)
    ctx.serial.send("memtest churn")
    time.sleep(8)  # wall 时间压测（QEMU 膨胀下虚拟上百万次操作）
    ctx.serial.send("memtest off")
    ctx.serial.expect(r"\[POTATO\]\[MEM\] CHURN-DONE ops=(\d+) faults=(\d+) fail=(\d+)",
                      timeout=30)
    m = re.findall(r"CHURN-DONE ops=(\d+) faults=(\d+) fail=(\d+)",
                   ctx.serial.text)[-1]
    assert int(m[0]) >= 10000, f"压测操作数不足: {m[0]}"
    faults0, fail0 = st0["faults"], st0["fail"]
    assert int(m[1]) <= faults0, "churn 期间出现新损坏"
    assert int(m[2]) <= fail0, "churn 期间出现新失败"
    st1 = _memstat(ctx)
    assert st1["used"] == 0, f"churn 后泄漏: used={st1['used']}"
    assert st1["alloc"] - st0["alloc"] == st1["free_n"] - st0["free_n"], \
        "churn 后 alloc/free 不平衡"


def test_mem_trace(ctx):
    """追踪环含 MEM_ALLOC/MEM_FREE 事件（分配流水可回放）。

    审计修复：dump 期间环持续滑动是结构性 flaky；改用 trace meta 的
    累计 counts 前后差值（真机侧 test_hw_m4 同判据）。"""
    ctx.serial.expect(READY_LINE, timeout=20)
    ctx.serial.send("trace meta")
    m = ctx.serial.expect(r"\[POTATO\]\[TRM\] (\{[^\r\n]*\})\r", timeout=10)
    before = {c["ev"]: int(c["n"]) for c in json.loads(m.group(1)).get("counts", [])}

    ctx.serial.send("memtest holes")  # 产生成对 alloc/free
    ctx.serial.expect(r"\[POTATO\]\[MEM\] HOLES-PASS", timeout=30)

    ctx.serial.send("trace meta")
    m = ctx.serial.expect(r"\[POTATO\]\[TRM\] (\{[^\r\n]*\})\r", timeout=10)
    after = {c["ev"]: int(c["n"]) for c in json.loads(m.group(1)).get("counts", [])}
    da = after.get("MEM_ALLOC", 0) - before.get("MEM_ALLOC", 0)
    df = after.get("MEM_FREE", 0) - before.get("MEM_FREE", 0)
    assert da >= 8, f"MEM_ALLOC 事件增量不足: {da}"
    assert df >= 8, f"MEM_FREE 事件增量不足: {df}"


def test_heap_double_free(ctx):
    """D-06 回归：合并后旧地址二次释放被墓碑捕获，堆不出现重叠空闲块。"""
    ctx.serial.expect(READY_LINE, timeout=20)
    ctx.serial.send("memtest dblfree")
    m = ctx.serial.expect(r"\[POTATO\]\[MEM\] DBLFREE-CAPTURED faults=(\d+)\(\+(\d+)\) "
                          r"free=(\d+) largest=(\d+)", timeout=15)
    assert int(m.group(2)) == 1, f"二次释放未被捕获: +{m.group(2)}"
    assert int(m.group(4)) > 0, "堆回收异常（largest=0）"
    # 堆必须仍然自洽：selftest 复验 + 系统存活
    ctx.serial.send("memtest selftest 16")
    ctx.serial.expect(r"\[POTATO\]\[MEM\] SELFTEST-PASS n=16", timeout=15)
    ctx.serial.send("ping")
    ctx.serial.expect(r"pong", timeout=10)


def test_heap_size_corrupt(ctx):
    """D-05 回归：块头 size 被砸后释放——隔离（泄漏）而非越界写/坏表。"""
    ctx.serial.expect(READY_LINE, timeout=20)
    ctx.serial.send("memtest szcorrupt")
    m = ctx.serial.expect(r"\[POTATO\]\[MEM\] SZCORRUPT-QUARANTINED faults=(\d+)\(\+(\d+)\) "
                          r"used=(\d+)", timeout=15)
    assert int(m.group(2)) == 1, f"头破坏未被捕获: +{m.group(2)}"
    assert int(m.group(3)) > 0, "被隔离块应泄漏（used 不得下溢/异常清零）"
    # 系统存活且堆自洽
    ctx.serial.send("memtest selftest 16")
    ctx.serial.expect(r"\[POTATO\]\[MEM\] SELFTEST-PASS n=16", timeout=15)
    ctx.serial.send("ping")
    ctx.serial.expect(r"pong", timeout=10)
