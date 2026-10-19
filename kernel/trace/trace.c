#include <stdint.h>

#include "console.h"
#include "intc.h"
#include "sb.h"
#include "time.h"
#include "trace.h"
#include "uart.h"

/*
 * 最小追踪环（D-18 修正后的真实并发模型）：
 * 写入方横跨三种上下文：任务态（msg/heap/sched 检查点）、level-2 UART
 * RX 中断、level-1 tick 异常（v5 起含抢占切换事件）——写入序列
 * （读 head → 写 5 字段 → seq++ → 写 head）非原子，必须短临界区保护。
 * 临界区只抬到 INTLEVEL=1（屏蔽 tick），且在更高 INTLEVEL/EXCM 现场
 * 不降级（rsil 会覆盖绝对值，先判后抬）。seq 为 32 位（uint16 会在
 * 数百事件/秒下约 2 分钟回绕，丢弃推算随之失真）。
 *
 * M8：新增 JSONL 导出（trace_export_jsonl / trace_export_since）与
 * 类型计数（trace_count_of），供健康快照与 AI 分析流水线消费。
 */

typedef struct {
    uint32_t t;    /* ccount 低 32 位（40MHz 假设，~107s 回绕，仅作差分） */
    uint32_t a;    /* 参数 1 */
    uint32_t b;    /* 参数 2 */
    uint16_t type;
    uint16_t seq;  /* 全局递增序号低 16 位（全量在 g_seq，32 位） */
} pot_trace_ev_t;  /* 16 字节 */

#define TR_TYPE_SLOTS 256u

static pot_trace_ev_t g_ring[POT_TRACE_CAPACITY];
static volatile uint16_t g_head; /* 下一个写入槽 */
static volatile uint32_t g_seq;
static uint32_t g_type_count[TR_TYPE_SLOTS]; /* 按类型累计（M8 健康快照） */

/* M8 重要事件副环：不被 sched/tick 噪声冲刷，容量 64（约 4KB BSS） */
static pot_trace_ev_t g_imp_ring[POT_TRACE_IMP_CAPACITY];
static volatile uint16_t g_imp_head;
static volatile uint32_t g_imp_seq;

int trace_is_important(uint16_t type)
{
    switch (type) {
    case TR_EXCEPTION:
    case TR_TICK_GAP:
    case TR_SCHED_DROP:
    case TR_SCHED_AGING:
    case TR_MSG_DROP:
    case TR_MSG_RETRY:
    case TR_MEM_FAULT:
    case TR_NET_DROP:
    case TR_NET_LINK:
    case TR_MCP_DUP:
    case TR_MCP_RESYNC:
    case TR_MCP_REPLAY:
    case TR_CPU_UP:
    case TR_SMP_MIGRATE:
    case TR_INJECT_BEGIN:
    case TR_INJECT_END:
        return 1;
    default:
        return 0;
    }
}

/* P0-3/P1-2：写者横跨任务态、level-1 tick、level-2 UART/EMAC ISR，且
 * SMP 下两核都可能写——本核 rsil1 不是互斥。统一用跨核自旋锁；
 * 加锁函数保存/恢复完整 PS，不会在更高 INTLEVEL/EXCM 现场降级。 */
static pot_lock_t g_trace_lock;

static inline uint32_t trace_lock(void)
{
    return pot_lock_acquire(&g_trace_lock);
}

static inline void trace_unlock(uint32_t ps)
{
    pot_lock_release(&g_trace_lock, ps);
}

void trace_init(void)
{
    g_head = 0;
    g_seq = 0;
    g_imp_head = 0;
    g_imp_seq = 0;
    for (unsigned i = 0; i < TR_TYPE_SLOTS; i++) {
        g_type_count[i] = 0;
    }
}

void trace_push(uint16_t type, uint32_t a, uint32_t b)
{
    uint32_t ps = trace_lock();
    uint16_t h = g_head;
    uint32_t t = time_ticks_raw();
    g_ring[h].t = t;
    g_ring[h].a = a;
    g_ring[h].b = b;
    g_ring[h].type = type;
    g_ring[h].seq = (uint16_t)g_seq;
    if (type < TR_TYPE_SLOTS) {
        g_type_count[type]++;
    }
    g_seq++;
    g_head = (uint16_t)((h + 1) & (POT_TRACE_CAPACITY - 1));
    if (trace_is_important(type)) { /* 副环：根因现场，长期可导出 */
        uint16_t ih = g_imp_head;
        g_imp_ring[ih].t = t;
        g_imp_ring[ih].a = a;
        g_imp_ring[ih].b = b;
        g_imp_ring[ih].type = type;
        g_imp_ring[ih].seq = (uint16_t)g_imp_seq;
        g_imp_seq++;
        g_imp_head = (uint16_t)((ih + 1) & (POT_TRACE_IMP_CAPACITY - 1));
    }
    trace_unlock(ps);
}

uint32_t trace_important_total(void)
{
    return g_imp_seq;
}

uint32_t trace_dropped(void)
{
    uint32_t s = g_seq;
    return s > POT_TRACE_CAPACITY ? s - POT_TRACE_CAPACITY : 0;
}

uint32_t trace_total(void)
{
    return g_seq;
}

uint32_t trace_count_of(uint16_t type)
{
    if (type >= TR_TYPE_SLOTS) {
        return 0;
    }
    return g_type_count[type];
}

const char *trace_event_name(uint16_t type)
{
    switch (type) {
    case TR_IRQ_ENTER:    return "IRQ_ENTER";
    case TR_IRQ_EXIT:     return "IRQ_EXIT";
    case TR_EXCEPTION:    return "EXCEPTION";
    case TR_TICK_GAP:     return "TICK_GAP";
    case TR_SCHED_SWITCH: return "SCHED_SWITCH";
    case TR_SCHED_DROP:   return "SCHED_DROP";
    case TR_SCHED_AGING:  return "SCHED_AGING";
    case TR_MSG_ENQ:      return "MSG_ENQ";
    case TR_MSG_DEQ:      return "MSG_DEQ";
    case TR_MSG_DROP:     return "MSG_DROP";
    case TR_MSG_RETRY:    return "MSG_RETRY";
    case TR_MSG_ACK:      return "MSG_ACK";
    case TR_MEM_ALLOC:    return "MEM_ALLOC";
    case TR_MEM_FREE:     return "MEM_FREE";
    case TR_MEM_FAULT:    return "MEM_FAULT";
    case TR_NET_TX:       return "NET_TX";
    case TR_NET_RX:       return "NET_RX";
    case TR_NET_DROP:     return "NET_DROP";
    case TR_NET_LINK:     return "NET_LINK";
    case TR_MCP_REQ:      return "MCP_REQ";
    case TR_MCP_RESP:     return "MCP_RESP";
    case TR_MCP_DUP:      return "MCP_DUP";
    case TR_MCP_RESYNC:   return "MCP_RESYNC";
    case TR_MCP_REPLAY:   return "MCP_REPLAY";
    case TR_CPU_UP:       return "CPU_UP";
    case TR_IPI_SEND:     return "IPI_SEND";
    case TR_IPI_RECV:     return "IPI_RECV";
    case TR_SMP_MIGRATE:  return "SMP_MIGRATE";
    case TR_INJECT_BEGIN: return "INJECT_BEGIN";
    case TR_INJECT_END:   return "INJECT_END";
    default:              return "UNKNOWN";
    }
}

/* 快照 head/seq：与写入方临界区不重叠（导出期间仍可继续写入） */
static void trace_snapshot(uint32_t *total, uint16_t *head)
{
    uint32_t ps = trace_lock();
    *total = g_seq;
    *head = g_head;
    trace_unlock(ps);
}

/* 最旧可读序号（被覆盖的已不可恢复） */
static uint32_t trace_oldest(uint32_t total)
{
    uint32_t avail = (total < POT_TRACE_CAPACITY) ? total : POT_TRACE_CAPACITY;
    return total - avail;
}

uint32_t trace_oldest_seq(void)
{
    return trace_oldest(g_seq);
}

static unsigned export_range(uint32_t from_seq, unsigned max_n)
{
    uint32_t total;
    uint16_t head;
    trace_snapshot(&total, &head);

    if (from_seq < trace_oldest(total)) {
        from_seq = trace_oldest(total);
    }
    if (from_seq >= total) {
        return 0u;
    }
    unsigned n = (unsigned)(total - from_seq);
    if (max_n != 0u && n > max_n) {
        n = max_n;
    }
    if (n > 0u) {
        uart_line_begin(); /* K2：后台流式行不从控制台提示符后接续 */
    }

    for (unsigned i = n; i > 0u; i--) {
        uint16_t idx = (uint16_t)((uint16_t)(head - i) & (POT_TRACE_CAPACITY - 1));
        char line[160];
        pot_sb_t b;
        pot_sb_init(&b, line, sizeof(line));
        pot_sb_ch(&b, '{');
        pot_sb_kv_u32(&b, "seq", total - i, 0);
        pot_sb_kv_u32(&b, "t", g_ring[idx].t, 1);
        pot_sb_kv_str(&b, "ev", trace_event_name(g_ring[idx].type), 1);
        pot_sb_kv_u32(&b, "a", g_ring[idx].a, 1);
        pot_sb_kv_u32(&b, "b", g_ring[idx].b, 1);
        pot_sb_ch(&b, '}');
        potato_printf("[POTATO][TRJ] %s\r\n", line);
    }
    return n;
}

unsigned trace_export_jsonl(unsigned max_n)
{
    if (max_n == 0u) {
        max_n = POT_TRACE_CAPACITY;
    }
    uint32_t total = trace_total();
    return export_range(trace_oldest(total), max_n);
}

unsigned trace_export_since(uint32_t *cursor, unsigned max_n)
{
    if (cursor == NULL) {
        return 0u;
    }
    uint32_t total = trace_total();
    uint32_t oldest = trace_oldest(total);
    uint32_t from = *cursor;
    if (from < oldest) { /* 游标落后于环：从最旧可读处继续（导出侧不静默丢） */
        from = oldest;
    }
    unsigned n = export_range(from, max_n);
    if (n != 0u) {
        *cursor = from + n;
    } else if (*cursor < oldest) {
        *cursor = oldest;
    }
    return n;
}

/* ---- M8 重要事件副环导出（行尾带 "imp":1 供分析器区分可靠历史） ---- */

static void imp_snapshot(uint32_t *total, uint16_t *head)
{
    uint32_t ps = trace_lock();
    *total = g_imp_seq;
    *head = g_imp_head;
    trace_unlock(ps);
}

static unsigned export_imp_range(uint32_t from_seq, unsigned max_n)
{
    uint32_t total;
    uint16_t head;
    imp_snapshot(&total, &head);

    uint32_t avail = (total < POT_TRACE_IMP_CAPACITY) ? total : POT_TRACE_IMP_CAPACITY;
    uint32_t oldest = total - avail;
    if (from_seq < oldest) {
        from_seq = oldest;
    }
    if (from_seq >= total) {
        return 0u;
    }
    unsigned n = (unsigned)(total - from_seq);
    if (max_n != 0u && n > max_n) {
        n = max_n;
    }
    if (n > 0u) {
        uart_line_begin(); /* K2：后台流式行不从控制台提示符后接续 */
    }

    for (unsigned i = n; i > 0u; i--) {
        uint16_t idx = (uint16_t)((uint16_t)(head - i) & (POT_TRACE_IMP_CAPACITY - 1));
        char line[176];
        pot_sb_t b;
        pot_sb_init(&b, line, sizeof(line));
        pot_sb_ch(&b, '{');
        pot_sb_kv_u32(&b, "seq", total - i, 0);
        pot_sb_kv_u32(&b, "t", g_imp_ring[idx].t, 1);
        pot_sb_kv_str(&b, "ev", trace_event_name(g_imp_ring[idx].type), 1);
        pot_sb_kv_u32(&b, "a", g_imp_ring[idx].a, 1);
        pot_sb_kv_u32(&b, "b", g_imp_ring[idx].b, 1);
        pot_sb_kv_u32(&b, "imp", 1, 1);
        pot_sb_ch(&b, '}');
        potato_printf("[POTATO][TRJ] %s\r\n", line);
    }
    return n;
}

unsigned trace_export_important(unsigned max_n)
{
    if (max_n == 0u) {
        max_n = POT_TRACE_IMP_CAPACITY;
    }
    return export_imp_range(0u, max_n);
}

unsigned trace_export_important_since(uint32_t *cursor, unsigned max_n)
{
    if (cursor == NULL) {
        return 0u;
    }
    uint32_t total = trace_important_total();
    uint32_t avail = (total < POT_TRACE_IMP_CAPACITY) ? total : POT_TRACE_IMP_CAPACITY;
    uint32_t oldest = total - avail;
    uint32_t from = *cursor;
    if (from < oldest) {
        from = oldest;
    }
    unsigned n = export_imp_range(from, max_n);
    if (n != 0u) {
        *cursor = from + n;
    } else if (*cursor < oldest) {
        *cursor = oldest;
    }
    return n;
}

uint32_t trace_meta_json(char *buf, uint32_t cap, int stream_on)
{
    pot_sb_t b;
    pot_sb_init(&b, buf, cap);
    pot_sb_ch(&b, '{');
    pot_sb_kv_u32(&b, "total", g_seq, 0);
    pot_sb_kv_u32(&b, "dropped", trace_dropped(), 1);
    pot_sb_kv_u32(&b, "cap", POT_TRACE_CAPACITY, 1);
    pot_sb_kv_u32(&b, "stream", stream_on ? 1u : 0u, 1);
    pot_sb_str(&b, ",\"imp\":{");
    pot_sb_kv_u32(&b, "total", g_imp_seq, 0);
    pot_sb_kv_u32(&b, "cap", POT_TRACE_IMP_CAPACITY, 1);
    pot_sb_kv_u32(&b, "dropped",
                 g_imp_seq > POT_TRACE_IMP_CAPACITY ? g_imp_seq - POT_TRACE_IMP_CAPACITY : 0u, 1);
    pot_sb_ch(&b, '}');
    pot_sb_str(&b, ",\"counts\":[");
    int first = 1;
    for (unsigned t = 0; t < TR_TYPE_SLOTS; t++) {
        if (g_type_count[t] == 0u) {
            continue;
        }
        if (!first) {
            pot_sb_ch(&b, ',');
        }
        first = 0;
        pot_sb_ch(&b, '{');
        pot_sb_kv_str(&b, "ev", trace_event_name((uint16_t)t), 0);
        pot_sb_kv_u32(&b, "n", g_type_count[t], 1);
        pot_sb_ch(&b, '}');
    }
    pot_sb_str(&b, "]}");
    return b.len;
}

void trace_dump(unsigned max_n)
{
    uint32_t total;
    uint16_t head_now;
    trace_snapshot(&total, &head_now);

    unsigned n = (total < max_n) ? total : max_n;
    if (n > POT_TRACE_CAPACITY) {
        n = POT_TRACE_CAPACITY;
    }

    potato_printf("[POTATO][TR] total=%u dropped=%u last %u:\r\n",
                  total, trace_dropped(), n);
    for (unsigned i = n; i > 0u; i--) {
        uint16_t idx = (uint16_t)((uint16_t)(head_now - i) & (POT_TRACE_CAPACITY - 1));
        potato_printf("[POTATO][TR] #%u t=%u %s a=0x%x b=0x%x\r\n",
                      g_ring[idx].seq, g_ring[idx].t,
                      trace_event_name(g_ring[idx].type),
                      g_ring[idx].a, g_ring[idx].b);
    }
}
