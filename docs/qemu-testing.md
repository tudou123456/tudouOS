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

调试 M2/M3 时实测撞到的坑，内核已按此适配（相应代码有注释）：

| 现象 | 规避 |
| --- | --- |
| TIMG0/SYSTIMER 计数器一启用（写 T0_CONFIG 置 EN，与中断无关）QEMU 进程约 1s 内崩溃（0xC0000409） | 时基改用核内 ccount；TIMG/SYSTIMER 驱动保留（`POT_TIME_TIMG=1`）供真机/WSL Linux QEMU |
| PS.INTLEVEL=15 期间置起设备中断源会使 QEMU 陷入活锁（monitor 都无响应） | 初始化顺序纪律：先开全局中断（INTENABLE 为空时安全）再逐级使能设备源；**任何热路径禁止持有高 INTLEVEL**（M3 教训：printf/追踪环加锁后切换风暴必中彩票） |
| **EPS1 特殊寄存器不可访问**：工具链配置未导出 eps1 名，SR 194 读写实测无效（读恒 0）→ rfe 无法可控恢复 PS | M3 切换全部走 syscall + 手动 wsr.ps + ret.n（不依赖 epc1/eps1）；中断驱动抢占留给真机阶段 |
| **syscall 异常的 EPC1 指向 syscall 指令本身**（故障 PC 语义），直接恢复会无限重入 | （现设计不依赖 EPC 恢复；若改回 rfe 方案需 EPC+3，FreeRTOS 移植同款处理） |
| 中断×手动异常退出的交互窗口存在竞态（M3 三个版本切换设计均在该窗口随机冻结） | **调度核心去中断化**：tick 由 pot_tick_poll() 在检查点/空闲循环轮询推进，UART 保留中断（该路径稳定）；idle 纯忙轮询+条件让位（自切换风暴会以百万异常/秒饿死 QEMU 主循环，UART TX 停摆） |
| ccount 按翻译块大步推进，比较点被整段跳过（触发率 ~53%） | tick 补记跨过的周期数，计数与虚拟时间对齐 |
| 通用异常不回填 EPC1/EPS1/EXCVADDR（level-1 中断会回填） | 异常现场保留字段，测试只断言 EXCCAUSE 与恢复语义 |
| 多任务负载下虚拟时间显著膨胀（且常快于墙钟） | 测试用宿主时钟+宽容差断言，顺序敏感断言改为无序等待 |
| 高频轮询 MMIO 不可取 | 时序热点避免 MMIO 轮询，用 ccount |
| **GCC 会为零开销 loop 指令发射代码，但 QEMU 的 esp32s3 核心配置未实现该选项**（工具链 core-isa.h `XCHAL_HAVE_LOOPS` 为 undef，dynconfig 却放行）——执行即随机错值/非法指令，表现为 memset/strcmp/扫描函数间歇性出错（M2 的 strcmp 崩溃、M3.5 的 ack 窗口误命中皆此因） | 内核全部计数循环用 **volatile 索引**禁掉该优化（lib_shim.c/sched.c/console.c/msg.c 有统一注释）；新增代码含 for/while 计数循环时同此写法，或构建后 `objdump -d | grep loop` 验证为 0 |

另有两坑在构建侧（已固化进 build.py/链接脚本，详见代码注释）：
- Xtensa `l32r` 字面量必须在指令之前且 4 字节对齐——链接脚本把 `.literal`
  集中放 `.text` 之前，并全程 `-Wl,--no-relax`（开了 relax 会被重排成坏码）。
- 统一工具链 C 文件用 `-mauto-litpools`；汇编文件不走长调用（内核尚小，
  直跳 + 无字面量最稳）。

## 约定

- 内核输出统一带 `[POTATO]` 前缀，作为断言锚点；新增模块请保持该约定。
- 用例必须幂等：runner 对每个用例新建一台 QEMU，禁止跨用例共享状态。
- 端口由 runner 动态分配，可与交互模式的 QEMU（4444/5555）并行使用。
