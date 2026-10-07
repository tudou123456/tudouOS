#include <stdint.h>

#include "console.h"
#include "lib_shim.h"
#include "msg.h"
#include "msgdemo.h"
#include "task.h"
#include "uart.h"

/*
 * 消息基座演示/压测（M3.5 验收配套）——`msgdemo` 命令驱动。
 *   flood N   : QoS0 洪泛无接收方 → 验证有界不 OOM、丢新计数准确
 *   ping N    : QoS1 收发+回执 → 全部送达且回执，零重发
 *   lazy N    : 接收方延迟回执 → 触发重发+去重（恰好一次投递）
 *   timeout   : 无接收方 QoS1 → 重发耗尽后失败上报
 *   heal N    : 慢回执流中 QMP stop/cont 冻结 → 恢复后无丢无重
 *   off       : 停止请求
 * 通道容量 32（cap 2 的幂）；重生成纪律同 demo.c（等 DEAD）。
 */

#define MSG_CH_CAP 32u

static pot_channel_t g_ch;
static pot_msg_t g_ring[MSG_CH_CAP];
static int g_ch_ready;

static pot_task_t g_t_snd, g_t_rcv;
static uint32_t g_stk_snd[1024], g_stk_rcv[1024];

static volatile unsigned g_n;       /* 本轮消息数 */
static volatile unsigned g_ack_ms;  /* 接收方回执延迟 */
static volatile unsigned g_snd_to;  /* 发送超时 ms */
static volatile unsigned g_snd_qos; /* 发送 QoS（避免任务里再 strcmp） */

static void ch_init_once(void)
{
    if (!g_ch_ready) {
        pot_channel_init(&g_ch, "demo", g_ring, MSG_CH_CAP, 0);
        g_ch_ready = 1;
    }
}

/* ---------- 发送方 ---------- */

static void sender_task(void *arg)
{
    (void)arg;
    uint32_t ok = 0, dropped = 0, failed = 0;
    unsigned n = g_n;
    unsigned qos = g_snd_qos;

    for (uint32_t i = 1; i <= n && !pot_task_self()->stop_req; i++) {
        pot_msg_t m;
        memset(&m, 0, sizeof(m));
        m.id = i;
        m.type = 1;
        m.qos = (uint16_t)qos;
        m.len = 4;
        m.data[0] = (uint8_t)i;
        int r = pot_msg_send(&g_ch, &m, g_snd_to);
        if (r == 0) {
            ok++;
        } else if (r == 1) {
            dropped++;
        } else {
            failed++;
        }
    }
    if (qos == POT_MSG_QOS0) {
        potato_printf("[POTATO][M] FLOOD-DONE sent=%u ok=%u dropped=%u\r\n",
                      n, ok, dropped);
    } else {
        potato_printf("[POTATO][M] PING-DONE ok=%u failed=%u n=%u\r\n",
                      ok, failed, n);
    }
    pot_task_exit();
}

/* 超时演示专用：单条 QoS1，直接打印结果 */
static void sender_timeout_task(void *arg)
{
    (void)arg;
    pot_msg_t m;
    memset(&m, 0, sizeof(m));
    m.id = 1;
    m.type = 1;
    m.qos = POT_MSG_QOS1;
    m.len = 4;
    int r = pot_msg_send(&g_ch, &m, 100u);
    potato_printf("[POTATO][M] TIMEOUT-RESULT r=%d retry=%u dead=%u\r\n",
                  r, g_ch.c_retry, g_ch.c_drop_deadline);
    pot_task_exit();
}

/* ---------- 接收方 ---------- */

static uint32_t g_rmin = 0xFFFFFFFFu, g_rmax, g_rdup;

static void receiver_task(void *arg)
{
    (void)arg;
    unsigned n = g_n;
    uint32_t got = 0;
    uint32_t previd = 0;
    pot_msg_t m;
    for (;;) {
        if (pot_task_self()->stop_req) {
            break;
        }
        if (pot_msg_recv(&g_ch, &m, 1000u) != 0) {
            if (got >= n) {
                break; /* 收齐后超时退出 */
            }
            continue;
        }
        if (m.id < g_rmin) {
            g_rmin = m.id;
        }
        if (m.id > g_rmax) {
            g_rmax = m.id;
        }
        if (got > 0 && m.id == previd) {
            g_rdup++;
        }
        previd = m.id;
        if (g_ack_ms != 0u) {
            pot_task_sleep(g_ack_ms); /* 延迟回执（触发重发/去重） */
        }
        pot_msg_ack(&g_ch, &m);
        got++;
        if (got >= n) {
            break;
        }
    }
    potato_printf("[POTATO][M] RECV-DONE n=%u min=%u max=%u dup=%u\r\n",
                  got, g_rmin, g_rmax, g_rdup);
    pot_task_exit();
}

/* ---------- 任务管理 ---------- */

static int spawn(pot_task_t *t, uint32_t *stk, const char *name,
                 pot_task_class_t cls, pot_task_fn_t fn, void *arg)
{
    if (t->name != 0 && t->state != POTTASK_DEAD) {
        return -1;
    }
    return pot_task_create(t, name, cls, fn, arg, stk, 1024);
}

static int all_dead(void)
{
    if (g_t_snd.name != 0 && g_t_snd.state != POTTASK_DEAD) {
        return 0;
    }
    if (g_t_rcv.name != 0 && g_t_rcv.state != POTTASK_DEAD) {
        return 0;
    }
    return 1;
}

static const char *g_arg_q0 = "q0";
static const char *g_arg_q1 = "q1";

void msgdemo_stop(void)
{
    g_t_snd.stop_req = 1;
    g_t_rcv.stop_req = 1;
}

int msgdemo_start(const char *mode, unsigned n)
{
    msgdemo_stop();
    for (volatile int i = 0; i < 300 && !all_dead(); i++) {
        pot_task_sleep(1);
    }
    if (!all_dead()) {
        return -1;
    }

    pot_channel_init(&g_ch, "demo", g_ring, MSG_CH_CAP, 0); /* 每轮重置 */
    g_ch_ready = 1;
    g_n = (n == 0u) ? 8u : n;
    g_ack_ms = 0;
    g_snd_to = 1000u;

    if (strcmp(mode, "flood") == 0) {
        g_snd_qos = POT_MSG_QOS0;
        return spawn(&g_t_snd, g_stk_snd, "msd", POTCLS_BACKGROUND,
                     sender_task, (void *)g_arg_q0);
    }
    if (strcmp(mode, "ping") == 0) {
        g_snd_qos = POT_MSG_QOS1;
        g_snd_to = 800u;
        if (spawn(&g_t_rcv, g_stk_rcv, "mrc", POTCLS_AI_COMMS,
                  receiver_task, 0) != 0) {
            return -1;
        }
        return spawn(&g_t_snd, g_stk_snd, "msd", POTCLS_AI_COMMS,
                     sender_task, (void *)g_arg_q1);
    }
    if (strcmp(mode, "lazy") == 0) {
        g_snd_qos = POT_MSG_QOS1;
        g_snd_to = 100u;   /* 100ms 回执窗口：200ms 延迟必触发 2 次重发 */
        g_ack_ms = 200u;
        if (spawn(&g_t_rcv, g_stk_rcv, "mrc", POTCLS_UI,
                  receiver_task, 0) != 0) {
            return -1;
        }
        return spawn(&g_t_snd, g_stk_snd, "msd", POTCLS_AI_COMMS,
                     sender_task, (void *)g_arg_q1);
    }
    if (strcmp(mode, "timeout") == 0) {
        return spawn(&g_t_snd, g_stk_snd, "msd", POTCLS_BACKGROUND,
                     sender_timeout_task, 0);
    }
    if (strcmp(mode, "heal") == 0) {
        g_snd_qos = POT_MSG_QOS1;
        g_snd_to = 1000u;
        g_ack_ms = 120u; /* 慢回执流：给 QMP 冻结留时间窗 */
        if (spawn(&g_t_rcv, g_stk_rcv, "mrc", POTCLS_UI,
                  receiver_task, 0) != 0) {
            return -1;
        }
        return spawn(&g_t_snd, g_stk_snd, "msd", POTCLS_AI_COMMS,
                     sender_task, (void *)g_arg_q1);
    }
    return -2;
}

void msgdemo_stat(void)
{
    if (!g_ch_ready) {
        uart_puts("[POTATO][M] msg channel not created yet\r\n");
        return;
    }
    potato_printf("[POTATO][M] ch=%s cap=%u cnt=%u hw=%u enq=%u deq=%u "
                  "full=%u dedup=%u dead=%u retry=%u ack=%u "
                  "dd0=%x/%u ak0=%x/%u ak1=%x/%u\r\n",
                  g_ch.name, g_ch.cap, g_ch.count,
                  g_ch.high_water, g_ch.c_enq, g_ch.c_deq, g_ch.c_drop_full,
                  g_ch.c_drop_dedup, g_ch.c_drop_deadline, g_ch.c_retry,
                  g_ch.c_ack,
                  (uint32_t)g_ch.dedup[0].sender, g_ch.dedup[0].id,
                  (uint32_t)g_ch.acked[0].sender, g_ch.acked[0].id,
                  (uint32_t)g_ch.acked[1].sender, g_ch.acked[1].id);
}

const char *msgdemo_mode_help(void)
{
    return "msgdemo flood|ping|lazy|timeout|heal [n] | off";
}
