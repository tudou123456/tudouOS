#ifndef TUDOU_TICK_H
#define TUDOU_TICK_H

#include <stdint.h>

/* 核内 CCOMPARE0 tick：标定 CPU 频率并启动（intc_init 之后调用） */
void tick_init(void);
uint32_t tick_cpu_hz(void);

/* 最近一次触发补记的周期数（调度器记账用） */
uint32_t tick_last_elapsed(void);

/* v4：轮询推进 tick（任务级上下文调用——调度检查点与空闲循环） */
void pot_tick_poll(void);

/* intc 的 level-1 分发调用（genex EXCCAUSE=4 路径），勿直接调用 */
void tick_irq_handler(void);

#endif /* TUDOU_TICK_H */
