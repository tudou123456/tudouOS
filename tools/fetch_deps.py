#!/usr/bin/env python3
"""下载并校验 tudouOS 固定版本的交叉工具链与 QEMU（Windows x64）。

为保证构建/回归可复现，本脚本把 README 里"手工下载解压"的三步
固化成一条命令，并用 GitHub Release 的 SHA256 摘要做供应链校验。

用法（项目根目录）::

    python tools/fetch_deps.py            # 缺失才下载，已存在则跳过
    python tools/fetch_deps.py --check    # 只检查，不下载
    python tools/fetch_deps.py --force    # 重新下载并覆盖解压

产物：
    third_party/toolchain/xtensa-esp-elf/.../bin/xtensa-esp-elf-gcc.exe
    third_party/qemu/.../bin/qemu-system-xtensa.exe

注意：当前固定的是 **Windows x64** 构建（与 envlib.py 的 URL 一致）。
Linux/macOS 请用 TUDOU_CC / TUDOU_QEMU 指向本机工具链，或使用 WSL2。
"""

from __future__ import annotations

import argparse
import hashlib
import os
import sys
import tarfile
import urllib.request
import zipfile
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import envlib  # noqa: E402

TOOLCHAIN_SHA256 = "e0eae3964d603d15fca309d2c02af40c25429ced443460b9cb07980b261e5069"
TOOLCHAIN_SIZE = 281165282
QEMU_SHA256 = "3c483d77f5350a568df1faf4d8dbc82c95d6bc2b826d0d4be910485e0a68ca2a"
QEMU_SIZE = 35996720

THIRD_PARTY = Path(envlib.THIRD_PARTY)
DOWNLOAD_DIR = THIRD_PARTY / "_downloads"


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def checked_member(dest: Path, name: str) -> Path:
    """拒绝绝对路径/`..`/盘符成员，再解析到目标目录内（防 zip-slip）。"""
    norm = name.replace("\\", "/")
    parts = Path(norm).parts
    if norm.startswith("/") or ".." in parts or (len(norm) > 1 and norm[1] == ":"):
        raise SystemExit("拒绝解压越界路径: %s" % name)
    root = dest.resolve()
    target = (dest / norm).resolve()
    if target != root and root not in target.parents:
        raise SystemExit("拒绝解压越界路径: %s" % name)
    return target


def safe_zip_extract(archive: Path, dest: Path) -> None:
    dest.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(archive) as zf:
        for info in zf.infolist():
            checked_member(dest, info.filename)
        zf.extractall(dest)


def safe_tar_extract(archive: Path, dest: Path) -> None:
    dest.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive, "r:xz") as tf:
        for member in tf.getmembers():
            checked_member(dest, member.name)
        # Python 3.12+ 的 data 过滤会再检查一次链接/设备节点
        try:
            tf.extractall(dest, filter="data")
        except TypeError:  # pragma: no cover - Python < 3.12
            tf.extractall(dest)


def fetch(name: str, url: str, sha256: str, size: int, dest: Path,
          kind: str, force: bool) -> None:
    DOWNLOAD_DIR.mkdir(parents=True, exist_ok=True)
    archive = DOWNLOAD_DIR / url.rsplit("/", 1)[-1]
    if archive.is_file() and not force:
        actual = sha256_file(archive)
        if actual == sha256 and archive.stat().st_size == size:
            print("[fetch] 复用已下载归档: %s" % archive)
        else:
            archive.unlink()
    if not archive.is_file():
        print("[fetch] 下载 %s (%d bytes) ..." % (name, size))

        def hook(blocks: int, block_size: int, total: int) -> None:
            done = blocks * block_size
            if total > 0 and (blocks % 64 == 0 or done >= total):
                print("  %5.1f%%  %d/%d" % (100.0 * done / total, done, total))

        urllib.request.urlretrieve(url, str(archive), reporthook=hook)  # noqa: S310
    actual_size = archive.stat().st_size
    if actual_size != size:
        raise SystemExit("[fetch] %s 大小不符: %d != %d" % (name, actual_size, size))
    actual = sha256_file(archive)
    if actual != sha256:
        raise SystemExit("[fetch] %s SHA256 不符:\n  got  %s\n  want %s"
                         % (name, actual, sha256))
    print("[fetch] %s SHA256 OK: %s" % (name, actual))
    print("[fetch] 解压到 %s ..." % dest)
    if kind == "zip":
        safe_zip_extract(archive, dest)
    else:
        safe_tar_extract(archive, dest)
    print("[fetch] %s 就绪" % name)


def main() -> int:
    ap = argparse.ArgumentParser(description="下载/校验固定版本工具链与 QEMU")
    ap.add_argument("--check", action="store_true", help="只检查是否可用")
    ap.add_argument("--force", action="store_true", help="重新下载并解压")
    args = ap.parse_args()

    if os.name != "nt":
        print("[fetch] 注意: 固定产物是 Windows x64 构建；本机请改用 "
              "TUDOU_CC/TUDOU_QEMU 指向原生工具链。", file=sys.stderr)

    cc = envlib.find_cc()
    qemu = envlib.find_qemu()
    if args.check:
        print("toolchain:", cc or "MISSING")
        print("qemu     :", qemu or "MISSING")
        return 0 if (cc and qemu) else 1

    if cc and not args.force:
        print("[fetch] 交叉编译器已存在，跳过:", cc)
    else:
        fetch("toolchain", envlib.TOOLCHAIN_URL, TOOLCHAIN_SHA256, TOOLCHAIN_SIZE,
              THIRD_PARTY / "toolchain", "zip", args.force)

    if qemu and not args.force:
        print("[fetch] QEMU 已存在，跳过:", qemu)
    else:
        fetch("qemu", envlib.QEMU_URL, QEMU_SHA256, QEMU_SIZE,
              THIRD_PARTY / "qemu", "tarxz", args.force)

    cc = envlib.find_cc()
    qemu = envlib.find_qemu()
    print("[fetch] 完成: cc=%s qemu=%s" % (cc, qemu))
    return 0 if (cc and qemu) else 1


if __name__ == "__main__":
    sys.exit(main())
