"""M3 验收用例：三分类调度 / 检查点抢占 / 老化防饿死 / 后台预算 / 睡眠节拍 / 调度追踪。

M3 调度核心说明（详见 docs/architecture.md 与 docs/qemu-testing.md）：
- 三分类优先级域（AI通信/UI/后台）+ 轮转 + 老化防饿死 + 后台预算限速
- 切换发生在任务检查点（syscall）；tick 轮询驱动（该 QEMU 无 EPS1、
  中断×手动退出存在竞态，去中断化核心，真机阶段恢复中断驱动）
- QEMU 多任务负载下虚拟时间显著膨胀：全部用宿主时钟计时并放宽容差
"""

import re
import time

READY_LINE = r"\[POTATO\] READY"


def test_sched_checkpoint_preempt(ctx):
    """后台忙烧心跳 + UI 周期睡醒：UI 在睡醒点抢占后台（检查点粒度）。"""
    ctx.serial.expect(READY_LINE, timeout=20)
    t0 = time.monotonic()
    ctx.serial.send("taskdemo preempt")
    for i in range(5):
        ctx.serial.expect(rf"\[POTATO\]\[D\] U {i}", timeout=90)
    # 两个 DONE 到达顺序不确定（后台被老化加速后可能先完成），轮询等齐
    deadline = time.monotonic() + 120
    while time.monotonic() < deadline:
        txt = ctx.serial.text
        if "UI-DONE" in txt and "BG-DONE" in txt:
            break
        time.sleep(1.0)
    assert "UI-DONE" in ctx.serial.text and "BG-DONE" in ctx.serial.text, \
        "UI/BG 未全部完成"
    dur = time.monotonic() - t0

    lines = ctx.serial.text.splitlines()
    g = sum(1 for l in lines if re.search(r"\[D\] G \d+\s*$", l))
    assert g >= 40, f"后台心跳 {g} 拍不足"
    assert dur < 130, f"全程 {dur:.0f}s 过长"
    # 抢占证据：U 行穿插在 G 行之间（而非 G 全部排后）
    gidx_last = max((i for i, l in enumerate(lines) if re.search(r"\[D\] G \d+\s*$", l)), default=-1)
    uidx = [i for i, l in enumerate(lines) if re.search(r"\[D\] U \d+\s*$", l)]
    assert gidx_last > uidx[0], "后台心跳未与 UI 并行（无抢占交织）"
    ctx.serial.send("taskdemo off")


def test_three_class_stress(ctx):
    """AI/UI/后台三路忙烧：AI 优先级最高先完成，老化保证其余推进。"""
    ctx.serial.expect(READY_LINE, timeout=20)
    ctx.serial.send("taskdemo stress")
    ctx.serial.expect(r"AI-DONE", timeout=120)

    lines = ctx.serial.text.splitlines()
    a = sum(1 for l in lines if re.search(r"\[D\] A \d+\s*$", l))
    u = sum(1 for l in lines if re.search(r"\[D\] U \d+\s*$", l))
    g = sum(1 for l in lines if re.search(r"\[D\] G \d+\s*$", l))
    assert a >= 120, f"AI 拍数 {a}（最高优先级应先跑完）"
    assert u >= 1, "UI 在 AI 忙烧下零运行（老化失效）"
    assert g >= 1, "后台零运行（老化失效）"
    ctx.serial.send("taskdemo off")


def test_bg_budget(ctx):
    """后台预算限速：仅后台忙烧时不超预算（≤85% 容差），且能跑完。"""
    ctx.serial.expect(READY_LINE, timeout=20)
    ctx.serial.send("taskdemo bg")
    ctx.serial.expect(r"BG-DONE", timeout=60)
    ctx.serial.send("tasks")
    m = ctx.serial.expect(r"\[POTATO\]\[TS\] dbg cls=BG st=\d+ cpu=(\d+)", timeout=8)
    m2 = ctx.serial.expect(r"\[POTATO\]\[TS\] now=(\d+) switches=\d+", timeout=5)
    cpu, now = int(m.group(1)), int(m2.group(1))
    share = cpu / now if now else 1.0
    assert share <= 0.85, f"后台 CPU 占用 {share:.0%} 超预算+容差"
    assert share > 0.0, "后台零占用异常"


def test_sleep_cadence(ctx):
    """任务睡眠节拍：sleep 250 的宿主时长有界（膨胀容差 120~1500ms）。"""
    ctx.serial.expect(READY_LINE, timeout=20)
    t0 = time.monotonic()
    ctx.serial.send("sleep 250")
    ctx.serial.expect(r"\[POTATO\]\[D\] SLEPT 250", timeout=15)
    dt = (time.monotonic() - t0) * 1000
    assert 120 <= dt <= 1500, f"sleep 250 实际 {dt:.0f}ms 超容差"


def test_sched_trace(ctx):
    """追踪环包含 SCHED_SWITCH 事件（可观测性：调度行为可回放）。"""
    ctx.serial.expect(READY_LINE, timeout=20)
    ctx.serial.send("sleep 50")
    ctx.serial.expect(r"\[POTATO\]\[D\] SLEPT 50", timeout=10)
    ctx.serial.send("trace 32")
    m = ctx.serial.expect(r"\[POTATO\]\[TR\] total=\d+ dropped=\d+ last (\d+):", timeout=5)
    n_printed = int(m.group(1))
    n_sched = 0
    for _ in range(n_printed):
        try:
            m2 = ctx.serial.expect(
                r"\[POTATO\]\[TR\] #\d+ t=\d+ (SCHED_SWITCH|IRQ_ENTER|IRQ_EXIT)", timeout=3)
            if m2.group(1) == "SCHED_SWITCH":
                n_sched += 1
        except Exception:
            break
    assert n_sched >= 2, f"SCHED_SWITCH 事件 {n_sched} 条不足"
