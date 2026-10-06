#ifndef TUDOU_TRACE_H
#define TUDOU_TRACE_H

#include <stdint.h>

/*
 * 土豆内核最小追踪环（支柱3起步）：
 * 静态定容 RAM 环形缓冲，事件定长 16 字节，写满覆盖最旧（有丢弃计数）。
 * 时间戳用 systimer 低 32 位（16MHz tick），用于顺序与差值，不用于绝对时刻。
 * M8 前导出为文本行，事件结构已按 JSONL 导出器的字段设计。
 */

#define POT_TRACE_CAPACITY 256 /* 必须是 2 的幂 */

enum {
    TR_IRQ_ENTER = 1,   /* a=level                b=0 */
    TR_IRQ_EXIT  = 2,   /* a=level                b=0 */
    TR_EXCEPTION = 3,   /* a=EXCCAUSE             b=EPC */
    /* 预留段：TR_SCHED_* = 16..（M3），TR_MSG_* = 32..（M3.5） */
};

void trace_init(void);
void trace_push(uint16_t type, uint32_t a, uint32_t b);
uint32_t trace_dropped(void);
void trace_dump(unsigned max_n); /* 打印最近 max_n 条（console 命令） */

#endif /* TUDOU_TRACE_H */
