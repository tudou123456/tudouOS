#ifndef TUDOU_SMP_H
#define TUDOU_SMP_H

#include <stdint.h>

/*
 * 土豆内核 SMP（M9）——双核启动、核间中断、负载均衡。
 *
 * 策略：默认单核（CPU1 不上电）。`smp start` 显式启动 CPU1，
 * 现有单核行为零变化（回归只增不减），M9 用例显式开启。
 *
 * CPU1 启动协议（QEMU 与真机一致的 ROM 握手）：
 *   - 芯片复位后 APP CPU（CPU1）停在 ROM 的轮询循环里，反复读
 *     SYSTEM_CORE_1_CONTROL_1_REG（0x600C0004，QEMU ROM 反汇编
 *     `l32i.n a2, a3, 0; beqz a2, loop; callx8 a2` 实证）；
 *   - CPU0 把 CPU1 入口地址写入该寄存器，CPU1 立即跳转；
 *   - 真机侧同一语义的 ROM 函数是 ets_set_appcpu_boot_addr(0x40000720)，
 *     因此本路径在真机同样适用（带板待验）。
 *
 * 核间中断（IPI）：
 *   - SYSTEM_CPU_INTR_FROM_CPU_0..3（0x600C0030/34/38/3C，写 1 置起、
 *     写 0 清除）→ 中断矩阵外设源 ETS_FROM_CPU_INTR0..3（79..82）；
 *   - 矩阵每 CPU 一张映射表：CPU0 表 0x600C2000 + 4*src，
 *     CPU1 表 0x600C2008_... 由 intc_route_source_cpu() 写入；
 *   - 本内核把"本核接收"的源映射到 int21（level2，与 UART0/EMAC 向量
 *     共用入口，靠 INTERRUPT & INTENABLE 区分），"对核接收"的源映射到
 *     int4（level1 外部中断，INTENABLE 永不使能 ⇒ 只抬线不响应），
 *     避免矩阵未分配默认值 6 误触发本核 tick。
 */

#define POT_SMP_MAX_CPUS 2

/* IPI 原因位（可组合） */
#define POT_IPI_RESCHED 0x1u /* 对核任务队列变化/需要重新调度 */
#define POT_IPI_WAKE    0x2u /* 对核有任务被唤醒（与 RESCHED 同路径） */
#define POT_IPI_HALT    0x4u /* 请求对核停到空闲态（预留） */

/* 每 CPU 运行统计（smp 命令/健康快照/用例断言用） */
typedef struct {
    volatile uint32_t online; /* 1=本 CPU 已进入调度循环（启动 CPU1 前为 0） */
    uint32_t up_count;        /* 上线次数 */
    uint32_t ipi_sent;        /* 本 CPU 发出的 IPI */
    uint32_t ipi_recv;        /* 本 CPU 收到的 IPI */
    uint32_t ipi_unknown;     /* 收到但原因位不认识的 IPI */
    uint32_t resched;         /* IPI 触发的重调度 */
    uint32_t tick_switches;   /* 本 CPU 中断上下文完成的切换 */
    uint32_t ticks;           /* 本 CPU 定时器 tick 次数（CPU1 自有 CCOMPARE0） */
    uint32_t mig_in;          /* 迁入任务数 */
    uint32_t mig_out;         /* 迁出任务数 */
    uint32_t switches;        /* 本 CPU 累计上下文切换 */
    uint32_t mig_last_ms;     /* 最近一次迁移时刻（pot_sched_now 基准） */
    uint32_t idle_loops;      /* 空闲循环（含 waiti）次数 */
    uint32_t boot_ms;         /* 上线时刻（pot_sched_now 基准） */
} pot_smp_cpu_t;

/* CPU1 入口栈（cpu1_entry.S 的 .bss.cpu1_stack，4KB） */
#define POT_SMP_CPU1_STACK_WORDS 1024

/* ---------- 调度器跨核互斥（sched.c 的容器复合操作串行化） ----------
 * 实现：rsil 15（本核中断全屏蔽）+ S32C1I 原子交换抢锁；解锁恢复 PS。
 * 必须在异常上下文/任务上下文成对调用；不可重入。 */
uint32_t pot_smp_sched_lock(void);
void pot_smp_sched_unlock(uint32_t ps);

/* 双核压测/演示任务：cpu_affinity -1=任意核（由负载均衡迁移），0/1=绑核。
 * 返回实际创建数（<0 失败，0=已达上限）。count 计数由 smp work 打印。 */
#define POT_SMP_WORK_MAX 4
int pot_smp_spawn(int cpu_affinity, uint32_t n);
void pot_smp_work_dump(void);

/* CPU1 tick 抢占开关（默认 0：QEMU"genex 切走 + CCOMPARE 活动"丢 tick
 * 模型缺陷规避，与 CPU0 的 preempt 开关同源；真机可开）。 */
void pot_smp_set_cpu1_preempt(int on);
int pot_smp_cpu1_preempt(void);


int pot_smp_init(void);      /* 启动期初始化（CPU0 侧，清状态） */
int pot_smp_start(void);     /* 启动 CPU1：0=ok、1=已在线、-1=失败 */
int pot_smp_online(void);    /* 已上线 CPU 数（含 CPU0） */
uint32_t pot_smp_cpu_count(void); /* 探测到的 CPU 数（QEMU 恒 2） */
uint32_t pot_smp_cpu_id(void);    /* 当前 CPU 号（THREADPTR 每核私有） */
void pot_smp_ipi_send(uint32_t cpu, uint32_t reason);
void pot_smp_ipi_handler(void *frame_base); /* level-2 IPI 向量入口 */
uint32_t pot_smp_ipi_pending(uint32_t cpu);
const pot_smp_cpu_t *pot_smp_cpu(uint32_t i);
void pot_smp_dump(void);     /* 控制台 smp 状态输出 */
int pot_smp_selftest(void);  /* 地址/掩码自检：0=ok */

/* CPU1 入口（cpu1_entry.S 调用）与每 tick 钩子（intc_l1_dispatch 分核调用） */
void pot_smp_cpu1_main(void);
void pot_smp_cpu1_tick(void *frame_base);

/* 调度器侧钩子（sched.c 提供、smp.c 调用）：
 * IPI/CPU1 tick 触发的"必要时重调度"（可能切换，不返回时表示已切换） */
void pot_sched_resched_from_irq(void *frame_base);

#endif /* TUDOU_SMP_H */
