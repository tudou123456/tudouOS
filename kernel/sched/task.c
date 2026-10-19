#include <stdint.h>

#include "intc.h"
#include "smp.h"
#include "task.h"
#include "tick.h"
#include "time.h"
#include "trace.h"

/*
 * 任务 API（M3，统一 IRQ 帧制）：
 * 阻塞/让出/退出都经 syscall 进入异常上下文，由调度器统一处理。
 * 栈由调用方静态提供；任务函数返回视为退出。
 */

extern void pot_task_trampoline(void);        /* switch.S */
void pot_sched_syscall_yield(void);           /* switch.S */
void pot_sched_syscall_sleep(uint32_t until); /* switch.S */
void pot_sched_syscall_exit(void);            /* switch.S */
void pot_task_register(pot_task_t *t);
int sched_static_enqueue(pot_task_t *t);      /* sched.c：0=ok -1=队列满 */
uint32_t pot_sched_now(void);

extern uint32_t pot_task_ps; /* sched.c：任务常态 PS */

#define FRAME_WORDS 22 /* 88 字节，见 switch.S/vectors.S 帧布局 */

/* 任务注册表：main/idle/动态任务全登记，tasks 命令遍历打印。
 * D-12：登记前去重（TCB 复用时不得重复追加），表满计数告警。
 * M9：SMP 演示任务/CPU1 idle 也登记，容量 16→24（de） */
#define REG_MAX 24
static pot_task_t *g_registry[REG_MAX];
static int g_reg_n;
static uint32_t g_reg_overflow; /* 登记溢出计数（tasks 命令可见，应为 0） */
static pot_lock_t g_reg_lock;   /* P2-8：CPU1 也会登记任务（idle/演示任务） */
static uint32_t g_stack_guard_faults;

void pot_task_register(pot_task_t *t)
{
    uint32_t ps = pot_lock_acquire(&g_reg_lock);
    for (volatile int i = 0; i < g_reg_n; i++) {
        if (g_registry[i] == t) {
            pot_lock_release(&g_reg_lock, ps);
            return; /* 已登记（TCB 复用场景） */
        }
    }
    if (g_reg_n >= REG_MAX) {
        g_reg_overflow++;
        pot_lock_release(&g_reg_lock, ps);
        return;
    }
    g_registry[g_reg_n++] = t;
    pot_lock_release(&g_reg_lock, ps);
}

uint32_t pot_task_reg_overflow(void)
{
    return g_reg_overflow;
}

int pot_task_count(void)
{
    return g_reg_n;
}

pot_task_t *pot_task_at(int i)
{
    return (i >= 0 && i < g_reg_n) ? g_registry[i] : 0;
}

uint32_t pot_task_stack_guard_faults(void)
{
    return g_stack_guard_faults;
}

/* 切换时校验栈底哨兵：命中即计数、重新布防并留追踪事件（P2-3）。
 * 只读/写栈底之外的空闲字，不影响当前栈帧。 */
void pot_task_stack_guard_check(pot_task_t *t)
{
    if (t == 0 || t->stk_lo == 0 || t->stk_words < 2u) {
        return;
    }
    if (t->stk_lo[0] != POT_STACK_GUARD) {
        g_stack_guard_faults++;
        if (pot_smp_cpu_id() == 0u) { /* 追踪环 CPU0 单写者 */
            trace_push(TR_MEM_FAULT, 5 /* stack guard */, (uint32_t)t);
        }
        t->stk_lo[0] = POT_STACK_GUARD; /* 重新布防，避免每次切换重复刷屏 */
    }
}

/* 栈水位（D-13）：创建时填充花样，tasks 命令扫描从未触碰的连续字数 */
void pot_task_stack_paint(uint32_t *stack, uint32_t words)
{
    for (volatile uint32_t i = 0; i < words; i++) {
        stack[i] = POT_STACK_PATTERN;
    }
}

uint32_t pot_task_stack_unused(const pot_task_t *t)
{
    if (t == 0 || t->stk_lo == 0 || t->stk_words == 0) {
        return 0;
    }
    uint32_t n = 0;
    while (n < t->stk_words && t->stk_lo[n] == POT_STACK_PATTERN) {
        n++;
    }
    return n;
}

static int task_create_common(pot_task_t *tcb, const char *name,
                              pot_task_class_t cls, pot_task_fn_t fn, void *arg,
                              uint32_t *stack, uint32_t stack_words,
                              int cpu_affinity)
{
    if (tcb == 0 || stack == 0 || stack_words < FRAME_WORDS + 8) {
        return -1;
    }
    if (cpu_affinity < -1 || cpu_affinity >= POT_SMP_MAX_CPUS) {
        return -1;
    }

    /* 栈顶 16 字节对齐后留出首启帧；其余填充水位花样 */
    uint32_t top = (uint32_t)(stack + stack_words) & ~15u;
    uint32_t *frame = (uint32_t *)(top - FRAME_WORDS * 4);
    pot_task_stack_paint(stack, (uint32_t)(frame - stack));
    stack[0] = POT_STACK_GUARD; /* P2-3：栈底哨兵（最后一个布，防被 paint 覆盖） */

    for (volatile int i = 0; i < FRAME_WORDS; i++) {
        frame[i] = 0;
    }
    frame[0] = (uint32_t)pot_task_trampoline; /* a0：首启 PC */
    frame[1] = (uint32_t)fn;                   /* a2 */
    frame[2] = (uint32_t)arg;                  /* a3 */

    tcb->next = 0;
    tcb->sp = frame;
    tcb->state = POTTASK_READY;
    tcb->frame_kind = 0;
    tcb->cls = cls;
    tcb->name = name;
    tcb->cpu_ticks = 0;
    tcb->switches = 0;
    tcb->sleep_until = 0;
    tcb->last_run_tick = pot_sched_now();
    tcb->stop_req = 0;
    tcb->stk_lo = stack;
    tcb->stk_words = stack_words;
    tcb->cpu = -1;
    tcb->affinity = cpu_affinity;

    /* P1-3：入队失败（就绪表满）必须让创建方看到失败，不得留下 READY 幽灵 */
    if (sched_static_enqueue(tcb) != 0) {
        tcb->state = POTTASK_DEAD;
        return -1;
    }
    pot_task_register(tcb);
    return 0;
}

int pot_task_create(pot_task_t *tcb, const char *name, pot_task_class_t cls,
                    pot_task_fn_t fn, void *arg,
                    uint32_t *stack, uint32_t stack_words)
{
    /* 默认跟随 CPU0：M9 前所有任务都在 CPU0 上跑，保持单核语义不变；
     * 只有显式 pot_task_create_aff(..., -1) 的任务才允许被负载均衡迁移。 */
    return task_create_common(tcb, name, cls, fn, arg, stack, stack_words, 0);
}

int pot_task_create_aff(pot_task_t *tcb, const char *name, pot_task_class_t cls,
                        pot_task_fn_t fn, void *arg,
                        uint32_t *stack, uint32_t stack_words,
                        int cpu_affinity)
{
    return task_create_common(tcb, name, cls, fn, arg, stack, stack_words,
                              cpu_affinity);
}
pot_task_t *pot_task_self(void)
{
    return pot_sched_self(); /* M9：按 THREADPTR 取本核当前任务 */
}

void pot_task_yield(void)
{
    pot_tick_poll(); /* v4：检查点先推进 tick（唤醒/记账） */
    pot_sched_syscall_yield();
}

void pot_task_sleep(uint32_t ms)
{
    pot_tick_poll();
    pot_sched_syscall_sleep(pot_sched_now() + ms);
}

void pot_task_exit(void)
{
    pot_sched_syscall_exit();
    for (;;) {
    }
}
