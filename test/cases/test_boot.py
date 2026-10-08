"""M1 验收用例：启动横幅 / ping-pong / QMP 复位重启 / QMP 停止-恢复。

约定：内核输出的 [POTATO] 前缀行是断言锚点；ROM 日志（ESP-ROM:...）
证明走了完整的 ROM→镜像 启动链路。
"""

import time

from test.serial_tap import SerialTimeoutError

ROM_BANNER = r"ESP-ROM:?\s*esp32s3"
VERSION_LINE = r"\[POTATO\] tudouOS v(\d+\.\d+\.\d+)"
READY_LINE = r"\[POTATO\] READY"


def test_boot_banner(ctx):
    """完整启动链路：ROM 日志 → 横幅/版本 → 内存布局 → READY。"""
    ctx.serial.expect(ROM_BANNER, timeout=20)
    m = ctx.serial.expect(VERSION_LINE, timeout=10)
    assert m.group(1) == "0.5.0", f"版本号异常: {m.group(1)}"
    ctx.serial.expect(r"\[POTATO\] build .+", timeout=5)
    ctx.serial.expect(
        r"\[POTATO\] mem text=0x[0-9a-f]+-0x[0-9a-f]+"
        r" data=0x[0-9a-f]+-0x[0-9a-f]+"
        r" bss=0x[0-9a-f]+-0x[0-9a-f]+", timeout=5)
    ctx.serial.expect(READY_LINE, timeout=5)
    # 就绪后 QEMU 应处于 running 状态
    st = ctx.qmp.command("query-status")
    assert st["status"] == "running", f"QEMU 状态异常: {st}"


def test_ping_pong(ctx):
    """串口命令交互：ping→pong 与 echo 回显。"""
    ctx.serial.expect(READY_LINE, timeout=20)
    ctx.serial.send("ping")
    ctx.serial.expect(r"pong", timeout=5)
    ctx.serial.send("echo hello potato")
    ctx.serial.expect(r"hello potato", timeout=5)
    ctx.serial.send("info")
    ctx.serial.expect(r"\[POTATO\] info: tudouOS v0\.5\.0", timeout=5)


def test_qmp_reset(ctx):
    """QMP system_reset 复位后完整重启（无残留状态），并收到 RESET 事件。"""
    ctx.serial.expect(READY_LINE, timeout=20)
    ctx.serial.expect(r"potato> ", timeout=5)  # 进入命令循环

    ctx.qmp.command("system_reset")

    ctx.serial.expect(ROM_BANNER, timeout=20)
    ctx.serial.expect(READY_LINE, timeout=10)
    assert any(e.get("event") == "RESET" for e in ctx.qmp.events), \
        "复位后未收到 QMP RESET 事件"

    # 复位后功能依旧可用
    ctx.serial.send("ping")
    ctx.serial.expect(r"pong", timeout=5)


def test_qmp_stop_cont(ctx):
    """QMP stop 冻结 CPU（串口命令不应被处理），cont 恢复后补处理。"""
    ctx.serial.expect(READY_LINE, timeout=20)

    st = ctx.qmp.command("query-status")
    assert st["status"] == "running", f"初始状态应为 running: {st}"

    ctx.qmp.command("stop")
    st2 = ctx.qmp.command("query-status")
    assert st2["status"] == "paused", f"stop 后应为 paused: {st2}"

    ctx.serial.send("ping")  # 冻结期间送达的命令
    try:
        ctx.serial.expect(r"pong", timeout=1.5)
        raise AssertionError("CPU 暂停期间不应出现 pong")
    except SerialTimeoutError:
        pass  # 预期：冻结期间无响应

    ctx.qmp.command("cont")
    ctx.serial.expect(r"pong", timeout=5)  # 恢复后补处理

    st3 = ctx.qmp.command("query-status")
    assert st3["status"] == "running", f"cont 后应为 running: {st3}"
