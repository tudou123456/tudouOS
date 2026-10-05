"""tudouOS 环境体检：交叉编译器 / esptool / QEMU 三件套。

用法:
  python tools/check_env.py    # 检查并打印报告

工具链与 QEMU 的下载地址会在缺失时打印出来；解压到 third_party/ 下任意
子目录即可被自动发现（也可用环境变量 TUDOU_CC / TUDOU_QEMU 直接指定）。
"""

import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import envlib  # noqa: E402

OK, MISS = "[OK]  ", "[MISS]"


def main():
    print("tudouOS 环境体检")
    print("-" * 60)
    failures = 0

    # 0. 路径体检：binutils 不支持非 ASCII 路径，中文路径需走目录联接
    try:
        envlib.PROJECT_ROOT.encode("ascii")
    except UnicodeEncodeError:
        print("[注意] 项目路径含非 ASCII 字符，构建请通过 ASCII 联接路径进行，")
        print("        例如已创建的 D:\\tudouOS（见 README 常见问题）。")

    # 1. 交叉编译器
    cc = envlib.find_cc()
    if cc:
        try:
            ver = subprocess.run([cc, "--version"], capture_output=True, text=True,
                                 timeout=30).stdout.splitlines()[0]
            print(f"{OK} 交叉编译器: {cc}")
            print(f"      {ver}")
        except Exception as e:  # noqa: BLE001
            print(f"{OK} 交叉编译器: {cc} (执行失败: {e})")
    else:
        failures += 1
        print(f"{MISS} 交叉编译器 xtensa-esp-elf-gcc 未找到")
        print(f"      下载: {envlib.TOOLCHAIN_URL}")
        print(f"      解压到 third_party/toolchain/ 下即可（或设置 TUDOU_CC）")

    # 2. esptool
    if envlib.esptool_module():
        import esptool
        print(f"{OK} esptool: {esptool.__version__} (python 模块)")
    else:
        failures += 1
        print(f"{MISS} esptool 未安装，请手动执行: python -m pip install esptool")

    # 3. QEMU
    qemu = envlib.find_qemu()
    if qemu:
        supports = False
        try:
            r = subprocess.run([qemu, "-machine", "help"], capture_output=True,
                               text=True, timeout=30)
            supports = "esp32s3" in (r.stdout + r.stderr)
        except Exception:  # noqa: BLE001
            pass
        print(f"{OK} QEMU: {qemu}" + ("" if supports else "  (警告: 未见 esp32s3 机器!)"))
    else:
        failures += 1
        print(f"{MISS} qemu-system-xtensa 未找到")
        print(f"      下载: {envlib.QEMU_URL}")
        print(f"      解压到 third_party/qemu/ 下即可（或设置 TUDOU_QEMU）")

    print("-" * 60)
    if failures == 0:
        print("环境就绪，可以执行: python tools/build.py")
    else:
        print(f"还有 {failures} 项缺失，按上面提示补齐后重试。")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
