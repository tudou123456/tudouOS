#include <stdint.h>

#include "console.h"
#include "intc.h"
#include "io.h"
#include "smp.h"
#include "task.h"
#include "tick.h"
#include "time.h"
#include "trace.h"

/* 启动阶段标记（kernel/kernel/boot_trace.c；TUDOU_BOOT_TRACE=0 时为空操作） */
void pot_boot_mark(char c);

/*
 * 土豆内核 SMP（M9）——CPU1 启动、核间中断、双核调度、负载均衡。
 *
 * CPU1 启动协议见 kernel/smp/cpu1_entry.S 与 smp.h 注释。要点：
 *  - 芯片复位后 APP CPU 停在 ROM 轮询桩里读 SYSTEM_CORE_1_CONTROL_1_REG
 *    （0x600C0004）；CPU0 写入入口地址 → ROM `callx8` 跳转（QEMU 反汇编实证，
 *    真机同语义 ROM 函数 ets_set_appcpu_boot_addr=0x40000720）。
 *  - CPU1 入口重设栈/向量表/PS/THREADPTR 后进 pot_smp_cpu1_main。
 *
 * 中断：CPU1 用自己的 CCOMPARE0（int6，level1）做 1ms tick；核间中断
 * 走中断矩阵源 79/80（SYSTEM_CPU_INTR_FROM_CPU_0/1）→ CPU1/CPU0 的 int21
 * （level2）。自己发出的源在对核矩阵里映射到 int4（永不使能），防止矩阵
 * 默认值 6（tick）被误触发。
 *
 * 双核调度（阶段 2）：
 *  - 调度器容器按核分片（sched.c），本文件提供：
 *    · 跨核互斥锁 pot_smp_sched_lock/unlock（S32C1I；每核先置 SCOMPARE1=0）
 *    · CPU1 主循环：挂接本核 idle 上下文 → 上线 → 有空闲就绪任务就 yield
 *    · CPU1 自有 tick：本核记账 + 可选抢占；不推进全局时间
 *    · CPU0 tick 驱动的负载均衡：单侧积压 ≥2 且对侧空闲时迁移一个任务
 *    · 双核演示/压测任务（console `smp spawn/work`）
 */

#define SYSTEM_CORE_1_CONTROL_0    0x600C0000u
#define SYSTEM_CORE_1_CONTROL_1    0x600C0004u
#define SYSTEM_CPU_INTR_FROM_CPU_0 0x600C0030u
#define SYSTEM_CPU_INTR_FROM_CPU_1 0x600C0034u

/* CPU1 定时器周期：CC 40MHz / 1000Hz = 40000 */
#define SMP_TICK_CC 40000u

extern void _cpu1_entry(void);
extern uint32_t pot_cpu1_stack[]; /* cpu1_entry.S：POT_SMP_CPU1_STACK_WORDS 字 */

static pot_smp_cpu_t g_cpus[POT_SMP_MAX_CPUS];
static volatile uint32_t g_ipi_pending[POT_SMP_MAX_CPUS];
static volatile uint32_t g_cpu1_started;
static uint32_t g_cpu1_next_cc;
static uint32_t g_selftest_runs;
static uint32_t g_balance_seq;
static int g_bal_diag_r0;
static int g_bal_diag_r1;
static int g_bal_diag_aff;
static uint32_t g_bal_diag_n;
static uint32_t g_balance_hold_ms; /* 迁移冷却截止时刻（tick/ms） */

/* CPU1 的 idle 上下文（boot 栈即其栈；不参与就绪队列） */
static pot_task_t g_cpu1_idle_tcb;
static volatile int g_cpu1_preempt;

/* 双核演示任务（console smp spawn） */
static pot_task_t g_work_tcb[POT_SMP_WORK_MAX];
static uint32_t g_work_stack[POT_SMP_WORK_MAX][512]; /* 2KB/task */
static const char *const g_work_name[POT_SMP_WORK_MAX] = {
    "smpw0", "smpw1", "smpw2", "smpw3",
};
static volatile uint32_t g_work_count[POT_SMP_WORK_MAX];
static uint32_t g_work_n;

/* ---------- 跨核调度锁（S32C1I） ---------- */

static volatile uint32_t g_sched_lock;

static inline void smp_scompare1_set(uint32_t v)
{
    asm volatile("wsr.scompare1 %0\n\t"
                 "isync"
                 :: "r"(v) : "memory");
}

uint32_t pot_smp_sched_lock(void)
{
    uint32_t ps;
    volatile uint32_t *p = &g_sched_lock;
    asm volatile("rsil %0, 15" : "=r"(ps) :: "memory");
    for (;;) {
        /* aT=要写入的值(1)，SCOMPARE1=0（未锁）；成功时 aT←SCOMPARE1=0 */
        uint32_t v = 1u;
        asm volatile("s32c1i %0, %1, 0" : "+r"(v) : "r"(p) : "memory");
        if (v == 0u) {
            break;
        }
    }
    asm volatile("memw" ::: "memory");
    return ps;
}

void pot_smp_sched_unlock(uint32_t ps)
{
    uint32_t zero = 0u;
    asm volatile("memw\n\t"
                 "s32i %1, %0, 0\n\t"
                 "memw"
                 :: "r"((volatile uint32_t *)&g_sched_lock), "r"(zero)
                 : "memory");
    asm volatile("wsr.ps %0\n\t"
                 "esync"
                 :: "r"(ps) : "memory");
}

/* ---------- 每核身份 ---------- */

uint32_t pot_smp_cpu_id(void)
{
    uint32_t v;
    /* THREADPTR（Xtensa SR231，每核私有）：CPU0=0（boot.S）、CPU1=1（入口） */
    asm volatile("rur %0, threadptr" : "=r"(v));
    return v & 1u;
}

/* ---------- 底层寄存器 ---------- */

static inline uint32_t smp_ccount(void)
{
    uint32_t v;
    asm volatile("rsr.ccount %0" : "=r"(v));
    return v;
}

static inline void smp_ccompare0(uint32_t cc)
{
    asm volatile("wsr.ccompare0 %0\n\t"
                 "esync"
                 :: "r"(cc) : "memory");
}

/* 发送方 CPU 号 → 它写哪个 FROM_CPU 寄存器 */
static inline uint32_t smp_intr_reg(uint32_t sender)
{
    return (sender == 0u) ? SYSTEM_CPU_INTR_FROM_CPU_0 : SYSTEM_CPU_INTR_FROM_CPU_1;
}

/* ---------- 初始化 ---------- */

int pot_smp_init(void)
{
    for (uint32_t i = 0; i < POT_SMP_MAX_CPUS; i++) {
        g_cpus[i].online = 0;
        g_cpus[i].up_count = 0;
        g_cpus[i].ipi_sent = 0;
        g_cpus[i].ipi_recv = 0;
        g_cpus[i].ipi_unknown = 0;
        g_cpus[i].resched = 0;
        g_cpus[i].tick_switches = 0;
        g_cpus[i].ticks = 0;
        g_cpus[i].mig_in = 0;
        g_cpus[i].mig_out = 0;
        g_cpus[i].switches = 0;
        g_cpus[i].mig_last_ms = 0;
        g_cpus[i].idle_loops = 0;
        g_cpus[i].boot_ms = 0;
        g_ipi_pending[i] = 0;
    }
    g_cpu1_started = 0;
    g_cpu1_next_cc = 0;
    g_selftest_runs = 0;
    g_balance_seq = 0;
    g_balance_hold_ms = 0;
    g_cpu1_preempt = 0;
    g_work_n = 0;
    g_sched_lock = 0;
    smp_scompare1_set(0u); /* S32C1I 比较值：0=未锁（每核私有寄存器） */

    g_cpus[0].online = 1;
    g_cpus[0].up_count = 1;
    g_cpus[0].boot_ms = pot_sched_now();

    /* CPU0 视角映射：谁写 FROM_CPU_1（CPU1）→ 本核 int21；自己写的
     * FROM_CPU_0 映射到掩蔽号，避免自触发 */
    intc_route_source_cpu(0, INTC_FROM_CPU1_SOURCE, INTC_IPI_CPU_INT);
    intc_route_source_cpu(0, INTC_FROM_CPU0_SOURCE, INTC_IPI_MASK_INT);
    intc_enable_int(INTC_IPI_CPU_INT);
    return 0;
}

/* ---------- CPU1 启动 ---------- */

int pot_smp_start(void)
{
    if (g_cpu1_started != 0u) {
        return 1; /* 已启动过 */
    }
    g_cpu1_started = 1;
    {
        uint32_t entry = (uint32_t)(uintptr_t)&_cpu1_entry;
        potato_printf("[POTATO][SMP] boot cpu1: entry=0x%x -> reg 0x%x\r\n",
                      entry, SYSTEM_CORE_1_CONTROL_1);
#if TUDOU_PLATFORM != 0
        /*
         * 真机差异：QEMU 直接复位 CPU1 并让它跑 ROM（ROM 轮询入口寄存器），
         * 所以只写 CONTROL_1 就能启动；真机复位后 APP CPU 由
         * SYSTEM_CORE_1_CONTROL_0(0x600C0000) 控制，ROM 默认 0x04：
         *   bit0 RUNSTALL=1  停核
         *   bit1 CLKGATE_EN=0 关时钟
         *   bit2 RESETING=1   保持复位
         * 不先释放复位/开时钟，写 CONTROL_1 只落到一个不复位的核上，CPU1
         * 永不上线（实测 smp start 返回 -1 TIMEOUT）；且 APP CPU 长期不活动
         * 会触发 super watchdog 周期复位（rst:0x12 SUPER_WDT_RST）。
         * 因此先清 RUNSTALL/RESETING、置 CLKGATE_EN，再写入口地址。
         */
        uint32_t ctl0 = mmio_read(SYSTEM_CORE_1_CONTROL_0);
        ctl0 &= ~((1u << 0) | (1u << 2));
        ctl0 |= (1u << 1);
        mmio_write(SYSTEM_CORE_1_CONTROL_0, ctl0);
        for (volatile uint32_t d = 0; d < 2000u; d++) {
            /* 等 CPU1 时钟/复位稳定（约几十 us） */
        }
        potato_printf("[POTATO][SMP] hw core1 release: ctl0 0x%x -> 0x%x (read 0x%x)\r\n",
                      (unsigned)(ctl0 | (1u << 0) | (1u << 2)),
                      (unsigned)ctl0,
                      (unsigned)mmio_read(SYSTEM_CORE_1_CONTROL_0));
#endif
        mmio_write(SYSTEM_CORE_1_CONTROL_1, entry);
    }
    /* 等 CPU1 报在线（最多 ~200ms）；期间睡让出 CPU0 */
    for (uint32_t i = 0; i < 200u; i++) {
        if (g_cpus[1].online != 0u) {
            break;
        }
        pot_task_sleep(1);
    }
    if (g_cpus[1].online == 0u) {
        potato_printf("[POTATO][SMP] boot cpu1: TIMEOUT (no online ack)\r\n");
        return -1;
    }
    potato_printf("[POTATO][SMP] cpu1 online confirm online=%u\r\n",
                  (unsigned)pot_smp_online());
    trace_push(TR_CPU_UP, 1, pot_sched_now());
    return 0;
}

int pot_smp_online(void)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < POT_SMP_MAX_CPUS; i++) {
        if (g_cpus[i].online != 0u) {
            n++;
        }
    }
    return (int)n;
}

uint32_t pot_smp_cpu_count(void)
{
    return (uint32_t)POT_SMP_MAX_CPUS;
}

/* ---------- CPU1 主函数（cpu1_entry.S call0 进来） ---------- */

void pot_smp_cpu1_main(void)
{
    /* 真机带板标记 'e'：已进 C 主函数（TUDOU_BOOT_TRACE=1 才真正输出） */
    pot_boot_mark('e');

    /* 本核矩阵：CPU0 写的 FROM_CPU_0 → int21；本核自己写的 FROM_CPU_1
     * 映射到掩蔽号（防止自触发） */
    intc_route_source_cpu(1, INTC_FROM_CPU0_SOURCE, INTC_IPI_CPU_INT);
    intc_route_source_cpu(1, INTC_FROM_CPU1_SOURCE, INTC_IPI_MASK_INT);
    intc_enable_int(INTC_IPI_CPU_INT);
    intc_enable_int(6); /* CCOMPARE0（level1） */

    g_cpu1_next_cc = smp_ccount() + SMP_TICK_CC;
    smp_ccompare0(g_cpu1_next_cc);
    smp_scompare1_set(0u); /* 本核 SCOMPARE1；必须在任何 sched_lock 之前 */

    /* 本核 idle 上下文：boot 栈就是它的栈（不参与就绪队列，只作为
       "当前任务"接收记账/抢占；空闲让路由 loop 里的 yield 完成） */
    g_cpu1_idle_tcb.name = "cpu1idle";
    g_cpu1_idle_tcb.cls = POTCLS_IDLE;
    g_cpu1_idle_tcb.state = POTTASK_RUNNING;
    g_cpu1_idle_tcb.stk_lo = pot_cpu1_stack;
    g_cpu1_idle_tcb.stk_words = (uint32_t)POT_SMP_CPU1_STACK_WORDS;
    g_cpu1_idle_tcb.affinity = 1;
    pot_task_register(&g_cpu1_idle_tcb);
    pot_sched_attach_cpu(1, &g_cpu1_idle_tcb);

    g_cpus[1].online = 1;
    g_cpus[1].up_count++;
    g_cpus[1].boot_ms = pot_sched_now();

    /* 放开 INTLEVEL（入口置了 15）。此后 tick/IPI 可进来。 */
    {
        uint32_t ps;
        asm volatile("rsil %0, 0" : "=r"(ps) :: "memory");
        (void)ps;
    }

    potato_printf("[POTATO][SMP] cpu1 online id=%u\r\n", (unsigned)pot_smp_cpu_id());
    pot_boot_mark('f'); /* 已上线、中断已放开 */

    /* 空闲循环：无 waiti（本核配置无该指令），自旋等中断；
     * 分到本核的任务只要出现在就绪队列，yield 即让调度器挑它运行。 */
    for (;;) {
        g_cpus[1].idle_loops++;
        if (pot_sched_has_ready(1)) {
            pot_task_yield();
        }
    }
}

/* CPU1 的 level-1 tick（intc_l1_dispatch 分核调用；不推进全局时间） */
void pot_smp_cpu1_tick(void *frame_base)
{
    g_cpus[1].ticks++;
    g_cpu1_next_cc += SMP_TICK_CC;
    if ((int32_t)(smp_ccount() - g_cpu1_next_cc) >= 0) {
        /* 落后（QEMU 虚拟时间大步推进）：重新对齐 */
        g_cpu1_next_cc = smp_ccount() + SMP_TICK_CC;
    }
    smp_ccompare0(g_cpu1_next_cc);

    pot_sched_cpu_tick_account(1, 1u);

    /* CPU1 tick 抢占：默认关（QEMU genex 切换丢 tick 模型缺陷规避）。
     * 该核的切换主要由 IPI/检查点（yield/sleep）驱动，不影响正确性。 */
    if (g_cpu1_preempt != 0 && pot_sched_should_preempt_cpu(1)) {
        g_cpus[1].tick_switches++;
        pot_sched_preempt_from_irq_cpu(1, frame_base); /* 不返回 */
    }
}

/* ---------- 负载均衡（CPU0 tick 调用；每 4 tick 决策一次） ---------- */

void pot_smp_balance_tick(void)
{
    if (g_cpus[1].online == 0u) {
        return;
    }
    if ((++g_balance_seq & 3u) != 0u) {
        return;
    }
    /* 冷却：一次迁移后至少 20ms 不再迁，避免两核往返搬运（迁移本身有
     * IPI + 上下文切换成本，抖动比收益大）。 */
    uint32_t now = pot_sched_now();
    if ((int32_t)(now - g_balance_hold_ms) < 0) {
        return;
    }
    int r0 = pot_sched_ready_count(0);
    int r1 = pot_sched_ready_count(1);
    uint32_t dst;
    if (r0 >= 2 && r1 == 0 && !pot_sched_cpu_running_nonidle(1)) {
        dst = 1u; /* CPU0 积压、CPU1 空闲 → 迁一个过去 */
    } else if (r1 >= 2 && r0 == 0 && !pot_sched_cpu_running_nonidle(0)) {
        dst = 0u; /* 反向均衡 */
    } else {
        return;
    }
    uint32_t src = dst ^ 1u;
    /* 只取可迁移任务：绑核任务不参与负载均衡（test_m9_work_pinned 断言零迁移） */
    pot_task_t *t = pot_sched_take_ready_movable(src);
    if (t == 0) {
        return;
    }
    g_bal_diag_r0 = r0;
    g_bal_diag_r1 = r1;
    g_bal_diag_aff = t->affinity;
    g_bal_diag_n++;
    pot_sched_put_ready(dst, t);
    g_balance_hold_ms = pot_sched_now() + 20u;
    g_cpus[dst].mig_in++;
    g_cpus[src].mig_out++;
    g_cpus[dst].mig_last_ms = pot_sched_now();
    trace_push(TR_SMP_MIGRATE, (uint32_t)t, src | (dst << 8));
}

/* ---------- 双核演示/压测任务 ---------- */

static void smp_work_main(void *arg)
{
    uint32_t i = (uint32_t)(uintptr_t)arg;
    for (;;) {
        g_work_count[i]++;
        for (volatile uint32_t k = 0; k < 2000u; k++) {
        }
        pot_task_yield();
    }
}

int pot_smp_spawn(int cpu_affinity, uint32_t n)
{
    if (cpu_affinity < -1 || cpu_affinity >= POT_SMP_MAX_CPUS) {
        return -1;
    }
    int made = 0;
    while (n-- > 0u && g_work_n < (uint32_t)POT_SMP_WORK_MAX) {
        uint32_t id = g_work_n;
        g_work_count[id] = 0;
        if (pot_task_create_aff(&g_work_tcb[id], g_work_name[id],
                                POTCLS_BACKGROUND, smp_work_main,
                                (void *)(uintptr_t)id,
                                g_work_stack[id],
                                (uint32_t)(sizeof(g_work_stack[id]) /
                                           sizeof(uint32_t)),
                                cpu_affinity) != 0) {
            return (made > 0) ? made : -1;
        }
        g_work_n++;
        made++;
    }
    return made;
}

void pot_smp_work_dump(void)
{
    potato_printf("[POTATO][SMP] work n=%u\r\n", (unsigned)g_work_n);
    for (uint32_t i = 0; i < g_work_n; i++) {
        potato_printf("[POTATO][SMP] work%u count=%u\r\n",
                      (unsigned)i, (unsigned)g_work_count[i]);
    }
}

/* ---------- 核间中断 ---------- */

void pot_smp_ipi_send(uint32_t cpu, uint32_t reason)
{
    uint32_t me = pot_smp_cpu_id();
    if (cpu >= (uint32_t)POT_SMP_MAX_CPUS || cpu == me) {
        return;
    }
    g_ipi_pending[cpu] |= reason;
    g_cpus[me].ipi_sent++;
    if (me == 0u) {
        trace_push(TR_IPI_SEND, cpu, reason); /* 追踪环 CPU0 单写者 */
    }
    /* 写"发送方"的 FROM_CPU 寄存器：置 1 抬源线，接收侧 ISR 清 0 */
    mmio_write(smp_intr_reg(me), 1u);
}

void pot_smp_ipi_handler(void *frame_base)
{
    uint32_t me = pot_smp_cpu_id();
    uint32_t sender = me ^ 1u;
    uint32_t pend = g_ipi_pending[me];

    g_ipi_pending[me] = 0;
    mmio_write(smp_intr_reg(sender), 0u); /* 清源（发送方的 FROM_CPU 寄存器） */
    g_cpus[me].ipi_recv++;

    if ((pend & ~(POT_IPI_RESCHED | POT_IPI_WAKE | POT_IPI_HALT)) != 0u) {
        g_cpus[me].ipi_unknown++;
    }
    if (me == 0u) {
        trace_push(TR_IPI_RECV, pend, me);
    }
    if (pend != 0u) {
        g_cpus[me].resched++;
        /* M9 关键约束：level-2 中断帧不能用 level-1 的 rfe 恢复路径切换
         * （硬件只为 level-2 保存 EPC2/EPS2，帧里 +76 的 EPC1 是 stale 值，
         * 若在此切走会 rfe 到上次 level-1 中断的旧 PC）。这里只计数，
         * 调度点交给：①目标核 idle 循环轮询就绪队列；②任务检查点
         * yield/sleep；③该核自有 level-1 tick（g_cpu1_preempt 开启时）。 */
        (void)frame_base;
    }
}

uint32_t pot_smp_ipi_pending(uint32_t cpu)
{
    return (cpu < (uint32_t)POT_SMP_MAX_CPUS) ? g_ipi_pending[cpu] : 0u;
}

const pot_smp_cpu_t *pot_smp_cpu(uint32_t i)
{
    return (i < (uint32_t)POT_SMP_MAX_CPUS) ? &g_cpus[i] : 0;
}

void pot_smp_set_cpu1_preempt(int on)
{
    g_cpu1_preempt = on ? 1 : 0;
}

int pot_smp_cpu1_preempt(void)
{
    return g_cpu1_preempt;
}

/* ---------- 诊断输出 ---------- */

void pot_smp_dump(void)
{
    potato_printf("[POTATO][SMP] cpus=%u online=%u started=%u ipi_pending=%u/%u\r\n",
                  (unsigned)pot_smp_cpu_count(), (unsigned)pot_smp_online(),
                  (unsigned)g_cpu1_started,
                  (unsigned)g_ipi_pending[0], (unsigned)g_ipi_pending[1]);
    for (uint32_t i = 0; i < POT_SMP_MAX_CPUS; i++) {
        const pot_smp_cpu_t *c = &g_cpus[i];
        uint32_t sw = pot_sched_cpu_switches(i); /* 调度器实时值（不写回） */
        pot_task_t *cur = pot_sched_cpu_current(i);
        potato_printf("[POTATO][SMP] cpu%u online=%u up=%u ipi_tx=%u ipi_rx=%u "
                      "resched=%u ticksw=%u ticks=%u mig_in=%u mig_out=%u "
                      "sw=%u unk=%u idle=%u rq=0/%d cur=%s boot=%ums\r\n",
                      (unsigned)i, (unsigned)c->online, (unsigned)c->up_count,
                      (unsigned)c->ipi_sent, (unsigned)c->ipi_recv,
                      (unsigned)c->resched, (unsigned)c->tick_switches,
                      (unsigned)c->ticks, (unsigned)c->mig_in,
                      (unsigned)c->mig_out, (unsigned)sw,
                      (unsigned)c->ipi_unknown, (unsigned)c->idle_loops,
                      pot_sched_ready_count(i),
                      (cur != 0 && cur->name != 0) ? cur->name : "-",
                      (unsigned)c->boot_ms);
    }
    potato_printf("[POTATO][SMP] work n=%u cpu1_preempt=%d balance_seq=%u "
                  "diag=%d/%d/aff%d/n%u\r\n",
                  (unsigned)g_work_n, (int)g_cpu1_preempt,
                  (unsigned)g_balance_seq,
                  g_bal_diag_r0, g_bal_diag_r1, g_bal_diag_aff,
                  (unsigned)g_bal_diag_n);
}

int pot_smp_selftest(void)
{
    int fail = 0;
    if (pot_smp_cpu_id() != 0u) {
        fail |= 1;
    }
    if (pot_smp_cpu_count() < 2u) {
        fail |= 2;
    }
    if (pot_smp_online() < 1) {
        fail |= 4;
    }
    if ((uint32_t)(uintptr_t)&_cpu1_entry == 0u) {
        fail |= 8;
    }
    if ((uint32_t)SYSTEM_CORE_1_CONTROL_1 != 0x600C0004u) {
        fail |= 16;
    }
    if (POT_SMP_MAX_CPUS != 2) {
        fail |= 32;
    }
    g_selftest_runs++;
    return fail;
}




