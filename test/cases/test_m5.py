"""M5 验收用例：自研二级映射引导 + flash XIP + PAL + 驱动框架。

XIP 核心断言：内存布局行显示 irom 在 0x42 flash 映射窗口（代码在
flash 执行），IRAM 只留向量+汇编（KB 级）；ROM 只搬 RAM 段。
"""

import re
import time

READY_LINE = r"\[POTATO\] READY"
MEM_LINE = (r"\[POTATO\] mem iram=(0x[0-9a-f]+)-(0x[0-9a-f]+) "
            r"irom\(flash\)=(0x[0-9a-f]+)-(0x[0-9a-f]+) "
            r"data=(0x[0-9a-f]+)-(0x[0-9a-f]+)")


def test_xip_memory_map(ctx):
    """XIP 布局：代码+只读数据在 0x42 flash 窗口，IRAM 只留汇编。"""
    m = ctx.serial.expect(MEM_LINE, timeout=20)
    irom_lo, irom_hi = int(m.group(3), 16), int(m.group(4), 16)
    iram_lo, iram_hi = int(m.group(1), 16), int(m.group(2), 16)
    assert irom_lo >= 0x42000000 and irom_hi < 0x43000000, \
        f"IROM 不在 flash 映射窗口: {hex(irom_lo)}-{hex(irom_hi)}"
    assert iram_hi - iram_lo <= 0x1000, \
        f"IRAM 应只留向量+汇编（≤4KB）: {hex(iram_hi - iram_lo)}"
    assert irom_hi - irom_lo > 0x4000, \
        f"IROM 应有主体代码（>16KB）: {hex(irom_hi - irom_lo)}"


def test_xip_boot_chain(ctx):
    """ROM 只搬 RAM 段（load 行不含 0x42/0x3c 窗口）且系统完整启动。"""
    ctx.serial.expect(r"load:0x3fc88000", timeout=15)   # DRAM 段
    ctx.serial.expect(r"load:0x40380000", timeout=15)   # IRAM 段
    ctx.serial.expect(READY_LINE, timeout=15)
    # 不应有 0x42/0x3c 窗口的 load 行（ROM 无映射能力）
    assert "load:0x42" not in ctx.serial.text, "ROM 不应搬运 IROM 段"
    assert "load:0x3c" not in ctx.serial.text, "ROM 不应搬运 DROM 段"


def test_pal_info(ctx):
    """PAL：平台/时基模式可查询（双端同源接口成形）。"""
    ctx.serial.expect(READY_LINE, timeout=20)
    ctx.serial.send("pal")
    m = ctx.serial.expect(r"\[POTATO\]\[PAL\] platform=(\S+) tick=(\w+) cpu_hz=(\d+)\r",
                          timeout=8)
    assert m.group(1) == "qemu-esp32s3", f"平台名异常: {m.group(1)}"
    assert m.group(2) == "poll", f"QEMU 时基应为轮询制: {m.group(2)}"
    assert int(m.group(3)) == 40000000, f"cpu_hz: {m.group(3)}"


def test_driver_framework(ctx):
    """驱动框架：链接期注册、启动期初始化、运行期可枚举。"""
    m = ctx.serial.expect(r"\[POTATO\]\[DRV\] (\S+) init ok", timeout=20)
    assert m.group(1) == "uart", f"首个注册驱动应为 uart: {m.group(1)}"
    ctx.serial.expect(READY_LINE, timeout=15)
    ctx.serial.send("drivers")
    ctx.serial.expect(r"\[POTATO\]\[DRV\] uart status=0", timeout=8)
    # 驱动注册后系统功能不受影响
    ctx.serial.send("ping")
    ctx.serial.expect(r"pong", timeout=8)


def test_xip_full_function(ctx):
    """XIP 下全子系统烟测：调度/消息/内存都工作（flash 执行无损）。"""
    ctx.serial.expect(READY_LINE, timeout=20)
    ctx.serial.send("sleep 100")
    ctx.serial.expect(r"SLEPT 100", timeout=10)
    ctx.serial.send("memtest selftest 16")
    ctx.serial.expect(r"SELFTEST-PASS", timeout=15)
    ctx.serial.send("msgdemo ping 3")
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        if "PING-DONE" in ctx.serial.text:
            break
        time.sleep(0.4)
    m = re.findall(r"PING-DONE ok=(\d+) failed=(\d+)", ctx.serial.text)
    assert m and m[-1] == ("3", "0"), f"消息基座 XIP 下异常: {m}"
    ctx.serial.send("msgdemo off")
