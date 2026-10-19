"""M1 真机验收：启动链 / banner / mem 行 / READY / 控制台基础命令。

断言来源：test/cases/test_boot.py（QEMU 同源：ROM banner、版本号、build 行、mem 行、
READY、ping/echo/help/info）。真机差异就地注释，不放宽语义：

- 真机 ROM 在 kernel 之前打印 ``ESP-ROM:esp32s3-*``、``rst:0x.. (REASON)``、
  ``entry 0x40xxxxxx``（QEMU 有 ESP-ROM 行但没有真实复位原因/entry）；
- 真机硬件初始化阶段会多出 hw 专属行：``hw rtc-cntl``、EMAC 缺席、
  ``[DRV] net init skipped``、``no EMAC device: ethernet disabled``、
  串口 MCP 通道 ``[NETMCP] transport=serial-json``（M7 的传输面，M1 只验证
  它们没挡住启动）；
- 版本号从 ``kernel/include/potato.h`` 读取，和 QEMU 用例同一唯一来源。
"""

from __future__ import annotations

import time

from test.hw_runner import hw_case
from test.hw_util import (BANNER, BUILD, READY, count, kernel_versions,
                          no_crash, poll, tickstat)

# ---- 启动链锚点 -----------------------------------------------------------
ROM = r"ESP-ROM:esp32s3[^\r\n]*\r"
RST = r"rst:0x[0-9a-f]+ \([A-Z0-9_]+\)"
ENTRY = r"entry 0x40[0-9a-f]{6}"
HW_RTC = r"\[POTATO\] hw rtc-cntl: swd-conf=0x[0-9a-f]+[^\r\n]*\r"
# hw 构建走 emac.c 的 hw 分支（ESP32-S3 硅片无内部 MAC）：
EMAC_ABSENT = (r"\[POTATO\]\[NET\] EMAC(?: probe: [^\r\n]* -> absent"
               r"|: ESP32-S3 has no internal MAC[^\r\n]*)\r")
DRV_NET_SKIP = r"\[POTATO\]\[DRV\] net init skipped\r"
NET_DISABLED = r"\[POTATO\]\[NET\] no EMAC device: ethernet disabled\r"
TIMEBASE = r"\[POTATO\] hw timebase calibrated: cpu=(\d+)Hz tick_period=(\d+)cc\r"
NETMCP_SERIAL = r"\[POTATO\]\[NETMCP\] transport=serial-json[^\r\n]*\r"
MEM_LINE = (r"\[POTATO\] mem iram=0x[0-9a-f]+-0x[0-9a-f]+ "
            r"irom\(flash\)=0x[0-9a-f]+-0x[0-9a-f]+ "
            r"data=0x[0-9a-f]+-0x[0-9a-f]+ bss=0x[0-9a-f]+-0x[0-9a-f]+")
INTC_READY = r"\[POTATO\] intc\+tick\(1ms\)\+trace ready\r"
HELP_HINT = r"\[POTATO\] type 'help' for commands\r"
PROMPT = "potato> "


@hw_case("m1/boot_banner", timeout=12, smoke=True)
def test_boot_banner(ctx):
    """启动链完整性与 M1 banner/READY 契约（真机方式：RTS 复位后看整段 ROM+kernel 日志）。"""
    pos = ctx.start_pos          # 复位前的文本游标：此时游标之后就是本次启动日志
    ctx.expect(ROM, 4.0, pos=pos)
    ctx.expect(RST, 4.0)
    ctx.expect(ENTRY, 4.0)
    # hw 专属早期初始化行（QEMU 没有；缺了说明固件不是 hw 构建）
    ctx.expect(HW_RTC, 3.0)
    ctx.expect(EMAC_ABSENT, 3.0)
    ctx.expect(DRV_NET_SKIP, 3.0)
    m = ctx.expect(TIMEBASE, 3.0)
    cpu_hz, tick_cc = int(m.group(1)), int(m.group(2))
    assert cpu_hz > 0, "hw timebase cpu_hz 非正: %d" % cpu_hz
    assert tick_cc > 0, "hw tick_period_cc 非正: %d" % tick_cc
    ctx.expect(NET_DISABLED, 3.0)
    # M7 的串口 MCP 传输面在 net_start 里启用（无网口也能远程控制）
    ctx.expect(NETMCP_SERIAL, 3.0)

    ver, target = kernel_versions()
    m = ctx.expect(BANNER, 3.0)
    assert m.group(1) == ver, "banner 版本 %s != kernel/include/potato.h %s" % (
        m.group(1), ver)
    ctx.expect(BUILD, 3.0)
    ctx.expect(MEM_LINE, 3.0)
    ctx.expect(INTC_READY, 3.0)
    ctx.expect(READY, 4.0)
    ctx.expect(HELP_HINT, 3.0)
    assert PROMPT in ctx.hw.raw_text(), "控制台提示符 'potato> ' 未出现"
    # 目标名唯一来源校验：banner 的 "on ESP32-S3" 已由 BANNER 锚定，这里再确认 hw 构建
    assert target == "ESP32-S3", "potato.h POTATO_TARGET=%r 非 ESP32-S3" % (target,)
    ctx.checks.append("boot chain ROM→kernel→READY, banner=v%s, timebase=%dHz"
                      % (ver, cpu_hz))


@hw_case("m1/info_help_echo", timeout=15, smoke=True)
def test_info_help_echo(ctx):
    """控制台基础命令：info 版本/目标、help 命令表、echo 回显、ping 通。"""
    ver, target = kernel_versions()
    line = ctx.cmd_line(
        "info",
        r"\[POTATO\] info: tudouOS v%s kernel=potato target=%s\r"
        % (re_escape(ver), re_escape(target)))
    assert "tudouOS v" in line

    mark = ctx.mark()
    ctx.send("help")
    text = poll(ctx, "help 输出", lambda t: all(
        token in t for token in ("taskdemo", "memtest", "netmcp", "smp",
                                 "inject", "obs", "trace")), timeout=5.0)
    for token in ("help", "ping", "echo", "info", "tickstat", "tasks",
                  "msgdemo", "memstat", "pal", "drivers", "fs", "mcp",
                  "crash", "looptest", "health"):
        assert token in text, "help 输出缺命令 %r" % token
    ctx.checks.append("help 命令表包含 %d 个关键命令" % 15)

    ctx.cmd("echo hello potato", r"hello potato\r")
    assert "hello potato" in ctx.hw.text(), "echo 未回显"
    ctx.cmd("ping", r"pong\r")
    ctx.checks.append("info/help/echo/ping")


@hw_case("m1/idle_stable", timeout=15)
def test_idle_stable(ctx):
    """READY 后空转稳定：tick 持续推进、无异常/恐慌、控制台仍响应。"""
    mark = ctx.mark()
    t1 = tickstat(ctx)
    time.sleep(1.2)
    t2 = tickstat(ctx)
    assert t2["ticks"] > t1["ticks"], "空闲 1.2s tick 未推进: %d -> %d" % (
        t1["ticks"], t2["ticks"])
    delta = t2["ticks"] - t1["ticks"]
    assert delta >= 200, "空闲 1.2s tick 推进过少: %d" % delta
    no_crash(ctx.text_since(mark), "idle")
    ctx.cmd("ping", r"pong\r")
    ctx.checks.append("idle 1.2s ticks +%d, 无异常" % delta)


def re_escape(text: str) -> str:
    import re
    return re.escape(text)
