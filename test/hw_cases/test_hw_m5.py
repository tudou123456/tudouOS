"""M5 真机验收：PAL/驱动框架、RAM-only 启动布局、FS、MMU 实验台、跨模块功能链。

断言来源：test/cases/test_m5.py（QEMU）与 test/cases/test_m7_nodev.py（无网口降级）。
真机差异（均给出判据，不放宽）：
- QEMU XIP 布局（irom 0x42000000..、load:0x42…）在真机不适用：tools/build.py 的
  hw 默认是「全 RAM 布局」（无 .irom_text，只烧 potato_boot_hw.bin），真机不跑
  flash XIP 取指。故 boot 布局断言改为 RAM-only 判据：ROM 只加载 0x3fc8…（DRAM）
  与 0x4038…（IRAM），不得出现 load:0x42/load:0x3c；mem 行 iram 基址=0x40380000。
  该差异是构建决定，不是降低要求：XIP 语义在 QEMU 用例 test_m5 已覆盖，真机
  另用 m5/xip_not_applicable 显式 SKIP 记录（附原因）。
- 驱动：真机 uart 驱动 ok，net 驱动因无 EMAC 为 skipped（status=1）——这正是
  M7 无网口降级要求的证据。
"""

from __future__ import annotations

import re

from test.hw_runner import HwSkip, hw_case
from test.hw_util import (count, kernel_versions, memstat, no_crash, poll,
                          tickstat)

PAL = r"\[POTATO\]\[PAL\] platform=(\S+) tick=(\w+) cpu_hz=(\d+)\r"
DRV_OK = r"\[POTATO\]\[DRV\] (\S+) init ok\r"
DRV_UART = r"\[POTATO\]\[DRV\] uart status=0\r"
DRV_NET = r"\[POTATO\]\[DRV\] net status=1\r"
FS_FORMAT = r"\[POTATO\]\[FS\] formatted\r"
FS_WRITE = r"\[POTATO\]\[FS\] wrote test.txt 11 bytes\r"
FS_CAT = r"hello-potfs\r"
FS_LS_ONE = r"\[POTATO\]\[FS\] test.txt 11\r"
FS_RM = r"\[POTATO\]\[FS\] removed test.txt\r"
FS_LS_ZERO = r"\[POTATO\]\[FS\] 0 files, 0 bytes\r"
FS_DF = r"\[POTATO\]\[FS\] store=potfs\(ram\) files=(\d+)/(\d+) bytes=(\d+)"
MMU_DUMP = (r"\[POTATO\]\[E3\] mmu\[0\]=0x([0-9a-f]+) \[1\]=0x([0-9a-f]+) "
            r"\[2\]=0x([0-9a-f]+) \[3\]=0x([0-9a-f]+) \[32\]=0x([0-9a-f]+) "
            r"\[511\]=0x([0-9a-f]+)\r")
MMU_LOAD = r"\[POTATO\]\[E3\] load 0x42020000 -> 0x[0-9a-f]+ \(no fault\)\r"
SELFTEST = r"\[POTATO\]\[MEM\] SELFTEST-(PASS|FAIL) n=(\d+) faults=(\d+)\r"


@hw_case("m5/boot_ram_only_layout", timeout=12, smoke=True)
def test_boot_ram_only_layout(ctx):
    """真机 RAM-only 启动布局：ROM 只加载 DRAM(0x3fc8…) + IRAM(0x4038…)，无 flash XIP 段。"""
    boot = ctx.hw.text()[ctx.start_pos:]
    assert re.search(r"load:0x3fc[0-9a-f]{5}", boot), "ROM 未加载 DRAM 段 (0x3fc8…)"
    assert re.search(r"load:0x403[0-9a-f]{5}", boot), "ROM 未加载 IRAM 段 (0x4038…)"
    assert "load:0x42" not in boot, "出现 flash XIP 段 load:0x42…（真机全 RAM 构建不应有）"
    assert "load:0x3c" not in boot, "出现 DROM 段 load:0x3c…（真机全 RAM 构建不应有）"
    m = re.search(r"\[POTATO\] mem iram=(0x[0-9a-f]+)-(0x[0-9a-f]+)", boot)
    assert m, "boot 日志缺 mem 行"
    lo, hi = int(m.group(1), 16), int(m.group(2), 16)
    # 注：mem 行打印的是 .text 区间起点（真机实测 0x40380460，ROM boot header
    # 之前 0x460 字节不属 .text），不是 linker ORIGIN(0x40380000)；因此只要求
    # lo 落在 [ORIGIN, ORIGIN+0x1000) 且整体不越出 RAM-only iram 长度。
    assert 0x40380000 <= lo < 0x40381000, (
        "iram .text 起点=0x%x 不在 [0x40380000,0x40381000)（RAM-only 布局）" % lo)
    assert hi > lo and (hi - lo) <= 0x20000, "iram 区间异常: 0x%x-0x%x" % (lo, hi)
    ctx.checks.append("RAM-only: load 0x3fc8/0x4038, iram 0x%x-0x%x, 无 XIP 段"
                      % (lo, hi))


@hw_case("m5/xip_not_applicable", timeout=10)
def test_xip_not_applicable(ctx):
    """QEMU XIP 布局断言（irom 0x42…/load:0x42…）在真机全 RAM 构建不适用，显式 SKIP。"""
    raise HwSkip(
        "真机默认全 RAM 构建（tools/build.py --platform hw：无 .irom_text，"
        "只烧 potato_boot_hw.bin），不做 flash XIP 取指；QEMU test_m5 的 XIP 布局"
        "断言不适用。真机对应判据见 m5/boot_ram_only_layout。")


@hw_case("m5/pal_hw", timeout=10)
def test_pal_hw(ctx):
    """pal：platform=hw-esp32s3、tick=irq、cpu_hz 与 tickstat 一致（QEMU 对应 qemu-esp32s3/poll/40M）。"""
    line = ctx.cmd_line("pal", PAL, timeout=6.0)
    m = re.search(PAL, line)
    platform, tick, cpu_hz = m.group(1), m.group(2), int(m.group(3))
    assert platform == "hw-esp32s3", "platform=%r（真机应为 hw-esp32s3）" % platform
    assert tick == "irq", "tick=%r（真机为中断 tick）" % tick
    t = tickstat(ctx)
    assert cpu_hz == t["cpu_hz"], "pal cpu_hz=%d != tickstat cpu_hz=%d" % (
        cpu_hz, t["cpu_hz"])
    ctx.checks.append("pal platform=%s tick=%s cpu_hz=%d" % (platform, tick, cpu_hz))


@hw_case("m5/driver_framework", timeout=12)
def test_driver_framework(ctx):
    """驱动框架：uart init ok 且 status=0；net 无硬件 status=1（skipped，QEMU+nodev 合判据）。"""
    boot = ctx.hw.text()[ctx.start_pos:]
    m = re.search(r"\[POTATO\]\[DRV\] (\S+) init ok\r", boot)
    assert m, "boot 日志无驱动 init ok 行"
    assert m.group(1) == "uart", "第一个 init ok 的驱动是 %r（QEMU 判据应为 uart）" % m.group(1)
    ctx.cmd_line("drivers", DRV_UART, timeout=6.0)
    ctx.expect(DRV_NET, timeout=6.0)
    ctx.cmd("ping", r"pong\r", timeout=5.0)
    ctx.checks.append("drivers: uart status=0, net status=1(skipped)")


@hw_case("m5/fs_workflow", timeout=20)
def test_fs_workflow(ctx):
    """potfs 全流程：mkfs→write→cat→ls→rm→ls→df（QEMU test_m6 fs 段同锚点，M5 也验收）。"""
    ctx.cmd_line("fs mkfs", FS_FORMAT, timeout=8.0)
    ctx.cmd_line("fs write test.txt hello-potfs", FS_WRITE, timeout=8.0)
    ctx.cmd_line("fs cat test.txt", FS_CAT, timeout=8.0)
    ctx.cmd_line("fs ls", FS_LS_ONE, timeout=8.0)
    ctx.cmd_line("fs rm test.txt", FS_RM, timeout=8.0)
    ctx.cmd_line("fs ls", FS_LS_ZERO, timeout=8.0)
    line = ctx.cmd_line("fs df", FS_DF, timeout=8.0)
    assert re.search(FS_DF, line), "fs df 行格式不符: %r" % line
    ctx.checks.append("potfs mkfs/write/cat/ls/rm/ls/df 全流程")


@hw_case("m5/mmutest_dump", timeout=12)
def test_mmutest_dump(ctx):
    """mmutest dump：E-03 实验台在真机上可读 PMP/MMU 条目（记录实测值）。"""
    line = ctx.cmd_line("mmutest", MMU_DUMP, timeout=8.0)
    m = re.search(MMU_DUMP, line)
    vals = [int(m.group(i), 16) for i in range(1, 7)]
    no_crash(ctx.text_since(ctx.start_pos), "mmutest dump")
    ctx.checks.append("mmu[0]=0x%x [1]=0x%x [2]=0x%x [3]=0x%x [32]=0x%x [511]=0x%x"
                      % tuple(vals))


@hw_case("m5/mmutest_load", timeout=15)
def test_mmutest_load(ctx):
    """mmutest load：读 0x42020000 不触发故障（QEMU 模型语义；真机结果如实记录）。"""
    ctx.cmd_line("mmutest load", MMU_LOAD, timeout=10.0)
    ctx.cmd("ping", r"pong\r", timeout=5.0)
    ctx.checks.append("mmutest load 0x42020000 -> no fault")


@hw_case("m5/cross_module_chain", timeout=20)
def test_cross_module_chain(ctx):
    """跨模块链路：sleep 100 → memtest selftest 16 → msgdemo ping 3 → pong（QEMU xip_full_function 同链）。"""
    ctx.cmd_line("sleep 100", r"\[POTATO\]\[D\] SLEPT 100\r", timeout=10.0)
    line = ctx.cmd_line("memtest selftest 16", SELFTEST, timeout=12.0)
    assert "SELFTEST-PASS n=16" in line, "selftest 16 未 PASS: %r" % line
    mark = ctx.mark()
    ctx.send("msgdemo ping 3")
    text = poll(ctx, "ping3 完成", lambda t: (
        re.search(r"\[POTATO\]\[M\] PING-DONE ok=(\d+) failed=(\d+)", t) is not None),
        timeout=15.0)
    ok = int(re.search(r"PING-DONE ok=(\d+)", text).group(1))
    assert ok == 3, "msgdemo ping 3 ok=%d != 3" % ok
    ctx.cmd("taskdemo off", r"\[POTATO\]\[D\] demo stop-req\r", timeout=6.0)
    ctx.cmd("ping", r"pong\r", timeout=5.0)
    ctx.checks.append("sleep 100 + selftest16 + msgdemo ping3 + pong")
