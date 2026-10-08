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
    TR_SCHED_SWITCH = 16, /* a=from TCB 指针       b=to TCB 指针 */
    TR_MSG_ENQ   = 32,  /* a=通道指针             b=消息 id */
    TR_MSG_DEQ   = 33,  /* a=通道指针             b=消息 id */
    TR_MSG_DROP  = 34,  /* a=通道指针             b=原因(1full/2deadline/3dedup) */
    TR_MSG_RETRY = 35,  /* a=通道指针             b=消息 id */
    TR_MSG_ACK   = 36,  /* a=通道指针             b=消息 id */
    TR_MEM_ALLOC = 64,  /* a=块尺寸               b=tag */
    TR_MEM_FREE  = 65,  /* a=块尺寸               b=tag */
    TR_MEM_FAULT = 66,  /* a=原因(1overflow/2badfree) b=tag */
};

void trace_init(void);
void trace_push(uint16_t type, uint32_t a, uint32_t b);
uint32_t trace_dropped(void);
void trace_dump(unsigned max_n); /* 打印最近 max_n 条（console 命令） */

#endif /* TUDOU_TRACE_H */
