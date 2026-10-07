#include <stdint.h>
#include <stddef.h>

#include "intc.h"
#include "task.h"
#include "tick.h"
#include "time.h"
#include "trace.h"
/*
 * 土豆调度器（M3）——三分类优先级域 + 轮转 + 时间片 + 老化防饥饿 + 后台预算。
 *
 * 切换核心（统一 IRQ 帧制，v2）：
 *  - 任何切换都发生在异常上下文（tick 抢占或任务阻塞 syscall）
 *  - 被切出任务的异常帧整体挂到其 TCB（sp 指向帧基址）
 *  - 恢复一律 pot_do_resume（rfe，从帧取 epc1/eps1）
 *  - 任务首启 = 伪造帧（epc1=蹦床，eps1=常态 PS，a2=fn，a3=arg）
 * 教训（v1 链表版）：链表环 + 双帧恢复导致任务凭空消失，容器改定宽数组、
 * 恢复统一走 rfe。
 */

pot_task_t *volatile pot_current;
uint32_t pot_task_ps;      /* 任务常态 PS（sched_init 捕获） */
uint32_t pot_task_ps_il1;  /* 常态 PS + INTLEVEL=1（切换退出窗口屏蔽 tick） */

/* 切换意图（syscall 魔数，switch.S 定义同值） */
#define MAGIC_YIELD 0x50534C01u
#define MAGIC_SLEEP 0x50534C02u
#define MAGIC_EXIT  0x50534C03u

#define SLOTS 8

static pot_task_t *g_ready[POTCLS_IDLE + 1][SLOTS];
static int g_ready_n[POTCLS_IDLE + 1];
static pot_task_t *g_sleep[SLOTS];
static int g_sleep_n;

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
        /* v4：纯忙轮询推进 tick/唤醒（无 syscall——自切换会以百万次/秒
           的异常饿死 QEMU 主循环，UART TX 停摆）；仅当有其它任务就绪
           才 yield 让路 */
        pot_tick_poll();
        if (pot_sched_others_ready()) {
            pot_task_yield();
        }
    }
}

static pot_task_t g_idle_tcb;
static uint32_t g_idle_stack[192];

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

void pot_task_register(pot_task_t *t); /* task.c */

/* task.c 创建任务时的入队口 */
void sched_static_enqueue(pot_task_t *t)
{
    enqueue_ready(t);
}

/* 诊断计数导出（TODO(M3 debug): 稳定后移除） */
void pot_sched_diag(uint32_t *out)
{
    out[0] = g_diag_wake;
    out[1] = g_diag_preempt;
    out[2] = g_diag_switch;
    out[3] = g_diag_idle;
}
