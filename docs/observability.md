# 可观测体系（M8）

M8 把「内核里发生了什么」变成**可导出、可分析、可回归**的三件套：
结构化事件流（JSONL）、周期健康快照、以及一条把日志变成根因结论的分析流水线。
设计目标是验收判据里那句话：*注入卡死/泄漏场景，AI/脚本能从导出日志定位根因*。

## 1. 数据面：三类可导出行

所有行都以 `[POTATO]` 开头（测试台/脚本的断言锚点），后接固定标签：

| 标签 | 内容 | 生产者 |
|---|---|---|
| `[POTATO][TRJ]` | 单条事件 JSONL：`{"seq":N,"t":N,"ev":"NAME","a":N,"b":N}`，重要事件多一个 `"imp":1` | `kernel/trace/trace.c` |
| `[POTATO][TRM]` | 追踪元信息：`{"total":N,"dropped":N,"cap":256,"stream":0|1,"imp":{...},"counts":[{"ev":"X","n":N}...]}` | `trace_meta_json()` |
| `[POTATO][HEALTH]` | 健康快照（单行 JSON，见 §2） | `kernel/trace/obs.c` |
| `[POTATO][PANIC]` | 崩溃现场快照 + 紧随其后的副环 JSONL | `pot_obs_panic_dump()` |
| `[POTATO][INJ]` | 注入锚点（`inject <mode> arg=N started` / `mode=M done`） | `kernel/trace/inject.c` |

`t` 是 ccount（40MHz，QEMU/真机同一时基），`seq` 是内核单调序号。事件名用
`trace_event_name()` 映射，未知类型输出 `UNKNOWN`（向前兼容：新事件先上线，
分析器后跟进，不会因未知事件崩掉）。

### 双环设计（重要事件不被噪声冲刷）

实测：空闲系统的追踪环写入速率约 **2.8k 事件/秒**（主要来自 1ms tick 下的
`SCHED_SWITCH`），256 条主环约 **93ms** 就被覆盖一轮。若只有一个环，"从导出
日志定位根因"会被采样率吃掉——真正的异常事件极可能在导出前就被冲掉。

因此追踪层分两个环：

* **主环** `g_ring[256]`：全量事件，短历史（~90ms），用于看时序细节。
* **副环** `g_imp_ring[64]`：只收**重要事件**（异常、drop/fault、调度异常、
  网络丢包、MCP 重同步/重放、SMP 迁移、注入锚点、tick 停摆），写入速率极低，
  历史可以跨越数秒到数分钟——**根因现场在这里**。

`trace_is_important()` 是唯一判据（`kernel/trace/trace.c`），新增事件类型时
同步决定是否重要。导出 API：`trace_export_jsonl()`（主环尾 N 条）、
`trace_export_important()`（副环全量）、`trace_export_since()` /
`trace_export_important_since()`（流式游标，导出后游标前移，缺口计入 `lost`）。

### 流式导出与缺口记账

`trace stream on` 启动后台 `obs` 任务（BACKGROUND 类，100ms 一个 pump，每轮最多
4 条主环 + 4 条副环），因此**主机侧日志是采样而非全量**：健康快照里的
`trace.lost` / `imp.lost` 明确记录了多少事件因环覆盖而来不及导出。分析器据此
判断「事件间隔」是否可用（见 §3 R_IRQ_STALL）。

## 2. 健康快照（`health` 命令 / 周期自动打印）

单行 JSON，字段分块（键名稳定，测试直接断言）：

```json
{"health_n":3,"t":4123,"now":2533,"stream":1,
 "trace":{"total":7395,"oldest":7139,"dropped":7139,"lost":0,"pumped":48},
 "imp":{"total":2,"cap":64,"pumped":2,"lost":0},
 "heap":{"total":85552,"used":4312,"free":81240,"largest":81240,
         "maxused":4312,"alloc":49,"free_n":0,"fail":0,"fault":0},
 "sched":{"wake":1826,"preempt":0,"sw":3603,"idle":1,"sw_total":3603,
          "bg_used":1,"rdrop":0,"sdrop":0,"regover":0},
 "irq":{"rx":5,"rxb":43,"jitmin":32,"jitmax":16222,"wd":0,"lag":0},
 "tasks":[{"name":"main","tcb":...,"cls":"UI","state":"run","cpu":115,
           "sw":1778,"stk":3942,"stktot":4096,"last":2533}],
 "ch":[{"name":"mcp","enq":0,"deq":0,"drop_full":0,"drop_dl":0,
        "drop_dup":0,"retry":0,"ack":0,"hw":0,"count":0}],
 "inject":0}
```

要点：

* `now` = 调度器时钟（`pot_sched_now()`，tick 计），`t` = tick 硬件计数
  （`time_tick_count()`）。**两者必须同步**，`irq.lag = t - now` 就是漂移量。
* `irq.wd` = 看门狗自愈路径（int6 丢失补偿）补记的 tick 次数。
* `irq.jitmin/jitmax` = tick 实测触发间隔（us）。QEMU 空闲基线 jitmax 可达
  10~16ms（宿主调度/翻译块大步推进），真机应在数十 µs 量级。
* `tasks[].stk` = 栈未用水位（`0xC0DEC0DE` 花样扫描），`stktot` = 容量。
* 周期打印：`obs` 任务每 10 个 pump（约 1s）自动打一条；`health` 命令随时取。

## 3. 分析流水线（`tools/ai_analysis.py`）

仅标准库。输入任意串口日志（多个文件可拼接），输出人类可读报告 +
`--json` 结构化报告（schema `potato.obs.report/1`）；`--fail-on high` 时
退出码 1，可直接当 CI 判据。

```
python tools/ai_analysis.py test/logs/*.serial.log --json report.json --fail-on high
```

规则表（rule / 级别 / 触发 / 根因方向）：

| 规则 | 级别 | 触发 | 根因方向 |
|---|---|---|---|
| `R_EXCEPTION` | high | `EXCEPTION` 事件（a=EXCCAUSE,b=EPC） | 取指/数据异常、栈溢出写坏返回地址 |
| `R_MEM_FAULT` | high | `MEM_FAULT` 或 `heap.fault` 增长 | 堆越界/重复释放 |
| `R_SCHED_DROP` | high | `rdrop`/`sdrop` > 0 | 就绪/睡眠表容量不足 |
| `R_SCHED_LAG` | high | `irq.lag` 增长超阈值 | **tick 补账未驱动调度器**（D-08） |
| `R_LEAK` | high | `heap.used` 单调上升 + alloc-free 差值增长 | 分配未释放（tag 定位） |
| `R_IRQ_STALL` | high | 内核 `TICK_GAP` 实测事件 ≥ 阈值 | 长临界区/自旋屏蔽 tick |
| `R_TASK_STARVE` | high | 任务 `last` 落后 `now` 超阈值 | 高优先级任务霸占/老化失效 |
| `R_CPU_HOG` | medium | 单任务 tick 份额超阈值 | 后台预算失效 |
| `R_MSG_LOSS` | medium | 通道 drop_*/retry 或 `MSG_DROP` | 通道容量/超时配置 |
| `R_NET_LOSS` | medium | `NET_DROP` 事件 | 缓冲不足/链路质量（M7） |
| `R_IRQ_UNBALANCED` | medium | `IRQ_ENTER`/`IRQ_EXIT` 不配对 | 异常路径未收尾 |
| `R_TRACE_LOSS` | medium | `trace.dropped`/`lost` 增长 | 观测本身不完整，结论需谨慎 |
| `R_DATA_GAP` | low | JSON 行解析失败 | 串口截断/交织 |

阈值全部走 CLI（`--tick-gap-ms`/`--sched-lag-ms`/`--jit-us`/`--leak-bytes-per-s`
/…），报告里回写一份 `thresholds` 便于追溯；阈值默认值按 QEMU 实测基线定，
真机（抖动小一个数量级）应相应收紧。

**诚实性约定**：`R_IRQ_STALL` 只在两种证据下成立——内核实测的 `TICK_GAP`
事件，或**导出无缺口**（`trace.lost==0 && imp.lost==0`）时的事件流空洞；
采样导出下的"间隔"不作为空洞判据（`metrics.export_complete` 明确标注）。

## 4. 故障注入（`inject` 命令）

可观测性要可验证，必须有可控的坏输入：

| 命令 | 行为 | 指纹 |
|---|---|---|
| `inject leak [n]` | 后台任务每 50ms 分配 64B 且不释放（n=0 → 到堆耗尽） | `heap.used` 单调↑、`alloc-free_n` 差值↑、`MEM_ALLOC` 多于 `MEM_FREE` |
| `inject hang [ms]` | 任务态把 INTLEVEL 抬到 1（屏蔽 tick）后自旋（默认 200ms） | `TICK_GAP` 停摆事件、`jitmax` 突增、随后 tick 补记跳变 |
| `inject spin [ms]` | 以最高类自旋不让出（默认 300ms） | 该任务 `cpu` 陡增、其他任务 `last` 落后 |

`hang` 只抬到 INTLEVEL=1（不碰 level2/3），避免踩到「高 INTLEVEL 期间置起
设备中断源活锁」的已知陷阱（见 `docs/qemu-testing.md`）。

## 5. 验收用例（`test/cases/test_obs.py`）

| 用例 | 断言 |
|---|---|
| `test_obs_contract_trace_and_health` | TRJ 字段/序号单调、HEALTH 字段齐全、TRM 含副环统计、`trace jsonl` 可解析 |
| `test_obs_inject_leak_root_cause` | 注入泄漏 → 分析器 `R_LEAK(high)`，含增长量指标 |
| `test_obs_inject_hang_tick_gap` | 注入卡死 → 副环 `TICK_GAP` ≥150ms → `R_IRQ_STALL(high)` |
| `test_obs_panic_dump` | `obs panic` → PANIC 快照字段齐全 + 副环锚点可导出、零解析失败 |
| `test_obs_sched_clock_no_drift` | 连续 5s：`irq.lag ≤ 100ms`、切换计数持续增长、无 `R_SCHED_LAG` |
| `test_obs_sched_lag_rule_detects` | 合成漂移日志 → 规则契约（`R_SCHED_LAG(high)`、`sw_frozen`） |

## 6. 体系自己发现的缺陷：D-08（调度时钟漂移）

M8 落地当天，`health` 快照暴露了一个 M6 测试台没抓到的真实缺陷：空闲运行约
2.5s 后，`t`（tick 计数）继续增长而 `now`（调度器时钟）停摆，`sw`/`wake`
计数冻结——**sleep 到期不唤醒，时间片与防饥饿老化失效**。实测漂移 4s 时达
3.6s，长跑（soak）必然失真。

定位：`kernel/time/ccompare_tick.c` 的看门狗自愈分支（QEMU 会随机丢 int6，
该分支负责补账）只调用了 `tick_irq_handler()`，而 `pot_sched_tick()` 是由
中断分发路径 `intc_l1_dispatch()` 调的——旁路补账只推进了 tick 计数，没有
驱动调度器。修复：看门狗分支补上 `pot_sched_tick(tick_last_elapsed())`，
并新增 `g_wd_ticks` 计数器（健康快照 `irq.wd`）作为"IRQ 路线丢过 tick"的
证据。修复后 `irq.lag` 恒为 0，`test_obs_sched_clock_no_drift` 长期守住。

这条链路正是 M8 的价值：**新机制 → 新追踪事件/健康字段 → 分析规则 → 回归
用例**，缺陷从"跑久了就不对"变成"日志里一眼可证"。
