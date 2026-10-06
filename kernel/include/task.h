#ifndef TUDOU_TASK_H
#define TUDOU_TASK_H

/* 帧类型：switch.S 亦引用（纯数字宏，汇编可用） */
#define POT_FRAME_CALL 0
#define POT_FRAME_IRQ  1

#ifndef __ASSEMBLER__

#include <stdint.h>

/*
 * 土豆内核任务模型（M3，支柱4：三分类低抖动调度）
 *
 * 任务分类（优先级域，0 最高）：
 *   POTCLS_AI_COMMS   AI 通信类——响应稳定低延迟
 *   POTCLS_UI         UI 交互类——顺滑
 *   POTCLS_BACKGROUND 后台运算类——限速、可被压制
 * 调度规则：
 *   - 恒取最高优先级就绪任务，同级轮转
 *   - 时间片：5 tick；更高优先级就绪立即抢占（tick 驱动）
 *   - 防饥饿老化：任何任务连续 200ms 未运行 → 临时提升到最高域
 *   - 后台预算：滚动 1000 tick 窗口内后台最多消耗 700 tick（限速率，
 *     给系统留余量；老化提升优先于预算，防饿死）
 *
 * 上下文切换（call0 ABI，双帧制，帧布局见 switch.S）：
 *   - 阻塞/让出：调用帧（ret 恢复）
 *   - tick 抢占：IRQ 帧（含 EPC1/EPS1，rfe 恢复）
 *   - 任务级 pot_sched 恢复 IRQ 帧任务时经 syscall 进入异常上下文统一处理
 */

#define POT_TASK_NAME_MAX 12
#define POT_SCHED_QUANTUM_TICKS 5
#define POT_SCHED_AGING_MS      200
#define POT_SCHED_BG_BUDGET     700u /* 每 1000 tick */
#define POT_SCHED_WINDOW_TICKS  1000u

typedef enum {
    POTCLS_AI_COMMS = 0,
    POTCLS_UI = 1,
    POTCLS_BACKGROUND = 2,
    POTCLS_IDLE = 3, /* 仅 idle 任务，永不参与预算/老化 */
} pot_task_class_t;

typedef enum {
    POTTASK_READY = 0,
    POTTASK_RUNNING = 1,
    POTTASK_SLEEPING = 2,
    POTTASK_DEAD = 3,
} pot_task_state_t;

/* 帧类型：0=调用帧 1=IRQ 帧（见 switch.S 注释）——已前置供汇编引用 */

typedef struct pot_task {
    struct pot_task *next;   /* 就绪/睡眠链表 */
    uint32_t *sp;            /* 保存的栈帧基址（帧在各自栈顶） */
    volatile int state;
    int frame_kind;
    pot_task_class_t cls;
    const char *name;
    uint32_t cpu_ticks;      /* 累计占用（按补记 tick 记账） */
    uint32_t switches;       /* 被切入次数 */
    uint32_t sleep_until;    /* 唤醒时刻（tick） */
    uint32_t last_run_tick;  /* 老化依据：最近一次运行时刻 */
    volatile int stop_req;   /* demo 任务停止请求 */
} pot_task_t;

typedef void (*pot_task_fn_t)(void *arg);

/* 创建任务（栈由调用方静态提供，M4 引入堆后可加动态版） */
int pot_task_create(pot_task_t *tcb, const char *name, pot_task_class_t cls,
                    pot_task_fn_t fn, void *arg,
                    uint32_t *stack, uint32_t stack_words);
void pot_task_exit(void);          /* 任务自尽（不返回） */
void pot_task_yield(void);         /* 主动让出 */
void pot_task_sleep(uint32_t ms);  /* 睡眠指定毫秒（tick 驱动唤醒） */
pot_task_t *pot_task_self(void);

/* 调度器（sched.c 内部为主，这里暴露给 tick/异常路径） */
extern pot_task_t *volatile pot_current;

void pot_sched_init(void);
void pot_sched_tick(uint32_t elapsed);   /* tick 补记后调用（记账+唤醒） */
int  pot_sched_try_syscall(void *frame_base); /* 调度 syscall 识别+切换（不返回） */
int  pot_sched_others_ready(void);            /* 有非 idle 就绪任务（idle 让路判据） */
void pot_sched_diag(uint32_t *out);           /* 诊断计数 [wake,preempt,switch,idle] */

/* 统计（console/测试用） */
uint32_t pot_sched_now(void);
uint32_t pot_sched_total_switches(void);
uint32_t pot_sched_bg_used(void);

/* 任务注册表（main/idle/动态创建都登记，tasks 命令遍历） */
void pot_task_register(pot_task_t *t);
int pot_task_count(void);
pot_task_t *pot_task_at(int i);

const char *pot_class_name(pot_task_class_t c);

#endif /* __ASSEMBLER__ */

#endif /* TUDOU_TASK_H */
