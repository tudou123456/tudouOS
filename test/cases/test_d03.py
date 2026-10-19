"""D-03 v5 回归用例：中断驱动 tick + 调度容器健康 + 抢占接线（可开关）。

v5 架构（见 docs/qemu-testing.md 已知问题表）：
- tick 走 int6 中断驱动（genex EXCCAUSE=4 → 记账/唤醒 → RESTORE+rfe），
  时间推进不再依赖任务检查点（v4 的"忙任务冻结时间基准"病理消除）
- 抢占切换接线完整（IRQ 帧 + rfe，E-02 实证路线，`preempt on|off`）；
  QEMU 模型在 genex 上下文切走时会随机丢 CCOMPARE（M3 三版冻结同源，
  看门狗自愈兜时钟），默认 off，真机默认应开
"""

import re
import time

READY_LINE = r"\[POTATO\] READY"


def _tick_count(ctx):
    ctx.serial.send("tickstat")
    # 匹配到 cpu_hz= 才算整行到齐（防半行早匹配：ticks=2 抢在 2023 前）
    m = ctx.serial.expect(r"\[POTATO\]\[TICK\] ticks=(\d+) cpu_hz=", timeout=5)
    return int(m.group(1))


def test_no_yield_no_starvation(ctx):
    """无检查点忙烧期间时间基准不再冻结（v4 病理的判别性断言）。

    v4 检查点制：flood 循环无 yield → tick 停摆、睡眠任务永不醒。
    v5 中断 tick：洪泛全程 ticks 持续推进，且事后系统健康。"""
    ctx.serial.expect(READY_LINE, timeout=20)
    t0 = _tick_count(ctx)
    ctx.serial.send("flood 3000")  # 控制台长洪泛：忙循环打印，无 yield
    deadline = time.monotonic() + 90
    while time.monotonic() < deadline:
        if "FLOOD-DONE" in ctx.serial.text:
            break
        time.sleep(1.0)
    assert "FLOOD-DONE" in ctx.serial.text, "洪泛未完成"
    t1 = _tick_count(ctx)
    assert t1 > t0 + 100, f"洪泛期间 tick 停摆（v4 病理复发）: {t0} -> {t1}"
    ctx.serial.send("ping")
    ctx.serial.expect(r"pong", timeout=5)


def test_task_registry_dedup(ctx):
    """多轮任务复用后注册表去重、容器零溢出（D-12/D-11）。"""
    ctx.serial.expect(READY_LINE, timeout=20)
    for _ in range(3):
        ctx.serial.send("taskdemo bg")
        ctx.serial.expect(r"BG-DONE", timeout=60)
        ctx.serial.send("taskdemo off")
        time.sleep(0.3)
    for _ in range(2):
        ctx.serial.send("msgdemo ping 5")
        ctx.serial.expect(r"PING-DONE", timeout=60)
        ctx.serial.send("msgdemo off")
        time.sleep(0.3)
    ctx.serial.send("tasks")
    text = ""
    for _ in range(30):
        chunk_m = ctx.serial.expect(r"(\[POTATO\]\[TS\][^\r]*\r)", timeout=5)
        text += chunk_m.group(1)
        if "drop ready=" in text:
            break
    names = re.findall(r"\[TS\] (\w+) cls=", text)
    assert len(names) == len(set(names)), f"注册表重复: {names}"
    m = re.search(r"drop ready=(\d+) sleep=(\d+) reg=(\d+)", text)
    assert m, f"诊断行缺失: {text[-200:]}"
    assert (int(m.group(1)), int(m.group(2)), int(m.group(3))) == (0, 0, 0), \
        f"容器溢出: {m.group(0)}"
