# tudouOS 缺陷评审报告（实施清单）

> 本文档面向实施者（人 / AI Agent）。每条缺陷含 **位置 → 证据 → 根因 → 修复 → 验收** 五段，
> 可直接按编号逐条施工。
>
> **评审基线**：`HEAD = 703d689`（M5），工作树含未提交的 M6 在途改动
> （`kernel/mcp/`、`kernel/fs/`、`kernel/include/{mcp,json,potfs}.h`，以及
> `msg.h / potato.h / console.c / main.c / uart.c / build.py` 的修改）。
> 本报告中标注 M6 的问题均位于**未提交**代码。
>
> **证据分级**
> - 【实测】= 本次评审通过命令/探针直接观测到的现象（附复现命令）
> - 【源码】= 本次评审逐行读代码确认
> - 【待核】= 深挖阶段报告、本次未逐行复核的坐标（施工前请先确认行号）
>
> **评审方法（可复现，全部只读或可回滚）**
> ```bash
> cmd /c "cd /d D:\tudouOS && python tools/build.py"     # 构建（注意走 ASCII 联接路径）
> cmd /c "cd /d D:\tudouOS && python -m test.runner"     # 31 用例测试台（两轮结果一致）
> ```
> 另使用了：`objdump -d/-dl`、`readelf`、`nm -u`、工具链汇编器探针、
> 自制 QMP 探测脚本（MCP 多请求复现）、12 轮组合 soak（约 1090 万次 alloc/free）。

---

## 1. 结论摘要

| 优先级 | 编号 | 标题 | 状态 |
| --- | --- | --- | --- |
| **判定实验** | E-01 | `loop` 指令真相未定，全项目 `volatile` 纪律可能是安慰剂 | 必须实验 |
| **判定实验** | E-02 | "EPS1 不可访问"很可能是读错寄存器，`rfe` 路线未验证 | 必须实验 |
| **判定实验** | E-03 | MMU 未映射表项的语义（野指针是否 fault） | 必须实验 |
| **P0** | D-01 | MCP 端点开机后只处理第一条请求，其余静默丢弃且谎报成功 | 已实测复现 |
| **P0** | D-02 | 上下文切换丢失任务参数（`a3` 被 PS 值覆盖） | 已反汇编确认 |
| **P0** | D-03 | 抢占不存在；时间片轮转与预算压制均为死代码 | 已确认 |
| **P0** | D-04 | 全内核无临界区，启用 level-1 中断即损坏调度器 | 已确认 |
| **P0** | D-32 | 真机烧录指令错误（只烧引导镜像必崩），真机上手的第一道坎 | 已实测 |
| **P1** | D-05 | 堆护栏命中后继续用不可信 `size` 回收 | 已确认 |
| **P1** | D-06 | 二次释放完全不被检测（合并后旧块头仍是 ALLOC） | 已确认 |
| **P1** | D-07 | `CMD_BUF_LEN` 重复定义，MCP 单行被静默砍到 95 字节 | 已确认 |
| **P1** | D-08 | 测试硬编码版本号，当前工作树 29/31 | 已实测 |
| **P1** | D-09 | `.error.log` 永远是 `NoneType: None`，失败不可诊断 | 已实测 |
| **P1** | D-10 | 无 CI / 无 `-Werror` / 无门禁 / 无增量构建 | 已确认 |
| **P1** | D-11 | 就绪队列/睡眠表满则静默丢任务 | 已确认 |
| **P1** | D-12 | 任务注册表只增不减不去重，容量 8 | 已确认 |
| **P1** | D-13 | 异常路径 `pot_current` 未判空；无栈溢出护栏；idle 栈 768B | 已确认 |
| **P1** | D-14 | 切换退出窗口是"半恢复上下文"，保护变量从未接线 | 部分【待核】 |
| **P1** | D-15 | MMU 未映射表项保持 0（= 有效映射物理页 0） | 源码确认 + 推断 |
| **P1** | D-16 | QoS2"满则不丢"会被 `drop_oldest` 打破且无受害者通知 | 【待核】 |
| **P1** | D-17 | 去重/回执窗口各 8 格且跨发送方共享，"恰一次"有边界 | 【待核】 |
| **P1** | D-18 | 追踪环任务/中断双写者竞争；`uint16` 序号致丢弃统计失真 | 已确认 |
| **P1** | D-19 | JSON 不支持 `\uXXXX`，非 ASCII 被静默损坏（非拒绝） | 【待核】 |
| **P1** | D-20 | `mcp.c` 不是 MCP 实现（无 `initialize` / 无 `tools/call`） | 【待核】 |
| **P2** | D-21 | `pot_heap_alloc` 尺寸回绕：超大请求返回 32B 块 | 【待核】 |
| **P2** | D-22 | 上邻 footer 的 `self` 未做上界校验即解引用 | 【待核】 |
| **P2** | D-23 | 异常现场文档/实现不一致：`frame_kind`、IRQ 帧是纸面概念 | 【待核】 |
| **P2** | D-24 | `largest` 是近似值却按"最大可用块"承诺；位图只写不读 | 【待核】 |
| **P2** | D-25 | potfs 写失败即毁旧文件；无并发保护；零测试 | 【待核】 |
| **P2** | D-26 | 双端时基硬编码 40MHz，hw 后端无法构建且数值矛盾 | 已确认 |
| **P2** | D-27 | 追踪时间戳来源/单位注释错误；`systimer.c` 整文件 `#if 0` | 已确认 |
| **P2** | D-28 | 死代码与被推翻的注释成堆 | 已确认 |
| **P3** | D-29 | 仓库根目录混入 15,352 字节二进制文件 `-` | 已实测 |
| **P3** | D-30 | `udiv32` 的理由不成立；`CPENABLE` 未初始化；`%d` 的 UB | 【待核】 |
| **P3** | D-31 | 文档与实现偏差需同步收口（见第 7 节对照表） | 已确认 |

---

## 2. 三个必须先做的判定实验（改代码之前）

这三件事之所以排在所有修复之前：**当前有三处关键设计决策建立在本可避免的误判之上**，
在判定之前动手改代码，可能是在错误的地基上加固。

### E-01 `loop` 指令真相（影响面：全项目编码规范）

**矛盾事实**
- `docs/qemu-testing.md:88` 断言该 QEMU 核心配置未实现零开销 `loop` 指令，
  "执行即随机错值/非法指令"，要求"构建后 `objdump -d | grep loop` 验证为 0"，
  并要求所有计数循环用 `volatile` 索引规避。全内核确实照此写了。
- 【实测】但当前镜像里**仍有 3 条 `loop` 指令**，且其中一条在测试会执行的路径上：

| 地址 | 源码位置 | 说明 |
| --- | --- | --- |
| `42000c6d` | `kernel/kernel/console.c:342` | `cmd_fs` 子命令词扫描 |
| `42000df9` | `kernel/kernel/console.c:382` | `cmd_fs` 文件名扫描（`loop a11` 计 15 = `POTFS_NAME_MAX-1`） |
| `42001589` | `kernel/kernel/console.c:246` | `cmd_msgdemo` 模式词提取（内联进 `console_run`，`loop a11` 计 11 = `sizeof(mode)-1`） |

  `console.c:246` 在**每次 `msgdemo <mode>` 命令**上都会执行，而 5 种模式的测试全部解析成功。

**为什么必须实验**
两条互斥的结论都不可接受：

| 若实验结论是 | 后果 |
| --- | --- |
| `loop` 其实能正常工作 | 全内核 `volatile` 索引是**误诊后的安慰剂**；M2 的 strcmp 崩溃、M3.5 的 ack 窗口误命中的**真因从未找到**，随时可能复发 |
| `loop` 确实会坏 | 那么 `console.c:246/342/382` 现在就是坏的，而测试没抓到（`fs` 命令零用例） |

**实验设计**
1. 写一个最小测试：在任务上下文里跑一个确定会生成 `loop` 的计数循环
   （可仿照 `console.c:246` 的 `while` 形式），比对期望值与实际值，跑 ≥1000 次。
2. 分别在"任务首启"、"切换若干次后"、"手动异常退出后"三种状态下各跑一遍
   （`lib_shim.c:8` 的原始描述是"任务切换/手动异常退出后行为异常"，这是关键变量）。
3. 用 `objdump -d build/kernel.elf | grep -c '\bloop\b'` 确认该测试确实生成/未生成 `loop`。

**实验后必做**
- 无论结论如何，**把校验脚本化进 `tools/build.py`**（见 D-10），让纪律成为构建门禁。
- 同步修正 `docs/qemu-testing.md:88` 与 `lib_shim.c:6-11`、`kernel/include/heap.h:17-18`
  等处的注释，统一口径。

---

### E-02 "EPS1 不可访问"的再验证（影响面：整个异常退出 / 抢占架构）

**当前设计依据**
`docs/qemu-testing.md:79` 记录：

> **EPS1 特殊寄存器不可访问**：工具链配置未导出 eps1 名，SR 194 读写实测无效（读恒 0）
> → rfe 无法可控恢复 PS
> 规避：M3 切换全部走 syscall + 手动 wsr.ps + ret.n（不依赖 epc1/eps1）；中断驱动抢占留给真机阶段

**本次实测的事实（用工具链自身译码表判定）**

```bash
# 实验 1：汇编器接受哪些名字
#   rsr.eps1  -> Error: unknown opcode or format name 'rsr.eps1'
#   rsr.eps2  -> 20 c2 03   (SR 号 = 0xc2 = 194)
#   rsr.eps3  -> 20 c3 03   (195)
#   rsr.epc1  -> 20 b1 03   (177, 交叉验证基准)
#   rsr.ps    -> 20 e6 03   (230, 交叉验证基准)
# 实验 2：把裸字节反汇编，取 binutils 自己的名字表
#   .byte 0x20,0xc1,0x03 -> excw      (SR 193 不可译码)
#   .byte 0x20,0xc2,0x03 -> rsr.eps2  (SR 194 = EPS2)
#   .byte 0x20,0xc3,0x03 -> rsr.eps3  (SR 195)
```

**结论**：`eps1` 名字不存在是**正常现象**——Xtensa 的 EPS 家族从 2 开始，**根本没有 EPS1**；
而 **SR 194 是 `EPS2`**。因此文档所述实验存在两个问题：

1. "工具链未导出 eps1 名"被当成工具链缺陷，实际是该寄存器不存在；
2. 后续探针读的 **SR 194 是 EPS2**，它在"没有发生 level-2 异常"的上下文里返回 0 是
   **预期行为**，完全不能证明"不可访问"。

另外 `kernel/intc/vectors.S:107` 用 `rsr a2,194` 并把它注释成 eps1——实际读的是 EPS2。
（对 level-2 中断帧而言这**可能恰好是正确的寄存器**，只是标签错了。）

**实验设计**
用现成的 `crash` 命令思路做最小验证：在 level-1 异常上下文里正确设置 EPC1/PS，
用 `rfe` 返回，看是否可控恢复。**注意**：`rfe` 需要 EPC1 语义配合
（`docs/qemu-testing.md:80` 指出 syscall 的 EPC1 指向 syscall 指令本身，需 `EPC+3`，
FreeRTOS 移植同款处理）。

**为什么重要**
若 `rfe` 路线本可行，那么"去中断化 + 手写 PS 恢复"这一整套绕行、
以及由此缺失的抢占能力（D-03），**可能从一开始就不必要**。
本项直接决定真机抢占是"接线"还是"重写"。

---

### E-03 MMU 未映射表项语义（影响面：真机排障难度）

**事实**（`kernel/boot/boot.S:31-41`【源码】）
启动代码只写 MMU 表项 `0 .. _irom_pages-1`（当前镜像 `_irom_pages=1`，因 IROM 仅 0x78cc < 64KB）。
`boot.S:28` 注释声明表项格式为 `[13:0]物理页号 | [14]invalid | [15]type`。

**推断**：复位后未写的表项为 0 → `invalid=0` 表示**有效**，映射物理页 0（= 引导镜像所在 flash 页）。
则 0x42 窗口内任何野指针/跑飞 PC **不会立即 fault**，而是执行引导镜像字节或 0xFF，
把"地址错误"变成"随机指令错误"。

**实验设计**：跳转到 0x42 窗口内未映射区域，观察是 `LoadStoreError`/`InstrFetchError`
还是"正常取指"。同时确认是否需要补 `rsync/isync`（`boot.S:42-44` 写 ICACHE_CTRL 后无同步，
也无 cache/MMU invalidate，真机 ROM → 自研引导的切换点建议按 TRM 序列复核）。

**修复方向**：把 `_irom_pages .. 511` 写为 `invalid=1`。

---

## 3. P0 — 阻断级

### D-01 MCP 端点开机后只处理第一条请求，其余静默丢弃且谎报成功

**位置**：`kernel/mcp/mcp.c:45-63`（投递）、`kernel/msg/msg.c:44-83`（去重）、`kernel/msg/msg.c:152-155`（QoS0 返回）

**现象**【实测，已复现】
```
请求 1: {"jsonrpc":"2.0","id":1,"method":"ping"}       -> {"jsonrpc":"2.0","id":1,"result":{"pong":true,"tick":1516}}
请求 2: {"jsonrpc":"2.0","id":2,"method":"ping"}       -> (无响应)
请求 3: {"jsonrpc":"2.0","id":3,"method":"tools/list"} -> (无响应)
请求 4: {"jsonrpc":"2.0","id":4,"method":"ping"}       -> (无响应)
```
四次都打印 `[POTATO][MCP] submitted`（**谎报成功**）。

复现方法：拉 QEMU → `mcp on` → 依次发送上述四行 → 观察串口。

**根因**（两个独立缺陷叠加）
1. `kernel/mcp/mcp.c:53-57`：`memset(&m, 0, sizeof(m))` 之后**从未给 `m.id` 赋值**，
   恒为 0。
2. `kernel/msg/msg.c:78`：`enq()` 的**第一步**就是 `dedup_hit(ch, m->sender, m->id)`，
   键为 `(发送方 TCB 指针, id)`。`pot_msg_send`（`msg.c:131`）只设 `sender`，不动 `id`。
   于是控制台任务固定、`id` 恒 0 → 第 1 条 `dedup_add` 写入 `(console, 0)`（`msg.c:56-61`），
   **第 2 条起必命中去重** → `return 1`。
3. `kernel/msg/msg.c:152-155`：`r == 1`（去重命中，注释写"重发幂等"）与 `r == 0`（首次入队）
   走**同一条 QoS0 分支** `return 0` —— 调用方无法区分，于是打印 submitted。

**附带影响**：去重项永驻（`g_ch_ready` 保护下通道只 init 一次，`mcp.c:34-37`），
`mcp off/on` 无效，**只有 QMP 复位能再来一条**。

**修复**
1. 给 `m.id` 赋唯一值：在 `mcp.c` 增加 `static uint32_t g_line_seq;` 并 `m.id = ++g_line_seq;`
   （跨 epoch 唯一性待 D-23 的 epoch 字段落地后一并考虑）。
2. **去重不应作用于 QoS0**：QoS0 不存在重发路径（`msg.c:153-155` 直接返回），
   去重对它只能误杀合法新消息。建议在 `enq()` 内把 dedup 检查限定为
   `if (m->qos >= POT_MSG_QOS1 && dedup_hit(...))`。
3. 让"去重命中"对调用方可见（返回独立错误码或计数），不要与"入队成功"共用返回值。

**验收**
- 新增用例 `test_mcp_multi_request`：连发 ≥3 条 JSON-RPC，断言**每条**都有对应 `id` 的响应
  （见第 8 节）。
- 手工复现上面的四请求序列，应得 4 条响应。

---

### D-02 上下文切换丢失任务参数（`a3` 被 PS 值覆盖）

**位置**：`kernel/sched/switch.S:67-98`（`pot_do_resume`）、`kernel/sched/task.c:63-65`

**现象/证据**【实测，反汇编确认】
```
40380710:  l32i.n  a3, a1, 8      <- 取出帧槽 +8（即保存的 a3 / 任务 arg）
4038072a:  l32i    a3, a1, 60     <- 被 sar 覆盖
4038072f:  l32i    a3, a1, 64     <- 被 lbeg 覆盖
4038073b:  l32i    a3, a1, 72     <- 被 lcount 覆盖
40380744:  l32i.n  a3, a3, 0      <- 被 PS 值覆盖
4038074c:  l32i.n  a2, a1, 4      <- a2 正确重载 ✓
40380751:  ret.n                  <- a3 == PS 寄存器值 ✗
```
对照源码：`switch.S:70` 取值后，`:83-96` 把 `a3` 反复挪作
sar/lbeg/lend/lcount/PS 的暂存，`:96-98` 只重载了 `a2`。

而 `kernel/sched/task.c:63-65` 确实把参数写进了帧槽 2：
```c
frame[0] = (uint32_t)pot_task_trampoline; /* a0：首启 PC */
frame[1] = (uint32_t)fn;                   /* a2 */
frame[2] = (uint32_t)arg;                  /* a3 */
```
首启蹦床 `switch.S:54-57` 靠 `mov a2, a3` 取参数 → **每个任务的入口参数都是 PS 值**。

**为什么现在没炸**（不要因此降级）
全部任务函数都以 `(void)arg;` 开头显式丢弃参数：
`kernel/kernel/msgdemo.c:47`、`:82`、`:101` 等。
而 `msgdemo` **确实传了非空参数**（`msgdemo.c:190` `(void *)g_arg_q0`、`:200` 等），
只是接收方扔掉了。所以这是**被全体调用方掩盖的 API 契约破坏**：
`pot_task_fn_t` 声明接受参数，但该参数从未真正送达。
第一个写出 `static void t(void *arg){ cfg_t *c = arg; c->x = 1; }` 的人
会拿到 PS 值当指针解引用，得到一次原因不明的致命异常。

**修复**（一行）
在 `switch.S` 的 `esync`（当前 :95）之后、`l32i a2, a1, 4`（当前 :96）之前插入：
```asm
    l32i    a3, a1, 8
```
此时 `addi a1, a1, 88`（:97）尚未执行，槽偏移仍有效。
（替代方案：全程用其它寄存器作暂存，让 `a3` 只从帧来。）

**验收**
- 新增用例 `test_task_arg`：创建一个**真正使用**参数的探测任务
  （如传一个魔数指针，任务内校验并打印），断言任务收到的值等于创建时传入的值。
- `objdump -d build/kernel.elf` 确认 `pot_do_resume` 在 `ret.n` 前有从 `+8` 的重载。

---

### D-03 抢占不存在；时间片轮转与预算压制均为死代码

**位置**：`kernel/time/ccompare_tick.c:69-76`、`kernel/intc/intc.c:34-37`、`kernel/intc/exception.c:74-78`、`kernel/sched/sched.c:38-39,306-319,331-332`

**证据链**（全部为代码事实）
1. `ccompare_tick.c:69-76` `tick_init()` 注释"**v4：不使能 int6**——调度核心去中断化……
   真机阶段再恢复中断驱动"，函数体内**没有任何** `set_ccompare0` 或 `intc_enable_int(6)`。
2. `intc.c:34-37` 的 `intc_enable_int()` **全内核零调用**【实测 grep】；
   `intc.c:42` 使能的唯一中断源是 UART int19。
3. `exception.c:74-78`：即使 level-1 中断发生，该分支只 `intc_l1_dispatch(); return 1;`，
   注释明写"**不在中断上下文切换**"。
   ⇒ `intc_l1_dispatch()`（`intc.c:60-67`）与 `tick_irq_handler()`（`ccompare_tick.c:138-168`）
   是**不可达死代码**。
4. `sched.c:306-310` 把 `g_quantum` 递减到 0，**全文件再无读取点**
   → `POT_SCHED_QUANTUM_TICKS = 5` 的**时间片轮转完全没有强制力**（`task.h:33`）。
5. `sched.c:314`、`:318` 给 `g_need_sched` 置 1，而该标志**只写不读**，
   `sched.c:312` 注释自己承认"仅用于统计观察"。
6. ⇒ "后台超预算即压制"（`task.h:35` `POT_SCHED_BG_BUDGET = 700u/1000tick`）只是软提醒。

**后果**
- tick 只由 `pot_tick_poll()` 在任务检查点（`task.c:90,96`、`sched.c:58`、`msg.c:138,159,193`）
  内联推进。**不达检查点的任务永久占用 CPU，并把整个时间基准冻结**
  （`g_now`/`g_ticks` 只在 poll 里推进），连带老化、预算、睡眠唤醒全部失效。
- 立刻可指的例子：`kernel/kernel/console.c:232-239` 的 `cmd_flood` 循环内**没有检查点**。
- `idle` 优先级最低（`sched.c:166`），只有"无人就绪"时才跑，无法充当兜底轮询者。

**量化【实测】**：`test/logs/test_sched_checkpoint_preempt.serial.log` 中，
UI 任务 `pot_task_sleep(100)` 后**要等 47 个后台心跳**才拿到 CPU。
按 `demo.c` 标称约 8ms/拍，即**实际睡眠约 370ms，是请求值 100ms 的 3.7 倍**。

**测试为何没抓到**
`test/cases/test_m3.py:41` 的"抢占证据"只断言
`gidx_last > uidx[0]`（最后一个 G 行在第一个 U 行之后）——
**协作式调度天然满足**，该断言没有区分抢占与协作的能力。

**修复**（顺序不可颠倒，见 D-04）
1. 先把 `g_quantum`/`g_need_sched` 接上真实语义，或**删掉它们并同步改文档**
   （不留"看起来实现了但没用"的代码）。
2. 真机恢复中断驱动需要**三件事同时做**，当前一件都没做：
   ① `tick_init()` 使能 int6；
   ② IRQ 路径真正切换（含 `pot_do_resume` 支持 IRQ 帧，**必须走 `rfe`**，见 D-23）；
   ③ 临界区（D-04）。
3. **在此之前，不得在任何对外材料使用"抢占"**（见第 7 节）。

**验收**
- 新增负向用例 `test_no_yield_no_starvation`：启动一个**永不 yield** 的忙任务，
  断言更高优先级任务仍能得到 CPU。当前必然失败 —— 这正是要修的。
- 若短期不做真机中断驱动，则改为**文档降级 + 变量清理**，并加一条
  "轮询间隔"指标断言（`tickstat` 的 `jit_min/jit_max` 应改名为 poll interval，
  当前实测 `jit_max` 达 11892us，标称节拍 1ms；该读数来自深挖阶段，建议复核）。

---

### D-04 全内核无临界区；启用 level-1 中断即损坏调度器

**位置**：`kernel/include/intc.h:43-55`（`irq_save`/`irq_restore`）、
`kernel/sched/sched.c:270-320`、`kernel/kernel/console.c:71-73`

**证据**
- `intc.h:43-55` 提供了 `irq_save()`（`rsil %0, 15`）/`irq_restore`，
  但**全内核零调用**【实测 grep】。
- `console.c:71-73` 明确写"坚决不在这里持有高 INTLEVEL"（因 INTLEVEL=15 会活锁 QEMU，
  见 `docs/qemu-testing.md:78`）。
- 而 `intc.c:60-67` 会在中断上下文调用 `pot_sched_tick()`，
  它会改 `g_now / g_bg_used / g_win_start / g_sleep[] / g_sleep_n / g_ready[] / g_ready_n[]`
  （`sched.c:270-320`）；任务上下文也通过 `pot_tick_poll()` 调用**同一个函数**。
  `pot_task_create`（`task.c:79-80`）、`enqueue_ready/dequeue` 的移位循环
  （`sched.c:106-118`）均非可重入。

**后果**：目前是**潜伏**（int6 未使能）。按路线图接线的那一刻起，
睡眠表移位循环被中断打断 → 表项丢失/重复、任务失踪。
**这是必须先于启用中断修掉的前置条件。**

**修复**
- 把 `pot_sched_tick` 与所有就绪/睡眠表操作放进临界区；
- **用 `rsil 1`（只屏蔽 level-1）而不是 15**，避开已踩过的 INTLEVEL=15 活锁；
- 或者让 ISR 只置标志、把表操作留给任务上下文（更贴合现有检查点模型，改动更小）。

**验收**
- 新增 soak 用例：高频 UART 输入 + 任务风暴 + 调度压测同时进行，
  断言 `tasks` 的切换计数与就绪任务数自洽、无任务失踪。

---

### D-32 真机烧录指令错误：只烧引导镜像会必然崩溃（M5 遗留）

**位置**：`README.md:58-59`、`tools/build.py:165-166`

**现象**【实测】
README 与构建结束提示都只让人烧 `potato_boot.bin`：
```
README.md:58-59
  esptool --chip esp32s3 --port COMx --baud 921600 \
      write-flash 0x0 build/potato_boot.bin

build.py:165-166（构建结束时打印）
  真机烧录: esptool --chip esp32s3 write-flash 0x0 build/potato_boot.bin
```

但 **M5 之后 C 代码全部在 flash，不在这个文件里**：

```
$ python -m esptool --chip esp32s3 image-info build/potato_boot.bin
Image size: 2016 bytes
Segments: 2
  0  0x00038  0x3fc88000  DRAM
  1  0x00754  0x40380000  IRAM            ← 只有 RAM 段，没有 IROM

$ xtensa-esp-elf-nm build/kernel.elf | grep -E 'wdt_early_disable|kernel_main'
420032b4 T wdt_early_disable               ← 在 0x42 flash 窗口
420020c0 T kernel_main                     ← 在 0x42 flash 窗口
```

**根因**
- `kernel/linker/tudou.ld:54`：`.irom_text 0x42000020 : AT(0x00010020)`，
  `:72` `_irom_flash_page = 1` → MMU 把 **flash 物理页 1（0x10000）** 映射到 `0x42000000`。
- `kernel/boot/boot.S:31-44` 配好 MMU + icache 后，`:49` 就 `call0 wdt_early_disable`
  → **这是第一次 XIP 取指**，而 `wdt_early_disable` 在 `0x420032b4`
  → 对应 flash `0x10032b4`。
- 只烧 `potato_boot.bin` 时，flash `0x10000` 起**从未被写入**
  （空白片为 `0xFF`；若板上有旧固件则是旧数据）→ 取到 `0xFF`/垃圾 → 非法指令 → 启动即崩，
  串口停在 ROM 日志之后。

**正确的烧录方式**（二选一）
```bash
# 方案 A：整体烧 4MB flash 镜像（含 IROM@0x10020，最省心，约 45s+）
esptool --chip esp32s3 --port COMx --baud 921600 \
    write-flash 0x0 build/flash_image.bin

# 方案 B：两段分开烧（快，推荐）
esptool --chip esp32s3 --port COMx --baud 921600 \
    write-flash 0x0 build/potato_boot.bin 0x10020 build/irom.bin
```

**修复**
1. 改 `README.md:58-59` 与 `tools/build.py:165-166` 的提示为方案 B
   （快，且明确体现两段布局）。
2. `build.py:163` 的注释 "`flash_image.bin (4MB, QEMU 直接使用)`" 与模块 docstring
   `build.py:6` "（QEMU 用，**也可整体烧入真机**）" 表述不一致，统一口径。
3. **加一条构建期自检**（防止再次脱节）：校验 `flash_image.bin[0x10020..0x178EC]`
   与 `build/irom.bin` 逐字节相同（深挖阶段已用此法验证过一次，mismatches=0），
   不一致即构建失败。

**验收**
- 构建结束打印的命令，逐字执行即可在真机上跑起 `[POTATO] READY`。

---

## 4. P1 — 高

### D-05 堆护栏命中后继续用不可信 `size` 回收

**位置**：`kernel/mem/heap.c:218-226`（校验通过/失败后的共同路径）、`:250-255`

```c
hfoot_t *f = foot_of(b);
if (f->canary != canary_of(b) || f->self != b) {
    g_stat.n_fault++;
    trace_push(TR_MEM_FAULT, 1 /* overflow */, b->tag);
    /* footer 被踩：头信息仍可信（尺寸来自头），继续回收 */   // ← 前提不成立
}
uint32_t asize = b->size;   // ← 直接用 size
...
foot_of(b)->self = b;       // ← 用 size 算脚位置并写入 8 字节
```

**根因**：注释称"头信息仍可信"，但 `size` 正是下越界写最先命中的字段
（`ptr-12..ptr-9` 命中 `size`，而 `magic` 完好，`:212` 的检查会通过）。
`size` 为垃圾时 → 在任意地址写 8 字节（`:253`）+ `free_insert` 插入跨越任意范围的空闲块
→ 与活动分配重叠；`g_stat.used -= asize`（`:257`）无符号下溢。

**这直接推翻 `kernel/include/heap.h:12-13` 的承诺**：
"越界即捕获并报告（不致命，计数 + 追踪，**系统继续运行**）"。

**修复**
- 命中即**隔离**：把该块从空闲表剔除并永久泄漏，或直接 panic/复位；
- 恢复面至少加四项校验：`size % 8 == 0 && size >= H_MIN_BLOCK && b + size <= g_heap_hi && b >= g_heap_lo`；
- `used` 改饱和减法。

---

### D-06 二次释放完全不被检测（合并后旧块头仍是 ALLOC）

**位置**：`kernel/mem/heap.c:238-255`

上邻合并路径把块基址改成 `pb`（`:245`），但**只重写 `pb` 的头**（`:250-252`）。
原块的头仍保留 `H_MAGIC_ALLOC` 与原尺寸，且原块脚从未被覆盖。

**可达路径**：`P(free) A(alloc) N(free)` → `free(A)` → 下邻合并 N、上邻合并 P →
基址变 P；此时再 `free(A)`：
- `:212` `magic == ALLOC` 通过（旧头未动）；
- `:219` canary 校验通过（旧脚未动，合并写的是 `P` 的脚）；
- ⇒ **零 fault、零 trace**，直接 `free_insert(A)`。

**后果**：空闲表同时含 `P` 与 `A` 两块重叠区域 → 下次分配可能把同一段内存交给两个调用者，
**静默数据损坏**。与 `heap.h:20-21` 的"free 非法指针……计 fault"承诺矛盾。

**修复**：释放时把块头 `magic` 清成墓碑值（如 `H_MAGIC_DEAD`），
并在合并前把被吞并块头也置为不可识别值。

---

### D-07 `CMD_BUF_LEN` 重复定义，MCP 单行被静默砍到 95 字节

**位置**：`kernel/kernel/console.c:23`、`:138`、`:554`、`:580`

```c
:23   #define CMD_BUF_LEN 224 /* MCP JSON-RPC 单行较长 */
:138  #define CMD_BUF_LEN 96
```
中间无 `#undef`，**后者生效**（GCC 只告警，因构建无 `-Werror`，见 D-10）。
而 `kernel/mcp/mcp.c:21` 按 `MCP_LINE_MAX 192` 设计 → **内部自相矛盾**，
`MCP_LINE_MAX` 成死代码。`console.c:580-584` 超长输入**静默丢弃**、无错误应答。

**后果**：MCP 请求实测上限 95 字符，超出即被截断并只回 `-32700 parse error`，
客户端会误判为"服务器解析器坏了"而非"输入被截断"。

**修复（必须与 D-19 一起改）**
- 删掉两处定义之一；
- **注意陷阱**：把 224 改回生效会**放大 JSON 释放递归的栈压力**——
  `json.c:39-53` 的 `pot_json_free` 对 `next` 链也递归，深度 = 元素个数；
  mcp 任务栈 1024 words = 4096B（`mcp.c:30`），且构建为 `-fno-stack-protector`。
  当前 96 字节输入"勉强不炸"，改成 224 后可能爆栈。
  **因此必须先把 `pot_json_free` 的 `next` 链改成 while 循环**（深度压到 ≤8），
  再加解析节点数上限。
- 超长行应显式回 `-32600`/too long，而不是静默截断。

---

### D-08 测试硬编码版本号，当前工作树 29/31

**位置**：`test/cases/test_boot.py:20`、`:41`；`kernel/include/potato.h:6`

```python
test_boot.py:20   assert m.group(1) == "0.6.0", f"版本号异常: {m.group(1)}"
test_boot.py:41   ctx.serial.expect(r"\[POTATO\] info: tudouOS v0\.6\.0", timeout=5)
```
而工作树的 `potato.h` 已改为 `"0.7.0"`（`git diff` 显示 HEAD 为 0.6.0，M6 在途未提交）。

**实测**【两轮，结果一致】
```
结果: 29/31 通过，2 失败
  FAIL test_boot_banner  AssertionError: 版本号异常: 0.7.0
  FAIL test_ping_pong    SerialTimeoutError: 等待 '\[POTATO\] info: tudouOS v0\.6\.0' 超时
```
`test_ping_pong` 的 ping/echo/info 全部正常，**仅因 info 打印 v0.7.0 而失败**。

**修复**：断言改为格式匹配 `v\d+\.\d+\.\d+`，或从 `potato.h` 解析单一真源
（纯 Python 读 C 头文件即可）。同类问题还有 `test/cases/test_m5.py:31-32` 硬编码
`load:0x3fc88000` / `load:0x40380000`（链接脚本一改就红）。

**验收**：`python -m test.runner` 恢复全绿（31/31）。

---

### D-09 `.error.log` 永远是 `NoneType: None`，失败不可诊断

**位置**：`test/runner.py:148-156`、`:190-196`

`dump_logs()` 在 `finally`（`:196`）中被调用，而 `:155` 执行
`traceback.format_exc()` 时异常早已在 `:193-194` 被 `except Exception` 吞掉并压成字符串
`error`，**此时无活动异常** → 返回 `"NoneType: None"`。
真正有用的 `error` 字符串只打到 stdout（`:203`），从不落盘。

**实测**：`test/logs/` 下 **15 个 `.error.log` 全部是 16 字节 `NoneType: None`**
（时间跨度 10-06 至 10-08）。

**后果**：`docs/qemu-testing.md:36` 承诺"失败时附 `*.error.log`（断言栈）"，
实际零诊断价值；无人值守/CI 场景完全不可用。这也解释了历史上多次失败看不到原因。

**修复**：在 `except` 内 `tb = traceback.format_exc()`，`dump_logs` 写 `error + "\n" + tb`；
`:194` 的异常压字符串同时丢失 traceback，一并修。

---

### D-10 无 CI / 无 `-Werror` / 无门禁 / 无增量构建

**位置**：`tools/build.py:92`、`:101-119`；仓库根（无 CI 配置）

**实测**
- 无 `.github/`、`.gitlab-ci.yml`、`Jenkinsfile`、`.circleci/`；
  无 `requirements.txt`、`pyproject.toml`、`setup.py`、`pytest.ini`、`conftest.py`；
  无 `Makefile`、`CMakeLists.txt`。
- `build.py` 是手写 Python（174 行），**无 argparse**、无 `--clean`、无增量
  （对每个源文件无条件 `subprocess.run`，不比对 mtime、无头文件依赖跟踪）。
- 警告等级 `-Wall -Wextra`（`build.py:92`）但**无 `-Werror`**。
- 实证后果：当前构建带 6 处警告通过，其中
  `console.c:138: warning: "CMD_BUF_LEN" redefined`（D-07）与
  `console.c:313: warning: implicit declaration of function 'mem_cmd_dispatch'`
  都是真实缺陷，被静默放过。
- `docs/qemu-testing.md:88` 要求的 `objdump -d | grep loop` 校验**没有脚本化**（E-01）。

**修复**
1. `build.py` 加 `-Werror`（或至少 `-Werror=implicit-function-declaration`、
   `-Werror=macro-redefined`）。
2. 构建后自动跑 `objdump -d build/kernel.elf | grep -c '\bloop\b'`，非 0 即失败。
3. 加最小 CI：`check_env → build → python -m test.runner`，上传 `test/logs` 作 artifact。
4. `build.py` 加 `--clean` 与 mtime 增量。
5. 固定工具链/QEMU 版本校验：`tools/envlib.py:66-73` 定义了
   `TOOLCHAIN_URL`（`esp-13.2.0_20250707`）与 QEMU 版本
   （`esp-develop-9.2.2-20260417`），但**没有任何代码强制使用**
   —— 而该 QEMU 构建的 6 个坑正是所有测试容差的成因，版本漂移会直接改变测试结果。

---

### D-11 就绪队列/睡眠表满则静默丢任务

**位置**：`kernel/sched/sched.c:94-96`（`enqueue_ready`）、`:122-124`（`sleep_add`）

```c
static void enqueue_ready(pot_task_t *t)
{
    int c = slot_of(t);
    if (g_ready_n[c] >= SLOTS) {     // SLOTS = 8 (sched.c:30)
        return;                       // ← 无计数、无断言、无 trace
    }
```
**后果**：任务 `state = POTTASK_READY` 却不在任何队列 →
**永久静默死锁**，而 `tasks` 命令（`console.c:156-175`）显示的仍是"就绪/运行"，
排障会完全走错方向。

**修复**：扩容 + 失败计数 + `TR_SCHED_*` 丢弃事件 + 断言。

---

### D-12 任务注册表只增不减不去重，容量 8

**位置**：`kernel/sched/task.c:26-36`、`:79`

```c
#define REG_MAX 8
static pot_task_t *g_registry[REG_MAX];
static int g_reg_n;

void pot_task_register(pot_task_t *t)
{
    if (g_reg_n < REG_MAX) {
        g_registry[g_reg_n++] = t;   // ← 不去重、不淘汰
    }
}
```
`pot_task_create` 每次调用都 `pot_task_register(tcb)`（`:79`），而
`demo.c:82-89`、`msgdemo.c:142-149` 会**复用 TCB** → 同一 TCB 重复登记；
8 次登记后新任务彻底从 `tasks` 消失；已死任务的 TCB 永远留在表里。

**后果**：支柱3（可观测性）在几轮压测后失真——`tasks` 的 CPU 占用/切换数读的是过期 TCB。

**修复**：登记前去重；TCB 复用时更新而非追加；表满时计数告警。

---

### D-13 异常路径 `pot_current` 未判空；无栈溢出护栏；idle 栈 768B

**位置**：`kernel/intc/exception.c:97-98`、`kernel/sched/sched.c:66`、
`kernel/sched/task.c:52`、`kernel/intc/vectors.S:81`

- `exception.c:98` 直接 `pot_current->name`，**无判空**。
  而 `pot_sched_init()` 在 `main.c:35` 才执行，之前的任何故障
  （如 `pot_drivers_init`/`pot_heap_init` 阶段）`pot_current == NULL`
  → 取 0x0C 地址 → `LoadStoreError` → **双重异常，原始 cause 丢失**
  —— 正是最需要现场的时候。
- 无栈溢出检测：全内核无 canary/水位/护栏（唯一的 canary 机制在堆块上，
  `heap.c:20,219`）。`task.c:52` 只要求 `stack_words >= FRAME_WORDS + 8`（30 words = 120B）
  就放行；`sched.c:66` 的 idle 栈仅 192 words = **768B**。
- `vectors.S:81` 的 `addi a1,a1,-88` 是在**被打断上下文的栈**上压帧，
  `exception.c:93-99` 随后用 `potato_printf`（多层调用 + `char buf[12]`）
  在**同一个栈**上输出 → 栈溢出场景下异常报告本身就不可靠（也没有独立异常栈）。

**修复**：加栈填充水位 + 检查点校验；设最小栈策略；异常用独立栈；`pot_current` 判空。

---

### D-14 切换退出窗口是"半恢复上下文"，保护变量从未接线

**位置**：`kernel/sched/switch.S:94-97`、`kernel/sched/sched.c:23`、`:327`

`pot_do_resume` 先 `wsr.ps`（把 INTLEVEL 降到常态）**再**恢复 `a2`、`a1`，然后 `ret.n`。
`sched.c:23` 定义了 `pot_task_ps_il1`（注释"INTLEVEL=1：屏蔽 level-1 tick"，切换退出窗口用），
`:327` 赋值，但**全内核无任何读取**【待核：需确认无读取点】—— 设计意图落空。

**后果**：level2 UART 中断可在此窗口进入，ISR 将以 `a1` = 帧基址运行，
而帧基址 = `栈顶 - 88` ≡ **8 (mod 16)**，违反 call0 ABI 的 16 字节栈对齐要求，
并在半恢复的寄存器组上压新帧。

**修复**：把 `a1/a2` 恢复移到 `wsr.ps` 之前（用其它 scratch 装载 PS），
或真正启用 `pot_task_ps_il1`（`rsil 1` 而非 15，见 D-04），
并在恢复期间避免任何 C 调用。

---

### D-15 MMU 未映射表项保持 0

见 **E-03**。修复方向：`boot.S` 把 `_irom_pages .. 511` 写为 `invalid=1`；
并复核 ICACHE_CTRL 使能后的同步序列（`boot.S:42-44` 无 `rsync/isync`）。

---

### D-16 QoS2"满则不丢"会被 `drop_oldest` 打破

**位置**：`kernel/msg/msg.c:90-94`【待核】

驱逐的是**队首那条**，其 `qos` 可以是 QoS2；只记 `c_drop_full++`，
**从不通知受害者发送方**。开启后 `msg.h:18` / 设计稿 §4"满则阻塞不丢弃"失效，
且丢失是静默的。当前 `mcp.c:35`、`msgdemo.c:38/181` 均传 0
→ **该策略树内零启用、零测试**，却被文档列为已交付特性。

**修复**：驱逐时校验 `ring[head].qos != POT_MSG_QOS2`；给受害者计数 + 独立 trace reason。

---

### D-17 去重/回执窗口各 8 格且跨发送方共享，"恰一次"有边界

**位置**：`kernel/include/msg.h:60-71`（`dedup[8]` / `acked[8]`）、
`kernel/msg/msg.c:46-71`、`:56-61`【待核】

- **假重复执行**：≥9 个发送方并发重发时窗口互相覆盖 → 某发送方的重发**不再被判重**
  → 消息二次入队 → 接收方**二次执行工具**（对 `fs.write` 类副作用是真实的"至少一次"破口）。
  当前用例全是单发送方，恰好永不触发。
- **假失败**：`acked[8]` 被后续 ACK 挤掉后，发送方永远等不到回执 → 重发被 dedup 吃掉、
  接收方不会重新 ACK → attempts 耗尽返回 `-1`（`msg.c:162-166`），
  而消息其实早已投递处理完毕。调用方据 `-1` 补偿就会造成重复副作用。

**修复**：窗口按发送方分桶（或 `(sender,id)` 哈希表 + 代际）；
dedup 命中时**重放一次回执**，让 `-1` 只表示"未送达"。

---

### D-18 追踪环任务/中断双写者竞争；`uint16` 序号致丢弃统计失真

**位置**：`kernel/trace/trace.c:8-11`（文件头注释）、`:31-44`（写入）、`:46-50`（丢弃推算）、`:73-74`

**注释与实现自相矛盾**：
- `trace.c:9` 文件头写"**写入关中断**，读出（dump）同样关中断保证一致性"；
- `trace.c:33-35` 却论证"所有写入方都在异常上下文（EXCM=1 天然互斥，单异常串行），
  **无需关中断**"；
- 而实际写入方横跨三种上下文：任务上下文（`heap.c:201,214,221,259`、
  `msg.c:80,87,92,109,122,146,164,168,213`、`sched.c:195`）
  与 **UART RX 中断**（`intc.c:48,54`，level-2，1 字节阈值使能）。
- `trace.c:36-43` 的写入序列非原子（读 `g_head` → 写 5 个字段 → `g_seq++` → 写 `g_head`）
  → 事件覆盖、字段撕裂、`g_head` 丢更新。
- `trace.c:73-74` 只承认 dump 侧"可能轻微撕裂（罕见、仅为可读性损失）"。

**另有 `uint16` 序号问题**：`g_seq` 是 `uint16_t`（`trace.c:18,23`），
`trace_dropped()` 用 `s > 256 ? s - 256 : 0`（`:46-50`）推算。
按"每收 1 字节 2 条 IRQ 事件 + 每次切换 1 条 + 收发各 1 条"估，
几百事件/秒很正常 → **开机约 2 分钟 dropped 归零重算**，
`total` 也从 0 重新计。这与 `architecture.md:145`"丢弃有计数可查"矛盾，
且 `exception.c:97` 的异常现场会打出错的数。

**修复**：`g_seq` 改 32 位（slot 索引仍可 16 位）；
写入走 `rsil 1` 短临界区（**不要用 15**），或改成"原子取号 + release 提交"的 seqlock 风格；
至少把 `:33-35` 的错误注释改掉，避免后人据此继续在任务上下文裸写。

---

### D-19 JSON 不支持 `\uXXXX`，非 ASCII 被静默损坏（非拒绝）

**位置**：`kernel/mcp/json.c:88-98`（转义处理）、`:66-80` 与 `:85-104`（两趟处理）【待核】

转义 `switch` 只处理 `n/t/r/"/\\//`，其余 `default: ch = *c->p;`
→ `\u4E2D` 被写成字面 `u4E2D`（反斜杠被吞），surrogate pair 同样变垃圾，
`\b`/`\f` 未处理。两趟长度统计与拷贝步调一致 → **不越界，只损坏数据**。

**后果**：真实 MCP 客户端对中文/emoji 默认发 `\uXXXX` →
工具参数被改成乱码后**照样执行成功**并返回 200 级"成功"，调用方无从察觉。
自反例：序列化端会输出 `\u00XX`（`json.c:366-371`），
**本实现读不回自己产生的 JSON**。

**修复**：实现 `\uXXXX` → UTF-8（含代理对合成）；未知转义按 RFC 8259 拒绝；
补 round-trip 单测。

**已做对的地方（保留）**：`JSON_MAX_DEPTH = 8`（`json.c:20`）+ 检查（`:203-205`）
且每条返回路径 depth 恰好 -1 → 解析递归不会栈溢出。这是本项最担心的一点，做对了。
但注意 `pot_json_free`（`:39-53`）对 `next` 链递归**没有**深度约束（见 D-07 的陷阱说明）。

---

### D-20 `mcp.c` 不是 MCP 实现

**位置**：`kernel/mcp/mcp.c:217-225`（工具表）、`:252-319`（`handle_line`）【待核】

对照 MCP 规范（2025-06-18 tools 章节）：

| 规范要求 | 现状 |
| --- | --- |
| `initialize` 握手 + `notifications/initialized` | **完全没有** → 客户端握手必得 `-32601` |
| `capabilities.tools` 声明 | 无 capabilities 概念 |
| `tools/list` → `{tools:[{name,description,inputSchema}]}` | 返回 `"tools":"ping,echo,..."` **字符串** |
| `tools/call` `{name,arguments}` → `{content:[...],isError}` | **不存在**，改成 `method:"<工具名>"` 直接分发 |
| JSON-RPC 2.0：id 三态原样回显、notification 不回包、batch、`-32602` | id 只认 number 且缺失即 0；缺 id 也强行回包；无 batch；无 `-32602` |
| stdio 传输 = 换行分隔 JSON | 流里混入回显与 `potato> ` 提示符 → 严格客户端解析失败 |
| 安全 MUST：输入校验 / 访问控制 / 限流 | 全无 |

**后果**：**任何现成 MCP 客户端（Claude Desktop / MCP Inspector / 官方 SDK）都连不上**，
只能被自研脚本按自定义 RPC 调用。`kernel/include/mcp.h:11` 自己写的是"JSON-RPC **子集**"，
但对外表述是"内核原生 MCP"。

**修复优先级**：① D-01（否则除第一条请求外免谈）→ ② D-07 + `pot_json_free` 迭代化
→ ③ `initialize` / `tools/list` 正确 schema / `tools/call`
（约 300 行）→ ④ 传输独占 UART 或加帧标识 → ⑤ 可靠性（ACK 成真消息 + cursor/epoch）
→ ⑥ 负载（`fs.read` 的 offset/length + `truncated`，当前 144B 缓冲导致
1000B 文件被报成 `size=143` 且无截断标志）→ ⑦ 安全。

---

## 5. P2 — 中

### D-21 `pot_heap_alloc` 尺寸回绕：超大请求返回 32B 块
**位置**：`kernel/mem/heap.c:176-180`【待核】
`size ∈ [0xFFFFFFF9, 0xFFFFFFFF]` → 圆整回绕为 0；`[0xFFFFFFE9, 0xFFFFFFF8]` → `need` 回绕为 0..16。
两者都被 `if (need < H_MIN_BLOCK) need = H_MIN_BLOCK;` 钳到 32 →
**返回 8 字节可用载荷，而调用方认为自己拿到 ~4GB**，且不 `n_fail++`。
危险入口：`potfs_write` → `pot_heap_alloc(len)` 后按 `len` 逐字节拷贝。
**修复**：在圆整之前加 `if (size > HEAP_MAX - H_HDR - H_FOOT - H_ALIGN) { n_fail++; return 0; }`。

### D-22 上邻 footer 的 `self` 未做上界校验即解引用
**位置**：`kernel/mem/heap.c:238-242`【待核】
只校验 `pb >= g_heap_lo`，**没有** `pb + sizeof(hblock_t) <= g_heap_hi`，
随后 `pb->magic` 就是对任意 ≥heap_lo 地址的解引用 →
被越界写坏的脚可让 `pb` 落在 heap_hi 之上乃至未映射区 → 致命异常。
**这正好发生在"越界已经发生、护栏本该兜底"的时刻**，直接反驳"越界不崩溃"。
**修复**：解引用前做双端校验。

### D-23 异常现场文档/实现不一致：IRQ 帧是纸面概念
**位置**：`kernel/include/task.h:5-6,26-29`、`kernel/intc/exception.c:15`、
`kernel/sched/switch.S:96-98`、`kernel/sched/task.c:70`【待核】
`task.h` 描述"双帧制：tick 抢占用 IRQ 帧（含 EPC1/EPS1，`rfe` 恢复）"，
但 `pot_do_resume` 用 `ret.n`，**帧里的 `epc1`(+76)/`eps1`(+80) 槽根本不参与恢复**；
`tcb->frame_kind` 只在 `task.c:70` 被赋 0、无人读取。
⇒ 该设计在现有 `pot_do_resume` 下**无法实现**（IRQ 帧必须走 `rfe`），
这是 D-03 里"真机恢复中断驱动"缺的第二件事。
另：`vectors.S` 的写序是 `lcount→lend→lbeg→sar`，`pot_do_resume` 是
`sar→lbeg→lend→lcount`，两处不一致（建议按 ISA 惯例 `lcount` 最后写）。

### D-24 `largest` 是近似值；位图只写不读
**位置**：`kernel/mem/heap.c:266-272`、`:41,72,78,150`（位图）、`:111-122`（`find_block`）【待核】
`largest` 取"最高非空桶的**头块** size"，不是桶内最大值，误差可达 2 倍；
`memstat` 的 `fragpct` 建立在这个量上（`console.c:279-282`）。
`g_bitmap` 只被写、`find_block` 用的是 `g_bucket[i]==0` 线性扫描 →
`heap.h:10` / `architecture.md:123` 的"位图快速定位首个非空桶"名不副实。
**修复**：要么在 split/merge 时维护每桶最大块，要么更名 `largest_approx` 并标注近似。
另外 `heap.h:15` 与 `:259` 的"分配流水可回放"：`heap.c:252` 先 `b->tag = 0` 再
`:259` `trace_push(..., b->tag)` → **释放事件的 tag 恒为 0**，来源标签丢失。

### D-25 potfs 写失败即毁旧文件；无并发保护；零测试
**位置**：`kernel/fs/potfs.c:71-85`、`:15-26`【待核】
`write` 先在 `:71-74` `pot_heap_free(f->data)`，再分配新缓冲；
分配失败时 `used=0; name[0]='\0'` → **旧内容 + 文件名一起消失**（"写失败"等于"删文件"）。
`console.c`（UI/main 任务）与 `mcp.c`（AI_COMMS 任务）并发改写同一 `g_files[]`，
无锁、无重入保护——当前不出错**仅因**内部没有让出点，这是个未被写下也未被测试保护的不变量。
`test/` 全目录无 `potfs` 引用，但内核 help（`console.c:145-146`）与
MCP 工具表（`mcp.c:222-224`）已把它当可用命令暴露。
**修复**：写时复制（先分配新缓冲并拷贝，成功后再释放旧块）；加互斥或把所有入口收敛到单任务；
M6 用例补齐前标注 experimental。
**定位澄清**：potfs 是 **RAM 里的扁平命名 blob 表**
（12 文件 / 15 字符名 / 无目录 / 无持久化 / 无块设备 / 无掉电一致性 / 无磨损均衡），
不是文件系统。`potfs.h:11` 提到的 `potfs_bd_t` 块设备抽象**全仓只此一处注释、无类型无实现**。

### D-26 双端时基硬编码 40MHz，hw 后端无法构建且数值矛盾
**位置**：`kernel/time/ccompare_tick.c:18`、`kernel/pal/pal.c:41-64`、`tools/build.py:87`、`kernel/include/time.h:17`
`POT_CPU_HZ 40000000u` 同时决定 tick 周期（`:22`）与微秒换算（`:60`）；
而 `pal.c:58` 的 hw 后端返回 `240000000u`，且 `build.py:87` 硬编码
`-DTUDOU_PLATFORM=0`（**hw 分支从未被编译过一次**，无 CLI/环境变量开关）。
`time.h:17` 硬编码 `#define POT_TIME_TIMG 0`（与 `TUDOU_PLATFORM` 无关），
`timg_timer.c` 整个文件被该宏编译掉，`systimer.c` 整文件 `#if 0`。
⇒ "同一份镜像直接烧真机"会启动，但**时基差 6 倍**（`pot_task_sleep`/超时/老化/抖动统计全错）。
**修复**：时钟源运行时标定（读回 CPU 频率），把 `pot_pal_cpu_hz()` 与 tick 周期统一到同一来源；
`build.py` 增加 `--platform hw` 并加一条只编译不运行的 hw 构建门禁。

### D-27 追踪时间戳来源/单位注释错误
**位置**：`kernel/include/trace.h:9`、`kernel/trace/trace.c:14`
注释写"16MHz tick / systimer 低 32 位"，实际 `time_ticks_raw()` 返回 `ccount`（40MHz）
→ 时间戳 **107 秒回绕**（跨回绕差分/顺序分析会错判）；
`architecture.md:62`"抖动用 **systimer** 时间戳写入追踪环"而 `systimer.c` 整文件 `#if 0`。
**修复**：改注释；dump 输出做回绕归一化。

### D-28 死代码与被推翻的注释成堆
- 死代码：`intc_enable_int`（`intc.c:34`，零调用）、`intc_l1_dispatch`（`intc.c:60-67`）、
  `tick_irq_handler`（`ccompare_tick.c:138-168`）、`pot_sched_wake`（`sched.c:252-266`，零调用）、
  `frame_kind`、`POT_FRAME_*`、`ch->waiters[4]`/`n_waiters`（`msg.h:73-74`）、
  `potfs_append`（无调用者，且 `potfs.c:96` 的 `f->size + len` 溢出后会用小缓冲拷贝 `f->size` 字节 → 堆越界）。
- 注释与实现相反：`intc.c:43`"int6 由 tick_init 使能"；
  `main.c:19-21` 初始化顺序注释与实际顺序不符；
  `sched.c:13`"任何切换都发生在异常上下文（tick 抢占或任务阻塞 syscall）"；
  `trace.c:9` vs `:33-35`（见 D-18）；`heap.c:222`"头信息仍可信"（见 D-05）。
- 文档内部互相打架：`tudou.ld:9`"ROM 加载时会自动配置 MMU" 与
  `docs/qemu-testing.md:86`"**ROM 无 flash 映射能力**（实测）"直接矛盾。
- 头文件在撒谎：`msg.h:20-22` 以"显式唤醒等待者（条件变量模式，允许虚假唤醒）"
  描述公开语义，实际实现是 2ms 切片轮询。
- 结构体尺寸注释错误：`msg.h:33` 写"（56 字节定长）"，实际 `sizeof = 220` 字节
  （头 24B + 192B 负载）→ mcp 环 8×220 与 msgdemo 环 32×220 的 RAM 预算被低估。

---

## 6. P3 — 工程卫生

### D-29 仓库根目录混入 15,352 字节二进制文件 `-`
**实测**：`git ls-files -s -- "-"` → blob `1e262f6`，引入于提交 `c3df7d3`（M3.5）。
文件内容是裸 Xtensa 目标码（非 ELF），应为 `-o -` 之类把 stdout 重定向到文件名 `-` 的意外产物。
**修复**：`git rm --cached -- "-"`；`.gitignore` 增补 `-`、`*.log`、`*.pid`。
**说明**：`build/`（36 文件）与 `test/logs/`（108 文件）**都没有被跟踪**，
`.gitignore` 本身合理（含 `!docs/**/*.bin` 例外）。**唯一错提交的产物就是 `-`。**

### D-30 `udiv32` 的理由不成立；`CPENABLE` 未初始化；`%d` 的 UB
- `console.c:27-43` 注释"LX7 无硬件除法指令，自实现长除法避免链接 libgcc"——
  深挖阶段实测工具链对 `a/b` 直接生成硬件 `quou`（`memstat` 的 `fragpct` 已在 QEMU 正常执行）
  【待核】。理由不成立，但**功能无害**，属注释准确性问题。
- 全内核无 `wsr.cpenable`，而浮点会生成 `mul.s` 且能链接成功
  → 未来任何 float 使用会在**运行期异常**而非编译期报错【待核】。
- `console.c:107-108` 的 `%d` 用 `-(long)v`，对 `INT_MIN` 是 UB。

### D-31 文档与实现偏差需同步收口
见第 7 节。

---

## 7. 文档与实现偏差对照表（需同步修改）

| 文档位置 | 声称 | 代码实际 | 建议 |
| --- | --- | --- | --- |
| `docs/architecture.md:121`（M3 行） | "**三分类抢占调度**……真机阶段恢复中断驱动抢占" | 无抢占（D-03）；"真机恢复"**没有任何代码分支** | 改为"检查点协作调度（当前版本）"，把真机中断驱动列为**未开工计划** |
| `README.md:8` / `architecture.md:124` | "测试台 **31 用例全绿**" | 工作树 **29/31**（D-08） | 修 D-08 后恢复；并在 CI 中把关 |
| `README.md:72` | "1kHz tick 与真实时间对齐（±20%）" | tick 计数与 `now_us` **同源于同一个硬编码 40MHz ccount**，用例只比两个自报数字 → **自证循环**；且容差实为 ±9.1%（`test_m2.py:26-29` 是 800~1200，1.1s） | 改为"ccount 与 40MHz 假设自洽"；另立真实节拍指标 |
| `architecture.md:62`（支柱4） | "抖动用 **systimer** 时间戳写入追踪环，用数据证明'低抖动'" | `systimer.c` 整文件 `#if 0`；时间戳是 ccount；抖动数据是**轮询间隔**，实测 `jit_max` 达 11892us（标称 1ms） | 未实现；在真机中断驱动落地前**不要给出抖动预算数字** |
| `architecture.md:120`（M2 行） | "1kHz tick（核内 CCOMPARE0，**level1 通用异常路径+RFE**）" | 该路径已不可达（无 level-1 源使能） | 标注为 v4 之前的历史行为 |
| `architecture.md:96` / `README.md:9` | "自带极小 libc 与**软除法**，不链接任何预编译库" | 不链预编译库 ✓（`nm -u` 为空）；但 32 位除法由编译器**直接生成硬件 `quou`** | "软除法"表述不准 |
| `architecture.md:39` | "**测试台提供双端事件日志 diff 用例**固化这一点" | 全仓**无任何 diff 用例**、无黄金日志、无第二平台产物 | 改为规划中，或补用例 |
| `architecture.md:49`（支柱3） | "事件导出为 **JSONL**，直接喂给 AI 分析" | 零实现；只有明文 `[POTATO][TR]` 打印；`trace.h:10` 自称"已按 JSONL 导出器设计"但导出器不存在 | 标注为 M8 规划 |
| `architecture.md:123`（M4 行） | "**48h 长测入口已备**（churn+批量复位脚本）" | **该脚本不存在**；最长 soak 是 `test_m4.py:79` 的 `time.sleep(8)` | 改为规划中，或补脚本 |
| `architecture.md:147`（强化4） | "碎片率有上界" | 无任何机制支撑；`fragpct` 建立在近似量上（D-24）；31 用例无一断言碎片率 | 补断言或删除该指标 |
| `architecture.md:124`（M5 行） | "PAL……**TUDOU_PLATFORM 编译期选择**" | `build.py:87` 硬编码；hw 后端从未编译（D-26） | 改为"接口已成形，hw 后端待实现/验证" |
| `architecture.md:123`（M4 行） | "**TLSF 类**分配器……位图快速定位" | 骨架真实（15 桶 + 首适配 + 双向合并）；但位图只写不读（D-24），`largest` 不维护，且有 D-05/D-06 两个致命洞 | 降级为"分级空闲表 + 首适配 + 双向合并" |
| `architecture.md:122` / `:123` | "消息事件可回放" / "alloc/free 追踪可回放" | 只打印标签（`test_m35.py:108-132` 断言的是字符串存在，不是顺序/内容/重放）；环里只存 `(t,a,b,type,seq)`，无消息 data/len/qos | 改为"事件标签可观测" |
| `architecture.md:126`（M7 行） | "断线重连后消息**纪元重同步**" | `epoch`/`cursor` **全仓只出现在 docs**，内核无字段无逻辑；`m->seq` 只写不读 | 标注为待实现 |
| `docs/qemu-testing.md:79` | "EPS1 不可访问……SR 194 读写实测无效" | SR 194 = **EPS2**；`eps1` 名字不存在是正常的（无此寄存器）——见 **E-02** | 重测 `rfe` 路线后改写 |
| `docs/qemu-testing.md:88` | "`objdump -d | grep loop` 验证为 0" | 镜像里仍有 3 条 `loop`，且该校验未脚本化——见 **E-01**、D-10 | 脚本化进 build.py |
| `docs/qemu-testing.md:36` | "失败时附 `*.error.log`（断言栈）" | 15 个 `.error.log` 全是 16 字节 `NoneType: None`（D-09） | 修 runner.py |
| `docs/architecture.md:97` | "单核运行（CPU1 保持不动）" | 无任何 CPU1 代码 ✓，但也没有**显式**保持手段（无复位/停机序列、无 CPU1 侧保护） | 加注释或早期显式确认 |
| `kernel/include/msg.h:18`, `mcp-bus-design.md:57` | QoS2"满则阻塞**不丢弃**" | 可被 `drop_oldest` 打破（D-16），且该策略零启用零测试 | 修 D-16 或降级表述 |
| `mcp-bus-design.md:60-63` | "credit 流控""纪元+游标重同步" | 零实现 | 标注为规划 |

**建议**：把 `docs/qemu-testing.md:81` 的表述作为**对外唯一口径**
（"调度核心去中断化，UART 保留中断，硬性抖动预算留待真机"）——
它是全仓最诚实的一段，应替换 README/architecture 里的"抢占/低抖动"措辞。

---

## 8. 必须新增的测试用例

修复必须配测试，否则会回归。建议进 `test/cases/`：

| 用例 | 断言 | 对应缺陷 |
| --- | --- | --- |
| `test_mcp_multi_request` | 连发 ≥3 条 JSON-RPC，**每条**都有对应 `id` 的响应 | D-01 |
| `test_task_arg` | 创建真正使用参数的任务，收到的指针 == 创建时传入值 | D-02 |
| `test_no_yield_no_starvation` | 永不 yield 的忙任务不得饿死更高优先级任务 | D-03 |
| `test_sched_irq_stress` | 高频 UART 输入 + 任务风暴下，切换计数与就绪数自洽、无任务失踪 | D-04、D-11 |
| `test_heap_double_free` | 二次释放被捕获计数（当前会静默产生重叠空闲块） | D-06 |
| `test_heap_canary_corrupt_size` | 破坏 `size` 后释放不得越界写、不得插入跨范围空闲块 | D-05 |
| `test_no_loop_instruction` | 构建后 `objdump` 断言 `loop` 计数为 0 | E-01、D-10 |
| `test_version_format` | 版本号按 `\d+\.\d+\.\d+` 格式断言（非硬编码） | D-08 |
| `test_potfs_basic` | `fs write/cat/rm/ls` 全链路 + `truncated` 语义 | D-25 |
| `test_sleep_latency_bound` | `sleep(N)` 的宿主实测延迟上界（当前 100ms → 约 370ms） | D-03 |
| `test_task_registry_dedup` | 多轮任务复用后 `tasks` 输出不与实际任务数偏离 | D-12 |

---

## 9. 建议实施顺序与提交切分

**立即（2 分钟，真机上手的先决条件）**
0. D-32 修正 `README.md:58-59` 与 `tools/build.py:165-166` 的真机烧录提示
   （M5 之后必须把 `irom.bin` 烧到 `0x10020`，只烧引导镜像必崩），
   并给 `build.py` 加"`flash_image.bin` 的 IROM 区段与 `irom.bin` 逐字节一致"的自检。

**第 0 批（判定实验，不改产线代码）**
1. E-01 `loop` 真相实验 → 结论写进 `qemu-testing.md`
2. E-02 EPS/`rfe` 路线实验 → 决定 D-03 的路线
3. E-03 MMU 未映射表项语义

**第 1 批（一行级修复 + 门禁，风险最低、收益最高）**
4. D-02 `switch.S` 补 `l32i a3, a1, 8` + `test_task_arg`
5. D-08 版本号断言正则化 → 恢复 31/31
6. D-09 `runner.py` 抓 traceback
7. D-10 `build.py` 加 `-Werror`（先只对 `macro-redefined` / `implicit-function-declaration`）
   + `loop` 计数门禁 + `--clean`

**第 2 批（M6 可用化）**
8. D-01 `m.id` + 去重限定 QoS1 + `test_mcp_multi_request`
9. D-07 `CMD_BUF_LEN` + `pot_json_free` 迭代化（**必须同批**）+ 超长行显式报错
10. D-19 `\uXXXX` / D-20 id 三态与 notification 语义
11. D-25 potfs 写时复制 + M6 用例

**第 3 批（内存安全）**
12. D-05 护栏命中即隔离 + `size` 校验
13. D-06 墓碑 magic 防二次释放
14. D-21、D-22 尺寸回绕与上界校验
15. D-24 `largest` 语义修正或更名

**第 4 批（调度与真机前置）**
16. D-11、D-12 队列与注册表
17. D-13 异常路径判空 + 栈护栏
18. D-04 临界区（`rsil 1`）+ D-14 退出窗口
19. 完成 18 之后，才谈 D-03 的 int6 使能与 IRQ 帧 `rfe` 路径
20. D-26 时基运行时标定 + hw 构建门禁

**第 5 批（收口）**
21. D-28 死代码清理与注释修正
22. 第 7 节文档对照表逐条收口
23. D-29 `-` 文件清理

**每批验收统一口径**
```bash
cmd /c "cd /d D:\tudouOS && python tools/build.py"    # 必须零警告（D-10 后）
cmd /c "cd /d D:\tudouOS && python -m test.runner"    # 必须全绿
```
另加：`objdump -d build/kernel.elf | grep -c '\bloop\b'` 必须为 0。

---

## 10. 按文件索引（便于分批修改，避免并行冲突）

| 文件 | 相关缺陷 |
| --- | --- |
| `kernel/sched/switch.S` | D-02, D-14, D-23 |
| `kernel/sched/sched.c` | D-03, D-04, D-11, D-14 |
| `kernel/sched/task.c` | D-12, D-13, D-23 |
| `kernel/time/ccompare_tick.c` | D-03, D-26, D-27, D-28 |
| `kernel/intc/intc.c` | D-03, D-28 |
| `kernel/intc/exception.c` | D-03, D-13, D-28 |
| `kernel/intc/vectors.S` | D-13, D-23, E-02 |
| `kernel/boot/boot.S` | D-15, E-03 |
| `kernel/mem/heap.c` | D-05, D-06, D-21, D-22, D-24 |
| `kernel/mcp/mcp.c` | D-01, D-07, D-20 |
| `kernel/mcp/json.c` | D-07, D-19 |
| `kernel/msg/msg.c` | D-01, D-16, D-17 |
| `kernel/trace/trace.c` | D-18, D-27 |
| `kernel/fs/potfs.c` | D-25 |
| `kernel/pal/pal.c` | D-26 |
| `kernel/kernel/console.c` | D-07, D-28, D-30, E-01 |
| `kernel/include/*.h` | D-23, D-28（尺寸/语义注释） |
| `tools/build.py` | D-10, D-26, D-32 |
| `README.md` | D-32 |
| `test/runner.py` | D-09, D-10 |
| `test/cases/test_boot.py` | D-08 |
| `docs/architecture.md` | 第 7 节 |
| `docs/qemu-testing.md` | E-01, E-02, 第 7 节 |
| 仓库根 | D-29 |

---

## 11. 未复核项声明

本报告中【待核】标记的坐标来自分工深挖、**未逐行复核**，施工前请先确认行号。
【实测】【源码】标记的条目可直接采信。

**已确认无问题的地方（不要改动）**
- XIP 引导链：`flash_image.bin[0x10020..0x178ec]` 与 `irom.bin` **逐字节相同（mismatches=0）**；
  向量表 0x400 严格对齐、15 个槽位逐字节落在槽边界；IRAM 仅 0x754 字节且
  `nm` 显示 0x4038xxxx 全是 `.S` 符号；`nm -u` 为空（确实没链 libgcc/libc）。
- 消息通道：`msg.c` 全文件零动态分配，环形索引自洽无 off-by-one
  （判满 `count>=cap`、判空 `count==0`、槽位 `(head+count)&(cap-1)`），
  `cap` 强制 2 的幂；时间比较用回绕安全的 `(int32_t)(now - deadline) >= 0`。
- JSON 解析深度上限 `JSON_MAX_DEPTH = 8` 且每条返回路径 depth 恰好 -1；
  解析路径无内存泄漏（key 释放、半成品树释放、字符串分配回滚、`handle_line` 五条出口都 free）；
  序列化 `wputc` 记账式溢出检测无 off-by-one。
- 上下文切换保存/恢复了 `sar/lbeg/lend/lcount` 与 `a12-a15`（**除了 D-02 的 `a3`**）。
- `driver.c` 用影子槽表存状态，正确回避了回写 flash 上的只读 `.pot_drivers` 段。
- 内存分配器**正常路径**很稳：本次 12 轮组合 soak（约 1090 万次 alloc/free）
  faults=0、fail=0、alloc/free 平衡差=0、每轮 `used` 归零、
  `largest` 每轮都恢复满额 101648 字节。**基础功能没问题，问题在对抗性路径（D-05/D-06）。**
- `.gitignore` 合理；`build/` 与 `test/logs/` 均未被跟踪。
- 测试台设计正确：每用例独立 QEMU + 动态端口 + `-S` 冻结后先连 QMP/串口再 `cont`
  （实测所有 serial.log 从 ROM 第一行开始，零丢失）；两轮 31 用例结果完全一致，无 flaky。

---

*报告结束。修改任何一条后，请更新本文档中对应条目的状态，避免清单与代码再次脱节。*
