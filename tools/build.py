"""土豆内核构建脚本：编译 → 链接 → elf2image → merge-bin。

产物（均在 build/ 下）:
  kernel.elf        带符号的内核 ELF（GDB 调试用）
  potato_boot.bin   ESP 镜像格式内核（ROM 可直接加载）
  flash_image.bin   4MB flash 镜像（QEMU 用，也可整体烧入真机）

真机烧录偏移（二级引导偏移）会在构建结束时打印，见 README。
"""

import glob
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import envlib  # noqa: E402

ROOT = envlib.PROJECT_ROOT
BUILD = os.path.join(ROOT, "build")
OBJDIR = os.path.join(BUILD, "obj")
KERNEL_DIR = os.path.join(ROOT, "kernel")
INCLUDE_DIR = os.path.join(KERNEL_DIR, "include")
LINKER_SCRIPT = os.path.join(KERNEL_DIR, "linker", "tudou.ld")
ELF = os.path.join(BUILD, "kernel.elf")
BOOT_BIN = os.path.join(BUILD, "potato_boot.bin")
FLASH_IMG = os.path.join(BUILD, "flash_image.bin")

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
        out = os.path.join(td, "probe.o")
        r = subprocess.run([cc, "-mcpu=esp32s3", "-c", "-xc", "-", "-o", out],
                           input=PROBE_SRC, capture_output=True, text=True,
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


def main():
    cc = envlib.find_cc()
    if not cc:
        print("未找到 xtensa-esp-elf-gcc，先运行 python tools/check_env.py 按提示准备环境。")
        return 1
    if not envlib.esptool_module():
        print("未找到 esptool，请执行: python -m pip install esptool")
        return 1

    os.makedirs(OBJDIR, exist_ok=True)
    env = toolchain_env(cc)
    mcpu = probe_mcpu(cc, env)

    base_flags = ["-mabi=call0", "-mlongcalls", "-mtext-section-literals"] + mcpu
    cflags = base_flags + [
        "-O2", "-g", "-Wall", "-Wextra", "-std=c11",
        "-ffreestanding", "-fno-builtin", "-fno-stack-protector",
        "-ffunction-sections", "-fdata-sections",
        "-I", INCLUDE_DIR,
    ]

    # ---- 1. 编译 ----
    print("[1/4] 编译内核 ...")
    objs = []
    for src in sorted(glob.glob(os.path.join(KERNEL_DIR, "kernel", "*.c")) +
                      glob.glob(os.path.join(KERNEL_DIR, "boot", "*.S"))):
        obj = os.path.join(OBJDIR, os.path.splitext(os.path.basename(src))[0] + ".o")
        subprocess.run([cc] + (cflags if src.endswith(".c") else base_flags) +
                       ["-c", src, "-o", obj], check=True, shell=False, env=env)
        print("   CC", os.path.basename(src))
        objs.append(obj)

    # ---- 2. 链接 ----
    print("[2/4] 链接 kernel.elf ...")
    subprocess.run([cc, "-nostdlib", "-static",
                    "-T", LINKER_SCRIPT,
                    "-Wl,--gc-sections", "-Wl,--build-id=none", "-Wl,--no-warn-rwx-segments",
                    "-o", ELF] + objs, check=True, shell=False, env=env)

    # ---- 3. elf2image ----
    print("[3/4] 生成 ESP 镜像 potato_boot.bin ...")
    v = esptool_major()
    # v5 用连字符长选项(--flash-mode)；v4 用下划线(--flash_mode)
    fs = "--flash-" if v >= 5 else "--flash_"
    subprocess.run([sys.executable, "-m", "esptool", "--chip", "esp32s3", "elf2image",
                    fs + "mode", "dio",
                    fs + "freq", "80m",
                    fs + "size", "4MB",
                    "-o", BOOT_BIN, ELF], check=True, shell=False, env=env)

    # ---- 4. merge-bin（单镜像内核放在二级引导偏移处，其余留空）----
    print("[4/4] 合成 flash 镜像 flash_image.bin ...")
    offset = bootloader_offset()
    merge = "merge-bin" if v >= 5 else "merge_bin"
    pad = "--pad-to-size" if v >= 5 else "--fill-flash-size"
    subprocess.run([sys.executable, "-m", "esptool", "--chip", "esp32s3", merge,
                    "-o", FLASH_IMG, pad, "4MB",
                    format(offset, "#x"), BOOT_BIN], check=True, shell=False, env=env)

    print()
    print("构建完成:")
    print("  " + ELF)
    print("  " + BOOT_BIN + "  (flash 偏移 " + hex(offset) + ")")
    print("  " + FLASH_IMG + "  (4MB, QEMU 直接使用)")
    print()
    print("真机烧录: esptool --chip esp32s3 write-flash " + hex(offset) +
          " build/potato_boot.bin")
    print("下一步:")
    print("  交互运行:  python qemu/run_qemu.py")
    print("  自动测试:  python -m test.runner")
    return 0


if __name__ == "__main__":
    sys.exit(main())
