"""真机用例共享断言与解析工具（本文件不注册用例）。

被 test/cases/test_hw_*.py 复用，保持与 QEMU 用例（test/cases/test_*.py）同一套
锚点/期望值语义；真机差异在具体用例里显式注释，不在这里偷偷放宽。
"""

from __future__ import annotations

import json
import re
import time
from pathlib import Path

from test.hw_serial import HwTimeoutError

ROOT = Path(__file__).resolve().parent.parent

#: 真机通用锚点（行尾必须带 '\r'，设备输出均为 CRLF）
READY = r"\[POTATO\] READY\r"
BANNER = r"\[POTATO\] tudouOS v(\d+\.\d+\.\d+) \(potato kernel\) on ESP32-S3\r"
BUILD = r"\[POTATO\] build [^\r\n]+\r"
TICK = (r"\[POTATO\]\[TICK\] ticks=(\d+) cpu_hz=(\d+) now_us=0x([0-9a-f]+) "
        r"jit_min=(\d+)us jit_max=(\d+)us uart_irq=(\d+) rx_bytes=(\d+)")
MEMSTAT = (r"\[POTATO\]\[MEM\] total=(\d+) used=(\d+) free=(\d+) largest=(\d+) "
           r"maxused=(\d+) alloc=(\d+) free_n=(\d+) fail=(\d+) faults=(\d+)")
#: trace JSONL 单行（普通环与重要事件副环共用；副环行带 "imp":1）
TRJ = r"\[POTATO\]\[TRJ\] (\{[^\r\n]*\})\r"
TRM = r"\[POTATO\]\[TRM\] (\{[^\r\n]*\})\r"
#: trace_dump 单事件行（trace.c:394）
TR_EVENT = (r"\[POTATO\]\[TR\] #(\d+) t=(\d+) (\w+) "
            r"a=0x([0-9a-f]+) b=0x([0-9a-f]+)")

TICK_EVENTS = {"IRQ_ENTER", "IRQ_EXIT", "EXCEPTION", "SCHED_SWITCH"}


# ---------------------------------------------------------------------- 版本来源
def kernel_versions() -> tuple[str, str]:
    """从 kernel/include/potato.h 读 (版本, 目标名)，与 QEMU boot 用例同源。"""
    header = (ROOT / "kernel" / "include" / "potato.h").read_text(
        encoding="utf-8", errors="replace")
    mv = re.search(r'#define\s+POTATO_VERSION\s+"([^"]+)"', header)
    mt = re.search(r'#define\s+POTATO_TARGET\s+"([^"]+)"', header)
    if not mv or not mt:
        raise AssertionError("kernel/include/potato.h 缺 POTATO_VERSION/TARGET")
    return mv.group(1), mt.group(1)


# ---------------------------------------------------------------------- 解析工具
def _fields(m, names) -> dict:
    return {name: int(m.group(i + 1)) for i, name in enumerate(names)}


def parse_tick(line: str) -> dict:
    m = re.search(TICK, line)
    if not m:
        raise AssertionError("tickstat 行格式不符: %r" % (line,))
    # now_us 以 "0x%x" 打印（十六进制），其余字段为十进制
    return {
        "ticks": int(m.group(1)),
        "cpu_hz": int(m.group(2)),
        "now_us": int(m.group(3), 16),
        "jit_min": int(m.group(4)),
        "jit_max": int(m.group(5)),
        "uart_irq": int(m.group(6)),
        "rx_bytes": int(m.group(7)),
    }


def wait_tick_settle(ctx, timeout: float = 2.0) -> dict:
    """等真机启动瞬态结束：READY 后首个 tick IRQ 补账前 ticks/now 可能为 0。

    真机实测（2026-10-19 build 19:43:33）：READY 后立即 tickstat 可能
    ticks=0 now_us>0 jit_min=0xFFFFFFFF；约 100–200ms 后一次补账跳到 ticks≈200。
    本函数轮询到 ticks>0 且 now_us>0，返回该次 tickstat（限 timeout）。
    """
    t0 = time.monotonic()
    last = None
    while True:
        last = tickstat(ctx)
        if last["ticks"] > 0 and last["now_us"] > 0:
            return last
        if time.monotonic() - t0 >= timeout:
            raise AssertionError(
                "启动后 %.1fs 内 tick 仍未开始（ticks=%d now_us=%d，"
                "真机启动瞬态异常）" % (timeout, last["ticks"], last["now_us"]))
        time.sleep(0.05)


def parse_memstat(line: str) -> dict:
    m = re.search(MEMSTAT, line)
    if not m:
        raise AssertionError("memstat 行格式不符: %r" % (line,))
    return _fields(m, ("total", "used", "free", "largest", "maxused", "alloc",
                       "free_n", "fail", "faults"))


def tickstat(ctx, timeout: float = 5.0) -> dict:
    return parse_tick(ctx.cmd_line("tickstat", TICK, timeout))


def memstat(ctx, timeout: float = 5.0) -> dict:
    return parse_memstat(ctx.cmd_line("memstat", MEMSTAT, timeout))


def expect_json(ctx, pattern: str, timeout: float = 5.0):
    """等待形如 `前缀 ({...}) 行尾` 的 JSON 行，返回 (match, dict)。"""
    m = ctx.expect(pattern, timeout)
    try:
        return m, json.loads(m.group(1))
    except ValueError as exc:
        raise AssertionError("JSON 行解析失败: %r (%s)" % (m.group(1)[:200], exc)) from exc


def events(text: str) -> list[dict]:
    """解析文本中所有 trace JSONL 事件行（普通环 + 重要事件副环）。"""
    out = []
    for m in re.finditer(TRJ, text, re.MULTILINE):
        try:
            out.append(json.loads(m.group(1)))
        except ValueError:
            continue
    return out


def events_since(ctx, mark: int) -> list[dict]:
    return events(ctx.text_since(mark))


def tr_events(text: str) -> list[dict]:
    """解析 trace_dump 事件行，返回 [{seq,t,ev,a,b}]。"""
    return [
        {"seq": int(a), "t": int(b), "ev": ev, "a": int(c, 16), "b": int(d, 16)}
        for a, b, ev, c, d in re.findall(TR_EVENT, text, re.MULTILINE)
    ]


def poll(ctx, what: str, predicate, timeout: float = 10.0) -> str:
    """在用例游标之后轮询文本直到 predicate(text) 为真，返回该段文本。

    失败抛 HwTimeoutError（runner 会记 TIMEOUT 并保留锚点名）。
    """
    mark = ctx.mark()
    deadline = time.monotonic() + timeout
    while True:
        text = ctx.text_since(mark)
        if predicate(text):
            return text
        if time.monotonic() >= deadline:
            raise HwTimeoutError(what, timeout, ctx.hw.latest(40))
        time.sleep(0.05)


def poll_since(ctx, mark: int, what: str, predicate, timeout: float = 10.0) -> str:
    """在**指定**文本游标之后轮询直到 predicate(text) 为真（异步输出/异步事件用）。

    与 ``poll`` 的区别：mark 由调用方在发送命令**之前**取，避免命令响应先于
    轮询起点到达（真机 trace 头部/事件行、msg demo 完成行都是异步输出）。
    """
    deadline = time.monotonic() + timeout
    while True:
        text = ctx.text_since(mark)
        if predicate(text):
            return text
        if time.monotonic() >= deadline:
            raise HwTimeoutError(what, timeout, ctx.hw.latest(40))
        time.sleep(0.05)


def collect_tr(ctx, mark: int, want: int, timeout: float = 8.0) -> list[dict]:
    """等 trace_dump 事件行攒够 want 条后返回解析结果。

    真机 trace_dump 先打印头部，事件行随后经 UART 逐行到达；QEMU 用例用
    serial.text 轮询可立即读到，真机必须等齐，否则会误判「0 条事件」。
    """
    text = poll_since(ctx, mark, "trace 事件行 >=%d" % want,
                      lambda t: len(tr_events(t)) >= want, timeout)
    return tr_events(text)


def count(text: str, pattern: str) -> int:
    return len(re.findall(pattern, text, re.MULTILINE))


def no_crash(text: str, where: str = "") -> None:
    """确认真机没有异常/恐慌记录（真机必须活着跑，不能靠 HALTED 蒙混）。"""
    assert "[POTATO][EXC]" not in text, "出现异常转储%s" % ((" (%s)" % where) if where else "")
    assert "[POTATO][PANIC]" not in text, "出现 PANIC%s" % ((" (%s)" % where) if where else "")


def health(ctx, timeout: float = 8.0):
    """发送 ``health`` 并解析单行 JSON 快照（要求整行到达：以 '\\r' 结尾）。"""
    ctx.send("health")
    _, h = expect_json(ctx, r"\[POTATO\]\[HEALTH\] (\{[^\r\n]*\})\r", timeout)
    return h


def trace_meta(ctx, timeout: float = 8.0) -> dict:
    """发 ``trace meta`` 并解析 TRM JSON；附带 counts_map={事件名: 累计计数}。

    真机 trace 环只有 256 深，tick=irq 下 IRQ_ENTER/IRQ_EXIT 约 2000 条/s，
    窗口仅 ~128ms，且大 n ``trace N`` dump 在打印期间还会被继续覆盖（实测
    掉行/事件种类丢失）。因此「某类事件出现过」这类断言改用 meta 的累计计数
    做前后差值（QEMU 用 trace N 行文本，真机 tick=poll 无 IRQ 洪水）。
    """
    ctx.send("trace meta")
    _, obj = expect_json(ctx, TRM, timeout)
    obj["counts_map"] = {c["ev"]: int(c["n"]) for c in obj.get("counts", [])}
    return obj


def smoke_ready(ctx) -> None:
    """冒烟用例统一入口：确认 READY 之后控制台可用（ping）。"""
    ctx.cmd("ping", r"pong\r", 5.0)
