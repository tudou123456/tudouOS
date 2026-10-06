# QMP 研发检验测试台

测试台的目标：把"研发检验"变成一条命令——任何内核改动都能在 QEMU 里
自动验证，不依赖人工看串口。

## 架构

```
python -m test.runner
   │  每个用例拉起一台专属 QEMU（-S 冻结启动）
   ├── QMP  tcp:127.0.0.1:<动态端口>   控制面（复位/暂停/恢复/查状态）
   └── UART tcp:127.0.0.1:<动态端口>   数据面（采集内核输出、注入命令）
        │ 连接就绪后才 cont 放行 CPU，启动日志零丢失
        ▼
   qemu-system-xtensa -machine esp32s3 -drive file=build/flash_image.bin,...
```

- `test/qmp_client.py`：QMP 客户端（greeting → `qmp_capabilities` →
  `command()`；异步事件缓存于 `events` 列表）。
- `test/serial_tap.py`：串口采集器（后台线程收字节；`expect(正则, 超时)`
  顺序匹配并推进位置，避免误匹配旧输出）。
- `test/runner.py`：运行器（用例发现、VM 生命周期、日志落盘、汇总）。
- `test/cases/test_boot.py`：M1 验收用例（boot_banner / ping_pong /
  qmp_reset / qmp_stop_cont）。

## 常用命令

```bash
python -m test.runner                 # 全部用例
python -m test.runner --filter reset  # 按名过滤
python -m test.runner --list          # 列出用例
```

每个用例产生四份日志（test/logs/）：`*.serial.log`（串口全记录）、
`*.qemu.log`（QEMU 自身输出）、`*.qmp-events.json`（RESET/SHUTDOWN 等事件）、
失败时附 `*.error.log`（断言栈）。

## 写新用例

在 `test/cases/` 新建 `test_*.py`，模块级 `test_` 开头函数即用例：

```python
def test_my_feature(ctx):
    ctx.serial.expect(r"\[POTATO\] READY", timeout=20)
    ctx.serial.send("mycommand")
    ctx.serial.expect(r"expected output", timeout=5)
    st = ctx.qmp.command("query-status")   # 任意 QMP 命令
    assert st["status"] == "running"
```

## QMP 能力速查（本测试台可用）

| 命令 | 用途 |
| --- | --- |
| `query-status` | running/paused 状态断言 |
| `system_reset` | 整机复位（重启可靠性、掉电模拟） |
| `stop` / `cont` | 冻结/恢复 CPU（时序注入、暂停期静默断言） |
| `quit` | 干净退出 QEMU |
| `human-monitor-command` | 透传 HMP：`info registers`、`x/8i $pc`、`gdbserver tcp::1234` 等 |

## 内核跑挂了怎么查

1. 看该用例的 `*.serial.log` 尾部（最后输出到哪条日志，定位挂点）。
2. 交互复现：`python qemu/run_qemu.py`（串口直连终端）。
3. 上 GDB：`python qemu/run_qemu.py --gdb`，另开
   `xtensa-esp32s3-elf-gdb build/kernel.elf` → `target remote :1234` → `c`。
4. 无 GDB 时可用 QMP 透传 HMP 拿寄存器现场：
   `ctx.qmp.command("human-monitor-command",
                   {"command-line": "info registers"})`。

## 已知问题（乐鑫 QEMU Windows 构建 esp-develop-9.2.2-20260417）

调试 M2 时实测撞到的坑，内核已按此适配（相应代码有注释）：

| 现象 | 规避 |
| --- | --- |
| TIMG0/SYSTIMER 计数器一启用（写 T0_CONFIG 置 EN，与中断无关）QEMU 进程约 1s 内崩溃（0xC0000409） | M2 时钟方案改用核内 CCOMPARE0 tick + ccount 时基；TIMG/SYSTIMER 驱动保留（`POT_TIME_TIMG=1`）供真机/WSL Linux QEMU |
| PS.INTLEVEL=15 期间置起设备中断源会使 QEMU 陷入活锁（monitor 都无响应） | 初始化顺序纪律：先开全局中断（INTENABLE 为空时安全）再逐级使能设备源 |
| ccount 按翻译块大步推进，CCOMPARE 精确相等点被整段跳过（实测触发率 ~53%） | tick 处理按跨过的周期数补记，tick 计数与真实时间对齐（983Hz@1kHz 目标）；jitter 统计如实记录触发间隔 |
| 通用异常（如非法指令）不回填 EPC1/EPS1/EXCVADDR（level-1 中断却会回填） | 异常现场打印保留字段，测试只断言 EXCCAUSE 与恢复语义；真机上字段有值 |
| 高频轮询 MMIO（如标定循环锤 T0_UPDATE）不可取 | 时序热点避免 MMIO 轮询，用 ccount |

另有两坑在构建侧（已固化进 build.py/链接脚本，详见代码注释）：
- Xtensa `l32r` 字面量必须在指令之前且 4 字节对齐——链接脚本把 `.literal`
  集中放 `.text` 之前，并全程 `-Wl,--no-relax`（开了 relax 会被重排成坏码）。
- 统一工具链 C 文件用 `-mauto-litpools`；汇编文件不走长调用（内核尚小，
  直跳 + 无字面量最稳）。

## 约定

- 内核输出统一带 `[POTATO]` 前缀，作为断言锚点；新增模块请保持该约定。
- 用例必须幂等：runner 对每个用例新建一台 QEMU，禁止跨用例共享状态。
- 端口由 runner 动态分配，可与交互模式的 QEMU（4444/5555）并行使用。
