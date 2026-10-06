#include <stdint.h>

#include "intc.h"
#include "io.h"
#include "task.h"
#include "tick.h"
#include "trace.h"
#include "uart.h"

/*
 * 中断框架（M2）：
 *  - UART0 RX：外设源经中断矩阵(0x600C2000) UART 源(+0x06C) → CPU int19
 *    （level2，INTLEVEL2 向量投递）
 *  - 内核 tick：核内 CCOMPARE0（int6，level1，经通用异常 EXCCAUSE=4 路径，
 *    由 exception_dispatch → intc_l1_dispatch 分发，RFE 返回）
 * CPU 中断号的最终使能在 Xtensa INTENABLE 特殊寄存器。
 */

#define INTMATRIX_BASE      0x600C2000u
#define INTMAP_UART0        (INTMATRIX_BASE + 0x06Cu)

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

void intc_enable_int(unsigned int_num)
{
    intenable_or(UINT32_C(1) << int_num);
}

void intc_init(void)
{
    mmio_write(INTMAP_UART0, INTC_UART0_CPU_INT);
    intenable_or(UINT32_C(1) << INTC_UART0_CPU_INT);
    /* int6(CCOMPARE0) 由 tick_init() 在装好比较器后再使能 */
}

void intc_irq_dispatch(unsigned level)
{
    trace_push(TR_IRQ_ENTER, level, 0);

    if (level == 2) {
        uart0_irq_handler();
    }

    trace_push(TR_IRQ_EXIT, level, 0);
}

/* 通用异常路径的 level-1 中断分发；返回 1=已处理可恢复，0=意外源（致命）。
 * M3 使能的唯一 level-1 源是 int6(CCOMPARE0)；该核心无 INTSET SR，
 * 无法枚举挂起源，若未来启用多个 l1 源需改用 ERI INTSTAT。 */
unsigned intc_l1_dispatch(void)
{
    trace_push(TR_IRQ_ENTER, 1, 6);
    tick_irq_handler(); /* 重挂 CCOMPARE0 的写操作顺带清除挂起 */
    pot_sched_tick(tick_last_elapsed()); /* 调度器记账/唤醒/抢占决策 */
    trace_push(TR_IRQ_EXIT, 1, 6);
    return 1;
}
