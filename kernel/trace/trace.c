#include <stdint.h>

#include "console.h"
#include "intc.h"
#include "time.h"
#include "trace.h"

/*
 * 最小追踪环：写入关中断，读出（dump）同样关中断保证一致性。
 * 覆盖最旧事件时不做任何动态操作，丢弃数由 seq 总数推算。
 */

typedef struct {
    uint32_t t;    /* 16MHz tick 低 32 位 */
    uint32_t a;    /* 参数 1 */
    uint32_t b;    /* 参数 2 */
    uint16_t type;
    uint16_t seq;  /* 全局递增序号（uint16 自然回绕） */
} pot_trace_ev_t;  /* 16 字节 */

static pot_trace_ev_t g_ring[POT_TRACE_CAPACITY];
static volatile uint16_t g_head; /* 下一个写入槽 */
static volatile uint16_t g_seq;

void trace_init(void)
{
    g_head = 0;
    g_seq = 0;
}

void trace_push(uint16_t type, uint32_t a, uint32_t b)
{
    /* v3 检查点调度：所有写入方都在异常上下文（EXCM=1 天然互斥，单异常
       串行），无需关中断——绝不能在热路径持有 INTLEVEL=15（挂起 tick 会
       触发 QEMU 活锁，M2 已知问题；切换风暴下必然中奖） */
    uint16_t h = g_head;
    g_ring[h].t = time_ticks_raw();
    g_ring[h].a = a;
    g_ring[h].b = b;
    g_ring[h].type = type;
    g_ring[h].seq = g_seq;
    g_seq++;
    g_head = (uint16_t)((h + 1) & (POT_TRACE_CAPACITY - 1));
}

uint32_t trace_dropped(void)
{
    uint32_t s = g_seq;
    return s > POT_TRACE_CAPACITY ? s - POT_TRACE_CAPACITY : 0;
}

static const char *event_name(uint16_t type)
{
    switch (type) {
    case TR_IRQ_ENTER:    return "IRQ_ENTER";
    case TR_IRQ_EXIT:     return "IRQ_EXIT";
    case TR_EXCEPTION:    return "EXCEPTION";
    case TR_SCHED_SWITCH: return "SCHED_SWITCH";
    case TR_MSG_ENQ:      return "MSG_ENQ";
    case TR_MSG_DEQ:      return "MSG_DEQ";
    case TR_MSG_DROP:     return "MSG_DROP";
    case TR_MSG_RETRY:    return "MSG_RETRY";
    case TR_MSG_ACK:      return "MSG_ACK";
    default:              return "?";
    }
}

void trace_dump(unsigned max_n)
{
    /* 无锁 dump：与 tick 写入方可能轻微撕裂（罕见、仅为可读性损失）；
       同样禁止在这里持有 INTLEVEL=15 */
    uint32_t total = g_seq;
    unsigned n = (total < max_n) ? total : max_n;
    if (n > POT_TRACE_CAPACITY) {
        n = POT_TRACE_CAPACITY;
    }

    potato_printf("[POTATO][TR] total=%u dropped=%u last %u:\r\n",
                  total, trace_dropped(), n);
    for (unsigned i = n; i > 0u; i--) {
        uint16_t idx = (uint16_t)((g_head - i) & (POT_TRACE_CAPACITY - 1));
        potato_printf("[POTATO][TR] #%u t=%u %s a=0x%x b=0x%x\r\n",
                      g_ring[idx].seq, g_ring[idx].t,
                      event_name(g_ring[idx].type),
                      g_ring[idx].a, g_ring[idx].b);
    }
}
