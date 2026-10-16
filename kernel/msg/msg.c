#include <stdint.h>

#include "intc.h"
#include "lib_shim.h"
#include "msg.h"
#include "task.h"
#include "tick.h"
#include "trace.h"

/*
 * 消息基座实现（M3.5）。
 *
 * 等待语义 v2：切片睡眠轮询——send/recv 在等待期以 2ms 切片睡眠并重查
 * 条件，只用 pot_task_sleep/pot_tick_poll/pot_sched_now 三个 M3 已被
 * 14 用例验证的原语。v1 的"显式唤醒（waiters+pot_sched_wake）"在该
 * QEMU 上行为不可解释（唤醒永不落地），整体废弃；真机阶段可恢复。
 *
 * 一切容量静态：通道环形缓冲 + 定宽数组的去重/回执窗口。
 */

#define POT_MSG_SLICE_MS 2u /* 等待轮询切片 */

/* M8：通道注册表（健康快照遍历用；初始化即登记，去重） */
#define POT_CH_REG_MAX 8
static pot_channel_t *g_ch_reg[POT_CH_REG_MAX];
static int g_ch_n;

int pot_channel_count(void)
{
    return g_ch_n;
}

pot_channel_t *pot_channel_at(int i)
{
    return (i >= 0 && i < g_ch_n) ? g_ch_reg[i] : 0;
}

static void channel_register(pot_channel_t *ch)
{
    for (int i = 0; i < g_ch_n; i++) {
        if (g_ch_reg[i] == ch) {
            return;
        }
    }
    if (g_ch_n < POT_CH_REG_MAX) {
        g_ch_reg[g_ch_n++] = ch;
    }
}

static inline int past(uint32_t deadline)
{
    return (int32_t)(pot_sched_now() - deadline) >= 0;
}

int pot_channel_init(pot_channel_t *ch, const char *name,
                     pot_msg_t *ring, uint16_t cap, int drop_oldest)
{
    if (ch == 0 || ring == 0 || cap == 0 || (cap & (cap - 1)) != 0) {
        return -1;
    }
    memset(ch, 0, sizeof(*ch));
    ch->name = name;
    ch->ring = ring;
    ch->cap = cap;
    ch->drop_oldest = (uint8_t)(drop_oldest != 0);
    channel_register(ch);
    return 0;
}

/* ---------- 窗口操作 ----------
 * 扫描循环的 volatile 索引为历史纪律（E-01 已证伪 "QEMU 不支持 loop"，
 * 见 lib_shim.c 注释），保留无害。 */

static int dedup_hit(const pot_channel_t *ch, pot_task_t *s, uint32_t id)
{
    for (volatile int i = 0; i < 8; i++) {
        if (ch->dedup[i].sender == s && ch->dedup[i].id == id) {
            return 1;
        }
    }
    return 0;
}

static void dedup_add(pot_channel_t *ch, pot_task_t *s, uint32_t id)
{
    ch->dedup[ch->dedup_idx].sender = s;
    ch->dedup[ch->dedup_idx].id = id;
    ch->dedup_idx = (uint8_t)((ch->dedup_idx + 1) & 7);
}

static int acked_hit(const pot_channel_t *ch, pot_task_t *s, uint32_t id)
{
    for (volatile int i = 0; i < 8; i++) {
        if (ch->acked[i].sender == s && ch->acked[i].id == id) {
            return 1;
        }
    }
    return 0;
}

/* ---------- 入队/出队 ---------- */

/* 返回：0=入队 1=重复（幂等跳过） 2=满（需阻塞等空间） 3=QoS0 丢新 */
static int enq(pot_channel_t *ch, pot_msg_t *m)
{
    /* D-01：去重只对 QoS1/2 有意义（存在重发路径）；QoS0 无重发，
     * 去重只能误杀合法新消息（历史上 id 未赋值时曾整通道瘫痪） */
    if (m->qos != POT_MSG_QOS0 && dedup_hit(ch, m->sender, m->id)) {
        ch->c_drop_dedup++;
        trace_push(TR_MSG_DROP, (uint32_t)ch, 3);
        return 1; /* 重发且原消息已入队：幂等 */
    }

    if (ch->count >= ch->cap) {
        if (m->qos == POT_MSG_QOS0) {
            ch->c_drop_full++;
            trace_push(TR_MSG_DROP, (uint32_t)ch, 1);
            return 3; /* 丢新 */
        }
        if (m->qos == POT_MSG_QOS1 && ch->drop_oldest) {
            ch->c_drop_full++;
            trace_push(TR_MSG_DROP, (uint32_t)ch, 1);
            ch->head = (uint16_t)((ch->head + 1) & (ch->cap - 1));
            ch->count--;
        } else {
            return 2; /* 阻塞等空间 */
        }
    }

    uint16_t slot = (uint16_t)((ch->head + ch->count) & (ch->cap - 1));
    ch->ring[slot] = *m;
    ch->ring[slot].seq = ch->next_seq++;
    ch->count++;
    if (ch->count > ch->high_water) {
        ch->high_water = ch->count;
    }
    dedup_add(ch, m->sender, m->id);
    ch->c_enq++;
    trace_push(TR_MSG_ENQ, (uint32_t)ch, m->id);
    return 0;
}

static int deq(pot_channel_t *ch, pot_msg_t *out)
{
    if (ch->count == 0) {
        return -1;
    }
    *out = ch->ring[ch->head];
    ch->head = (uint16_t)((ch->head + 1) & (ch->cap - 1));
    ch->count--;
    ch->c_deq++;
    trace_push(TR_MSG_DEQ, (uint32_t)ch, out->id);
    return 0;
}

/* ---------- 公共 API ---------- */

int pot_msg_send(pot_channel_t *ch, pot_msg_t *m, uint32_t timeout_ms)
{
    uint32_t deadline = pot_sched_now() + timeout_ms;
    m->sender = pot_current;
    m->deadline = deadline;

    uint32_t backoff = POT_MSG_RETRY_BASE_MS;
    int attempts = 0;

    for (;;) {
        pot_tick_poll();
        /* D-04：通道环/窗口多步变更非重入（发送方与接收方任务在 v5
           真抢占下并发），入队段加临界区；等待段在临界区外睡眠 */
        uint32_t ps = irq_save();
        int r = enq(ch, m);
        irq_restore(ps);
        if (r == 3) {
            return 1; /* QoS0 满丢新 */
        }
        if (r == 2) {
            if (past(deadline)) {
                ch->c_drop_deadline++;
                trace_push(TR_MSG_DROP, (uint32_t)ch, 2);
                return -1;
            }
            pot_task_sleep(POT_MSG_SLICE_MS); /* 等空间 */
            continue;
        }
        /* r==0 首次入队 / r==1 重发幂等 */
        if (m->qos == POT_MSG_QOS0) {
            return 0;
        }

        /* QoS1/2：等回执（切片轮询） */
        for (;;) {
            uint32_t ps = irq_save();
            int hit = acked_hit(ch, m->sender, m->id);
            irq_restore(ps);
            if (hit) {
                break;
            }
            pot_tick_poll();
            if (past(deadline)) {
                attempts++;
                if (attempts > POT_MSG_MAX_RETRY) {
                    ch->c_drop_deadline++;
                    trace_push(TR_MSG_DROP, (uint32_t)ch, 2);
                    return -1; /* 重发预算耗尽，上报失败 */
                }
                ch->c_retry++;
                trace_push(TR_MSG_RETRY, (uint32_t)ch, m->id);
                if (m->qos != POT_MSG_QOS2) {
                    pot_task_sleep(backoff); /* QoS1 指数退避 */
                    deadline = pot_sched_now() + backoff;
                    backoff *= 2;
                } else {
                    deadline = pot_sched_now() + timeout_ms; /* QoS2 不退避 */
                }
                m->deadline = deadline;
                break; /* 回到 enq（去重窗口保证幂等） */
            }
            pot_task_sleep(POT_MSG_SLICE_MS);
        }
        {
            uint32_t ps = irq_save();
            int hit = acked_hit(ch, m->sender, m->id);
            irq_restore(ps);
            if (hit) {
                return 0; /* 已回执 */
            }
        }
        /* 超时重发路径：回到循环头 */
    }
}

int pot_msg_recv(pot_channel_t *ch, pot_msg_t *out, uint32_t timeout_ms)
{
    uint32_t deadline = pot_sched_now() + timeout_ms;

    for (;;) {
        pot_tick_poll();
        uint32_t ps = irq_save(); /* D-04：环指针多步变更 */
        int got = deq(ch, out);
        irq_restore(ps);
        if (got == 0) {
            return 0;
        }
        if (past(deadline)) {
            return -1;
        }
        pot_task_sleep(POT_MSG_SLICE_MS);
    }
}

void pot_msg_ack(pot_channel_t *ch, const pot_msg_t *m)
{
    uint32_t ps = irq_save(); /* D-04：回执窗口多步变更 */
    if (acked_hit(ch, m->sender, m->id)) {
        irq_restore(ps);
        return;
    }
    ch->acked[ch->acked_idx].sender = m->sender;
    ch->acked[ch->acked_idx].id = m->id;
    ch->acked_idx = (uint8_t)((ch->acked_idx + 1) & 7);
    ch->c_ack++;
    irq_restore(ps);
    trace_push(TR_MSG_ACK, (uint32_t)ch, m->id);
}
