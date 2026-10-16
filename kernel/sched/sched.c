#include <stdint.h>
#include <stddef.h>

#include "intc.h"
#include "potato.h"
#include "task.h"
#include "tick.h"
#include "time.h"
#include "trace.h"
#include "uart.h"
/*
 * 土豆调度器（M3，v5）——三分类优先级域 + 轮转 + 时间片 + 老化防饥饿 + 后台预算。
 *
 * 切换核心（v5）：
 *  - tick 走 int6 中断驱动（记账/唤醒/时间推进），检查点 syscall 切换保留
 *  - 抢占切换完整接线（IRQ 帧 + rfe，preempt on|off，QEMU 模型缺陷默认关）
 *  - 一切切换都发生在异常上下文（EXCM=1，天然互斥）
 *  - 恢复统一 rfe：EPC1=续体、SR194=常态 PS（E-02 实证 rfe 取数槽）
 *  - 任务首启 = 伪造帧（a0=蹦床，a2=fn，a3=arg）
 * 任务态非重入子系统（堆/potfs/消息环/追踪环）均有 rsil1 临界区（D-04）。
 */

pot_task_t *volatile pot_current;
uint32_t pot_task_ps;      /* 任务常态 PS（sched_init 捕获） */
uint32_t pot_task_ps_il1;  /* 常态 PS + INTLEVEL=1（切换退出窗口屏蔽 tick） */

/* 切换意图（syscall 魔数，switch.S 定义同值） */
#define MAGIC_YIELD 0x50534C01u
#define MAGIC_SLEEP 0x50534C02u
#define MAGIC_EXIT  0x50534C03u

#define SLOTS 16 /* D-11：就绪/睡眠表扩容（msgdemo 双任务+mcp+demo 三任务
                     +churn+main 并存时 8 槽已不够），溢出由计数暴露 */

static pot_task_t *g_ready[POTCLS_IDLE + 1][SLOTS];
static int g_ready_n[POTCLS_IDLE + 1];
static pot_task_t *g_sleep[SLOTS];
static int g_sleep_n;
static uint32_t g_diag_ready_drop; /* D-11：就绪表溢出丢任务计数（应为 0） */
static uint32_t g_diag_sleep_drop; /* D-11：睡眠表溢出丢任务计数（应为 0） */

static volatile uint32_t g_now;
static uint32_t g_quantum = POT_SCHED_QUANTUM_TICKS;
static volatile uint32_t g_need_sched;
static uint32_t g_total_switches;
static uint32_t g_bg_used;
static uint32_t g_win_start;

/* 诊断计数（TODO(M3 debug): 稳定后移除） */
volatile uint32_t g_diag_wake, g_diag_preempt, g_diag_switch, g_diag_idle;
volatile uint32_t g_diag_wake_ok, g_diag_wake_miss;

_Static_assert(offsetof(pot_task_t, sp) == 4, "switch.S TCB_SP_OFF");

static void idle_main(void *arg)
{
    (void)arg;
    g_diag_idle++;
    for (;;) {
        /* v5：中断 tick 推进时间；idle 自旋（含看门狗 pot_tick_poll），
           有任务就绪即 yield 让位 */
        pot_tick_poll();
        if (pot_sched_others_ready()) {
            pot_task_yield();
        }
    }
}

static pot_task_t g_idle_tcb;
static uint32_t g_idle_stack[1024]; /* D-13/D-03：idle 也吃 IRQ 抢占帧
                                       （88B×嵌套异常现场 + tick 处理器
                                       C 栈），1KB 实测见底越界砸 .bss，
                                       与正式任务同规格 4KB */

static pot_task_t g_main_tcb = {
    .name = "main",
    .cls = POTCLS_UI,
    .state = POTTASK_RUNNING,
};

const char *pot_class_name(pot_task_class_t c)
{
    switch (c) {
    case POTCLS_AI_COMMS:   return "AI";
    case POTCLS_UI:         return "UI";
    case POTCLS_BACKGROUND: return "BG";
    default:                return "IDLE";
    }
}

/* ---------- 就绪/睡眠容器（定宽数组，杜绝链表环） ---------- */

static int slot_of(const pot_task_t *t)
{
    return (t->cls == POTCLS_IDLE) ? POTCLS_IDLE : (int)t->cls;
}

static void enqueue_ready(pot_task_t *t)
{
    int c = slot_of(t);
    if (g_ready_n[c] >= SLOTS) {
        g_diag_ready_drop++; /* D-11：不再静默——计数暴露（tasks 命令可见） */
        trace_push(TR_SCHED_DROP, (uint32_t)t, c);
        return;
    }
    for (volatile int i = 0; i < g_ready_n[c]; i++) {
        if (g_ready[c][i] == t) {
            return; /* 已在表（防御） */
        }
    }
    g_ready[c][g_ready_n[c]] = t;
    g_ready_n[c]++;
}

static void dequeue(pot_task_t *t)
{
    int c = slot_of(t);
    for (volatile int i = 0; i < g_ready_n[c]; i++) {
        if (g_ready[c][i] == t) {
            for (volatile int j = i; j < g_ready_n[c] - 1; j++) {
                g_ready[c][j] = g_ready[c][j + 1];
            }
            g_ready_n[c]--;
            return;
        }
    }
}

static void sleep_add(pot_task_t *t)
{
    if (g_sleep_n >= SLOTS) {
        g_diag_sleep_drop++; /* D-11：计数暴露 */
        trace_push(TR_SCHED_DROP, (uint32_t)t, 0x10u);
        return;
    }
    for (volatile int i = 0; i < g_sleep_n; i++) {
        if (g_sleep[i] == t) {
            return;
        }
    }
    g_sleep[g_sleep_n] = t;
    g_sleep_n++;
}

/* ---------- 优先级与挑选 ---------- */

static int is_aged(const pot_task_t *t)
{
    return (g_now - t->last_run_tick) > POT_SCHED_AGING_MS;
}

static int eff_prio(const pot_task_t *t)
{
    if (t->cls == POTCLS_BACKGROUND) {
        if (is_aged(t)) {
            return 0; /* 老化提升优先于预算，防饿死 */
        }
        if (g_bg_used >= POT_SCHED_BG_BUDGET) {
            return 4;
        }
        return 2;
    }
    if (t->cls == POTCLS_UI && is_aged(t)) {
        return 0;
    }
    return (int)t->cls;
}

static pot_task_t *pick_best(void)
{
    pot_task_t *best = 0;
    int best_p = 99;
    int best_aged = 0;
    for (int c = 0; c <= POTCLS_IDLE; c++) {
        for (int i = 0; i < g_ready_n[c]; i++) {
            pot_task_t *t = g_ready[c][i];
            int p = (c == POTCLS_IDLE) ? 3 : eff_prio(t);
            /* 同优先级时老化者优先：否则类扫描顺序会让高类忙任务
               永远压住老化提升的低类任务（老化形同虚设） */
            int ag = (c == POTCLS_IDLE) ? 0 : is_aged(t);
            int better = (p < best_p) ||
                         (p == best_p && ag && !best_aged);
            if (better) {
                best_p = p;
                best_aged = ag;
                best = t;
            }
        }
    }
    return best;
}

/* ---------- 统一切换核心：仅在异常上下文调用 ---------- */

void pot_do_resume(pot_task_t *t); /* switch.S */

static void switch_account(pot_task_t *next)
{
    g_total_switches++;
    g_diag_switch++;
    next->switches++;
    next->state = POTTASK_RUNNING;
    next->last_run_tick = g_now;
    g_quantum = POT_SCHED_QUANTUM_TICKS;
    g_need_sched = 0;
    trace_push(TR_SCHED_SWITCH, (uint32_t)pot_current, (uint32_t)next);
    pot_current = next;
}

/* intent: MAGIC_YIELD/SLEEP/EXIT；until 仅 SLEEP 用 */
void pot_sched_switch_from_exception(void *frame_base, uint32_t magic,
                                     uint32_t until)
{
    pot_task_t *from = pot_current;
    from->sp = (uint32_t *)frame_base;
    from->frame_kind = POT_FRAME_CALL; /* syscall 帧：a0 槽续体，ret.n 恢复 */

    if (magic == MAGIC_SLEEP) {
        from->sleep_until = until;
        from->state = POTTASK_SLEEPING;
        sleep_add(from);
    } else if (magic == MAGIC_EXIT) {
        from->state = POTTASK_DEAD;
    } else { /* YIELD / PREEMPT */
        from->state = POTTASK_READY;
        enqueue_ready(from);
    }

    pot_task_t *next = pick_best();
    if (next == 0) {
        next = from; /* idle 兜底，防御 */
    }
    dequeue(next);
    switch_account(next);
    pot_do_resume(next); /* 不返回 */
}

/* ---------- D-03 v5：tick 抢占（IRQ 帧 + rfe，E-02 实证路线） ---------- */

void pot_do_resume_irq(pot_task_t *t); /* switch.S */

/* tick 中断上下文（EXCM=1）调用：被抢占任务的帧已在 SAVE_CALL0_REGS
 * 落在其栈上（帧 +76/+80 = 硬件 EPC1/异常 PS）。 */
void pot_sched_preempt_from_irq(void *frame_base)
{
    pot_task_t *from = pot_current;
    from->sp = (uint32_t *)frame_base;
    from->frame_kind = POT_FRAME_IRQ;
    from->state = POTTASK_READY;
    enqueue_ready(from);

    pot_task_t *next = pick_best();
    if (next == 0) {
        next = from; /* 防御 */
    }
    dequeue(next);
    switch_account(next);
    if (next->frame_kind == POT_FRAME_IRQ) {
        pot_do_resume_irq(next); /* rfe 恢复（不返回） */
    }
    pot_do_resume(next); /* 首启/syscall 帧：ret.n 恢复（不返回） */
}

/* syscall 切换入口识别（exception_dispatch 调用）
 * 返回 1=是调度 syscall 并已切换（不返回），0=不是 */
int pot_sched_try_syscall(void *frame_base)
{
    uint32_t *frame = (uint32_t *)frame_base;
    uint32_t magic = frame[1]; /* a2 */
    if (magic != MAGIC_YIELD && magic != MAGIC_SLEEP && magic != MAGIC_EXIT) {
        return 0;
    }
    pot_sched_switch_from_exception(frame_base, magic, frame[2] /* a3 */);
    return 1; /* 不可达 */
}

/* ---------- 消息基座配套的阻塞/唤醒原语（M3.5） ---------- */

void pot_sched_syscall_sleep(uint32_t until); /* switch.S */
void pot_sched_syscall_yield(void);           /* switch.S */

/* 当前任务睡到绝对时刻 until（tick）—— pot_task_sleep 的底层，消息通道等待复用 */
void pot_sched_sleep_until(uint32_t until)
{
    pot_sched_syscall_sleep(until);
}

/* 显式唤醒：从睡眠表移除并入就绪表（空转/协作唤醒，醒来者在检查点重查条件）。
 * 非睡眠态为无害空操作。返回 1=已唤醒 */
int pot_sched_wake(pot_task_t *t)
{
    for (volatile int i = 0; i < g_sleep_n; i++) {
        if (g_sleep[i] == t) {
            for (volatile int j = i; j < g_sleep_n - 1; j++) {
                g_sleep[j] = g_sleep[j + 1];
            }
            g_sleep_n--;
            t->state = POTTASK_READY;
            enqueue_ready(t);
            return 1;
        }
    }
    return 0;
}

/* ---------- tick 钩子（v3：仅记账+唤醒，切换在任务检查点） ---------- */

void pot_sched_tick(uint32_t elapsed)
{
    g_now += elapsed;

    if (pot_current != 0) {
        pot_current->cpu_ticks += elapsed;
        if (pot_current->cls == POTCLS_BACKGROUND) {
            g_bg_used += elapsed;
        }
    }

    if ((g_now - g_win_start) >= POT_SCHED_WINDOW_TICKS) {
        g_win_start += POT_SCHED_WINDOW_TICKS;
        g_bg_used = 0;
    }

    int woke_better = 0;
    int cur_p = (pot_current != 0) ? eff_prio(pot_current) : 0;
    for (volatile int i = 0; i < g_sleep_n; ) {
        pot_task_t *t = g_sleep[i];
        if ((int32_t)(g_now - t->sleep_until) >= 0) {
            for (volatile int j = i; j < g_sleep_n - 1; j++) {
                g_sleep[j] = g_sleep[j + 1];
            }
            g_sleep_n--;
            t->state = POTTASK_READY;
            enqueue_ready(t);
            g_diag_wake++;
            if (eff_prio(t) <= cur_p) {
                woke_better = 1;
            }
        } else {
            i++;
        }
    }

    if (g_quantum > elapsed) {
        g_quantum -= elapsed;
    } else {
        g_quantum = 0;
    }

    /* v3：need_sched 仅用于统计观察；切换在任务检查点发生 */
    if (woke_better) {
        g_need_sched = 1;
    }
    if (pot_current != 0 && pot_current->cls == POTCLS_BACKGROUND &&
        g_bg_used >= POT_SCHED_BG_BUDGET) {
        g_need_sched = 1;
    }
}

/* ---------- 初始化 ---------- */

void pot_sched_init(void)
{
    asm volatile("rsr.ps %0" : "=r"(pot_task_ps));
    pot_task_ps_il1 = pot_task_ps | 1u; /* INTLEVEL=1：屏蔽 level-1 tick */

    pot_current = &g_main_tcb;
    g_main_tcb.last_run_tick = 0;
    g_main_tcb.stk_lo = (uint32_t *)_stack_bottom; /* D-13：主任务栈水位 */
    g_main_tcb.stk_words = (uint32_t)(_stack_top - _stack_bottom) / 4u;
    {
        /* 主任务正在本栈上运行：只涂当前栈针以下的空闲区
           （局部变量地址近似 SP，留 16 词余量） */
        volatile uint32_t anchor = 0;
        uint32_t below = (uint32_t)((uint32_t *)&anchor - g_main_tcb.stk_lo);
        if (below > 16u) {
            pot_task_stack_paint(g_main_tcb.stk_lo, below - 16u);
        }
    }
    g_quantum = POT_SCHED_QUANTUM_TICKS;
    g_need_sched = 0;
    g_win_start = 0;
    g_now = 0;

    pot_task_register(&g_main_tcb);
    pot_task_create(&g_idle_tcb, "idle", POTCLS_IDLE, idle_main, 0,
                    g_idle_stack, sizeof(g_idle_stack) / sizeof(uint32_t));
}

uint32_t pot_sched_now(void)
{
    return g_now;
}

uint32_t pot_sched_total_switches(void)
{
    return g_total_switches;
}

uint32_t pot_sched_bg_used(void)
{
    return g_bg_used;
}

/* idle 用：是否有非 idle 任务就绪（决定要不要 yield 让路） */
int pot_sched_others_ready(void)
{
    return (g_ready_n[0] | g_ready_n[1] | g_ready_n[2]) != 0;
}

/* ---------- D-03 v5：抢占决策 ----------
 * 返回 1 = tick 中断应切换（更高有效优先级就绪，或时间片耗尽）。
 * 打印原子性约束：UART 输出按行组装（逐字节轮询 TX），若在行中切换，
 * 另一任务的输出会插进半行——控制台/演示任务的行锚点全靠 [POTATO]
 * 前缀逐行断言，故行组装期间（uart 行忙）推迟抢占到下一个 tick。 */
/* D-03 诊断开关：0=禁用 tick 抢占切换（中断 tick 仍记账/唤醒，
 * 切换只发生在检查点）——`preempt off|on` 命令控制。
 * 默认 0：QEMU esp32s3 模型在"genex 上下文切走 + CCOMPARE 活动"组合下
 * 会随机丢 tick（M3 三版设计冻结的同源模型缺陷，E-02 已排除寄存器
 * 用法因素，看门狗只能救时钟救不了任务重复执行）；真机（TRM 一致）
 * 上应默认开启 */
static volatile int g_preempt_enabled = 0;

int pot_sched_should_preempt(void)
{
    if (!g_preempt_enabled) {
        return 0;
    }
    if (pot_current == 0) {
        return 0;
    }
    if (uart_tx_midline()) {
        return 0; /* 行组装中：推迟（下一个 tick 再试，最多 ~1.4ms） */
    }
    pot_task_t *best = pick_best();
    if (best == 0) {
        return 0; /* 无人就绪：当前独占，连轮转都不需要 */
    }
    if (g_quantum == 0u) {
        return 1; /* 时间片耗尽且有任务等着：轮转/让位 */
    }
    return eff_prio(best) < eff_prio(pot_current);
}

void pot_task_register(pot_task_t *t); /* task.c */

/* task.c 创建任务时的入队口 */
void sched_static_enqueue(pot_task_t *t)
{
    uint32_t ps = irq_save(); /* D-04：与 tick IRQ 的队列操作互斥 */
    enqueue_ready(t);
    irq_restore(ps);
}

/* 诊断计数导出（TODO(M3 debug): 稳定后移除） */
void pot_sched_diag(uint32_t *out)
{
    out[0] = g_diag_wake;
    out[1] = g_diag_preempt;
    out[2] = g_diag_switch;
    out[3] = g_diag_idle;
}

/* D-11：就绪/睡眠表溢出计数（tasks 命令暴露；正常运行恒为 0） */
void pot_sched_drop_diag(uint32_t *ready_drop, uint32_t *sleep_drop)
{
    *ready_drop = g_diag_ready_drop;
    *sleep_drop = g_diag_sleep_drop;
}

int pot_sched_preempt_enabled(void)
{
    return g_preempt_enabled;
}

void pot_sched_set_preempt(int on)
{
    g_preempt_enabled = on;
}
