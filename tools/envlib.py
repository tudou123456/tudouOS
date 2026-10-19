"""tudouOS 环境探测共享库。

工具链/QEMU 的查找顺序：
  1. 环境变量 TUDOU_CC / TUDOU_QEMU（显式指定，优先级最高）
  2. PATH（shutil.which）
  3. 项目内 third_party/ 目录（check_env.py 或手工解压的安装位置）
"""

import os
import glob
import shutil

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
THIRD_PARTY = os.path.join(PROJECT_ROOT, "third_party")

_EXE = ".exe" if os.name == "nt" else ""


def _first_existing(paths):
    for p in paths:
        if p and os.path.isfile(p):
            return os.path.normpath(p)
    return None


def find_cc():
    """返回 xtensa-esp-elf-gcc 的完整路径，找不到返回 None。"""
    env = os.environ.get("TUDOU_CC")
    candidates = [
        env,
        shutil.which("xtensa-esp-elf-gcc" + _EXE),
        shutil.which("xtensa-esp32s3-elf-gcc" + _EXE),
    ]
    p = _first_existing(candidates)
    if p:
        return p
    hits = glob.glob(os.path.join(THIRD_PARTY, "**", "bin", "xtensa-esp-elf-gcc" + _EXE),
                     recursive=True)
    hits += glob.glob(os.path.join(THIRD_PARTY, "**", "bin", "xtensa-esp32s3-elf-gcc" + _EXE),
                      recursive=True)
    hits += glob.glob(os.path.expanduser(
        "~/.espressif/tools/**/bin/xtensa-esp-elf-gcc" + _EXE), recursive=True)
    return os.path.normpath(hits[0]) if hits else None


def find_qemu():
    """返回 qemu-system-xtensa 的完整路径，找不到返回 None。"""
    env = os.environ.get("TUDOU_QEMU")
    p = _first_existing([env, shutil.which("qemu-system-xtensa" + _EXE)])
    if p:
        return p
    hits = glob.glob(os.path.join(THIRD_PARTY, "**", "qemu-system-xtensa" + _EXE),
                     recursive=True)
    return os.path.normpath(hits[0]) if hits else None


def esptool_module():
    """返回调用 esptool 的 python 命令前缀（[python, -m, esptool]）；
    esptool 不可用时返回 None。"""
    import importlib.util
    if importlib.util.find_spec("esptool") is not None:
        return True
    return False


TOOLCHAIN_URL = (
    "https://github.com/espressif/crosstool-NG/releases/download/"
    "esp-13.2.0_20250707/xtensa-esp-elf-13.2.0_20250707-x86_64-w64-mingw32.zip"
)
QEMU_URL = (
    "https://github.com/espressif/qemu/releases/download/"
    "esp-develop-9.2.2-20260417/"
    "qemu-xtensa-softmmu-esp_develop_9.2.2_20260417-x86_64-w64-mingw32.tar.xz"
)
