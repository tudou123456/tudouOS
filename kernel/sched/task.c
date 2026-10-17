#include <stdint.h>

#include "intc.h"
#include "smp.h"
#include "task.h"
#include "tick.h"
#include "time.h"

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
void sched_static_enqueue(pot_task_t *t);     /* sched.c */
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

void pot_task_register(pot_task_t *t)
{
    for (volatile int i = 0; i < g_reg_n; i++) {
        if (g_registry[i] == t) {
            return; /* 已登记（TCB 复用场景） */
        }
    }
    if (g_reg_n >= REG_MAX) {
        g_reg_overflow++;
        return;
    }
    g_registry[g_reg_n++] = t;
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

    pot_task_register(tcb);
    sched_static_enqueue(tcb);
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
