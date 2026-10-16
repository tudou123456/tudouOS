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
| ~~EPS1 特殊寄存器不可访问~~ **【已证伪，E-02 判定实验 2026-10-16】**：`eps1` 名字不存在只是 binutils 未导出（数字 SR 可用）；通用异常入口 QEMU 会回填 EPC1 且把异常前 PS 存入 SR194（binutils 名 eps2）；**rfe 从 EPC1+SR194 完全可控恢复**（console `rfetest` 命令 + `test_rfe_controlled_return` 用例：往 SR161 写 INTLEVEL=1 变体证明 rfe 不读 161、读 194）。"去中断化+手动 wsr.ps+ret.n"绕行不必要，IRQ 帧 rfe 恢复路线可行（D-03 真机抢占为"接线"级工作） | 抢占/异常恢复走 wsr.epc1 + wsr194 + rfe；syscall 语义 EPC1 指向 syscall 指令本身，改 EPC 需 +3（FreeRTOS 同款） |
| **syscall 异常的 EPC1 指向 syscall 指令本身**（故障 PC 语义），直接恢复会无限重入 | （现设计不依赖 EPC 恢复；若改回 rfe 方案需 EPC+3，FreeRTOS 移植同款处理） |
| 中断×手动异常退出的交互窗口存在竞态（M3 三个版本切换设计均在该窗口随机冻结） | **v5 定论（2026-10-16 深挖）**：tick 已改 int6 中断驱动（genex EXCCAUSE=4 → 记账/唤醒 → RESTORE+rfe，稳定，抖动 11892µs→~2.7ms 级）；**genex 上下文切走任务（rfe 到别的 EPC1）+ CCOMPARE 活动**的组合仍会随机丢 tick/任务重复执行（模型级缺陷，临界区/寄存器用法均已排除，E-02 已实证 rfe 路线本身正确）→ 抢占切换接线完整但默认关闭（`preempt on|off`），**真机（TRM 一致）应默认开**；`pot_tick_poll` 改作看门狗（错过 ≥4 周期即补账重挂）自愈丢拍 |
| ccount 按翻译块大步推进，比较点被整段跳过（触发率 ~53%） | tick 补记跨过的周期数，计数与虚拟时间对齐 |
| 通用异常不回填 EPC1/EPS1/EXCVADDR（level-1 中断会回填） | 异常现场保留字段，测试只断言 EXCCAUSE 与恢复语义 |
| 多任务负载下虚拟时间显著膨胀（且常快于墙钟） | 测试用宿主时钟+宽容差断言，顺序敏感断言改为无序等待 |
| 高频轮询 MMIO 不可取 | 时序热点避免 MMIO 轮询，用 ccount |
| **ROM 无 flash 映射能力**：对 VMA 在 0x42/0x3c 映射窗口的镜像段走 RAM 拷贝路径，直接 LoadStoreError 踩崩（Guru Meditation）——IDF 的 XIP 是其 RAM 常驻 bootloader 配好 MMU 后再跳的 | 自研二级映射引导（M5）：IROM 段从引导 ELF 剥离（objcopy --remove-section），LMA 落 flash 0x10020；boot.S 配 MMU（表@0x600C5000）+ icache（0x600C4060）后跳 XIP |
| esptool 会把 flash 映射段排在镜像最前且强加 64KB 页对齐填充段 | 引导镜像只含 RAM 段（esptool 看不到 IROM），IROM 经 merge-bin 按 0x10020 独立合入 |
| **MMU 未映射表项语义【E-03 判定实验 2026-10-16】**：复位后未写表项为 0x4000（bit14=invalid，非 0）；**取指**访问 invalid 页 → EXCCAUSE=28（InstrPifDataError）异常、EPC 精确（野 PC 会被捕获，不会随机执行）；**数据读**访问 invalid 页 → **不 fault**，返回毒值 0xdeadbeef（QEMU 特有；真机 TRM 应为总线错误——真机排障勿以 QEMU 行为类推）。另：QEMU 表项模型为 32 位宽（半字读有复制别名），真机 TRM 为 16 位表项——boot.S 的 32 位步进写在 QEMU 正确，真机会把 entry[1] 误写为 0(有效@页0)，**真机带板需改为 16 位写并复核**（console `mmutest dump|load|jump` 为常备探针） | 依赖取指 fault 兜野 PC 可行；不要依赖数据读 fault；野指针读会得到 0xdeadbeef 静默垃圾 |
| ~~GCC 会为零开销 loop 指令发射代码，但 QEMU 的 esp32s3 核心配置未实现该选项~~ **【已证伪，E-01 判定实验 2026-10-16】**：手写 `loopnez` 长循环（console `looptest` 命令）在开机即测、数百次任务切换后、RX 中断风暴打断循环体三场景共 2250 轮零失配；镜像中编译器生成的 3 条 `loop` 也在测试路径长期正常（回归用例 `test_loop_instruction`）。历史误诊的 volatile 索引纪律正确无害，保留未动；M2 strcmp 崩溃 / M3.5 ack 误命中真因未明（已排除 loop 指令），若复发需另查 | 无需规避；`looptest [rounds]` 命令与 `test_loop_instruction` 用例为常备回归探针 |

另有两坑在构建侧（已固化进 build.py/链接脚本，详见代码注释）：
- Xtensa `l32r` 字面量必须在指令之前且 4 字节对齐——链接脚本把 `.literal`
  集中放 `.text` 之前，并全程 `-Wl,--no-relax`（开了 relax 会被重排成坏码）。
- 统一工具链 C 文件用 `-mauto-litpools`；汇编文件不走长调用（内核尚小，
  直跳 + 无字面量最稳）。

## 约定

- 内核输出统一带 `[POTATO]` 前缀，作为断言锚点；新增模块请保持该约定。
- 用例必须幂等：runner 对每个用例新建一台 QEMU，禁止跨用例共享状态。
- 端口由 runner 动态分配，可与交互模式的 QEMU（4444/5555）并行使用。
