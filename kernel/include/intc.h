#ifndef TUDOU_INTC_H
#define TUDOU_INTC_H

#include <stdint.h>

/*
 * 土豆内核 M2 中断框架 — ESP32-S3 (Xtensa LX7)
 *
 * S3 的外设中断经中断矩阵(0x600C2000)映射到 Xtensa CPU 中断号，
 * CPU 中断号的优先级由核心配置固定（无软件可编程优先级）：
 *   int 0-18,27..: level1（无专用向量，走通用异常，M2 不用）
 *   int 19/20/21:  level2（向量 VECBASE+0x180）
 *   int 22/23:     level3（向量 VECBASE+0x1C0）
 * 本内核占用：UART0→int19(level2)，SYSTIMER_TARGET0→int22(level3)。
 */

#define INTC_UART0_CPU_INT     19

/* 映射外设源到 CPU 中断号并置 INTENABLE（在开全局中断前调用） */
void intc_init(void);

/* 使能单个 CPU 中断号（如 tick_init 再开 int6） */
void intc_enable_int(unsigned int_num);

/* vectors.S 胶水在关中断现场保存后调用（汇编侧入口，勿直接调用） */
void intc_irq_dispatch(unsigned level);

/* 通用异常(EXCCAUSE=4)路径的 level-1 分发；1=已处理可恢复 */
unsigned intc_l1_dispatch(void);

/* 意外向量（窗口异常等 call0 内核不应出现的入口）的处理，不返回 */
void intc_unexpected_vector(unsigned vector_offset);

/* ---- 全局中断开关（PS.INTLEVEL） ---- */

static inline void intc_enable_interrupts(void)
{
    uint32_t ps;
    asm volatile("rsil %0, 0" : "=r"(ps) :: "memory");
    (void)ps;
}

static inline uint32_t irq_save(void)
{
    uint32_t ps;
    asm volatile("rsil %0, 15" : "=r"(ps) :: "memory");
    return ps;
}

static inline void irq_restore(uint32_t ps)
{
    asm volatile("wsr.ps %0\n\t"
                 "esync"
                 :: "r"(ps) : "memory");
}

#endif /* TUDOU_INTC_H */
