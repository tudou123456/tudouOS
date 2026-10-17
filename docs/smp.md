# M9：双核（SMP）子系统

> 适用：M9 阶段 1（CPU1 启动/IPI 骨架）+ 阶段 2（每核调度/亲和/负载均衡）。
> 默认单核启动，`smp start` 显式拉起 CPU1 —— 单核行为与 M8 完全一致。
> 代码入口：`kernel/include/smp.h`、`kernel/smp/smp.c`、`kernel/smp/cpu1_entry.S`、
> `kernel/sched/sched.c`（分核部分）、`kernel/intc/intc.c`（矩阵/IPI 分发）、
> `kernel/kernel/console.c`（`cmd_smp`）、`test/cases/test_m9.py`。

## 1. 目标与不变量

目标：从「单核 + CPU1 闲置」走到「两核在线、各自调度、任务可迁移、可观测、可回归」。

不变量（改 SMP 代码前先读完）：

1. **默认单核**：boot 后 CPU1 不上电，除非用户敲 `smp start`。61 例单核回归不受影响。
2. **全局时基只由 CPU0 推进**：`g_now` 只在 CPU0 的 CCOMPARE0 tick 路径前进；CPU1 的 tick
   只做本地记账（`pot_tick_poll` 在 `pot_smp_cpu_id() != 0` 时直接返回）。
3. **trace 环是 CPU0 单写者**：CPU1 不调 `trace_push`，只累加自己的结构计数器
   （`TR_CPU_UP=144`、`TR_IPI_SEND=145`、`TR_IPI_RECV=146`、`TR_SMP_MIGRATE=147` 全部由 CPU0 写）。
4. **跨核共享容器只在全局调度锁内修改**：就绪队列、睡眠表、迁移、attach。
   持锁期间不睡眠、不切换（锁内只做 O(1) 容器操作）。
5. **`pot_current` 仅兼容单核旧代码，只在 CPU0 维护**；新代码用
   `pot_sched_self()` / `pot_sched_cpu_current(cpu)`。

## 2. 硬件事实（QEMU esp32s3 与真机）

| 寄存器 | 地址 | 说明 |
|---|---|---|
| `SYSTEM_BASE` | `0x600C0000` | 系统寄存器块 |
| `SYSTEM_CORE_1_CONTROL_1` | `0x600C0004` | 写 CPU1 入口地址即释放 CPU1 |
| `SYSTEM_CPU_INTR_FROM_CPU_0..3` | `0x600C0030/34/38/3C` | 写 1 置起源线、写 0 清除 |
| 中断矩阵 | `0x600C2000` | 每 CPU 0x800；CPU0 表 `base+4*src`，CPU1 表 `base+0x800+4*src` |
| 矩阵项语义 | — | 写值 `& 0x1f` 为 CPU 中断号；**复位默认值 6**（`INTMATRIX_UNINT_VALUE`） |
| ETS 源号 | — | `UART0=27`、`EMAC=0`、`FROM_CPU_INTR0..3=79/80/81/82` |
| `SYSTEM_CORE_1_CONTROL_0` | `0x600C0000` | 真机 CPU1 运行控制：bit0 `RUNSTALL`、bit1 `CLKGATE_EN`、bit2 `RESETING`；**复位值 `0x04` = 复位保持**，真机必须先清 bit0/bit2、置 bit1，CPU1 才认入口地址；QEMU 自动拉起 CPU1，无需此步 |
| `RTC_CNTL_SWD_CONF` | `0x600080B4` | super watchdog 配置；复位默认**使能**且无自动喂，约 8s 后 rst:0x12 复位；需写 bit30 关闭（`SWD_WPROTECT`=0xB8，key `0x8F1D312A`） |

启动协议（QEMU ROM 反汇编实证）：ROM 桩在 `0x40043a38` 附近取 `0x600C0004`，
`l32i.n a2, a3, 0` → `beqz a2, 循环` → `callx8 a2` 跳到入口地址。真机等价 ROM 函数为
`ets_set_appcpu_boot_addr = 0x40000720`（`j 0x40043664`）。CPU1 被探测时停在
`PC=0x40043a3e`、`PS=0x00050020`（INTLEVEL=0/EXCM=0/UM=1/CALLINC=1/WOE=1）、
`VECBASE=0x40000000`、`INTENABLE=0`。

核能力（QEMU esp32s3 配置）：

| 项 | 值 | 影响 |
|---|---|---|
| `S32C1I` | 1 | 可用原子 cmpxchg 实现自旋锁（`SCOMPARE1`） |
| `EXCLUSIVE` | 0 | 不可用 LL/SC |
| `NUM_TIMERS` | 3 | CCOMPARE0 作 tick |
| `INTLEVELS` | 6 | 中断级别 1..6 |
| `WAITI` | 无 | 空闲只能自旋 |
| `THREADPTR` | 1（SR231） | **用 THREADPTR 存核号**（0=CPU0、1=CPU1）；PRID 在 QEMU 两核共用同一值，不能用 |

补充：
- `RTC_CNTL` 的 `SW_APPCPU_RESET` 在 QEMU 走 `qemu_system_reset_request`（整机复位的子集），
  且 `esp32s3_cpu_stall` 写入是空实现 —— 不能靠 stall 清 CPU1 状态。本实现**不复位 CPU1**，
  直接写入口地址，CPU1 入口自己重建窗口/向量/PS。
- 矩阵默认值 6 的含义：int6 是 CPU tick，未显式映射的源一旦被置起就可能误触发 tick。
  因此每个会被置起的源（含自己写的 FROM_CPU 源）都必须显式映射；自源映射到
  `INTC_IPI_MASK_INT=4`（level1，永不使能）防止自触发。

真机实测（2026-10-17，ESP32-S3 + 全 RAM 布局，见 `docs/hardware-bringup.md`）：

- `smp start` 返回 0 且 `online=2`；CPU1 入口依次打出 `a/b/c/e/f` 标记，说明窗口、
  向量、PS、栈与 C 入口全部就位；CPU1 自有 CCOMPARE0 tick 计数持续增长。
- `smp ipi <n>`：CPU1 侧 `ipi_rx`、`resched` 随发送增长，无 unknown 源。
- `smp spawn 1 2`：绑核任务在 CPU1 运行（`cpu1 ... cur=smpw0`，计数增长）。
- `smp spawn any 4`：`mig_in>0`，负载均衡迁移在真机实际发生。
- 关闭 SWD 后连续运行 40s 无 rst:0x12 复位。
- **中断帧 PS 槽在真机不同**：`vectors.S` 真机把 SR176 存帧 +80、SR194 存 +84；QEMU
  只有 `+80 = SR194` 有效。因此 `pot_do_resume_irq` 真机读 `+84`、QEMU 读 `+80`。
  CPU1 tick 抢占（`smp preempt on`）默认关，开启后才走该路径，需带板验证后再定默认定值。

## 3. 启动协议与 CPU1 主函数

`pot_smp_start()`（`kernel/smp/smp.c`）：

1. `g_cpu1_started=1`，打印 `boot cpu1: entry=0x... -> reg 0x600c0004`。
2. 写 `SYSTEM_CORE_1_CONTROL_1 = &_cpu1_entry`。
3. 轮询 `g_cpus[1].online`，最多 200 次 × `pot_task_sleep(1)`（≈200ms，期间让出 CPU0）；
   超时打印 `boot cpu1: TIMEOUT (no online ack)` 并返回 -1。
4. 成功打印 `cpu1 online confirm online=2`，写 `TR_CPU_UP`。

`_cpu1_entry`（`kernel/smp/cpu1_entry.S`）顺序：

```
rsil a2, 15                     // 关中断，重建上下文
movi a2, 1; wur a2, threadptr   // 核号=1
movi a1, pot_cpu1_stack_top     // 本核 4KB 栈（.bss.cpu1_stack）
movi a2, 1; wsr a2, windowstart // 窗口状态复位
movi a2, 0; wsr a2, windowbase
movi a2, pot_vectors; wsr.vecbase
movi a2, pot_task_ps; l32i; movi a3, 15; or; wsr a2, ps
call0 pot_smp_cpu1_main         // 不返回
```

> 注意：该文件代码段是**裸 `.section .text, "ax"`**。曾用 `.text.cpu1` 命名段，
> 被链接脚本第 57 行 `*(.text .text.*)` 收进 IROM（0x42 窗口），而字面量池在 IRAM
> （0x4038…），`l32r` 跨 28MB 直接报
> `dangerous relocation: l32r: literal target out of range`。保持裸 `.text` 与字面量同区。

`pot_smp_cpu1_main()`：

1. 本核矩阵：`FROM_CPU0_SOURCE(79) → int21`、`FROM_CPU1_SOURCE(80) → int4(掩蔽)`；使能 int21、int6。
2. `CCOMPARE0 = ccount + 40000`（40MHz/1000Hz）；`SCOMPARE1=0`（必须在任何调度锁之前）。
3. 建立 `cpu1idle` 占位任务：栈就是 `pot_cpu1_stack`（4KB，`POT_SMP_CPU1_STACK_WORDS`），
   `affinity=1`、`POTCLS_IDLE`，注册后 `pot_sched_attach_cpu(1, tcb)`。
4. `g_cpus[1].online=1`、`boot_ms`，`rsil 0`，打印 `[POTATO][SMP] cpu1 online id=1`。
5. 空闲循环：`idle_loops++`；`pot_sched_has_ready(1)` 时 `pot_task_yield()`。

## 4. 中断与 IPI

矩阵映射（本实现）：

| 视角 | 源 | 映射到 | 说明 |
|---|---|---|---|
| CPU0 | `FROM_CPU1`（80，CPU1 写） | int21（level2） | 使能，CPU0 的 IPI 入口 |
| CPU0 | `FROM_CPU0`（79，自己写） | int4（level1） | 掩蔽，永不使能 |
| CPU1 | `FROM_CPU0`（79，CPU0 写） | int21（level2） | 使能，CPU1 的 IPI 入口 |
| CPU1 | `FROM_CPU1`（80，自己写） | int4（level1） | 掩蔽 |

语义细节：

- **置起**：发送方写自己的 `SYSTEM_CPU_INTR_FROM_CPU_n` 位 0；**清除**：接收方写发送方的寄存器 0。
  源线保持到清除，所以 ISR 必须先清源再计数。
- 原因位：`POT_IPI_RESCHED=0x1`、`POT_IPI_WAKE=0x2`、`POT_IPI_HALT=0x4`，位图在
  `g_ipi_pending[cpu]`；未定义原因计入 `ipi_unknown`。
- `intc_irq_dispatch(level, frame_base)` 在 level2 按 pending&enable 分发 UART0(19)/EMAC(20)/IPI(21)；
  IPI 走 `pot_smp_ipi_handler(frame_base)`，**只计数/清源，不切换任务**（原因见 4.1）。

### 4.1 关键限制：level-2 中断帧不能用于上下文切换

Xtensa 硬件在 level-2 中断只保存 `EPC2/EPS2`；`SAVE_CALL0_REGS` 帧里 `+76/+80` 是
`EPC1/SR194` 的**上一次 level-1 中断的旧值**。`pot_do_resume_irq` 用这两个槽位 `rfe`，
如果从 level-2 ISR 里切换任务，恢复地址会跳到旧 PC。

实测（M9 首跑 `test_m9_ipi_path`）：

```
[POTATO][EXC] cause=28 epc=0x4200bef9 vaddr=0x69 ps=0x60532 task=idle
```

触发路径：IPI ISR → `pot_sched_resched_from_irq` →
`pot_do_resume_irq` → rfe 到 stale `EPC1`。`EXCCAUSE=28` 的标签口径尚待复核
（`exception.c` 记 `INSTR_PIF_DATA_ERROR`；Xtensa ISA 的 28 为 `LoadProhibited`，
两者在 E-03 的 MMU 实验里已有不一致记录）——但无论标签如何，机制结论一致：级别 2
不能走级别 1 的 `rfe` 恢复路径。

结论与实现约束：

- IPI ISR 只计数（`resched++`），不做调度。
- 调度点：①CPU1 空闲循环轮询就绪队列；②任务检查点 `yield/sleep`；③CPU1 自有 level-1 tick
  抢占（`smp preempt on` 开启，默认 off）。
- 抢占默认关闭的原因：QEMU 在 genex 帧与 CCOMPARE 递延模型下，level-1 tick 抢占路径的
  帧完整性仍有已知不确定性；真机验证后再考虑默认开。

## 5. 调度器分核改造（`kernel/sched/sched.c`）

| 状态 | 说明 |
|---|---|
| `g_cur[POT_SMP_MAX_CPUS]` | 每核当前任务（`pot_sched_self()` 读本核） |
| `g_ready[2][POTCLS_IDLE+1][16]` + `g_ready_n[2][4]` | 每核就绪队列（16 槽/类，原策略不变） |
| `g_quantum[2]`、`g_switches[2]` | 每核时间片与切换计数 |
| `g_sleep[16]` / `g_sleep_n` | 全局睡眠表（唯一） |
| `g_now` | 全局毫秒时基，仅 CPU0 推进 |
| `g_need_sched` | CPU0 的 tick 侧调度请求 |

关键 API（`kernel/include/task.h`）：

```c
pot_task_t *pot_sched_self(void);
pot_task_t *pot_sched_cpu_current(uint32_t cpu);
int  pot_sched_attach_cpu(uint32_t cpu, pot_task_t *t);
void pot_sched_cpu_tick_account(uint32_t cpu, uint32_t elapsed_ms);
int  pot_sched_has_ready(uint32_t cpu);
int  pot_sched_ready_count(uint32_t cpu);
int  pot_sched_cpu_running_nonidle(uint32_t cpu);
uint32_t pot_sched_cpu_switches(uint32_t cpu);
pot_task_t *pot_sched_take_ready(uint32_t cpu);
void pot_sched_put_ready(uint32_t cpu, pot_task_t *t);
int  pot_sched_should_preempt_cpu(uint32_t cpu, void *frame);
void pot_sched_preempt_from_irq_cpu(uint32_t cpu, void *frame);
void pot_sched_resched_from_irq(void *frame_base);
void pot_smp_balance_tick(void);   /* CPU0 每 4 tick 调一次 */
```

行为要点：

- 切换：`switch_account(from, next)` 在锁内完成容器/状态/记账（`from->cpu=me`、
  `next->cpu=me`），锁外 `pot_do_resume()`；CPU0 额外维护兼容全局 `pot_current`。
- CPU0 tick：推进 `g_now` → 周期唤醒（`pot_sched_wake` 按目标核路由）→ 时间片记账 →
  `g_need_sched` → `pot_smp_balance_tick()`。
- CPU1 tick（`pot_smp_cpu1_tick`）：重挂 CCOMPARE0 → `g_cpus[1].ticks++` →
  `pot_sched_cpu_tick_account(1, 1)`；`g_cpu1_preempt` 开启且 `should_preempt` 时走
  `pot_sched_preempt_from_irq_cpu(1, frame)`。
- `pot_tick_poll` 在 CPU1 上立即返回（不让 CPU1 的补账逻辑污染全局时基）。
- `pot_sched_should_preempt_cpu` 规则：本核当前是 idle → 抢占；时间片耗尽 → 抢占；
  否则按有效优先级比较（CPU0 保留「UART 行中间不切」的既有保护）。

## 6. 跨核调度锁

实现（`kernel/smp/smp.c`）：单把全局锁，`rsil 15` + `S32C1I`（`SCOMPARE1=0`，每核私有寄存器，
在 CPU0/CPU1 入口各设置一次）。

```
acquire: rsil 15 → aT=1 → s32c1i → 失败重试
release: memw → s32i 0 → memw → wsr.ps 恢复 INTLEVEL → esync
```

- 锁内只做：就绪/睡眠表、迁移、attach、take/put。
- 不允许持锁睡眠/切换 —— 切换前先 `unlock`，恢复后重新取锁。
- 单锁 vs 每核锁：临界区只有几十条指令，单锁避免了锁序/死锁分析；后续若出现争用热点再拆分。

## 7. 任务亲和与迁移

- `pot_task_t` 新字段：`volatile int cpu`（最近运行核）、`int affinity`（-1=任意核，0/1=钉核）。
- `pot_task_create_aff(...)`：affinity 越界（<-1 或 >=POT_SMP_MAX_CPUS）返回失败。
- 静态创建：`sched_static_enqueue` 按 affinity（无则当前核）入目标队列；跨核时发
  `POT_IPI_RESCHED`。
- 唤醒路由：`affinity >= 0 ? affinity : 任务最近核`；跨核入队 + IPI。
- 负载均衡 `pot_smp_balance_tick()`（CPU0 每 4 tick）：
  - `rq(src) >= 2 && rq(dst) == 0 && dst 当前非非-idle 任务` → 迁 1 个任务；
  - 两个方向对称（CPU0 积压→CPU1、CPU1 积压→CPU0）；
  - 迁移后 `g_balance_hold_ms = now + 20` 冷却 20ms。
- 历史教训：无冷却时两核会来回搬运任务 —— 实测 0.8s 内 `mig_out=151`。加冷却后
  `test_m9_balance` 稳定在 1~3 次迁入。
- 计数：`mig_in/mig_out/mig_last_ms`；trace `TR_SMP_MIGRATE`（data=tcb，aux=`src | dst<<8`）。

## 8. 初始化顺序（`kernel/kernel/main.c`）

```
pot_sched_init()   // 调度器（main 即控制台任务，idle 兜底）
tick_irq_arm()     // CPU0 tick 中断驱动抢占
pot_smp_init()     // M9：SMP 状态、CPU0 视角矩阵、使能 int21（默认单核）
... pot_heap_init / potfs_format / mcp_init / uart_irq_init
pot_obs_init()     // M8：可观测
net_start()        // M7：以太网 + 远程 MCP（无 EMAC 自动跳过）
console_run()
```

`pot_smp_init()` 不启动 CPU1、不改 CPU1 任何状态；只有 `smp start` 才写启动寄存器。

## 9. 控制台命令

| 命令 | 作用 | 关键输出 |
|---|---|---|
| `smp` / `smp stat` | 打印两核状态 | `cpus=… online=… started=… ipi_pending=…/…` + 每核一行 + work 摘要 |
| `smp selftest` | 常量/核号/结构自检 | `selftest rc=0` |
| `smp start` | 启动 CPU1（ROM 桩握手） | `boot cpu1: entry=0x…`、`cpu1 online confirm online=2`、`start rc=0 online=2` |
| `smp ipi <n>` | CPU0 向 CPU1 发 n 次 IPI（上限 10000） | `ipi sent=n to cpu1` |
| `smp spawn <cpu\|any> <n>` | 创建 n 个压测任务（n≤4；cpu=0/1 钉核） | `spawn aff=… n=… made=…` |
| `smp work` | 打印压测任务执行计数 | `work n=…`、`work<i> count=…` |
| `smp preempt on\|off` | CPU1 tick 抢占开关（默认 off） | `cpu1_preempt=1/0` |

每核状态行字段：

```
[POTATO][SMP] cpu0 online=1 up=1 ipi_tx=0 ipi_rx=0 resched=0 ticksw=0 ticks=0
                mig_in=0 mig_out=0 sw=12 unk=0 idle=0 rq=0/0 cur=console boot=0ms
```

| 字段 | 含义 |
|---|---|
| `online/up` | 在线标志 / 上电次数 |
| `ipi_tx/ipi_rx` | 发出的 IPI 数 / 收到的 IPI 数 |
| `resched` | IPI 中带原因位的次数（**不是**真实切换数） |
| `ticksw` | CPU1 tick 次数（CPU0 恒 0） |
| `ticks` | 本核 tick 计数 |
| `mig_in/mig_out` | 迁入/迁出任务数 |
| `sw` | 调度器实时上下文切换数（`pot_sched_cpu_switches`） |
| `unk` | 未知原因 IPI 次数（健康时应为 0） |
| `idle` | 本核空闲循环计数（CPU1） |
| `rq=0/N` | 本核非 idle 就绪任务数 |
| `cur` | 本核当前任务名（`-` = 无） |
| `boot=…ms` | 本核上电时刻 |

## 10. 验收用例（`test/cases/test_m9.py`）

| 用例 | 覆盖点 | 关键断言 |
|---|---|---|
| `test_m9_default_single_core` | 默认单核不变量 | online=1、started=0、CPU1 ticks=0、堆 ≥64KB |
| `test_m9_selftest_and_boot` | 自检 + ROM 握手 | `selftest rc=0`；`start rc=0 online=2`；CPU1 ticks 增长 |
| `test_m9_ipi_path` | IPI 双向通路 | `ipi_tx≥n`、`ipi_rx≥n`、`resched≥n`、`unk=0` |
| `test_m9_work_pinned` | 绑核执行 | CPU1 `sw>before`、无迁移、两个任务计数 >0 |
| `test_m9_balance` | 负载均衡 | `mig_in≥1`、`mig_out≥1`、CPU1 有切换、4 任务计数和 >0 |
| `test_m9_stress_stability` | 混合压测稳定 | 两核在线、unk=0、`drop ready=0 sleep=0 reg=0`、tick 前进 |

运行：

```powershell
python -m test.runner --filter m9      # 6/6
python -m test.runner                  # 全量 67 例
```

## 11. 限制与后续

- **真机已验证（2026-10-17）**：CPU1 释放（`SYSTEM_CORE_1_CONTROL_0`）、入口跳转、
  自有 tick、IPI 双向、绑核任务、负载均衡迁移均已带板实测通过；SWD 已关闭。
  仍未覆盖：真机上的 CPU1 tick 抢占（默认关）、两核缓存/内存序压力（S3 无硬件一致性，
  本实现靠锁）、`RTC_CNTL` 热复位后重新拉起 CPU1 的路径。
- **真机外设**：hw 平台的 DW GMAC 后端未实现（`emac_init` 直接跳过，网络在真机禁用）；
  UART0/EMAC 中断仍只在 CPU0 路由，CPU1 不碰外设。
- **抢占默认关**：当前以「空闲轮询 + 任务检查点 + 可选 tick 抢占」为主；真机验证后
  再评估默认开启。
- **trace 单写者**：CPU1 事件用计数替代；若需全事件追踪，需 per-CPU 环或 IPI 交接。
- **cause 28 标签口径**：`exception.c` 记 `INSTR_PIF_DATA_ERROR`，Xtensa ISA/QEMU 枚举
  有 `LoadProhibited` 说法，E-03 与 M9 两次观测现象一致但标签待真机/源码复核。
- **锁统计**：暂未统计调度锁空转次数（后续加 `spin` 计数便于定位争用）。

## 12. 代码索引

| 文件 | 职责 |
|---|---|
| `kernel/include/smp.h` | SMP 常量/结构/API 契约（IPI 原因位、每核状态、锁、spawn） |
| `kernel/smp/smp.c` | CPU1 启动/主函数/tick、矩阵映射、IPI、锁、均衡、诊断、压测任务 |
| `kernel/smp/cpu1_entry.S` | CPU1 入口（裸 `.text`，重建窗口/向量/PS/栈） |
| `kernel/sched/sched.c` | 每核队列/当前/时间片、切换、睡眠、唤醒路由、均衡钩子 |
| `kernel/sched/task.c` | 任务创建（含 `pot_task_create_aff`）、`pot_task_self` |
| `kernel/intc/intc.c` | 中断矩阵路由（`intc_route_source_cpu`）、IPI 分发、CPU1 tick 分核 |
| `kernel/intc/vectors.S` | level1/level2/genex 入口，IRQ 传帧 |
| `kernel/time/ccompare_tick.c` | CPU0 tick 与补账（CPU1 早退） |
| `kernel/kernel/console.c` | `smp` 命令族 |
| `test/cases/test_m9.py` | M9 验收用例 |

