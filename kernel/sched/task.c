#include <stdint.h>

#include "intc.h"
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

/* 任务注册表：main/idle/动态任务全登记，tasks 命令遍历打印 */
#define REG_MAX 8
static pot_task_t *g_registry[REG_MAX];
static int g_reg_n;

void pot_task_register(pot_task_t *t)
{
    if (g_reg_n < REG_MAX) {
        g_registry[g_reg_n++] = t;
    }
}

int pot_task_count(void)
{
    return g_reg_n;
}

pot_task_t *pot_task_at(int i)
{
    return (i >= 0 && i < g_reg_n) ? g_registry[i] : 0;
}

int pot_task_create(pot_task_t *tcb, const char *name, pot_task_class_t cls,
                    pot_task_fn_t fn, void *arg,
                    uint32_t *stack, uint32_t stack_words)
{
    if (tcb == 0 || stack == 0 || stack_words < FRAME_WORDS + 8) {
        return -1;
    }

    /* 栈顶 16 字节对齐后留出首启帧 */
    uint32_t top = (uint32_t)(stack + stack_words) & ~15u;
    uint32_t *frame = (uint32_t *)(top - FRAME_WORDS * 4);

    for (int i = 0; i < FRAME_WORDS; i++) {
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

    pot_task_register(tcb);
    sched_static_enqueue(tcb);
    return 0;
}
pot_task_t *pot_task_self(void)
{
    return pot_current;
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
