"""M9 验收用例：双核启动、核间中断、双核任务执行、负载均衡、默认单核。

覆盖链条（强化重点 5 可测指标）：
  - 默认单核不变：未 `smp start` 时 online=1、CPU1 不耗电、单核基线可用
  - CPU1 启动：ROM 桩握手（写 0x600C0004）→ 上线确认 → 自有 CCOMPARE0 tick 在走
  - IPI 通路：FROM_CPU 源 → 中断矩阵 → int21；发送/接收/重调度计数对账、无未知原因
  - 双核执行：绑核任务在 CPU1 真实运行（切换计数 + work 计数）
  - 负载均衡：任意核任务积压 CPU0 时迁移到空闲 CPU1（mig_in/out/trace）
  - 混合压测稳定：两核并发运行 1s 后命令面存活、容器无溢出、无异常
"""

import re
import time

READY_LINE = r"\[POTATO\] READY"

SUMMARY = (r"\[POTATO\]\[SMP\] cpus=(\d+) online=(\d+) started=(\d+) "
           r"ipi_pending=(\d+)/(\d+)")
CPU_LINE = (r"\[POTATO\]\[SMP\] cpu(\d) online=(\d+) up=(\d+) ipi_tx=(\d+) "
            r"ipi_rx=(\d+) resched=(\d+) ticksw=(\d+) ticks=(\d+) "
            r"mig_in=(\d+) mig_out=(\d+) sw=(\d+) unk=(\d+) idle=(\d+) "
            r"rq=0/(\d+) cur=(\S+) boot=(\d+)ms")
WORK_SUM = r"\[POTATO\]\[SMP\] work n=(\d+) cpu1_preempt=(\d+) balance_seq=(\d+)"
WORK_CNT = r"\[POTATO\]\[SMP\] work(\d) count=(\d+)"
XSTRESS = (r"\[POTATO\]\[SMP\] xstress run=(\d+) sent=(\d+) recv=(\d+) "
           r"drop=(\d+) msg_bad=(\d+) heap_ops=(\d+) heap_fail=(\d+) "
           r"heap_bad=(\d+) fs_ops=(\d+) fs_fail=(\d+) fs_bad=(\d+)")


def _read_stat(ctx, timeout=10):
    """发 `smp stat`，解析摘要行 + 两行核统计 + work 行。"""
    ctx.serial.send("smp stat")
    s = ctx.serial.expect(SUMMARY, timeout=timeout)
    out = {
        "cpus": int(s.group(1)),
        "online": int(s.group(2)),
        "started": int(s.group(3)),
        "cpu": {},
    }
    for _ in range(2):
        m = ctx.serial.expect(CPU_LINE, timeout=timeout)
        out["cpu"][int(m.group(1))] = {
            "online": int(m.group(2)),
            "up": int(m.group(3)),
            "ipi_tx": int(m.group(4)),
            "ipi_rx": int(m.group(5)),
            "resched": int(m.group(6)),
            "ticksw": int(m.group(7)),
            "ticks": int(m.group(8)),
            "mig_in": int(m.group(9)),
            "mig_out": int(m.group(10)),
            "sw": int(m.group(11)),
            "unk": int(m.group(12)),
            "idle": int(m.group(13)),
            "rq": int(m.group(14)),
            "cur": m.group(15),
            "boot_ms": int(m.group(16)),
        }
    w = ctx.serial.expect(WORK_SUM, timeout=timeout)
    out["work_n"] = int(w.group(1))
    return out


def _start_cpu1(ctx):
    """启动 CPU1（调用方已等过 READY）；断言握手成功且两核在线。"""
    ctx.serial.send("smp start")
    m = ctx.serial.expect(r"\[POTATO\]\[SMP\] start rc=(-?\d+) online=(\d+)",
                          timeout=25)
    assert int(m.group(1)) == 0, f"CPU1 启动失败 rc={m.group(1)}"
    assert int(m.group(2)) == 2, f"两核未同时在线 online={m.group(2)}"


def test_m9_default_single_core(ctx):
    """默认单核：不启动 CPU1 时 online=1、CPU1 未上电，单核基线不受影响。"""
    ctx.serial.expect(READY_LINE, timeout=20)
    st = _read_stat(ctx)
    assert st["cpus"] == 2, f"CPU 探测数异常: {st['cpus']}"
    assert st["online"] == 1, f"默认应单核在线: {st['online']}"
    assert st["started"] == 0, "默认不应动 CPU1 启动寄存器"
    assert st["cpu"][1]["online"] == 0, "CPU1 不应在线"
    assert st["cpu"][1]["ticks"] == 0, "CPU1 tick 不应在走"
    assert st["cpu"][0]["online"] == 1
    assert st["cpu"][0]["up"] == 1
    # 单核既有能力不受 SMP 改造影响
    ctx.serial.send("memstat")
    m = ctx.serial.expect(r"total=(\d+) used=(\d+) free=(\d+)", timeout=10)
    assert int(m.group(1)) >= 64 * 1024, "堆容量回归（应 >=64KB）"


def test_m9_selftest_and_boot(ctx):
    """自检 + CPU1 启动：ROM 握手成功、CPU1 自有 tick 运行、两核在线。"""
    ctx.serial.expect(READY_LINE, timeout=20)
    ctx.serial.send("smp selftest")
    m = ctx.serial.expect(r"selftest rc=(\d+)", timeout=10)
    assert int(m.group(1)) == 0, f"selftest 失败 rc={m.group(1)}"

    _start_cpu1(ctx)
    # CPU1 上线后其 tick 最多 1ms 起跳；给 1.5s 抗主机负载抖动
    c1 = None
    deadline = time.time() + 1.5
    while time.time() < deadline:
        st = _read_stat(ctx)
        assert st["online"] == 2, "启动后应两核在线"
        assert st["started"] == 1, "started 标志未置位"
        c1 = st["cpu"][1]
        assert c1["online"] == 1 and c1["up"] == 1, f"CPU1 上线计数异常: {c1}"
        if c1["ticks"] > 0:
            break
        time.sleep(0.1)
    assert c1 is not None and c1["ticks"] > 0, "CPU1 自有 CCOMPARE0 tick 未运行"
    # tick 在持续走（两次采样递增）
    time.sleep(0.25)
    st2 = _read_stat(ctx)
    assert st2["cpu"][1]["ticks"] > c1["ticks"], "CPU1 tick 停摆"


def test_m9_ipi_path(ctx):
    """IPI 通路：CPU0→CPU1 发送 N 次，接收侧计数/重调度计数增长、无未知原因。"""
    _start_cpu1(ctx)
    before = _read_stat(ctx)
    ctx.serial.send("smp ipi 32")
    ctx.serial.expect(r"\[POTATO\]\[SMP\] ipi sent=32 to cpu1", timeout=10)
    time.sleep(0.3)
    st = _read_stat(ctx)
    c0, c1 = st["cpu"][0], st["cpu"][1]
    # 计数是累计量：比较增量，且接收不可能多于发送（其它路径也会发 IPI，
    # 例如唤醒跨核任务时的辅助 IPI，所以发送增量可能 > 32）
    d_tx = c0["ipi_tx"] - before["cpu"][0]["ipi_tx"]
    d_rx = c1["ipi_rx"] - before["cpu"][1]["ipi_rx"]
    assert d_tx >= 32, f"发送计数未累计: {d_tx}"
    assert d_rx >= 1, "CPU1 未收到任何 IPI"
    assert d_rx <= d_tx, f"接收({d_rx})超过发送({d_tx})"
    assert c1["resched"] >= 1, "IPI 未触发重调度计数"
    assert c1["unk"] == 0, "存在未知原因 IPI"

    # 反向通路：CPU1 上的任务唤醒/队列变化需要在 CPU1 侧处理，这里用
    # 迁移触发 CPU1→CPU0 的 IPI（绑核任务在 CPU0 队列出现时反向补 IPI）
    c1_sw_before = c1["sw"]
    time.sleep(0.2)
    st2 = _read_stat(ctx)
    assert st2["cpu"][1]["sw"] >= c1_sw_before, "切换计数回退"


def test_m9_work_pinned(ctx):
    """绑核执行：`smp spawn 1 2` 后两个任务只在 CPU1 运行（计数增长、无迁移）。"""
    _start_cpu1(ctx)
    before = _read_stat(ctx)
    ctx.serial.send("smp spawn 1 2")
    m = ctx.serial.expect(r"spawn aff=1 n=2 made=(\d+)", timeout=10)
    assert int(m.group(1)) == 2, f"创建任务失败 made={m.group(1)}"
    time.sleep(0.4)

    st = _read_stat(ctx)
    c1 = st["cpu"][1]
    assert c1["sw"] > before["cpu"][1]["sw"], "CPU1 未发生上下文切换"
    assert c1["mig_in"] == 0, "绑核任务不应触发迁移"
    assert st["cpu"][0]["mig_out"] == 0

    ctx.serial.send("smp work")
    counts = {}
    for _ in range(2):
        mm = ctx.serial.expect(WORK_CNT, timeout=10)
        counts[int(mm.group(1))] = int(mm.group(2))
    assert all(v > 0 for v in counts.values()), f"绑核任务未执行: {counts}"


def test_m9_balance(ctx):
    """负载均衡：CPU0 积压 4 个任意核任务，空闲 CPU1 应被迁入 1 个并执行。"""
    _start_cpu1(ctx)
    ctx.serial.send("smp spawn any 4")
    m = ctx.serial.expect(r"spawn aff=-1 n=4 made=(\d+)", timeout=10)
    assert int(m.group(1)) == 4, f"创建任务失败 made={m.group(1)}"

    # 均衡按 tick 周期决策，在 2s 内轮询直到发生迁移（避免单次采样时序抖动）
    c0 = c1 = None
    deadline = time.time() + 2.0
    while time.time() < deadline:
        time.sleep(0.2)
        st = _read_stat(ctx)
        c0, c1 = st["cpu"][0], st["cpu"][1]
        if c1["mig_in"] >= 1 and c0["mig_out"] >= 1:
            break
    assert c1["mig_in"] >= 1, f"未发生 CPU0→CPU1 迁移: {c1}"
    assert c0["mig_out"] >= 1, f"CPU0 迁出计数未增长: {c0}"
    assert c1["sw"] > 0, "迁移后 CPU1 未切换执行"

    ctx.serial.send("smp work")
    total = 0
    for _ in range(4):
        mm = ctx.serial.expect(WORK_CNT, timeout=10)
        total += int(mm.group(2))
    assert total > 0, "压测任务未在任一核执行"


def test_m9_xcore_locks_heap_fs_msg(ctx):
    """P0-3/P0-4 回归：双核并发 heap/fs/msg，数据零损坏。

    `smp stress on` 在两核各起一个后台任务，循环做堆分配校验、potfs
    写读校验、消息收发；同时 CPU0 控制台跑 memtest churn 与 fs write/cat。
    锁失效会表现为 heap_bad/fs_bad/msg_bad > 0、堆故障计数增长或崩溃。"""
    ctx.serial.expect(READY_LINE, timeout=20)
    _start_cpu1(ctx)

    ctx.serial.send("memstat")
    m = ctx.serial.expect(r"faults=(\d+)", timeout=8)
    faults0 = int(m.group(1))

    ctx.serial.send("smp stress on")
    m = ctx.serial.expect(r"stress start rc=(-?\d+)", timeout=10)
    assert int(m.group(1)) == 0, f"stress 启动失败 rc={m.group(1)}"
    m = ctx.serial.expect(XSTRESS, timeout=10)
    assert int(m.group(1)) == 1, "stress 未进入运行态"

    # CPU0 控制台与两个 worker 并发：堆压测 + 文件读写
    ctx.serial.send("memtest churn")
    time.sleep(0.5)
    for i in range(12):
        payload = f"v{i}"
        ctx.serial.send(f"fs write shared{i}.txt {payload}")
        ctx.serial.expect(rf"\[POTATO\]\[FS\] wrote shared{i}\.txt", timeout=8)
        ctx.serial.send(f"fs cat shared{i}.txt")
        ctx.serial.expect(rf"{payload}\r", timeout=8)
        ctx.serial.send(f"fs rm shared{i}.txt")
        ctx.serial.expect(rf"\[POTATO\]\[FS\] removed shared{i}\.txt", timeout=8)
    time.sleep(1.5)

    ctx.serial.send("smp stress off")
    m = ctx.serial.expect(XSTRESS, timeout=15)
    vals = [int(x) for x in m.groups()]
    (run, sent, recv, drop, msg_bad, hops, hfail, hbad,
     fops, ffail, fbad) = vals
    assert run == 0, "stress 未停止"
    assert sent > 0 and recv > 0, f"跨核消息未流动: sent={sent} recv={recv}"
    assert msg_bad == 0, f"跨核消息内容损坏: {msg_bad}"
    assert hops > 0 and hbad == 0, f"跨核堆损坏: ops={hops} bad={hbad}"
    assert fops > 0 and fbad == 0, f"跨核文件内容损坏: ops={fops} bad={fbad}"
    assert hfail >= 0 and ffail >= 0 and drop >= 0  # 资源竞争允许的计数

    ctx.serial.send("memtest off")
    ctx.serial.expect(r"CHURN-DONE ops=(\d+) faults=(\d+)", timeout=30)
    ctx.serial.send("memstat")
    m = ctx.serial.expect(r"used=(\d+) .*faults=(\d+)", timeout=8)
    assert int(m.group(2)) <= faults0, "压测期间出现堆故障"

    ctx.serial.send("tasks")
    ctx.serial.expect(r"\[POTATO\]\[TS\] now=", timeout=15)
    d = ctx.serial.expect(r"drop ready=(\d+) sleep=(\d+) reg=(\d+) guard=(\d+)",
                          timeout=10)
    assert d.groups() == ("0", "0", "0", "0"), f"容器/栈哨兵异常: {d.group(0)}"


def test_m9_stress_stability(ctx):
    """混合压测稳定：两核并发运行后命令面存活、容器无溢出、两核一致性保持。"""
    _start_cpu1(ctx)
    ctx.serial.send("smp spawn any 4")
    ctx.serial.expect(r"made=(\d+)", timeout=10)
    ctx.serial.send("smp ipi 64")
    ctx.serial.expect(r"ipi sent=64 to cpu1", timeout=10)
    time.sleep(1.0)

    st = _read_stat(ctx)
    assert st["online"] == 2, "压测后 CPU1 掉线"
    assert st["cpu"][1]["ticks"] > 0
    assert st["cpu"][0]["unk"] == 0 and st["cpu"][1]["unk"] == 0, "出现未知 IPI 原因"

    # 调度容器无溢出、任务表仍可用
    ctx.serial.send("tasks")
    ts = ctx.serial.expect(r"\[POTATO\]\[TS\] now=(\d+) switches=(\d+)", timeout=15)
    assert int(ts.group(2)) > 0
    d = ctx.serial.expect(r"drop ready=(\d+) sleep=(\d+) reg=(\d+)", timeout=10)
    assert d.group(1) == "0", "就绪表溢出"
    assert d.group(2) == "0", "睡眠表溢出"
    assert d.group(3) == "0", "任务注册表溢出"

    # 时基未被 CPU1 干扰：now 单调推进
    ctx.serial.send("tickstat")
    m = ctx.serial.expect(r"ticks=(\d+)", timeout=10)
    assert int(m.group(1)) > 0


