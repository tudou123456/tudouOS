#include <stdint.h>

#include "console.h"
#include "intc.h"
#include "io.h"
#include "net.h"
#include "smp.h"
#include "task.h"
#include "tick.h"
#include "trace.h"
#include "uart.h"

/*
 * 中断框架（M2；M7 扩展为多外设源）：
 *  - UART0 RX：外设源 27 → 中断矩阵(0x600C2000) +0x06C → CPU int19
 *    （level2，INTLEVEL2 向量投递）
 *  - EMAC（M7）：外设源 0 → 矩阵 +0x000 → CPU int20（level2，同上向量）
 *  - 内核 tick：核内 CCOMPARE0（int6，level1，经通用异常 EXCCAUSE=4 路径，
 *    由 exception_dispatch → intc_l1_dispatch 分发，RFE 返回）
 * CPU 中断号的最终使能在 Xtensa INTENABLE 特殊寄存器。
 * level2 向量只有一个入口，靠 INTERRUPT & INTENABLE 枚举挂起源区分处理。
 */

#define INTMATRIX_BASE      0x600C2000u
#define INTMAP_SRC(src)     (INTMATRIX_BASE + 4u * (src))
#define INTMAP_UART0        INTMAP_SRC(INTC_UART0_SOURCE)

#define INTC_CCOMPARE0_INT  6  /* Xtensa 核内定时器0（level-1） */

static void intenable_or(uint32_t mask)
{
    uint32_t v;
    asm volatile("rsr.intenable %0" : "=r"(v));
    v |= mask;
    asm volatile("wsr.intenable %0\n\t"
                 "esync"
                 :: "r"(v) : "memory");
}

static uint32_t int_pending_enabled(void)
{
    uint32_t pend, en;
    asm volatile("rsr.interrupt %0" : "=r"(pend));
    asm volatile("rsr.intenable %0" : "=r"(en));
    return pend & en;
}

void intc_enable_int(unsigned int_num)
{
    intenable_or(UINT32_C(1) << int_num);
}

void intc_route_source(unsigned source, unsigned cpu_int)
{
    mmio_write(INTMAP_SRC(source), cpu_int);
}

/* M9：按 CPU 路由。CPU0 表基址 0x600C2000，CPU1 表基址 +0x800
 *（模型 get_map_entry：source_index = addr/4，cpu = idx/512，src = idx%512） */
void intc_route_source_cpu(unsigned cpu, unsigned source, unsigned cpu_int)
{
    uint32_t base = INTMATRIX_BASE;
    if (cpu != 0u) {
        base += INTC_MATRIX_CPU_STRIDE;
    }
    mmio_write(base + 4u * source, cpu_int & 0x1fu);
}

void intc_init(void)
{
    intc_route_source(INTC_UART0_SOURCE, INTC_UART0_CPU_INT);
    intenable_or(UINT32_C(1) << INTC_UART0_CPU_INT);
    /* int6(CCOMPARE0) 由 tick_init() 在装好比较器后再使能；
     * EMAC(int20) 由 emac_int_enable() 在设备就绪后使能 */
}

void intc_irq_dispatch(unsigned level, void *frame_base)
{
    /* M9：追踪环是 CPU0 单写者（跨核 ring 并发由后续 spinlock 解决），
     * CPU1 的 IPI 中断不写环，只累加 SMP 计数 */
    int on_cpu0 = (pot_smp_cpu_id() == 0u);
    if (on_cpu0) {
        trace_push(TR_IRQ_ENTER, level, 0);
    }

    if (level == 2) {
        /* 记录本次实际挂起且使能的源，按位分发 */
        uint32_t active = int_pending_enabled();
        if ((active & (UINT32_C(1) << INTC_UART0_CPU_INT)) != 0u) {
            uart0_irq_handler();
        }
        if ((active & (UINT32_C(1) << INTC_EMAC_CPU_INT)) != 0u) {
            emac_irq_handler();
        }
        if ((active & (UINT32_C(1) << INTC_IPI_CPU_INT)) != 0u) {
            pot_smp_ipi_handler(frame_base); /* 可能切换（不返回） */
        }
    }

    if (on_cpu0) {
        trace_push(TR_IRQ_EXIT, level, 0);
    }
}

/* 通用异常路径的 level-1 中断分发；返回 1=已处理可恢复（RESTORE+rfe），
 * 或直接在异常上下文完成抢占切换（不返回）。
 * D-03 v5：tick 记账/唤醒后，若有更高有效优先级就绪或时间片耗尽，
 * 把被打断任务的 IRQ 帧挂到其 TCB 并切换（rfe 恢复，E-02 实证路线）。
 * 唯一 level-1 源是 int6(CCOMPARE0)；该核心无 INTSET SR，
 * 无法枚举挂起源，若未来启用多个 l1 源需改用 ERI INTSTAT。 */


unsigned intc_l1_dispatch(void *frame_base)
{
    /* M9：CPU1 的 level-1 tick 走自己的时基与抢占判断（不推进全局时间） */
    if (pot_smp_cpu_id() != 0u) {
        pot_smp_cpu1_tick(frame_base);
        return 1;
    }
    trace_push(TR_IRQ_ENTER, 1, 6);
    tick_irq_handler(); /* 重挂 CCOMPARE0 的写操作顺带清除挂起 */
    pot_sched_tick(tick_last_elapsed()); /* 调度器记账/唤醒 */
    trace_push(TR_IRQ_EXIT, 1, 6);
    if (pot_sched_should_preempt()) {
        pot_sched_preempt_from_irq(frame_base); /* 不返回 */
    }
    return 1;
}

