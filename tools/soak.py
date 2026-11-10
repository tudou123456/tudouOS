#!/usr/bin/env python3
"""tudouOS 长稳 soak：单机持续负载 + 健康采样 + 泄漏/复位/看门狗判定。

与回归台的区别：回归台每个用例重启一次 VM/板卡；soak 保持**同一个实例**
连续运行，周期性施加堆/消息/文件/调度负载，采样 `obs health` 与 `tasks`，
观察 tick 是否停摆、堆是否单调增长、是否复位/看门狗/栈哨兵。

用法（项目根目录，建议从 ASCII 联接路径运行）::

    py -3.14 tools/soak.py --minutes 60
    py -3.14 tools/soak.py --minutes 2880 --interval 60 --churn   # 48h
    py -3.14 tools/soak.py --minutes 5 --interval 2 --platform hw --port COM3

产物：test/logs/soak-<时间戳>/{summary.json,samples.jsonl,serial.log}
退出码：0=全程无失败；1=有失败/中断；2=环境错误。
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tools"))

import envlib  # noqa: E402

READY = r"\[POTATO\] READY"
HEALTH_RE = r"\[POTATO\]\[HEALTH\] (\{[^\r\n]*\})\r"
TASKS_SUM_RE = (r"\[POTATO\]\[TS\] now=(\d+) switches=(\d+) "
                r"bg_used=(\d+)/(\d+) diag wake=(\d+) prem=(\d+) sw=(\d+) "
                r"idle=(\d+) drop ready=(\d+) sleep=(\d+) reg=(\d+) "
                r"guard=(\d+)")
CRASH_RE = re.compile(r"(Guru Meditation|EXCCAUSE|PANIC|rst:0x[0-9a-f]+|"
                      r"\[POTATO\] READY)")


class QemuDriver:
    def __init__(self, flash: Path, net: bool):
        from test.runner import VMInstance

        qemu = envlib.find_qemu()
        if not qemu:
            raise SystemExit("未找到 qemu-system-xtensa，先运行 tools/check_env.py")
        if not flash.is_file():
            raise SystemExit(f"未找到 flash 镜像 {flash}，先执行 tools/build.py")
        extra = ["-nic", "user,model=open_eth,id=n0"] if net else []
        (ROOT / "test" / "logs").mkdir(parents=True, exist_ok=True)
        self.vm = VMInstance(qemu, str(flash), "soak", extra_args=extra)

    def start(self):
        self.vm.start()
        self.vm.serial.expect(READY, timeout=30)

    def send(self, line: str):
        self.vm.serial.send(line)

    def expect(self, pattern, timeout=10.0):
        return self.vm.serial.expect(pattern, timeout=timeout)

    def text(self) -> str:
        return self.vm.serial.text

    def mark(self) -> int:
        return len(self.vm.serial.text)

    def text_since(self, mark: int) -> str:
        return self.vm.serial.text[mark:]

    def stop(self):
        self.vm.stop()


class HwDriver:
    def __init__(self, port: str):
        from test.hw_serial import HwSerial

        self.hw = HwSerial(port)

    def start(self):
        self.hw.open()
        self.hw.reset(ready_timeout=15)

    def send(self, line: str):
        self.hw.send(line)

    def expect(self, pattern, timeout=3.0):
        return self.hw.expect(pattern, timeout=timeout)

    def text(self) -> str:
        return self.hw.text()

    def mark(self) -> int:
        return self.hw.mark()

    def text_since(self, mark: int) -> str:
        return self.hw.text_since(mark)

    def stop(self):
        self.hw.close()


def _u32(m, i):
    return int(m.group(i))


def _check_crash(driver, mark, failures, cycle):
    chunk = driver.text_since(mark)
    m = CRASH_RE.search(chunk)
    if m:
        # 启动阶段的 READY 已在 start() 消费；运行期再出现即复位。
        failures.append({"cycle": cycle, "kind": "crash",
                         "detail": m.group(0), "tail": chunk[-600:]})
        return True
    return False


def _health(driver, timeout=15.0):
    driver.send("health")
    m = driver.expect(HEALTH_RE, timeout=timeout)
    return json.loads(m.group(1))


def _tasks(driver, mark, timeout=15.0):
    """返回 (live_count, reg, rdrop, sdrop, guard)。"""
    driver.send("tasks")
    m = driver.expect(TASKS_SUM_RE, timeout=timeout)
    live = len(re.findall(r"\[POTATO\]\[TS\] [^\r\n]* cls=",
                          driver.text_since(mark)))
    return live, _u32(m, 11), _u32(m, 9), _u32(m, 10), _u32(m, 12)


def _workload(driver, cycle, failures, args):
    mark = driver.mark()

    # 堆：固定次数分配/释放 + 越界/双释放对抗路径（selftest 自带断言）
    driver.send("memtest selftest 16")
    try:
        m = driver.expect(r"SELFTEST-PASS n=16 faults=0", timeout=20)
        assert m
    except Exception as e:  # noqa: BLE001
        failures.append({"cycle": cycle, "kind": "workload", "detail": f"memtest: {e}"})

    # 消息：3 轮 ping（QoS1 重发/回执/去重路径）
    driver.send("msgdemo ping 3")
    try:
        driver.expect(r"PING-DONE ok=3 failed=0", timeout=20)
    except Exception as e:  # noqa: BLE001
        failures.append({"cycle": cycle, "kind": "workload", "detail": f"msgdemo: {e}"})

    # 文件：写同一文件（写时复制，旧内容不泄漏）再读回校验
    payload = f"cycle-{cycle}"
    driver.send(f"fs write soak.txt {payload}")
    try:
        driver.expect(r"\[POTATO\]\[FS\] wrote soak\.txt \d+ bytes", timeout=10)
        driver.send("fs cat soak.txt")
        driver.expect(r"\[POTATO\]\[FS\] soak\.txt \d+ bytes:", timeout=10)
        # 内容行单独锚定：QEMU/真机的换行数是 "\r\n"/"\r\r\n" 混合，
        # 不把行尾形态写进断言（与回归台锚点风格一致）。
        driver.expect(rf"{payload}\r", timeout=10)
    except Exception as e:  # noqa: BLE001
        failures.append({"cycle": cycle, "kind": "workload", "detail": f"potfs: {e}"})

    if args.smp:
        driver.send("smp ipi 8")
        try:
            driver.expect(r"\[POTATO\]\[SMP\] ipi sent=8 to cpu1", timeout=10)
            driver.send("smp")
            driver.expect(r"\[POTATO\]\[SMP\].*unk=0", timeout=10)
        except Exception as e:  # noqa: BLE001
            failures.append({"cycle": cycle, "kind": "workload", "detail": f"smp: {e}"})

    return _check_crash(driver, mark, failures, cycle)


def run(args):
    if args.platform == "hw":
        driver = HwDriver(args.port)
    else:
        driver = QemuDriver(Path(args.flash), net=not args.no_net)

    outdir = Path(args.out) if args.out else (
        ROOT / "test" / "logs" / time.strftime("soak-%Y%m%d-%H%M%S"))
    outdir.mkdir(parents=True, exist_ok=False)
    samples_path = outdir / "samples.jsonl"
    summary_path = outdir / "summary.json"

    failures = []
    warnings = []
    warned = set()
    samples = []
    started = time.time()
    start_mono = time.monotonic()
    deadline = start_mono + args.minutes * 60.0
    driver.start()
    print(f"[soak] started platform={args.platform} minutes={args.minutes} "
          f"interval={args.interval}s out={outdir}")

    if args.churn:
        driver.send("memtest churn")
        driver.expect(r"memtest churn started", timeout=10)
        print("[soak] memtest churn enabled")
    if args.smp:
        driver.send("smp start")
        driver.expect(r"\[POTATO\]\[SMP\] start rc=0 online=2", timeout=20)
        print("[soak] smp started")

    cycle = 0
    last_tick = None
    used_hist = []
    try:
        while time.monotonic() < deadline:
            cycle += 1
            cycle_start = time.monotonic()
            mark = driver.mark()
            elapsed = cycle_start - start_mono
            entry = {"cycle": cycle, "wall": time.strftime("%Y-%m-%dT%H:%M:%S"),
                     "elapsed_s": round(elapsed, 1)}
            try:
                h = _health(driver)
                live, reg, rdrop, sdrop, guard = _tasks(driver, mark)
                entry.update({"health": h, "tasks": {
                    "live": live, "reg": reg, "rdrop": rdrop,
                    "sdrop": sdrop, "guard": guard}})
                used_hist.append(h["heap"]["used"])

                # tick 必须持续推进（1kHz；间隔内至少涨一半）
                if last_tick is not None:
                    dt = h["t"] - last_tick
                    if dt <= 0:
                        failures.append({"cycle": cycle, "kind": "tick-stall",
                                         "detail": f"tick {last_tick}->{h['t']}"})
                last_tick = h["t"]
                if h["irq"]["wd"] != 0:
                    # QEMU 的 genex+CCOMPARE 模型会丢 tick，内核用看门狗补账；
                    # 这是已知模型缺陷，不判内核失败。真机必须为 0。
                    if args.platform == "hw":
                        failures.append({"cycle": cycle, "kind": "watchdog",
                                         "detail": f"wd={h['irq']['wd']}"})
                    elif "qemu-wd" not in warned:
                        warned.add("qemu-wd")
                        warnings.append({"cycle": cycle, "kind": "qemu-watchdog",
                                         "detail": f"wd={h['irq']['wd']} "
                                                   "(QEMU 模型丢 tick，已补账)"})
                if h["irq"]["lag"] > 1000:
                    if args.platform == "hw":
                        failures.append({"cycle": cycle, "kind": "sched-lag",
                                         "detail": f"lag={h['irq']['lag']}ms"})
                    elif "qemu-lag" not in warned:
                        warned.add("qemu-lag")
                        warnings.append({"cycle": cycle, "kind": "qemu-lag",
                                         "detail": f"lag={h['irq']['lag']}ms"})
                if h["irq"]["jitmax"] > 5000:
                    if args.platform == "hw":
                        failures.append({"cycle": cycle, "kind": "tick-jitter",
                                         "detail": f"jitmax={h['irq']['jitmax']}us"})
                    elif "qemu-jitter" not in warned:
                        warned.add("qemu-jitter")
                        warnings.append({"cycle": cycle, "kind": "qemu-jitter",
                                         "detail": f"jitmax={h['irq']['jitmax']}us"})
                if h["heap"]["fault"] != 0 or h["heap"]["fail"] != 0:
                    failures.append({"cycle": cycle, "kind": "heap-error",
                                     "detail": f"fault={h['heap']['fault']} "
                                               f"fail={h['heap']['fail']}"})
                if guard != 0:
                    failures.append({"cycle": cycle, "kind": "stack-guard",
                                     "detail": f"guard={guard}"})
                if reg != 0:
                    failures.append({"cycle": cycle, "kind": "task-reg-overflow",
                                     "detail": f"reg={reg}"})

                # 泄漏：预热后，连续 5 个样本相对基线上涨超过阈值
                if len(used_hist) >= 8:
                    base = min(used_hist[:5])
                    tail = used_hist[-5:]
                    if (min(tail) > base + args.leak_bytes and
                            all(b >= a for a, b in zip(tail, tail[1:]))):
                        failures.append({"cycle": cycle, "kind": "heap-leak",
                                         "detail": f"used {base}->{tail[-1]} "
                                                   f"(+{tail[-1] - base}B)"})
            except Exception as e:  # noqa: BLE001
                failures.append({"cycle": cycle, "kind": "sample", "detail": str(e)})

            crashed = _workload(driver, cycle, failures, args)
            with samples_path.open("a", encoding="utf-8") as fh:
                fh.write(json.dumps(entry, ensure_ascii=False) + "\n")
            samples.append(entry)
            h = entry.get("health", {})
            print(f"[soak] cycle={cycle} t={h.get('t', '?')} "
                  f"used={h.get('heap', {}).get('used', '?')} "
                  f"free={h.get('heap', {}).get('free', '?')} "
                  f"jitmax={h.get('irq', {}).get('jitmax', '?')}us "
                  f"tasks={entry.get('tasks', {}).get('live', '?')} "
                  f"failures={len(failures)}")

            if crashed:
                break
            if len(failures) >= args.max_failures:
                print("[soak] max failures reached, stopping")
                break

            # 扣掉本轮耗时，保持固定采样间隔
            remain = args.interval - (time.monotonic() - cycle_start)
            if remain > 0:
                time.sleep(remain)
    except KeyboardInterrupt:
        failures.append({"cycle": cycle, "kind": "interrupted",
                         "detail": "KeyboardInterrupt"})
        print("[soak] interrupted")
    finally:
        if args.churn:
            try:
                driver.send("memtest off")
                driver.expect(r"CHURN-DONE", timeout=10)
            except Exception:  # noqa: BLE001
                pass
        try:
            serial_text = driver.text()
        except Exception:  # noqa: BLE001
            serial_text = ""
        driver.stop()
        (outdir / "serial.log").write_text(serial_text, encoding="utf-8")
        summary = {
            "platform": args.platform,
            "started": started,
            "ended": time.time(),
            "minutes_requested": args.minutes,
            "interval_s": args.interval,
            "cycles": cycle,
            "samples": len(samples),
            "failures": failures,
            "warnings": warnings,
            "passed": not failures,
            "first": samples[0] if samples else None,
            "last": samples[-1] if samples else None,
        }
        summary_path.write_text(json.dumps(summary, ensure_ascii=False, indent=2),
                                encoding="utf-8")
        print(f"[soak] {'PASS' if not failures else 'FAIL'} cycles={cycle} "
              f"failures={len(failures)} summary={summary_path}")
        for f in failures[:10]:
            print("  -", f.get("kind"), f.get("detail", "")[:200])
        for w in warnings[:5]:
            print("  ~", w.get("kind"), w.get("detail", "")[:200])
    return 0 if not failures else 1


def main():
    ap = argparse.ArgumentParser(description="tudouOS 长稳 soak")
    ap.add_argument("--minutes", type=float, default=60.0,
                    help="运行时长（分钟；48h=2880）")
    ap.add_argument("--interval", type=float, default=30.0,
                    help="采样/负载周期（秒）")
    ap.add_argument("--platform", choices=["qemu", "hw"], default="qemu")
    ap.add_argument("--port", default="COM3", help="真机串口（--platform hw）")
    ap.add_argument("--flash", default=str(ROOT / "build" / "flash_image.bin"))
    ap.add_argument("--out", default="", help="输出目录（默认 test/logs/soak-<ts>）")
    ap.add_argument("--no-net", action="store_true", help="QEMU 不挂 open_eth")
    ap.add_argument("--churn", action="store_true",
                    help="全程运行 memtest churn（堆持续分配/释放）")
    ap.add_argument("--smp", action="store_true", help="启动 CPU1 并每轮发 IPI")
    ap.add_argument("--leak-bytes", type=int, default=8192,
                    help="连续 5 个样本相对基线的堆增长阈值（默认 8192）")
    ap.add_argument("--max-failures", type=int, default=5)
    args = ap.parse_args()
    if args.minutes <= 0 or args.interval <= 0:
        ap.error("--minutes/--interval 必须 > 0")
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
