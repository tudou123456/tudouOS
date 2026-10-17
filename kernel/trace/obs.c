#include <stdint.h>

#include "console.h"
#include "heap.h"
#include "intc.h"
#include "msg.h"
#include "obs.h"
#include "sb.h"
#include "task.h"
#include "time.h"
#include "trace.h"
#include "uart.h"

/*
 * M8 可观测体系：流式 JSONL 导出 + 健康快照（见 obs.h 契约）。
 *
 * 流式导出任务（BACKGROUND 类）每 100ms 拉一次追踪环增量：
 *   - 游标 g_cursor 单调推进；落后于环最旧可读序号时，缺口计入 g_lost
 *     （诚实报告，不假装连续）
 *   - 每 10 次拉取附一条健康快照，供分析器做时序比对（泄漏/饥饿/抖动）
 *   - 停止导出时不销毁任务（下次开启继续用同一游标），不影响调度负载
 *
 * 输出行全部带 [POTATO] 前缀，与测试台断言锚点约定一致。
 */

#define OBS_STACK_WORDS 384u
#define OBS_HEALTH_EVERY_PUMPS 10u

static pot_task_t g_obs_tcb;
static uint32_t g_obs_stack[OBS_STACK_WORDS];
static pot_task_t *g_obs_task;
static volatile int g_stream_on;
static uint32_t g_cursor;
static uint32_t g_pumped;
static uint32_t g_lost;
static uint32_t g_imp_cursor; /* 重要事件副环游标（M8） */
static uint32_t g_imp_pumped;
static uint32_t g_imp_lost;
static uint32_t g_pump_calls;
static uint32_t g_health_n;
static int g_obs_started;

static char g_json[2048];

static const char *state_name(int st)
{
    switch (st) {
    case POTTASK_READY:    return "ready";
    case POTTASK_RUNNING:  return "run";
    case POTTASK_SLEEPING: return "sleep";
    case POTTASK_DEAD:     return "dead";
    default:               return "?";
    }
}

void pot_obs_init(void)
{
    g_stream_on = 0;
    g_cursor = 0;
    g_pumped = 0;
    g_lost = 0;
    g_imp_cursor = 0;
    g_imp_pumped = 0;
    g_imp_lost = 0;
    g_pump_calls = 0;
    g_health_n = 0;
    g_obs_started = 0;
    g_obs_task = 0;
}

int pot_obs_stream_on(void)
{
    return g_stream_on;
}

uint32_t pot_obs_pumped(void)
{
    return g_pumped;
}

uint32_t pot_obs_lost(void)
{
    return g_lost;
}

uint32_t pot_obs_pump_calls(void)
{
    return g_pump_calls;
}

unsigned pot_obs_pump(void)
{
    /* 重要事件副环优先：容量 64、不被 sched 噪声冲刷，是根因现场 */
    uint32_t imp_total = trace_important_total();
    uint32_t imp_avail = (imp_total < POT_TRACE_IMP_CAPACITY) ? imp_total
                                                              : POT_TRACE_IMP_CAPACITY;
    uint32_t imp_oldest = imp_total - imp_avail;
    if (g_imp_cursor < imp_oldest) {
        g_imp_lost += imp_oldest - g_imp_cursor;
        g_imp_cursor = imp_oldest;
    }
    unsigned ni = trace_export_important_since(&g_imp_cursor, OBS_PUMP_LINES);
    g_imp_pumped += ni;

    /* 缺口检测：游标落后于环内最旧可读序号 => 这些事件已被覆盖 */
    uint32_t oldest = trace_oldest_seq();
    if (g_cursor < oldest) {
        g_lost += oldest - g_cursor;
        g_cursor = oldest;
    }
    unsigned n = trace_export_since(&g_cursor, OBS_PUMP_LINES);
    g_pumped += n;
    g_pump_calls++;
    return n + ni;
}

uint32_t pot_obs_imp_pumped(void)
{
    return g_imp_pumped;
}

uint32_t pot_obs_imp_lost(void)
{
    return g_imp_lost;
}

static void obs_task(void *arg)
{
    (void)arg;
    while (1) {
        if (g_stream_on) {
            (void)pot_obs_pump();
            if ((g_pump_calls % OBS_HEALTH_EVERY_PUMPS) == 0u) {
                pot_obs_health();
            }
        }
        pot_task_sleep(OBS_PUMP_PERIOD_MS);
    }
}

void pot_obs_stream_set(int on)
{
    if (on != 0) {
        if (!g_obs_started) {
            int r = pot_task_create(&g_obs_tcb, "obs", POTCLS_BACKGROUND,
                                    obs_task, 0, g_obs_stack, OBS_STACK_WORDS);
            if (r != 0) {
                uart_puts("[POTATO][OBS] stream task create failed\r\n");
                return;
            }
            g_obs_task = &g_obs_tcb;
            g_obs_started = 1;
        }
        if (!g_stream_on) {
            g_cursor = trace_total(); /* 只导出开启之后的新事件 */
            g_imp_cursor = trace_important_total();
        }
        g_stream_on = 1;
        potato_printf("[POTATO][OBS] stream on cursor=%u pumped=%u lost=%u imp=%u\r\n",
                      g_cursor, g_pumped, g_lost, trace_important_total());
    } else {
        g_stream_on = 0;
        potato_printf("[POTATO][OBS] stream off pumped=%u lost=%u calls=%u\r\n",
                      g_pumped, g_lost, g_pump_calls);
    }
}

uint32_t pot_obs_health_count(void)
{
    return g_health_n;
}

void pot_obs_health(void)
{
    pot_heap_stat_t hs;
    pot_heap_getstat(&hs);
    uint32_t sd[4] = {0, 0, 0, 0};
    pot_sched_diag(sd);
    uint32_t rdrop = 0, sdrop = 0;
    pot_sched_drop_diag(&rdrop, &sdrop);

    pot_sb_t b;
    pot_sb_init(&b, g_json, sizeof(g_json));
    pot_sb_ch(&b, '{');
    pot_sb_kv_u32(&b, "health_n", g_health_n, 0);
    pot_sb_kv_u32(&b, "t", time_tick_count(), 1);
    pot_sb_kv_u32(&b, "now", pot_sched_now(), 1);
    pot_sb_kv_u32(&b, "stream", g_stream_on ? 1u : 0u, 1);

    pot_sb_str(&b, ",\"trace\":{");
    pot_sb_kv_u32(&b, "total", trace_total(), 0);
    pot_sb_kv_u32(&b, "oldest", trace_oldest_seq(), 1);
    pot_sb_kv_u32(&b, "dropped", trace_dropped(), 1);
    pot_sb_kv_u32(&b, "lost", g_lost, 1);
    pot_sb_kv_u32(&b, "pumped", g_pumped, 1);
    pot_sb_ch(&b, '}');

    pot_sb_str(&b, ",\"imp\":{");
    pot_sb_kv_u32(&b, "total", trace_important_total(), 0);
    pot_sb_kv_u32(&b, "cap", POT_TRACE_IMP_CAPACITY, 1);
    pot_sb_kv_u32(&b, "pumped", g_imp_pumped, 1);
    pot_sb_kv_u32(&b, "lost", g_imp_lost, 1);
    pot_sb_ch(&b, '}');

    pot_sb_str(&b, ",\"heap\":{");
    pot_sb_kv_u32(&b, "total", hs.total, 0);
    pot_sb_kv_u32(&b, "used", hs.used, 1);
    pot_sb_kv_u32(&b, "free", hs.total - hs.used, 1);
    pot_sb_kv_u32(&b, "largest", hs.largest, 1);
    pot_sb_kv_u32(&b, "maxused", hs.max_used, 1);
    pot_sb_kv_u32(&b, "alloc", hs.n_alloc, 1);
    pot_sb_kv_u32(&b, "free_n", hs.n_free, 1);
    pot_sb_kv_u32(&b, "fail", hs.n_fail, 1);
    pot_sb_kv_u32(&b, "fault", hs.n_fault, 1);
    pot_sb_ch(&b, '}');

    pot_sb_str(&b, ",\"sched\":{");
    pot_sb_kv_u32(&b, "wake", sd[0], 0);
    pot_sb_kv_u32(&b, "preempt", sd[1], 1);
    pot_sb_kv_u32(&b, "sw", sd[2], 1);
    pot_sb_kv_u32(&b, "idle", sd[3], 1);
    pot_sb_kv_u32(&b, "sw_total", pot_sched_total_switches(), 1);
    pot_sb_kv_u32(&b, "bg_used", pot_sched_bg_used(), 1);
    pot_sb_kv_u32(&b, "rdrop", rdrop, 1);
    pot_sb_kv_u32(&b, "sdrop", sdrop, 1);
    pot_sb_kv_u32(&b, "regover", pot_task_reg_overflow(), 1);
    pot_sb_ch(&b, '}');

    pot_sb_str(&b, ",\"irq\":{");
    pot_sb_kv_u32(&b, "rx", uart_rx_irq_count(), 0);
    pot_sb_kv_u32(&b, "rxb", uart_rx_bytes(), 1);
    pot_sb_kv_u32(&b, "jitmin", time_tick_jitter_min_us(), 1);
    pot_sb_kv_u32(&b, "jitmax", time_tick_jitter_max_us(), 1);
    pot_sb_kv_u32(&b, "wd", time_tick_watchdog_count(), 1);
    /* D-08：调度时钟漂移 = tick 计数 - 调度器时钟。正常应恒为 0/个位数；
       持续增长说明 tick 补账未驱动调度器（sleep 到点不唤醒）。 */
    pot_sb_kv_u32(&b, "lag", time_tick_count() - pot_sched_now(), 1);
    pot_sb_ch(&b, '}');

    pot_sb_str(&b, ",\"tasks\":[");
    int n = pot_task_count();
    for (int i = 0; i < n; i++) {
        pot_task_t *t = pot_task_at(i);
        if (t == 0) {
            continue;
        }
        if (i != 0) {
            pot_sb_ch(&b, ',');
        }
        pot_sb_ch(&b, '{');
        pot_sb_kv_str(&b, "name", t->name != 0 ? t->name : "?", 0);
        pot_sb_kv_u32(&b, "tcb", (uint32_t)(uintptr_t)t, 1);
        pot_sb_kv_str(&b, "cls", pot_class_name(t->cls), 1);
        pot_sb_kv_str(&b, "state", state_name(t->state), 1);
        pot_sb_kv_u32(&b, "cpu", t->cpu_ticks, 1);
        pot_sb_kv_u32(&b, "sw", t->switches, 1);
        pot_sb_kv_u32(&b, "stk", pot_task_stack_unused(t), 1);
        pot_sb_kv_u32(&b, "stktot", t->stk_words, 1);
        pot_sb_kv_u32(&b, "last", t->last_run_tick, 1);
        pot_sb_ch(&b, '}');
    }
    pot_sb_ch(&b, ']');

    pot_sb_str(&b, ",\"ch\":[");
    int nc = pot_channel_count();
    for (int i = 0; i < nc; i++) {
        pot_channel_t *ch = pot_channel_at(i);
        if (ch == 0) {
            continue;
        }
        if (i != 0) {
            pot_sb_ch(&b, ',');
        }
        pot_sb_ch(&b, '{');
        pot_sb_kv_str(&b, "name", ch->name != 0 ? ch->name : "?", 0);
        pot_sb_kv_u32(&b, "enq", ch->c_enq, 1);
        pot_sb_kv_u32(&b, "deq", ch->c_deq, 1);
        pot_sb_kv_u32(&b, "drop_full", ch->c_drop_full, 1);
        pot_sb_kv_u32(&b, "drop_dl", ch->c_drop_deadline, 1);
        pot_sb_kv_u32(&b, "drop_dup", ch->c_drop_dedup, 1);
        pot_sb_kv_u32(&b, "retry", ch->c_retry, 1);
        pot_sb_kv_u32(&b, "ack", ch->c_ack, 1);
        pot_sb_kv_u32(&b, "hw", ch->high_water, 1);
        pot_sb_kv_u32(&b, "count", ch->count, 1);
        pot_sb_ch(&b, '}');
    }
    pot_sb_ch(&b, ']');

    pot_sb_kv_u32(&b, "inject", (uint32_t)pot_inject_active(), 1);
    pot_sb_ch(&b, '}');

    g_health_n++;
    potato_printf("[POTATO][HEALTH] %s\r\n", g_json);
}

void pot_obs_panic_dump(void)
{
    pot_heap_stat_t hs;
    pot_heap_getstat(&hs);
    uint32_t sd[4] = {0, 0, 0, 0};
    pot_sched_diag(sd);
    pot_task_t *cur = pot_current;

    pot_sb_t b;
    pot_sb_init(&b, g_json, sizeof(g_json));
    pot_sb_ch(&b, '{');
    pot_sb_kv_str(&b, "reason", "panic", 0);
    pot_sb_kv_u32(&b, "t", time_tick_count(), 1);
    pot_sb_kv_u32(&b, "now", pot_sched_now(), 1);
    pot_sb_kv_str(&b, "cur", (cur != 0 && cur->name != 0) ? cur->name : "?", 1);
    pot_sb_kv_u32(&b, "cur_tcb", (uint32_t)(uintptr_t)cur, 1);
    pot_sb_kv_u32(&b, "heap_used", hs.used, 1);
    pot_sb_kv_u32(&b, "heap_free", hs.total - hs.used, 1);
    pot_sb_kv_u32(&b, "heap_fault", hs.n_fault, 1);
    pot_sb_kv_u32(&b, "sw", sd[2], 1);
    pot_sb_kv_u32(&b, "preempt", sd[1], 1);
    pot_sb_kv_u32(&b, "trace_total", trace_total(), 1);
    pot_sb_kv_u32(&b, "trace_dropped", trace_dropped(), 1);
    pot_sb_kv_u32(&b, "inject", (uint32_t)pot_inject_active(), 1);
    pot_sb_ch(&b, '}');
    potato_printf("[POTATO][PANIC] %s\r\n", g_json);
    (void)trace_export_important(0); /* 先给根因现场（64 条可靠历史） */
    (void)trace_export_jsonl(32);
}
