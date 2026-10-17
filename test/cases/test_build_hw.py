"""D-26 hw 构建门禁：--platform hw 只编译不运行。

真机后端（TUDOU_PLATFORM=1）在 D-26 修复前从未被编译过一次。本门禁
固化真机分叉的存在性与产物完整性：boot.S 16 位 MMU 表项（E-03）、
时基 systimer 运行标定 + rfe PS 槽双写/双读（E-02 真机对版）。不烧录、
不起 QEMU——真机运行验收在带板阶段人工/专用脚本进行。

2026-10-17 带板结论：真机 0x42 flash 取指路径不可用（见
docs/hardware-bringup.md），hw 默认改为"全 RAM 布局"：代码/只读数据
全在 IRAM，引导镜像即携带全部 RAM 段，不再产出 irom_hw.bin。
旧 XIP 实验布局仍可用 `--platform hw --xip` 构建。
"""

import os
import subprocess
import sys

import envlib


def test_build_hw_platform(ctx):
    """hw 平台完整构建必须成功且产物齐全（默认全 RAM 布局）。"""
    root = envlib.PROJECT_ROOT
    r = subprocess.run(
        [sys.executable, os.path.join(root, "tools", "build.py"),
         "--platform", "hw"],
        capture_output=True, text=True, shell=False, cwd=root, timeout=300)
    out = (r.stdout or "") + (r.stderr or "")
    assert r.returncode == 0, f"hw 构建失败:\n{out[-2000:]}"
    for name in ("kernel_hw.elf", "potato_boot_hw.bin"):
        p = os.path.join(root, "build", name)
        assert os.path.isfile(p) and os.path.getsize(p) > 0, \
            f"hw 构建缺产物: {name}"
    # 全 RAM 布局：内核代码段必须真的进了引导镜像（否则烧录后无代码可跑）
    boot = os.path.getsize(os.path.join(root, "build", "potato_boot_hw.bin"))
    assert boot > 60 * 1024, \
        f"hw 引导镜像应携带 RAM 中的内核代码（>60KB），实际 {boot} 字节"
    # 全 RAM 布局不应再引用 flash XIP 段
    assert "irom_hw.bin" not in out, "全 RAM 布局不应产出/提示 irom_hw.bin"
