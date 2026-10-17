"""土豆内核构建脚本：编译 → 链接 → elf2image → merge-bin。

产物（均在 build/ 下）:
  kernel.elf        带符号的内核 ELF（GDB 调试用）
  potato_boot.bin   ESP 镜像格式引导镜像（只含 RAM 段，flash 偏移 0x0）
  irom.bin          flash XIP 段（必须与引导镜像分开烧到 0x10020）
  flash_image.bin   4MB 全量 flash 镜像（QEMU 直接使用；真机可整体烧录）

真机烧录偏移（二级引导偏移）会在构建结束时打印，见 README。
门禁（D-10）：-Werror（编译警告即失败）+ flash 镜像 IROM 区段逐字节自检。
历史上有过 "objdump 验证 loop 指令为 0" 的门禁计划，E-01 判定实验已
证明 loop 指令在本 QEMU 正常（回归用例 test_loop_instruction 固化），
故不设该门禁。
"""

import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import envlib  # noqa: E402

ROOT = envlib.PROJECT_ROOT
BUILD = os.path.join(ROOT, "build")
KERNEL_DIR = os.path.join(ROOT, "kernel")
INCLUDE_DIR = os.path.join(KERNEL_DIR, "include")
LINKER_SCRIPT = os.path.join(KERNEL_DIR, "linker", "tudou.ld")


def want_platform():
    """--platform {qemu,hw}（默认 qemu）。hw 注入 TUDOU_PLATFORM=1（D-26）：
    boot.S 走 16 位 MMU 表项（E-03：QEMU 模型 32 位 vs 真机 TRM 16 位）、
    时基 systimer 运行标定、rfe PS 槽双写（真机 EPS1 编号实证前双覆盖）。
    产物加 _hw 后缀并用独立 obj 目录，防与 QEMU 增量产物互踩。"""
    argv = sys.argv[1:]
    plat = "qemu"
    if "--platform" in argv:
        i = argv.index("--platform")
        if i + 1 >= len(argv) or argv[i + 1] not in ("qemu", "hw"):
            print("用法: build.py [--clean] [--xip] [--platform qemu|hw]")
            sys.exit(2)
        plat = argv[i + 1]
    return plat


def want_hw_xip():
    """--xip（仅 hw）：保留旧的真机 flash XIP 布局做实验用。

    默认 hw 用"全 RAM 布局"：真机 0x42 flash 取指路径不可用（'X' 取指
    实验与带板实证见 docs/hardware-bringup.md），全部代码/只读数据进
    IRAM，ROM 把 RAM 段直接搬到链接地址后即可执行，不依赖 MMU/icache。
    QEMU 保持 XIP（模型可取指，且 RAM 容量更紧）。"""
    return "--xip" in sys.argv[1:]


PLATFORM = want_platform()
HW_XIP = want_hw_xip() and PLATFORM == "hw"  # qemu 的 XIP 由平台分叉决定
SUFFIX = "" if PLATFORM == "qemu" else "_hw"
TUDOU_PLATFORM_VAL = 0 if PLATFORM == "qemu" else 1
TUDOU_HW_XIP_VAL = 1 if HW_XIP else 0
RAM_ONLY_HW = PLATFORM == "hw" and not HW_XIP
# 真机启动分阶段标记（调试）：TUDOU_BOOT_TRACE=1 python tools/build.py --platform hw
BOOT_TRACE_VAL = os.environ.get("TUDOU_BOOT_TRACE", "0")
OBJDIR = os.path.join(BUILD, "obj" + SUFFIX)
ELF = os.path.join(BUILD, "kernel" + SUFFIX + ".elf")
BOOT_ELF = os.path.join(BUILD, "kernel_boot" + SUFFIX + ".elf")
BOOT_BIN = os.path.join(BUILD, "potato_boot" + SUFFIX + ".bin")
IROM_BIN = os.path.join(BUILD, "irom" + SUFFIX + ".bin")
FLASH_IMG = os.path.join(BUILD, "flash_image" + SUFFIX + ".bin")
# 预处理后的链接脚本（#if 选择 XIP / 全 RAM 布局，见 tudou.ld）
LD_GEN = os.path.join(BUILD, "tudou" + SUFFIX + ".ld")
# .irom_text 在 flash 的装载地址（tudou.ld 的 AT(0x00010020)：MMU 页 1 + 段内 0x20）
IROM_FLASH_OFFSET = 0x10020

PROBE_SRC = "int probe(void) { return 0; }\n"


def toolchain_env(cc):
    """GCC 13.x 统一 xtensa 工具链靠 XTENSA_GNU_CONFIG 选芯片配置，
    必须指向 esp32s3 的配置 DLL，否则默认大端配置、esptool 拒收。"""
    env = dict(os.environ)
    cfg = os.path.join(os.path.dirname(os.path.dirname(cc)), "lib", "xtensa_esp32s3.so")
    if os.path.isfile(cfg):
        env["XTENSA_GNU_CONFIG"] = cfg
    return env


def probe_mcpu(cc, env):
    """更新的统一工具链支持 -mcpu 选核；不支持的（如 13.2）返回空。"""
    with tempfile.TemporaryDirectory() as td:
        src = os.path.join(td, "probe.c")
        out = os.path.join(td, "probe.o")
        Path(src).write_text(PROBE_SRC, encoding="ascii")
        r = subprocess.run([cc, "-mcpu=esp32s3", "-c", "-xc", src, "-o", out],
                           capture_output=True, text=True,
                           shell=False, env=env)
        return ["-mcpu=esp32s3"] if r.returncode == 0 else []


def esptool_major():
    import esptool
    return int(esptool.__version__.split(".")[0])


def bootloader_offset():
    """S3 的二级引导偏移（esptool 目标类常量；S3=0x0，经典 ESP32 才是 0x1000）。"""
    try:
        from esptool.targets.esp32s3 import ESP32S3ROM  # esptool v5
        return int(ESP32S3ROM.BOOTLOADER_FLASH_OFFSET)
    except Exception:  # noqa: BLE001
        pass
    try:
        from esptool.targets import ESP32S3  # esptool v4
        return int(ESP32S3.BOOTLOADER_OFFSET)
    except Exception:  # noqa: BLE001
        return 0x0


def check_toolchain_versions(cc):
    """软性版本校验：工具链/QEMU 版本漂移会改变 QEMU 六坑相关测试的
    容差行为（envlib 固定的版本是实测基线），漂移即提醒。
    版本号从安装路径解析（crosstool 发布目录名含版本），不执行进程。"""
    m = re.search(r"(\d+\.\d+\.\d+)", cc)
    if m and m.group(1) != "13.2.0":
        print(f"警告: 工具链版本 {m.group(1)} != 实测基线 13.2.0，"
              "QEMU 相关测试容差可能失真")
    qemu = envlib.find_qemu()
    if qemu:
        # 版本目录可能在可执行文件的任一上级（third_party/qemu/<ver>/bin/..）
        p = qemu
        esp_branch = False
        for _ in range(5):
            p = os.path.dirname(p)
            if "esp-develop" in p or "esp-release" in p:
                esp_branch = True
                break
        if not esp_branch:
            print(f"警告: QEMU 非乐鑫分支构建（{qemu}），已知六坑假设可能不成立")


def want_clean():
    """--clean 独立小函数解析 argv（与构建命令分域），避免命令构造混入输入。"""
    return "--clean" in sys.argv[1:]


def main():
    cc = envlib.find_cc()
    if not cc:
        print("未找到 xtensa-esp-elf-gcc，先运行 python tools/check_env.py 按提示准备环境。")
        return 1
    if not envlib.esptool_module():
        print("未找到 esptool，请执行: python -m pip install esptool")
        return 1
    check_toolchain_versions(cc)

    if want_clean() and os.path.isdir(OBJDIR):
        shutil.rmtree(OBJDIR)
        print("已清理", OBJDIR)
    os.makedirs(OBJDIR, exist_ok=True)
    env = toolchain_env(cc)
    mcpu = probe_mcpu(cc, env)

    base_flags = ["-mabi=call0", "-mlongcalls", "-mauto-litpools",
                  "-DTUDOU_PLATFORM=" + str(TUDOU_PLATFORM_VAL),
                  "-DTUDOU_HW_XIP=" + str(TUDOU_HW_XIP_VAL),
                  "-DTUDOU_BOOT_TRACE=" + str(BOOT_TRACE_VAL)] + mcpu
    # 汇编：M5 起跨区调用（IRAM 汇编 ↔ IROM C 代码）需要长调用（l32r+callx0）；
    # 字面量由链接脚本归入 IRAM（l32r 向后取，详见 tudou.ld）。
    # TUDOU_PLATFORM 必须同样注入汇编（boot.S/switch.S/vectors.S 按
    # 平台分叉 MMU 表项写法与 rfe PS 槽）；TUDOU_HW_XIP 控制 boot.S 是否
    # 走旧的真机 flash XIP 自举链（默认全 RAM 布局为 0，链不编译）。
    asm_flags = ["-mabi=call0", "-mlongcalls", "-I", INCLUDE_DIR,
                 "-DTUDOU_PLATFORM=" + str(TUDOU_PLATFORM_VAL),
                 "-DTUDOU_HW_XIP=" + str(TUDOU_HW_XIP_VAL),
                 "-DTUDOU_BOOT_TRACE=" + str(BOOT_TRACE_VAL)] + mcpu
    # -Werror 为门禁（D-10）；确实需要临时放行的场合直接编辑本行，勿提交
    cflags = base_flags + [
        "-O2", "-g", "-Wall", "-Wextra", "-Werror", "-std=gnu11",
        "-ffreestanding", "-fno-builtin", "-fno-stack-protector",
        "-ffunction-sections", "-fdata-sections",
        "-I", INCLUDE_DIR,
    ]

    # ---- 1. 编译（mtime 增量：源头文件或任一头文件新于 .o 才重编） ----
    headers = sorted(glob.glob(os.path.join(INCLUDE_DIR, "*.h")))
    newest_hdr = max([os.path.getmtime(h) for h in headers] + [0])
    print("[1/4] 编译内核（增量）...")
    objs = []
    rebuilt = 0
    for src in sorted(glob.glob(os.path.join(KERNEL_DIR, "kernel", "*.c")) +
                      glob.glob(os.path.join(KERNEL_DIR, "boot", "*.S")) +
                      glob.glob(os.path.join(KERNEL_DIR, "intc", "*.c")) +
                      glob.glob(os.path.join(KERNEL_DIR, "intc", "*.S")) +
                      glob.glob(os.path.join(KERNEL_DIR, "time", "*.c")) +
                      glob.glob(os.path.join(KERNEL_DIR, "trace", "*.c")) +
                      glob.glob(os.path.join(KERNEL_DIR, "sched", "*.c")) +
                      glob.glob(os.path.join(KERNEL_DIR, "sched", "*.S")) +
                      glob.glob(os.path.join(KERNEL_DIR, "smp", "*.c")) +
                      glob.glob(os.path.join(KERNEL_DIR, "smp", "*.S")) +
                      glob.glob(os.path.join(KERNEL_DIR, "msg", "*.c")) +
                      glob.glob(os.path.join(KERNEL_DIR, "mem", "*.c")) +
                      glob.glob(os.path.join(KERNEL_DIR, "pal", "*.c")) +
                      glob.glob(os.path.join(KERNEL_DIR, "driver", "*.c")) +
                      glob.glob(os.path.join(KERNEL_DIR, "fs", "*.c")) +
                      glob.glob(os.path.join(KERNEL_DIR, "net", "*.c")) +
                      glob.glob(os.path.join(KERNEL_DIR, "mcp", "*.c"))):
        obj = os.path.join(OBJDIR, os.path.splitext(os.path.basename(src))[0] + ".o")
        if (os.path.isfile(obj) and os.path.getmtime(src) < os.path.getmtime(obj)
                and newest_hdr < os.path.getmtime(obj)):
            objs.append(obj)  # 增量命中
            continue
        flags = cflags if src.endswith(".c") else asm_flags
        subprocess.run([cc, *flags, "-c", src, "-o", obj],
                       check=True, shell=False, env=env)
        print("   CC", os.path.basename(src))
        objs.append(obj)
        rebuilt += 1
    print(f"   重编 {rebuilt} / {len(objs)} 个目标文件")

    # ---- 2. 链接（链接脚本先经 cpp 预处理：#if 选 XIP / 全 RAM 布局） ----
    print("[2/4] 链接 kernel.elf ...")
    subprocess.run([cc, "-E", "-P", "-x", "c",
                    "-DTUDOU_PLATFORM=" + str(TUDOU_PLATFORM_VAL),
                    "-DTUDOU_HW_XIP=" + str(TUDOU_HW_XIP_VAL),
                    "-o", LD_GEN, LINKER_SCRIPT],
                   check=True, shell=False, env=env)
    subprocess.run([cc, "-nostdlib", "-static",
                    "-T", LD_GEN,
                    "-Wl,--gc-sections", "-Wl,--build-id=none",
                    "-Wl,--no-warn-rwx-segments",
                    "-Wl,--no-relax",
                    "-o", ELF, *objs], check=True, shell=False, env=env)

    # ---- 3. elf2image ----
    print("[3] 生成 ESP 镜像 potato_boot.bin ...")
    v = esptool_major()
    # v5 用连字符长选项(--flash-mode)；v4 用下划线(--flash_mode)
    fs = "--flash-" if v >= 5 else "--flash_"
    objcopy = cc.replace("gcc", "objcopy")
    offset = bootloader_offset()
    if RAM_ONLY_HW:
        # 全 RAM 布局：没有 .irom_text（代码并入 IRAM 的 .iram_text），
        # 引导镜像直接携带全部 RAM 段；清掉旧 XIP 产物避免误烧。
        for stale in (IROM_BIN, FLASH_IMG):
            if os.path.isfile(stale):
                os.remove(stale)
        subprocess.run([objcopy, ELF, BOOT_ELF], check=True, shell=False, env=env)
    else:
        subprocess.run([objcopy, "--remove-section=.irom_text", ELF, BOOT_ELF],
                       check=True, shell=False, env=env)
    subprocess.run([sys.executable, "-m", "esptool", "--chip", "esp32s3", "elf2image",
                    fs + "mode", "dio",
                    fs + "freq", "80m",
                    fs + "size", "4MB",
                    "-o", BOOT_BIN, BOOT_ELF], check=True, shell=False, env=env)

    if not RAM_ONLY_HW:
        # ---- 4b. 抽取 IROM 段（C 代码+只读数据，LMA 0x10020 起）----
        print("[4b] 抽取 IROM 段 irom.bin ...")
        subprocess.run([objcopy, "-O", "binary",
                        "--only-section=.irom_text", ELF, IROM_BIN],
                       check=True, shell=False, env=env)

        # ---- 5. 合成 flash 镜像：引导镜像@0x0 + IROM@0x10020（自研二级映射布局）----
        print("[5] 合成 flash 镜像 flash_image.bin ...")
        merge = "merge-bin" if v >= 5 else "merge_bin"
        pad = "--pad-to-size" if v >= 5 else "--fill-flash-size"
        subprocess.run([sys.executable, "-m", "esptool", "--chip", "esp32s3", merge,
                        "-o", FLASH_IMG, pad, "4MB",
                        format(offset, "#x"), BOOT_BIN,
                        hex(IROM_FLASH_OFFSET), IROM_BIN], check=True, shell=False, env=env)

        # ---- 6. 自检：flash 镜像的 IROM 区段必须与 irom.bin 逐字节一致 ----
        # 防止"烧录提示"与镜像布局再次脱节（M5 后 C 代码在 flash 0x10020 起）
        print("[6] 自检 flash 镜像 IROM 区段 ...")
        img = Path(FLASH_IMG).read_bytes()
        seg = Path(IROM_BIN).read_bytes()
        region = img[IROM_FLASH_OFFSET:IROM_FLASH_OFFSET + len(seg)]
        if region != seg:
            print(f"自检失败: flash_image.bin[{IROM_FLASH_OFFSET:#x}..] 与 irom.bin 不一致")
            return 1
        print(f"   OK ({len(seg)} 字节 @ {IROM_FLASH_OFFSET:#x} 逐字节一致)")
    else:
        print("[4b] hw 全 RAM 布局：无 flash XIP 段，跳过 irom.bin/合并/自检")

    print()
    print("构建完成 (" + PLATFORM + "):")
    print("  " + ELF)
    print("  " + BOOT_BIN + "  (flash 偏移 " + hex(offset) + ")")
    if PLATFORM == "qemu":
        print("  " + FLASH_IMG + "  (4MB, QEMU 直接使用)")
    elif not RAM_ONLY_HW:
        print("  " + FLASH_IMG + "  (4MB 整体烧录用)")
    print()
    if PLATFORM == "hw" and RAM_ONLY_HW:
        print("真机烧录（hw 全 RAM 布局：只需引导镜像一段）:")
        print("  py -3.14 -m esptool --chip esp32s3 --port COMx --baud 921600 \\")
        print("      write-flash " + hex(offset) + " build/potato_boot_hw.bin")
        print("  串口监控: py -3.14 tools/hw_monitor.py --port COMx")
        print("  （hw 默认全 RAM：真机 0x42 flash 取指路径不可用，代码/只读数据")
        print("    全在 IRAM，ROM 加载即可执行，不依赖 MMU/icache；实验旧 XIP")
        print("    布局用 --platform hw --xip，见 docs/hardware-bringup.md）")
    elif PLATFORM == "hw":
        print("真机烧录（XIP 实验布局，两段都要烧，只烧引导镜像必崩）:")
        print("  py -3.14 -m esptool --chip esp32s3 --port COMx --baud 921600 \\")
        print("      write-flash " + hex(offset) + " build/potato_boot_hw.bin" +
              " 0x10020 build/irom_hw.bin")
        print("  串口监控: py -3.14 tools/hw_monitor.py --port COMx")
        print("  （--xip 布局：真机 0x42 取指路径尚不可用，仅用于实验定位）")
    else:
        print("真机烧录改用: python tools/build.py --platform hw（D-26/E-03/E-02 分叉）")
        print("下一步:")
        print("  交互运行:  python qemu/run_qemu.py")
        print("  自动测试:  python -m test.runner")
    return 0


if __name__ == "__main__":
    sys.exit(main())
