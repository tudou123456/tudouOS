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
 *   - 时间片：5 tick（v5 中断 tick 下真实生效）；更高优先级就绪立即抢占
 *     （preempt 开关，QEMU 默认关/真机默认开）
 *   - 防饥饿老化：任何任务连续 200ms 未运行 → 临时提升到最高域
 *   - 后台预算：滚动 1000 tick 窗口内后台最多消耗 700 tick（限速率，
 *     给系统留余量；老化提升优先于预算，防饿死）
 *
 * 上下文切换（call0 ABI，双帧制，帧布局见 switch.S）：
 *   - 阻塞/让出：调用帧（a0 槽续体，rfe 恢复：EPC1=a0 槽、SR194=常态 PS）
 *   - tick 抢占：IRQ 帧（+76/+80 = 硬件 EPC1/异常 PS，rfe 恢复）
 *   - frame_kind 标记恢复路径（0=调用帧 1=IRQ 帧），切换落帧时设置
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
    uint32_t *stk_lo;        /* 栈底（低地址，水位扫描用；追加字段不影响 switch.S） */
    uint32_t stk_words;      /* 栈容量（字） */
    /* M9：双核归属（追加字段；switch.S 只依赖 sp@4，布局安全） */
    volatile int cpu;        /* 最近一次运行的 CPU（调度器维护） */
    int affinity;            /* 绑核亲和：-1=任意核（可迁移），0/1=绑定 */
} pot_task_t;

typedef void (*pot_task_fn_t)(void *arg);

/* 创建任务（栈由调用方静态提供，M4 引入堆后可加动态版） */
int pot_task_create(pot_task_t *tcb, const char *name, pot_task_class_t cls,
                    pot_task_fn_t fn, void *arg,
                    uint32_t *stack, uint32_t stack_words);

/* M9：带绑核亲和创建。-1=任意核（进 CPU0 队列，由负载均衡迁移），
 * 0/1=钉核（直接进目标核就绪队列，跨核入队自动补 IPI）。
 * pot_task_create 等价 affinity=0（保持单核语义）。 */
int pot_task_create_aff(pot_task_t *tcb, const char *name, pot_task_class_t cls,
                        pot_task_fn_t fn, void *arg,
                        uint32_t *stack, uint32_t stack_words,
                        int cpu_affinity);
void pot_task_exit(void);          /* 任务自尽（不返回） */
void pot_task_yield(void);         /* 主动让出 */
void pot_task_sleep(uint32_t ms);  /* 睡眠指定毫秒（tick 驱动唤醒） */
pot_task_t *pot_task_self(void);

/* 调度器（sched.c 内部为主，这里暴露给 tick/异常路径） */
extern pot_task_t *volatile pot_current; /* CPU0 视图（兼容旧引用；跨核用 pot_task_self） */

/* ---------- M9：双核调度接口（sched.c 提供，smp.c/console 调用） ----------
 * 关键并发约束：所有就绪/睡眠容器/当前任务指针的复合操作都在
 * pot_smp_sched_lock() 临界区内完成（S32C1I 自旋 + rsil 15，见 smp.h）。
 * 单核默认路径行为与 M8 完全一致（CPU1 未启动时只有 CPU0 状态被触碰）。 */
pot_task_t *pot_sched_self(void);              /* 当前核正在运行的任务 */
int  pot_sched_has_ready(uint32_t cpu);        /* 目标核有非 idle 就绪任务 */
int  pot_sched_ready_count(uint32_t cpu);      /* 目标核非 idle 就绪任务数 */
int  pot_sched_cpu_running_nonidle(uint32_t cpu); /* 目标核正跑非 idle 任务 */
pot_task_t *pot_sched_take_ready(uint32_t cpu);
pot_task_t *pot_sched_take_ready_movable(uint32_t cpu); /* 取走目标核最优就绪任务（迁移用） */
void pot_sched_put_ready(uint32_t cpu, pot_task_t *t); /* 放到目标核就绪队列 */
void pot_sched_attach_cpu(uint32_t cpu, pot_task_t *tcb); /* 挂接目标核当前任务 */
void pot_sched_cpu_tick_account(uint32_t cpu, uint32_t elapsed); /* 分核 tick 记账 */
int  pot_sched_should_preempt_cpu(uint32_t cpu); /* 分核抢占决策（无全局开关门） */
void pot_sched_preempt_from_irq_cpu(uint32_t cpu, void *frame_base); /* 分核切换，不返回 */
uint32_t pot_sched_cpu_switches(uint32_t cpu); /* 分核切换次数 */
pot_task_t *pot_sched_cpu_current(uint32_t cpu); /* 目标核当前任务（诊断） */
void pot_smp_balance_tick(void);               /* smp.c：CPU0 tick 驱动的负载均衡（限频） */

void pot_sched_init(void);
void pot_sched_tick(uint32_t elapsed);   /* tick 补记后调用（记账+唤醒） */
int  pot_sched_try_syscall(void *frame_base); /* 调度 syscall 识别+切换（不返回） */
int  pot_sched_others_ready(void);            /* 有非 idle 就绪任务（idle 让路判据） */
void pot_sched_sleep_until(uint32_t until);   /* 睡到绝对 tick（消息通道等待复用） */
int  pot_sched_wake(pot_task_t *t);           /* 显式唤醒（通道进度通知），1=已唤醒 */
/* D-03 v5：tick 中断驱动抢占（IRQ 帧 + rfe，异常上下文调用） */
int  pot_sched_should_preempt(void);
void pot_sched_preempt_from_irq(void *frame_base);
int  pot_sched_preempt_enabled(void);   /* 诊断开关（preempt 命令） */
void pot_sched_set_preempt(int on);
void pot_sched_diag(uint32_t *out);           /* 诊断计数 [wake,preempt,switch,idle] */

/* 统计（console/测试用） */
uint32_t pot_sched_now(void);
uint32_t pot_sched_total_switches(void);
uint32_t pot_sched_bg_used(void);

/* 任务注册表（main/idle/动态创建都登记，tasks 命令遍历） */
void pot_task_register(pot_task_t *t);
int pot_task_count(void);
pot_task_t *pot_task_at(int i);
uint32_t pot_task_reg_overflow(void); /* 注册表溢出计数（D-12，应为 0） */

/* 就绪/睡眠表溢出计数（D-11，应为 0） */
void pot_sched_drop_diag(uint32_t *ready_drop, uint32_t *sleep_drop);

/* 栈水位：以 0xC0DEC0DE 花样填充，返回从未使用的连续字数 */
#define POT_STACK_PATTERN 0xC0DEC0DEu
void pot_task_stack_paint(uint32_t *stack, uint32_t words);
uint32_t pot_task_stack_unused(const pot_task_t *t);

const char *pot_class_name(pot_task_class_t c);

#endif /* __ASSEMBLER__ */

#endif /* TUDOU_TASK_H */




