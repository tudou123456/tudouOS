#ifndef TUDOU_TICK_H
#define TUDOU_TICK_H

#include <stdint.h>

/* 核内 CCOMPARE0 tick：标定 CPU 频率并启动（intc_init 之后调用） */
void tick_init(void);
uint32_t tick_cpu_hz(void);

/* intc 的 level-1 分发调用（genex EXCCAUSE=4 路径），勿直接调用 */
void tick_irq_handler(void);

#endif /* TUDOU_TICK_H */
