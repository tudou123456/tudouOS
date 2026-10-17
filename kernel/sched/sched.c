#include <stdint.h>
#include <stddef.h>

#include "intc.h"
#include "potato.h"
#include "smp.h"
#include "task.h"
#include "tick.h"
#include "time.h"
#include "trace.h"
#include "uart.h"
/*
 * 土豆调度器（M3 v5，M9 双核化）——三分类优先级域 + 轮转 + 时间片 +
 * 老化防饥饿 + 后台预算。
 *
 * 切换核心（v5）：
 *  - tick 走 int6 中断驱动（记账/唤醒/时间推进），检查点 syscall 切换保留
 *  - 抢占切换完整接线（IRQ 帧 + rfe，preempt on|off，QEMU 模型缺陷默认关）
 *  - 一切切换都发生在异常上下文（EXCM=1，天然互斥）
 *  - 恢复统一 rfe：EPC1=续体、SR194=常态 PS（E-02 实证 rfe 取数槽）
 *  - 任务首启 = 伪造帧（a0=蹦床，a2=fn，a3=arg）
 * 任务态非重入子系统（堆/potfs/消息环/追踪环）均有 rsil1 临界区（D-04）。
 *
 * M9 双核扩展（默认单核，CPU1 上电前行为与 M8 逐位一致）：
 *  - 每核一套：当前任务 g_cur[cpu]、就绪队列 g_ready[cpu][cls][slot]、
 *    时间片 g_quantum[cpu]、切换计数 g_switches[cpu]（THREADPTR 取核号）
 *  - 睡眠表/全局时钟 g_now/后台预算窗口仍为全局单份（CPU0 tick 独占推进）
 *  - 跨核就绪改动自动补 IPI（RESCHED/WAKE），对核 IPI/tick 触发重调度
 *  - 复合容器操作由 pot_smp_sched_lock()（S32C1I + rsil 15）串行化；
 *    单核路径下等价于 rsil 15，不引入额外语义
 *  - CPU1 只跑分给它的任务；CPU1 tick 不做全局时间推进（见 smp.c）
 */

pot_task_t *volatile pot_current; /* CPU0 视图（兼容旧引用） */
uint32_t pot_task_ps;      /* 任务常态 PS（sched_init 捕获） */
uint32_t pot_task_ps_il1;  /* 常态 PS + INTLEVEL=1（切换退出窗口屏蔽 tick） */

/* 切换意图（syscall 魔数，switch.S 定义同值） */
#define MAGIC_YIELD 0x50534C01u
#define MAGIC_SLEEP 0x50534C02u
#define MAGIC_EXIT  0x50534C03u

#define SLOTS 16 /* D-11：就绪/睡眠表扩容（msgdemo 双任务+mcp+demo 三任务
                     +churn+main 并存时 8 槽已不够），溢出由计数暴露 */

/* M9：每核一份就绪队列（定宽数组，杜绝链表环） */
static pot_task_t *g_cur[POT_SMP_MAX_CPUS];
static pot_task_t *g_ready[POT_SMP_MAX_CPUS][POTCLS_IDLE + 1][SLOTS];
static int g_ready_n[POT_SMP_MAX_CPUS][POTCLS_IDLE + 1];

/* 全局单份：睡眠表（到期由 CPU0 tick 统一唤醒）、诊断计数 */
static pot_task_t *g_sleep[SLOTS];
static int g_sleep_n;
static uint32_t g_diag_ready_drop; /* D-11：就绪表溢出丢任务计数（应为 0） */
static uint32_t g_diag_sleep_drop; /* D-11：睡眠表溢出丢任务计数（应为 0） */

static volatile uint32_t g_now;
static uint32_t g_quantum[POT_SMP_MAX_CPUS];
static volatile uint32_t g_need_sched;
static uint32_t g_total_switches;
static uint32_t g_switches[POT_SMP_MAX_CPUS];
static uint32_t g_bg_used;
static uint32_t g_win_start;

/* 诊断计数（TODO(M3 debug): 稳定后移除） */
volatile uint32_t g_diag_wake, g_diag_preempt, g_diag_switch, g_diag_idle;

_Static_assert(offsetof(pot_task_t, sp) == 4, "switch.S TCB_SP_OFF");

/* 本核身份（每核私有 THREADPTR，见 smp.c） */
static uint32_t cpu_id(void)
{
    return pot_smp_cpu_id();
}

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

/* ---------- 就绪/睡眠容器（调用方持锁） ---------- */

static int slot_of(const pot_task_t *t)
{
    return (t->cls == POTCLS_IDLE) ? POTCLS_IDLE : (int)t->cls;
}

static void enqueue_ready(uint32_t cpu, pot_task_t *t)
{
    int c = slot_of(t);
    if (g_ready_n[cpu][c] >= SLOTS) {
        g_diag_ready_drop++; /* D-11：不再静默——计数暴露（tasks 命令可见） */
        if (cpu == 0u) {
            trace_push(TR_SCHED_DROP, (uint32_t)t, (uint32_t)c);
        }
        return;
    }
    for (volatile int i = 0; i < g_ready_n[cpu][c]; i++) {
        if (g_ready[cpu][c][i] == t) {
            return; /* 已在表（防御） */
        }
    }
    g_ready[cpu][c][g_ready_n[cpu][c]] = t;
    g_ready_n[cpu][c]++;
}

static void dequeue_ready(uint32_t cpu, pot_task_t *t)
{
    int c = slot_of(t);
    for (volatile int i = 0; i < g_ready_n[cpu][c]; i++) {
        if (g_ready[cpu][c][i] == t) {
            for (volatile int j = i; j < g_ready_n[cpu][c] - 1; j++) {
                g_ready[cpu][c][j] = g_ready[cpu][c][j + 1];
            }
            g_ready_n[cpu][c]--;
            return;
        }
    }
}

static void sleep_remove_at(int i)
{
    for (volatile int j = i; j < g_sleep_n - 1; j++) {
        g_sleep[j] = g_sleep[j + 1];
    }
    g_sleep_n--;
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

/* ---------- 优先级与挑选（调用方持锁或只读快照） ---------- */

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

static pot_task_t *pick_best(uint32_t cpu)
{
    pot_task_t *best = 0;
    int best_p = 99;
    int best_aged = 0;
    for (int c = 0; c <= POTCLS_IDLE; c++) {
        for (int i = 0; i < g_ready_n[cpu][c]; i++) {
            pot_task_t *t = g_ready[cpu][c][i];
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

static void switch_account(pot_task_t *from, pot_task_t *next)
{
    uint32_t me = cpu_id();
    g_total_switches++;
    g_switches[me]++;
    g_diag_switch++;
    next->switches++;
    next->state = POTTASK_RUNNING;
    next->last_run_tick = g_now;
    next->cpu = (volatile int)me;
    g_quantum[me] = POT_SCHED_QUANTUM_TICKS;
    g_cur[me] = next;
    if (me == 0u) {
        g_need_sched = 0;
        /* 追踪环是 CPU0 单写者（M9 约束）：CPU1 的切换只计数不入环 */
        trace_push(TR_SCHED_SWITCH, (uint32_t)from, (uint32_t)next);
        pot_current = next;
    }
}

/* intent: MAGIC_YIELD/SLEEP/EXIT；until 仅 SLEEP 用 */
void pot_sched_switch_from_exception(void *frame_base, uint32_t magic,
                                     uint32_t until)
{
    uint32_t me = cpu_id();
    pot_task_t *from = g_cur[me];
    if (from == 0) {
        return; /* 防御：未挂接 CPU（不应发生） */
    }
    from->sp = (uint32_t *)frame_base;
    from->frame_kind = POT_FRAME_CALL; /* syscall 帧：a0 槽续体，ret.n 恢复 */

    uint32_t ps = pot_smp_sched_lock();
    if (magic == MAGIC_SLEEP) {
        from->sleep_until = until;
        from->state = POTTASK_SLEEPING;
        sleep_add(from);
    } else if (magic == MAGIC_EXIT) {
        from->state = POTTASK_DEAD;
    } else { /* YIELD / PREEMPT */
        from->state = POTTASK_READY;
        enqueue_ready(me, from);
    }

    pot_task_t *next = pick_best(me);
    if (next == 0) {
        next = from; /* idle 兜底，防御 */
    }
    dequeue_ready(me, next);
    switch_account(from, next);
    pot_smp_sched_unlock(ps);
    pot_do_resume(next); /* 不返回 */
}

/* ---------- D-03 v5：tick 抢占（IRQ 帧 + rfe，E-02 实证路线） ---------- */

void pot_do_resume_irq(pot_task_t *t); /* switch.S */

void pot_sched_preempt_from_irq_cpu(uint32_t cpu, void *frame_base)
{
    pot_task_t *from = g_cur[cpu];
    if (from == 0) {
        return;
    }
    from->sp = (uint32_t *)frame_base;
    from->frame_kind = POT_FRAME_IRQ;

    uint32_t ps = pot_smp_sched_lock();
    from->state = POTTASK_READY;
    enqueue_ready(cpu, from);
    if (cpu == 0u) {
        g_diag_preempt++;
    }
    pot_task_t *next = pick_best(cpu);
    if (next == 0) {
        next = from; /* 防御 */
    }
    dequeue_ready(cpu, next);
    switch_account(from, next);
    pot_smp_sched_unlock(ps);
    if (next->frame_kind == POT_FRAME_IRQ) {
        pot_do_resume_irq(next); /* rfe 恢复（不返回） */
    }
    pot_do_resume(next); /* 首启/syscall 帧：ret.n 恢复（不返回） */
}

void pot_sched_preempt_from_irq(void *frame_base)
{
    pot_sched_preempt_from_irq_cpu(0, frame_base);
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

/* 任务的就绪目标核：显式亲和优先，否则回最近运行核（M9） */
static uint32_t target_cpu_of(const pot_task_t *t)
{
    if (t->affinity >= 0) {
        return (uint32_t)t->affinity;
    }
    return (t->cpu >= 0) ? (uint32_t)t->cpu : 0u;
}

/* 显式唤醒：从睡眠表移除并入就绪表（空转/协作唤醒，醒来者在检查点重查条件）。
 * 非睡眠态为无害空操作。返回 1=已唤醒 */
int pot_sched_wake(pot_task_t *t)
{
    if (t == 0) {
        return 0;
    }
    uint32_t me = cpu_id();
    uint32_t dst = 0;
    uint32_t ps = pot_smp_sched_lock();
    for (volatile int i = 0; i < g_sleep_n; i++) {
        if (g_sleep[i] == t) {
            sleep_remove_at(i);
            t->state = POTTASK_READY;
            dst = target_cpu_of(t);
            t->cpu = (volatile int)dst;
            enqueue_ready(dst, t);
            pot_smp_sched_unlock(ps);
            if (dst != me) {
                pot_smp_ipi_send(dst, POT_IPI_WAKE);
            }
            return 1;
        }
    }
    pot_smp_sched_unlock(ps);
    return 0;
}

/* ---------- tick 钩子（v3：仅记账+唤醒，切换在任务检查点） ---------- */

/* M9：分核 tick 记账（CPU0 由 pot_sched_tick 调，CPU1 由 smp.c 调）。
 * 全局时钟/后台预算窗口只由 CPU0 pot_sched_tick 推进。 */
void pot_sched_cpu_tick_account(uint32_t cpu, uint32_t elapsed)
{
    if (cpu >= POT_SMP_MAX_CPUS) {
        return;
    }
    pot_task_t *c = g_cur[cpu];
    if (c != 0) {
        c->cpu_ticks += elapsed;
        if (c->cls == POTCLS_BACKGROUND) {
            g_bg_used += elapsed;
        }
    }
    if (g_quantum[cpu] > elapsed) {
        g_quantum[cpu] -= elapsed;
    } else {
        g_quantum[cpu] = 0;
    }
}

void pot_sched_tick(uint32_t elapsed)
{
    g_now += elapsed;
    pot_sched_cpu_tick_account(0, elapsed);

    if ((g_now - g_win_start) >= POT_SCHED_WINDOW_TICKS) {
        g_win_start += POT_SCHED_WINDOW_TICKS;
        g_bg_used = 0;
    }

    pot_task_t *c = g_cur[0];
    int woke_better = 0;
    int need_ipi = 0;
    int cur_p = (c != 0) ? eff_prio(c) : 0;

    uint32_t ps = pot_smp_sched_lock();
    for (volatile int i = 0; i < g_sleep_n; ) {
        pot_task_t *t = g_sleep[i];
        if ((int32_t)(g_now - t->sleep_until) >= 0) {
            sleep_remove_at(i);
            t->state = POTTASK_READY;
            uint32_t dst = target_cpu_of(t);
            t->cpu = (volatile int)dst;
            enqueue_ready(dst, t);
            g_diag_wake++;
            if (dst != 0u) {
                need_ipi = 1; /* 唤醒落在 CPU1：补 IPI 让它重调度 */
            } else if (eff_prio(t) <= cur_p) {
                woke_better = 1;
            }
        } else {
            i++;
        }
    }
    pot_smp_sched_unlock(ps);

    if (need_ipi) {
        pot_smp_ipi_send(1, POT_IPI_WAKE);
    }

    /* v3：need_sched 仅用于统计观察；切换在任务检查点发生 */
    if (woke_better) {
        g_need_sched = 1;
    }
    if (c != 0 && c->cls == POTCLS_BACKGROUND &&
        g_bg_used >= POT_SCHED_BG_BUDGET) {
        g_need_sched = 1;
    }

    /* M9：负载均衡（smp.c 内部限频；CPU1 未上线时立即返回） */
    pot_smp_balance_tick();
}

/* ---------- 初始化 ---------- */

void pot_sched_init(void)
{
    asm volatile("rsr.ps %0" : "=r"(pot_task_ps));
    pot_task_ps_il1 = pot_task_ps | 1u; /* INTLEVEL=1：屏蔽 level-1 tick */

    g_cur[0] = &g_main_tcb;
    pot_current = &g_main_tcb;
    g_main_tcb.last_run_tick = 0;
    g_main_tcb.stk_lo = (uint32_t *)_stack_bottom; /* D-13：主任务栈水位 */
    g_main_tcb.stk_words = (uint32_t)(_stack_top - _stack_bottom) / 4u;
    g_main_tcb.affinity = 0; /* 主任务固定 CPU0 */
    {
        /* 主任务正在本栈上运行：只涂当前栈针以下的空闲区
           （局部变量地址近似 SP，留 16 词余量） */
        volatile uint32_t anchor = 0;
        uint32_t below = (uint32_t)((uint32_t *)&anchor - g_main_tcb.stk_lo);
        if (below > 16u) {
            pot_task_stack_paint(g_main_tcb.stk_lo, below - 16u);
        }
    }
    g_quantum[0] = POT_SCHED_QUANTUM_TICKS;
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

uint32_t pot_sched_cpu_switches(uint32_t cpu)
{
    return (cpu < (uint32_t)POT_SMP_MAX_CPUS) ? g_switches[cpu] : 0u;
}

pot_task_t *pot_sched_cpu_current(uint32_t cpu)
{
    return (cpu < (uint32_t)POT_SMP_MAX_CPUS) ? g_cur[cpu] : 0;
}

uint32_t pot_sched_bg_used(void)
{
    return g_bg_used;
}

/* idle 用：本核是否有非 idle 任务就绪（决定要不要 yield 让路） */
int pot_sched_others_ready(void)
{
    return pot_sched_has_ready(cpu_id());
}

/* ---------- M9：双核调度接口 ---------- */

pot_task_t *pot_sched_self(void)
{
    return g_cur[cpu_id()];
}

int pot_sched_has_ready(uint32_t cpu)
{
    if (cpu >= (uint32_t)POT_SMP_MAX_CPUS) {
        return 0;
    }
    uint32_t ps = pot_smp_sched_lock();
    int n = (g_ready_n[cpu][0] | g_ready_n[cpu][1] | g_ready_n[cpu][2]) != 0;
    pot_smp_sched_unlock(ps);
    return n;
}

int pot_sched_ready_count(uint32_t cpu)
{
    if (cpu >= (uint32_t)POT_SMP_MAX_CPUS) {
        return 0;
    }
    uint32_t ps = pot_smp_sched_lock();
    int n = g_ready_n[cpu][0] + g_ready_n[cpu][1] + g_ready_n[cpu][2];
    pot_smp_sched_unlock(ps);
    return n;
}

int pot_sched_cpu_running_nonidle(uint32_t cpu)
{
    if (cpu >= (uint32_t)POT_SMP_MAX_CPUS) {
        return 0;
    }
    pot_task_t *c = g_cur[cpu];
    return (c != 0 && c->cls != POTCLS_IDLE) ? 1 : 0;
}

pot_task_t *pot_sched_take_ready(uint32_t cpu)
{
    pot_task_t *t = 0;
    if (cpu >= (uint32_t)POT_SMP_MAX_CPUS) {
        return 0;
    }
    uint32_t ps = pot_smp_sched_lock();
    pot_task_t *best = pick_best(cpu);
    if (best != 0 && best->cls != POTCLS_IDLE) {
        dequeue_ready(cpu, best);
        t = best;
    }
    pot_smp_sched_unlock(ps);
    return t;
}

/* 取一个可迁移（affinity < 0）的就绪任务。负载均衡不能直接抢队首：
 * 队首往往是绑核的 main/控制台任务，抢了再放回会永远原地打转。 */
pot_task_t *pot_sched_take_ready_movable(uint32_t cpu)
{
    pot_task_t *t = 0;
    if (cpu >= (uint32_t)POT_SMP_MAX_CPUS) {
        return 0;
    }
    uint32_t ps = pot_smp_sched_lock();
    for (int c = 0; c <= POTCLS_IDLE && t == 0; c++) {
        for (int i = 0; i < g_ready_n[cpu][c]; i++) {
            pot_task_t *cand = g_ready[cpu][c][i];
            if (cand != 0 && cand->cls != POTCLS_IDLE &&
                cand->affinity < 0) {
                dequeue_ready(cpu, cand);
                t = cand;
                break;
            }
        }
    }
    pot_smp_sched_unlock(ps);
    return t;
}

void pot_sched_put_ready(uint32_t cpu, pot_task_t *t)
{
    if (t == 0 || cpu >= (uint32_t)POT_SMP_MAX_CPUS) {
        return;
    }
    uint32_t me = cpu_id();
    uint32_t ps = pot_smp_sched_lock();
    t->state = POTTASK_READY;
    t->cpu = (volatile int)cpu;
    enqueue_ready(cpu, t);
    pot_smp_sched_unlock(ps);
    if (cpu != me) {
        pot_smp_ipi_send(cpu, POT_IPI_RESCHED);
    }
}

void pot_sched_attach_cpu(uint32_t cpu, pot_task_t *tcb)
{
    if (tcb == 0 || cpu >= (uint32_t)POT_SMP_MAX_CPUS) {
        return;
    }
    uint32_t ps = pot_smp_sched_lock();
    g_cur[cpu] = tcb;
    tcb->state = POTTASK_RUNNING;
    tcb->cpu = (volatile int)cpu;
    tcb->last_run_tick = g_now;
    g_quantum[cpu] = POT_SCHED_QUANTUM_TICKS;
    pot_smp_sched_unlock(ps);
}

/* ---------- D-03 v5：抢占决策 ----------
 * 返回 1 = 中断应切换（更高有效优先级就绪，或时间片耗尽）。
 * 打印原子性约束：UART 输出按行组装（逐字节轮询 TX），若在行中切换，
 * 另一任务的输出会插进半行——控制台/演示任务的行锚点全靠 [POTATO]
 * 前缀逐行断言，故行组装期间（uart 行忙）推迟抢占到下一个 tick。 */
/* D-03 诊断开关：0=禁用 tick 抢占切换（中断 tick 仍记账/唤醒，
 * 切换只发生在检查点）——`preempt off|on` 命令控制。
 * 默认 0：QEMU esp32s3 模型在"genex 上下文切走 + CCOMPARE 活动"组合下
 * 会随机丢 tick（M3 三版设计冻结的同源模型缺陷，E-02 已排除寄存器
 * 用法因素，看门狗只能救时钟救不了任务重复执行）；真机（TRM 一致）
 * 上应默认开启。M9：CPU1 tick 抢占另有 g_cpu1_preempt（smp.c） */
static volatile int g_preempt_enabled = 0;

int pot_sched_should_preempt_cpu(uint32_t cpu)
{
    if (cpu >= (uint32_t)POT_SMP_MAX_CPUS) {
        return 0;
    }
    pot_task_t *c = g_cur[cpu];
    if (c == 0) {
        return 0;
    }
    if (c->cls == POTCLS_IDLE && pot_sched_ready_count(cpu) == 0) {
        return 0; /* 无人就绪：连轮转都不需要 */
    }
    if (cpu == 0u && uart_tx_midline()) {
        return 0; /* 行组装中：推迟（下一个 tick 再试，最多 ~1.4ms） */
    }
    pot_task_t *best = pick_best(cpu);
    if (best == 0 && c->cls != POTCLS_IDLE) {
        return 0;
    }
    if (c->cls == POTCLS_IDLE) {
        return 1; /* idle 让路 */
    }
    if (g_quantum[cpu] == 0u) {
        return 1; /* 时间片耗尽且有任务等着：轮转/让位 */
    }
    return (best != 0) && (eff_prio(best) < eff_prio(c));
}

int pot_sched_should_preempt(void)
{
    if (!g_preempt_enabled) {
        return 0;
    }
    return pot_sched_should_preempt_cpu(0);
}

void pot_task_register(pot_task_t *t); /* task.c */

/* task.c 创建任务时的入队口（M9：绑核任务直接进目标核队列并补 IPI） */
void sched_static_enqueue(pot_task_t *t)
{
    uint32_t me = cpu_id();
    uint32_t dst;
    uint32_t ps = pot_smp_sched_lock(); /* D-04：与 tick IRQ 的队列操作互斥 */
    dst = (t->affinity >= 0) ? (uint32_t)t->affinity : me;
    t->state = POTTASK_READY;
    t->cpu = (volatile int)dst;
    enqueue_ready(dst, t);
    pot_smp_sched_unlock(ps);
    if (dst != me) {
        pot_smp_ipi_send(dst, POT_IPI_RESCHED);
    }
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

/* M9：IPI/CPU1 tick 触发的"必要时重调度"入口。
 * CPU0 侧：只在 preempt on 时切换（保持单核默认行为，QEMU 模型缺陷规避）；
 * 关时仅置 need_sched 由检查点处理。CPU1 侧总是允许（该核只为 SMP 服务）。 */
void pot_sched_resched_from_irq(void *frame_base)
{
    uint32_t me = cpu_id();
    if (g_cur[me] == 0) {
        return;
    }
    if (me == 0u && !g_preempt_enabled) {
        g_need_sched = 1;
        return;
    }
    if (!pot_sched_should_preempt_cpu(me)) {
        return;
    }
    pot_sched_preempt_from_irq_cpu(me, frame_base);
}
