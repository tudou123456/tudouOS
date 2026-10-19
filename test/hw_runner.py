"""tudouOS 真机串口自动回归台。

用法（仓库根 D:\\tudouOS 下，Python 3.14）：::

    py -3.14 -m test.hw_runner --port COM3 --list
    py -3.14 -m test.hw_runner --port COM3 --filter m2
    py -3.14 -m test.hw_runner --port COM3 --smoke
    py -3.14 -m test.hw_runner --port COM3 --keep-going

约定：
- 用例写在 test/hw_cases/test_hw_*.py 里，用 @hw_case("m2/tickstat", timeout=10) 注册；
- 默认每条用例先做一次硬件复位并等 `[POTATO] READY`（reset=False 可关闭）；
- 每条用例的串口完整原始日志写到 test/logs/hw/<case>.log；
- 终端打印 PASS/FAIL/TIMEOUT 汇总表；退出码 0=全过, 1=有失败, 2=台架/串口错误。

真机纪律：COM3 独占。烧录/别的串口工具占用时本工具会以退出码 2 报错，不要重试抢占。
"""

from __future__ import annotations

import argparse
import glob
import importlib.util
import json
import os
import re
import sys
import time
import traceback
from pathlib import Path

from test.hw_serial import HwSerial, HwSerialError, HwTimeoutError

ROOT = Path(__file__).resolve().parents[1]
CASE_PATTERN = str(ROOT / "test" / "hw_cases" / "test_hw_*.py")
LOG_DIR = ROOT / "test" / "logs" / "hw"

DEFAULT_CASE_TIMEOUT = 20.0


class HwSkip(Exception):
    """用例显式跳过（仅限真机无对应能力、且原因必须写清的场景）。

    例：M3.5 `msgdemo heal` 依赖 QEMU QMP `stop/cont` 冻结客户机，真机没有等效
    无损冻结手段（复位会丢链路状态），因此该子项在真机上显式 SKIP 并在报告
    中标注「真机限制」，而不是伪造通过。
    """


# ---------------------------------------------------------------------- 用例注册
class Case:
    def __init__(self, name, func, timeout, reset, smoke, module, source):
        self.name = name
        self.func = func
        self.timeout = timeout
        self.reset = reset
        self.smoke = smoke
        self.module = module
        self.source = source


CASES: list[Case] = []
_SEEN: set[str] = set()


def hw_case(name: str, timeout: float = DEFAULT_CASE_TIMEOUT, reset: bool = True,
            smoke: bool = False):
    """注册一条真机用例。name 形如 "m2/tickstat"，用于 --filter 与日志文件名。"""

    def deco(func):
        if name in _SEEN:
            raise RuntimeError("重复的 hw_case 名称: %s" % name)
        _SEEN.add(name)
        CASES.append(
            Case(
                name=name,
                func=func,
                timeout=float(timeout),
                reset=bool(reset),
                smoke=bool(smoke),
                module=func.__module__,
                source=inspect_source(func),
            )
        )
        return func

    return deco


def inspect_source(func) -> str:
    code = getattr(func, "__code__", None)
    if code is None:
        return "?"
    return "%s:%d" % (os.path.basename(code.co_filename), code.co_firstlineno)


def discover_cases() -> list[Case]:
    """按文件名排序导入 test/hw_cases/test_hw_*.py，注册顺序即执行顺序。"""
    paths = sorted(glob.glob(CASE_PATTERN))
    if not paths:
        return []
    sys.path.insert(0, str(ROOT))
    # 用例文件 `from test.hw_runner import hw_case` 会走模块名导入；若本进程是
    # `py -m test.hw_runner`（__name__ == "__main__"），必须把模块名映射到当前
    # 模块对象，否则装饰器会把用例注册进另一个模块实例的 CASES 里。
    sys.modules.setdefault("test.hw_runner", sys.modules[__name__])
    for path in paths:
        stem = Path(path).stem
        mod_name = "hwcases." + stem
        spec = importlib.util.spec_from_file_location(mod_name, path)
        if spec is None or spec.loader is None:
            raise RuntimeError("无法导入用例文件: %s" % path)
        module = importlib.util.module_from_spec(spec)
        sys.modules[mod_name] = module
        # 用例文件通过 `from test.hw_runner import hw_case` 注册
        spec.loader.exec_module(module)
    return CASES


# ---------------------------------------------------------------------- 用例上下文
class CaseContext:
    """传给每个用例函数的上下文对象（`def my_case(ctx)`）。"""

    def __init__(self, hw: HwSerial, case: Case, timeout_mult: float,
                 start_pos: int = 0):
        self.hw = hw
        self.case = case
        self.timeout_mult = timeout_mult
        #: 本用例开始时的文本游标（复位前的位置；boot 类用例据此回看整段启动日志）
        self.start_pos = start_pos
        self.deadline = time.monotonic() + case.timeout * timeout_mult
        self.checks: list[str] = []

    # -- 基元 ---------------------------------------------------------
    @property
    def serial(self) -> HwSerial:
        return self.hw

    def mark(self) -> int:
        return self.hw.mark()

    def text_since(self, pos: int) -> str:
        return self.hw.text_since(pos)

    def send(self, line: str) -> None:
        self.hw.send(line)

    def send_raw(self, data: bytes) -> None:
        self.hw.send_raw(data)

    def _effective(self, timeout: float | None) -> float:
        remaining = self.deadline - time.monotonic()
        if remaining <= 0:
            raise HwTimeoutError("用例级超时 %.1fs" % (self.case.timeout * self.timeout_mult),
                                 self.case.timeout * self.timeout_mult,
                                 self.hw.latest(30))
        want = 3.0 * self.timeout_mult if timeout is None else timeout * self.timeout_mult
        return max(0.05, min(want, remaining))

    def expect(self, pattern, timeout: float | None = None, pos: int | None = None):
        return self.hw.expect(pattern, self._effective(timeout), pos=pos)

    def wait_line(self, pattern, timeout: float | None = None) -> str:
        m = self.expect(pattern, timeout)
        text = self.hw.text()
        start = text.rfind("\n", 0, m.start()) + 1
        end = text.find("\n", m.end())
        return text[start:] if end < 0 else text[start:end]

    def cmd(self, command: str, pattern, timeout: float | None = None):
        """发送命令并从发送位置等锚点（最常用）。"""
        return self.hw.send_line_and_wait(command, pattern, self._effective(timeout))

    def cmd_line(self, command: str, pattern, timeout: float | None = None) -> str:
        m = self.cmd(command, pattern, timeout)
        text = self.hw.text()
        start = text.rfind("\n", 0, m.start()) + 1
        end = text.find("\n", m.end())
        return text[start:] if end < 0 else text[start:end]

    # -- 便捷断言 -----------------------------------------------------
    def check(self, pattern, what: str, timeout: float | None = None):
        """等待锚点；失败时给出可读断言信息。返回 re.Match。"""
        m = self.expect(pattern, timeout)
        self.checks.append(what)
        return m

    def check_line(self, pattern, what: str, timeout: float | None = None) -> str:
        line = self.wait_line(pattern, timeout)
        self.checks.append(what)
        return line

    def reboot(self, ready_timeout: float | None = None):
        """板级复位并等 READY（crash/SMP 用例内使用）。"""
        rt = (ready_timeout if ready_timeout is not None else 8.0) * self.timeout_mult
        self.hw.reset(ready_timeout=rt)

    def released(self, reset_after: bool = True):
        """让出 COM3 给子进程（M7 tools/serial_mcp.py 用例）。

        reset_after=False 用于「子进程自己 --reset 复位板子」的用例，避免退出后
        再复位一次干扰子进程刚建立的状态。
        """
        return self.hw.released(reset_after=reset_after)


# ---------------------------------------------------------------------- 用例执行
class CaseResult:
    def __init__(self, case: Case):
        self.case = case
        self.status = "FAIL"
        self.duration = 0.0
        self.detail = ""
        self.log_path = ""
        self.checks: list[str] = []

    def row(self) -> tuple:
        return (self.case.name, "%.2fs" % self.duration, self.status, self.detail)


def safe_name(name: str) -> str:
    return re.sub(r"[^0-9A-Za-z_.-]+", "_", name)


def run_case(hw: HwSerial, case: Case, args, build_info: str) -> CaseResult:
    res = CaseResult(case)
    start = time.monotonic()
    pos_raw = len(hw.raw_text())
    pos_text = hw.mark()

    if case.reset and not args.no_reset:
        try:
            hw.reset(ready_timeout=8.0 * args.timeout_mult)
        except HwTimeoutError as exc:
            res.duration = time.monotonic() - start
            res.status = "FAIL"
            res.detail = "复位后未等到 READY: %s" % _brief(exc)
            _write_case_log(hw, case, res, build_info, pos_raw, pos_text)
            return res
        except HwSerialError as exc:
            res.duration = time.monotonic() - start
            res.status = "FAIL"
            res.detail = "复位失败: %s" % _brief(exc)
            _write_case_log(hw, case, res, build_info, pos_raw, pos_text)
            return res

    # 用例级 deadline 在复位完成之后才开始计（复位+启动不占用用例预算）
    # start_pos = 复位前的文本游标：boot 类用例可回看启动日志；no_crash 等检查
    # 只覆盖本用例范围（否则 m2/exception 用例的预期 [EXC] 转储会污染后续用例）
    ctx = CaseContext(hw, case, args.timeout_mult, pos_text)
    status = "PASS"
    detail = ""
    try:
        case.func(ctx)
    except HwSkip as exc:
        status = "SKIP"
        detail = "SKIP 原因: %s" % _brief(exc)
    except HwTimeoutError as exc:
        status = "TIMEOUT"
        detail = "TIMEOUT 锚点 %r（%.1fs）" % (_pattern_of(exc), exc.timeout)
    except AssertionError as exc:
        status = "FAIL"
        detail = _brief(exc) or "断言失败"
    except HwSerialError as exc:
        status = "FAIL"
        detail = "串口错误: %s" % _brief(exc)
    except Exception as exc:  # 用例自身 bug 也要算失败并留痕
        status = "FAIL"
        detail = "异常 %s: %s" % (type(exc).__name__, _brief(exc))
        traceback.print_exc()

    res.duration = time.monotonic() - start
    res.status = status
    res.detail = detail.replace("\n", " ")[:160]
    res.checks = ctx.checks
    _write_case_log(hw, case, res, build_info, pos_raw, pos_text)
    return res


def _pattern_of(exc: HwTimeoutError) -> str:
    p = getattr(exc, "pattern", "")
    if hasattr(p, "pattern"):
        p = p.pattern
    return str(p)


def _brief(exc: BaseException) -> str:
    line = str(exc).strip().splitlines()
    return line[0] if line else ""


def _write_case_log(hw, case, res, build_info, pos_raw, pos_text):
    """把该用例期间的完整原始串口日志落盘（含 @@MCP 帧与提示符噪声）。"""
    try:
        LOG_DIR.mkdir(parents=True, exist_ok=True)
        path = LOG_DIR / (safe_name(case.name) + ".log")
        raw = hw.raw_text()
        body = raw[pos_raw:]
        with open(path, "w", encoding="utf-8", newline="\n") as fh:
            fh.write("# case: %s\n" % case.name)
            fh.write("# status: %s  duration: %.2fs  source: %s\n"
                     % (res.status, res.duration, case.source))
            if res.detail:
                fh.write("# detail: %s\n" % res.detail)
            if getattr(res, "checks", None):
                fh.write("# passed checks: %s\n" % "; ".join(res.checks))
            if build_info:
                fh.write("# firmware: %s\n" % build_info)
            fh.write("# ---- raw serial transcript ----\n")
            fh.write(body)
            if not body.endswith("\n"):
                fh.write("\n")
        res.log_path = str(path)
    except OSError as exc:
        print("  [warn] 写用例日志失败: %s" % exc)


# ---------------------------------------------------------------------- 主流程
def parse_args(argv=None):
    ap = argparse.ArgumentParser(description="tudouOS 真机串口自动回归台")
    ap.add_argument("--port", default=os.environ.get("POTATO_HW_PORT", "COM3"),
                    help="串口（默认 COM3，可用环境变量 POTATO_HW_PORT）")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--filter", default="", help="用例名子串过滤（如 m2、m7）")
    ap.add_argument("--list", action="store_true", help="只列出用例，不碰串口")
    ap.add_argument("--smoke", action="store_true", help="只跑 smoke 标记的冒烟用例")
    ap.add_argument("--no-reset", action="store_true", help="不做复位（板子已在跑）")
    ap.add_argument("--timeout-mult", type=float, default=1.0, help="所有超时倍率")
    ap.add_argument("--keep-going", action="store_true", help="失败后继续跑后续用例")
    ap.add_argument("--dry-run", action="store_true",
                    help="只发现/校验用例（不打开串口），用于 CI/等待板子时自检")
    return ap.parse_args(argv)


def main(argv=None) -> int:
    for stream in (sys.stdout, sys.stderr):
        with_error = getattr(stream, "reconfigure", None)
        if with_error:
            try:
                stream.reconfigure(encoding="utf-8", errors="replace")
            except Exception:
                pass

    args = parse_args(argv)
    if args.timeout_mult <= 0:
        print("[错误] --timeout-mult 必须 > 0")
        return 2

    try:
        cases = discover_cases()
    except Exception as exc:
        print("[错误] 用例发现失败: %r" % (exc,))
        traceback.print_exc()
        return 2

    selected = [c for c in cases if args.filter in c.name]
    if args.smoke:
        selected = [c for c in selected if c.smoke]
        if not selected:
            print("[错误] 没有 smoke 用例（--smoke 需要用例带 smoke=True）")
            return 2

    if args.list:
        print("发现 %d 条真机用例（过滤后 %d 条）：" % (len(cases), len(selected)))
        for c in selected:
            flags = []
            if c.reset:
                flags.append("reset")
            else:
                flags.append("no-reset")
            if c.smoke:
                flags.append("smoke")
            print("  %-34s timeout=%-5.1f %-14s %s" % (c.name, c.timeout, ",".join(flags), c.source))
        return 0

    if args.dry_run:
        return _dry_run(selected)

    if not selected:
        print("[错误] 过滤 %r 后没有可执行用例" % args.filter)
        return 2

    print("=" * 96)
    print("tudouOS 真机串口自动回归台  port=%s baud=%d  cases=%d  reset=%s  timeout_mult=%.1f"
          % (args.port, args.baud, len(selected), "no" if args.no_reset else "per-case",
             args.timeout_mult))
    print("=" * 96)

    hw = HwSerial(args.port, args.baud)
    try:
        hw.open()
    except HwSerialError as exc:
        print("[台架错误] %s" % exc)
        return 2

    build_info = ""
    results: list[CaseResult] = []
    try:
        if not args.no_reset:
            print("初始复位 + 等 READY ...", end="", flush=True)
            try:
                hw.reset(ready_timeout=8.0 * args.timeout_mult)
                print(" OK")
            except (HwTimeoutError, HwSerialError) as exc:
                print(" 失败: %s" % _brief(exc))
                print("[台架错误] 板子未进入 READY，检查 COM3 连线/固件/是否被占用")
                return 2
        m = re.search(r"\[POTATO\] build ([^\r\n]+)", hw.text())
        if m:
            build_info = m.group(1).strip()
            print("固件 build: %s" % build_info)
        else:
            print("[warn] 没抓到 build 行（--no-reset 时正常）")

        for idx, case in enumerate(selected, 1):
            print("[%2d/%2d] %-34s " % (idx, len(selected), case.name), end="", flush=True)
            res = run_case(hw, case, args, build_info)
            results.append(res)
            mark = {"PASS": "PASS", "FAIL": "FAIL", "TIMEOUT": "TIMEOUT",
                    "SKIP": "SKIP"}.get(res.status, "FAIL")
            print("%-7s %6.2fs  %s" % (mark, res.duration, res.detail))
            if res.status in ("FAIL", "TIMEOUT") and not args.keep_going:
                print("  [提前停止] 失败后默认停止；加 --keep-going 可继续跑")
                break
    finally:
        _write_session_log(hw, results, build_info)
        hw.close()

    # ---- 汇总表 ----
    print()
    print("=" * 96)
    print("汇总（%d/%d 已执行）" % (len(results), len(selected)))
    print("-" * 96)
    width = max([len(r.case.name) for r in results] + [8])
    print("%-*s  %8s  %-7s  %s" % (width, "CASE", "SEC", "STATUS", "DETAIL"))
    for r in results:
        print("%-*s  %8.2f  %-7s  %s" % (width, r.case.name, r.duration, r.status, r.detail))
    passed = sum(1 for r in results if r.status == "PASS")
    failed = sum(1 for r in results if r.status == "FAIL")
    timedout = sum(1 for r in results if r.status == "TIMEOUT")
    skipped = sum(1 for r in results if r.status == "SKIP")
    print("-" * 96)
    print("PASS=%d FAIL=%d TIMEOUT=%d SKIP=%d  未执行=%d"
          % (passed, failed, timedout, skipped, len(selected) - len(results)))
    if results and failed == 0 and timedout == 0 and len(results) == len(selected):
        if skipped:
            print("结论: 本批通过（%d 条显式 SKIP，原因见上表/日志，属真机限制）" % skipped)
        else:
            print("结论: 本批全部通过")
        return 0
    print("结论: 存在失败/超时/未执行用例，详见 test/logs/hw/*.log 与 _session.log")
    return 1


def _dry_run(selected: list[Case]) -> int:
    """不碰串口：校验用例可调用性/签名/名称唯一，并打印执行计划。"""
    import inspect

    ok = True
    seen = set()
    print("dry-run: 校验 %d 条用例（不打开串口）" % len(selected))
    for c in selected:
        if c.name in seen:
            print("  [错误] 重复用例名: %s" % c.name)
            ok = False
            continue
        seen.add(c.name)
        if not callable(c.func):
            print("  [错误] %s 不可调用" % c.name)
            ok = False
            continue
        try:
            sig = inspect.signature(c.func)
            params = [p for p in sig.parameters.values()
                      if p.kind in (p.POSITIONAL_ONLY, p.POSITIONAL_OR_KEYWORD)]
        except (TypeError, ValueError):
            params = [None]
        if len(params) != 1:
            print("  [错误] %s 签名应为 (ctx)，实际 %s" % (c.name, sig if params else "?"))
            ok = False
            continue
        if c.timeout <= 0:
            print("  [错误] %s timeout 必须 > 0" % c.name)
            ok = False
    if not ok:
        return 2
    by_milestone: dict[str, int] = {}
    for c in selected:
        key = c.name.split("/")[0]
        by_milestone[key] = by_milestone.get(key, 0) + 1
    print("  OK：" + "  ".join("%s=%d" % (k, by_milestone[k])
                               for k in sorted(by_milestone)))
    print("  复位用例=%d  no-reset=%d  smoke=%d"
          % (sum(1 for c in selected if c.reset),
             sum(1 for c in selected if not c.reset),
             sum(1 for c in selected if c.smoke)))
    return 0


def _write_session_log(hw, results, build_info):
    try:
        LOG_DIR.mkdir(parents=True, exist_ok=True)
        path = LOG_DIR / "_session.log"
        with open(path, "w", encoding="utf-8", newline="\n") as fh:
            fh.write("# tudouOS 真机回归会话 %s\n" % time.strftime("%Y-%m-%d %H:%M:%S"))
            fh.write("# firmware: %s\n" % (build_info or "unknown"))
            for r in results:
                fh.write("# result: %-34s %-7s %6.2fs %s\n"
                         % (r.case.name, r.status, r.duration, r.detail))
            fh.write("# ---- raw serial transcript（本次会话全量）----\n")
            fh.write(hw.raw_text())
            if not hw.raw_text().endswith("\n"):
                fh.write("\n")
        # 机器可读汇报（task-5 汇总用）
        with open(LOG_DIR / "results.json", "w", encoding="utf-8") as fh:
            json.dump(
                {
                    "time": time.strftime("%Y-%m-%d %H:%M:%S"),
                    "firmware": build_info,
                    "port": hw.port,
                    "results": [
                        {
                            "case": r.case.name,
                            "status": r.status,
                            "duration_s": round(r.duration, 3),
                            "detail": r.detail,
                            "log": r.log_path,
                            "source": r.case.source,
                        }
                        for r in results
                    ],
                },
                fh,
                ensure_ascii=False,
                indent=2,
            )
    except OSError as exc:
        print("[warn] 写会话日志失败: %s" % exc)


if __name__ == "__main__":
    sys.exit(main())
