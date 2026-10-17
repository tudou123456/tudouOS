#include <stdint.h>

#include "console.h"
#include "mcp.h"
#include "net.h"
#include "potato.h"
#include "task.h"
#include "trace.h"

/*
 * 远程 MCP 通道（M7 支柱2）：UDP 上的 JSON-RPC 隧道 + 消息纪元重同步。
 *
 * 为什么需要它：本地 MCP 走 UART 行协议（transport=uart-line），离开调试串口
 * 就没有控制面。M7 把同一条 MCP 行协议架到以太网上（transport=udp-json），
 * 并解决「链路断了又回来」这类嵌入式场景必然遇到的问题：
 *
 *   1) 纪元 epoch —— 每次链路 down→up 递增。跨纪元的残包按过期丢弃，
 *      不重放、不串味。
 *   2) 游标 seq/cursor —— 请求由对端编号，本端记录已处理的 (epoch,seq)，
 *      重复请求只重放缓存响应（幂等），不重复执行副作用。
 *   3) 去重 + 重放 —— 最近 NETMCP_SEEN 个请求的响应原文缓存，
 *      重连后把未确认（ack 之前）的响应重发，实现「至少一次 + 幂等」。
 *   4) 抖动/缺口可观测 —— 每个环节打追踪事件与 [POTATO][NETMCP] 锚点行。
 *
 * 传输细节：QEMU SLIRP 的 hostfwd 只支持 TCP，因此通道由**内核侧主动发起**
 * （周期 hello 到 netmcp_set_peer 指定的对端），对端记下源地址后即可回包。
 * 这正是真机场景（设备主动连服务器）的形态，QEMU 与真机同源。
 *
 * 本端输出抽头：uart_putc 的 sink 把 MCP 响应行（{"jsonrpc" 开头）抓出来，
 * 仅在「正在处理远程请求」期间转发，避免把本地串口流量镜像到网络。
 */

#define NETMCP_MAGIC     1u
#define NETMCP_HELLO_MS  500u    /* hello 重试周期 */
#define NETMCP_REPLAY_MS 400u    /* 未确认响应的重发周期 */
#define NETMCP_SEEN      8u      /* 请求去重窗口 */
#define NETMCP_SLOTS     8u      /* 响应重放槽（也是已发未确认窗口） */
#define NETMCP_BODY_MAX  1200u   /* 单条 MCP 行上限（含转义前） */
#define NETMCP_DGRAM_MAX 1400u   /* 单报文体上限 */

/* 一个重放槽：响应原文 + 元数据 */
typedef struct {
    char body[NETMCP_BODY_MAX];
    uint16_t len;
    uint32_t seq;
    uint32_t epoch;
    uint8_t valid;
    uint8_t sent_ms_ago_low; /* 占位，未用 */
} netmcp_slot_t;

static netmcp_slot_t g_slots[NETMCP_SLOTS];
static uint32_t g_next_slot;

/* 请求去重窗口 */
static uint32_t g_seen_seq[NETMCP_SEEN];
static uint32_t g_seen_epoch[NETMCP_SEEN];
static int8_t g_seen_slot[NETMCP_SEEN];
static uint32_t g_seen_head;

static uint32_t g_epoch;                   /* 会话纪元：0=未上线；首次上线后为 1，之后每次重连 +1 */
static uint16_t g_udp_port = 47654u;
static uint32_t g_peer_ip = 0x0A000202u;   /* 10.0.2.2 = SLIRP 宿主 */
static uint16_t g_peer_port = 47654u;
static int g_peer_known;                   /* 收到过对端任何报文 */
static int g_peer_configured;

static uint32_t g_tx_seq;      /* 已发出的响应序号（连续） */
static uint32_t g_ack;         /* 对端已确认到的响应序号（累积） */
static uint32_t g_peer_seq;    /* 已处理的请求序号（游标） */
static int g_have_cursor;

static uint32_t g_rx_msg, g_tx_msg, g_reqs, g_dups, g_replays, g_resyncs;
static uint32_t g_hellos, g_bad, g_gaps, g_trunc, g_ack_advances;
static uint32_t g_last_hello_ms, g_last_replay_ms;
static uint32_t g_send_fail;      /* netmcp_send 失败次数（转义/超长/发送失败） */
static int g_link;

/* 抽头 */
static char g_tap_buf[NETMCP_BODY_MAX + 64u];
static uint32_t g_tap_len;
static int g_tap_busy;
static char g_line[NETMCP_BODY_MAX + 64u];
static int g_in_remote_req;
static uint32_t g_resp_lines;
static uint32_t g_tap_long;       /* 行超长被丢弃 */
static uint32_t g_tap_filtered;   /* 非 jsonrpc 行或非远程请求上下文的行 */

static void netmcp_line_complete(const char *line, uint32_t len);
static void netmcp_handle_req(uint32_t epoch, uint32_t seq, const char *msg);
static uint32_t str_len(const char *s);

/* ---------- 输出抽头：行组装 ---------- */
void netmcp_uart_tap(char c)
{
    if (g_tap_busy) {
        return;   /* 处理过程中自身打印的行一律忽略（防重入/防自激） */
    }
    if (c == '\r') {
        return;
    }
    if (c == '\n') {
        if (g_tap_len == 0u) {
            return;
        }
        uint32_t n = g_tap_len;
        for (uint32_t i = 0; i < n; i++) {
            g_line[i] = g_tap_buf[i];
        }
        g_line[n] = '\0';
        g_tap_len = 0;
        g_tap_busy = 1;
        netmcp_line_complete(g_line, n);
        g_tap_busy = 0;
        return;
    }
    if (g_tap_len + 1u < sizeof(g_tap_buf)) {
        g_tap_buf[g_tap_len++] = c;
    } else {
        if (g_tap_len != 0u) {
            g_tap_long++;    /* 超长行：整行丢弃（不转发截断的假行） */
        }
        g_tap_len = 0;
    }
}

/* ---------- 极简 JSON 取值（信封专用，不做通用解析） ---------- */
static const char *jfind(const char *s, const char *key)
{
    uint32_t klen = 0;
    while (key[klen] != '\0') {
        klen++;
    }
    for (const char *p = s; *p != '\0'; p++) {
        if (*p != '"') {
            continue;
        }
        uint32_t i = 0;
        while (i < klen && p[1 + i] == key[i]) {
            i++;
        }
        if (i == klen && p[1 + klen] == '"') {
            return p + 2 + klen;
        }
    }
    return 0;
}

static int jget_u32(const char *s, const char *key, uint32_t *out)
{
    const char *p = jfind(s, key);
    if (p == 0) {
        return -1;
    }
    while (*p == ':' || *p == ' ') {
        p++;
    }
    if (*p < '0' || *p > '9') {
        return -1;
    }
    uint32_t v = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10u + (uint32_t)(*p - '0');
        p++;
    }
    *out = v;
    return 0;
}

static int jget_type(const char *s, char *out, uint32_t cap)
{
    const char *p = jfind(s, "type");
    if (p == 0) {
        return -1;
    }
    while (*p == ':' || *p == ' ') {
        p++;
    }
    if (*p != '"') {
        return -1;
    }
    p++;
    uint32_t o = 0;
    while (*p != '"' && *p != '\0' && o + 1u < cap) {
        out[o++] = *p++;
    }
    out[o] = '\0';
    return 0;
}

/* 取 body 字符串并反转义；返回长度，-1=缺失 */
static int jget_body(const char *s, char *out, uint32_t cap)
{
    const char *p = jfind(s, "body");
    if (p == 0) {
        return -1;
    }
    while (*p == ':' || *p == ' ') {
        p++;
    }
    if (*p != '"') {
        return -1;
    }
    p++;
    uint32_t o = 0;
    while (*p != '\0' && *p != '"') {
        char c = *p++;
        if (c == '\\') {
            char e = *p++;
            if (e == 'n') {
                c = '\n';
            } else if (e == 'r') {
                c = '\r';
            } else if (e == 't') {
                c = '\t';
            } else if (e == '"' || e == '\\' || e == '/') {
                c = e;
            } else if (e == 'u') {
                /* \uXXXX：只支持 ASCII 直落（内核 MCP 输出不含非 ASCII） */
                uint32_t v = 0;
                for (int i = 0; i < 4 && *p != '\0'; i++) {
                    char h = *p++;
                    v <<= 4;
                    if (h >= '0' && h <= '9') {
                        v |= (uint32_t)(h - '0');
                    } else if (h >= 'a' && h <= 'f') {
                        v |= (uint32_t)(h - 'a' + 10);
                    } else if (h >= 'A' && h <= 'F') {
                        v |= (uint32_t)(h - 'A' + 10);
                    }
                }
                c = (v < 0x80u) ? (char)v : '?';
            } else {
                c = e;
            }
        }
        if (o + 1u >= cap) {
            return -1;   /* 放不下：整条丢弃 */
        }
        out[o++] = c;
    }
    out[o] = '\0';
    return (int)o;
}

/* ---------- 发送 ---------- */
static uint32_t str_len(const char *s)
{
    uint32_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

/* 把 body 按 JSON 字符串转义写入 out（不含外层引号）；返回写入长度或 -1 */
static int jesc(const char *s, uint32_t len, char *out, uint32_t cap)
{
    uint32_t o = 0;
    for (uint32_t i = 0; i < len; i++) {
        char c = s[i];
        if (c == '"' || c == '\\') {
            if (o + 2u >= cap) {
                return -1;
            }
            out[o++] = '\\';
            out[o++] = c;
        } else if (c == '\n') {
            if (o + 2u >= cap) {
                return -1;
            }
            out[o++] = '\\';
            out[o++] = 'n';
        } else if (c == '\r' || c == '\t') {
            if (o + 2u >= cap) {
                return -1;
            }
            out[o++] = '\\';
            out[o++] = (c == '\r') ? 'r' : 't';
        } else {
            if (o + 1u >= cap) {
                return -1;
            }
            out[o++] = c;
        }
    }
    out[o] = '\0';
    return (int)o;
}

static char g_dgram[NETMCP_DGRAM_MAX + 160u];

static int netmcp_send(const char *type, uint32_t seq, const char *body,
                       uint32_t body_len)
{
    if (!g_peer_configured) {
        return -1;
    }
    char *p = g_dgram;
    uint32_t cap = sizeof(g_dgram);
    uint32_t o = 0;
    /* 手写信封：{"pot":1,"epoch":E,"seq":S,"ack":A,"type":"T","body":"..."} */
    const char *head = "{\"pot\":1,\"epoch\":";
    for (uint32_t i = 0; head[i] != '\0'; i++) {
        p[o++] = head[i];
    }
    uint32_t nums[3];
    nums[0] = g_epoch;
    nums[1] = seq;
    nums[2] = g_peer_seq;
    for (int k = 0; k < 3; k++) {
        char tmp[12];
        int n = 0;
        uint32_t v = nums[k];
        do {
            tmp[n++] = (char)('0' + (v % 10u));
            v /= 10u;
        } while (v != 0u);
        while (n > 0) {
            p[o++] = tmp[--n];
        }
        const char *mid = (k == 0) ? ",\"seq\":" : ((k == 1) ? ",\"ack\":" : "");
        for (uint32_t i = 0; mid[i] != '\0'; i++) {
            p[o++] = mid[i];
        }
    }
    const char *tail = ",\"type\":\"";
    for (uint32_t i = 0; tail[i] != '\0'; i++) {
        p[o++] = tail[i];
    }
    for (uint32_t i = 0; type[i] != '\0'; i++) {
        p[o++] = type[i];
    }
    const char *bt = "\",\"body\":\"";
    for (uint32_t i = 0; bt[i] != '\0'; i++) {
        p[o++] = bt[i];
    }
    int n = jesc(body, body_len, p + o, cap - o - 3u);
    if (n < 0) {
        g_send_fail++;
        g_trunc++;
        return -1;
    }
    o += (uint32_t)n;
    p[o++] = '"';
    p[o++] = '}';
    if (o >= NETMCP_DGRAM_MAX) {
        g_send_fail++;
        g_trunc++;
        return -1;
    }
    if (net_send_udp(g_peer_ip, g_peer_port, g_udp_port, g_dgram, o) != 0) {
        g_send_fail++;
        return -1;
    }
    g_tx_msg++;
    return 0;
}

static void netmcp_send_hello(void)
{
    char body[96];
    const char *mac = net_mac_str();
    uint32_t o = 0;
    const char *pre = "{\"mac\":\"";
    for (uint32_t i = 0; pre[i] != '\0'; i++) {
        body[o++] = pre[i];
    }
    for (uint32_t i = 0; mac[i] != '\0' && o + 2u < sizeof(body); i++) {
        body[o++] = mac[i];
    }
    body[o++] = '"';
    body[o++] = '}';
    body[o] = '\0';
    if (netmcp_send("hello", g_tx_seq, body, o) == 0) {
        g_hellos++;
    }
}

/* 重放槽：把响应原文存一份（供去重重放/重连重发） */
static int slot_store(const char *body, uint32_t len, uint32_t seq)
{
    if (len > NETMCP_BODY_MAX) {
        g_trunc++;
        return -1;
    }
    netmcp_slot_t *s = &g_slots[g_next_slot % NETMCP_SLOTS];
    g_next_slot++;
    for (uint32_t i = 0; i < len; i++) {
        s->body[i] = body[i];
    }
    s->body[len] = '\0';
    s->len = (uint16_t)len;
    s->seq = seq;
    s->epoch = g_epoch;
    s->valid = 1;
    return (int)((g_next_slot - 1u) % NETMCP_SLOTS);
}

static void slot_drop_upto(uint32_t ack)
{
    for (uint32_t i = 0; i < NETMCP_SLOTS; i++) {
        if (g_slots[i].valid && g_slots[i].epoch == g_epoch &&
            g_slots[i].seq <= ack) {
            g_slots[i].valid = 0;
        }
    }
}

static uint32_t slots_pending(void)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < NETMCP_SLOTS; i++) {
        if (g_slots[i].valid && g_slots[i].epoch == g_epoch) {
            n++;
        }
    }
    return n;
}

static void replay_pending(const char *why)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < NETMCP_SLOTS; i++) {
        if (g_slots[i].valid && g_slots[i].epoch == g_epoch) {
            if (netmcp_send("resp", g_slots[i].seq, g_slots[i].body,
                            g_slots[i].len) == 0) {
                n++;
            }
        }
    }
    if (n != 0u) {
        g_replays += n;
        trace_push(TR_MCP_REPLAY, n, g_epoch);
        potato_printf("[POTATO][NETMCP] replay %s n=%u epoch=%u cursor=%u\r\n",
                      why, (unsigned)n, (unsigned)g_epoch,
                      (unsigned)g_peer_seq);
    }
}

/* ---------- 收到的响应行（抽头回调，运行在处理远程请求的上下文里） ---------- */
static void netmcp_line_complete(const char *line, uint32_t len)
{
    if (len < 10u || line[0] != '{' || line[1] != '"') {
        g_tap_filtered++;
        return;
    }
    if (line[2] != 'j' || line[3] != 's' || line[4] != 'o' || line[5] != 'n') {
        g_tap_filtered++;
        return;
    }
    if (!g_in_remote_req) {
        g_tap_filtered++;
        return;   /* 本地串口发起的响应：不镜像到网络 */
    }
    uint32_t seq = g_tx_seq + 1u;
    int slot = slot_store(line, len, seq);
    if (slot < 0) {
        g_send_fail++;
        return;
    }
    g_tx_seq = seq;
    g_resp_lines++;
    if (netmcp_send("resp", seq, line, len) == 0) {
        trace_push(TR_MCP_RESP, seq, len);
        potato_printf("[POTATO][NETMCP] resp epoch=%u seq=%u bytes=%u cursor=%u\r\n",
                      (unsigned)g_epoch, (unsigned)seq, (unsigned)len,
                      (unsigned)g_peer_seq);
    } else {
        /* 行已进重放槽（本地拷出可用），但单报文发不出去：已由 netmcp_send
         * 记账；这里补一条可诊断打印，避免"请求已执行却杳无音信"的静默。 */
        potato_printf("[POTATO][NETMCP] resp send failed seq=%u bytes=%u "
                      "(body 上限 %u)\r\n",
                      (unsigned)seq, (unsigned)len, (unsigned)NETMCP_BODY_MAX);
    }
}

/* ---------- 收到报文 ---------- */
void netmcp_udp_rx(uint32_t src_ip, uint16_t src_port, const uint8_t *data,
                   uint32_t len)
{
    if (len == 0u || len > NETMCP_DGRAM_MAX) {
        g_bad++;
        return;
    }
    /* 直接以 NUL 结尾解析（data 指向帧内缓冲，末尾字节未必可用） */
    static char msg[NETMCP_DGRAM_MAX + 1u];
    if (len > NETMCP_DGRAM_MAX) {
        g_bad++;
        return;
    }
    for (uint32_t i = 0; i < len; i++) {
        msg[i] = (char)data[i];
    }
    msg[len] = '\0';
    g_rx_msg++;

    uint32_t epoch = 0, seq = 0, ack = 0;
    char type[16];
    if (jget_u32(msg, "epoch", &epoch) != 0 || jget_u32(msg, "seq", &seq) != 0 ||
        jget_type(msg, type, sizeof(type)) != 0) {
        g_bad++;
        g_peer_ip = src_ip;
        g_peer_port = src_port;
        g_peer_known = 1;
        return;
    }
    if (jget_u32(msg, "ack", &ack) != 0) {
        ack = 0;
    }
    /* 对端地址学习：SLIRP 下对端源地址就是回包地址 */
    if (!g_peer_known || g_peer_ip != src_ip || g_peer_port != src_port) {
        g_peer_ip = src_ip;
        g_peer_port = src_port;
        g_peer_known = 1;
        g_peer_configured = 1;
        potato_printf("[POTATO][NETMCP] peer %u.%u.%u.%u:%u epoch=%u\r\n",
                      (unsigned)(src_ip >> 24), (unsigned)((src_ip >> 16) & 0xFFu),
                      (unsigned)((src_ip >> 8) & 0xFFu), (unsigned)(src_ip & 0xFFu),
                      (unsigned)src_port, (unsigned)g_epoch);
    }

    if (ack > g_ack) {
        g_ack = ack;
        g_ack_advances++;
        slot_drop_upto(ack);
    }

    char t0 = type[0];
    if (t0 == 'a') {              /* ack */
        return;
    }
    if (t0 == 'h') {              /* hello */
        /* 首次 hello：立刻推进游标（对端新会话从 0 起） */
        return;
    }
    if (t0 == 'r' && type[1] == 'e' && type[2] == 's') {   /* resync */
        g_resyncs++;
        if (g_have_cursor && epoch != g_epoch) {
            potato_printf("[POTATO][NETMCP] resync epoch %u->%u cursor=%u\r\n",
                          (unsigned)g_epoch, (unsigned)epoch,
                          (unsigned)g_peer_seq);
            g_epoch = epoch;
            g_have_cursor = 0;
            g_peer_seq = 0;
            for (uint32_t i = 0; i < NETMCP_SEEN; i++) {
                g_seen_seq[i] = 0;
                g_seen_epoch[i] = 0;
                g_seen_slot[i] = -1;
            }
            trace_push(TR_MCP_RESYNC, epoch, g_tx_seq);
        }
        replay_pending("resync");
        return;
    }
    if (t0 != 'r') {              /* 其余（含 req 以外的未知类型） */
        g_bad++;
        return;
    }
    if (type[1] == 'e' && type[2] == 'q') {          /* req */
        netmcp_handle_req(epoch, seq, msg);
    }
}

static void netmcp_handle_req(uint32_t epoch, uint32_t seq, const char *msg)
{
    static char body[NETMCP_BODY_MAX];
    int blen = jget_body(msg, body, sizeof(body));
    if (blen <= 0) {
        g_bad++;
        return;
    }
    if (epoch > g_epoch) {
        /* 对端新纪元：接受并清理旧状态（断线重连后的第一包） */
        potato_printf("[POTATO][NETMCP] epoch %u->%u (peer new generation)\r\n",
                      (unsigned)g_epoch, (unsigned)epoch);
        trace_push(TR_MCP_RESYNC, epoch, g_epoch);
        g_epoch = epoch;
        g_have_cursor = 0;
        g_resyncs++;
    } else if (epoch < g_epoch) {
        /* 跨纪元残包：丢弃，并让对方用新纪元重放 */
        g_bad++;
        potato_printf("[POTATO][NETMCP] stale epoch=%u (cur=%u) seq=%u drop\r\n",
                      (unsigned)epoch, (unsigned)g_epoch, (unsigned)seq);
        netmcp_send("resync", g_tx_seq, "", 0);
        return;
    }

    /* 游标缺口统计（信息性：UDP 无序/丢包可见） */
    if (g_have_cursor && seq > g_peer_seq + 1u) {
        g_gaps += seq - g_peer_seq - 1u;
    }
    g_have_cursor = 1;

    /* 去重：同 (epoch,seq) 只执行一次，重复则重放缓存响应 */
    for (uint32_t i = 0; i < NETMCP_SEEN; i++) {
        if (g_seen_epoch[i] == epoch && g_seen_seq[i] == seq &&
            g_seen_seq[i] != 0u) {
            g_dups++;
            trace_push(TR_MCP_DUP, seq, epoch);
            int8_t s = g_seen_slot[i];
            if (s >= 0 && g_slots[s].valid) {
                if (netmcp_send("resp", g_slots[s].seq, g_slots[s].body,
                                g_slots[s].len) == 0) {
                    g_replays++;
                }
                potato_printf("[POTATO][NETMCP] dup epoch=%u seq=%u "
                              "replay seq=%u bytes=%u\r\n",
                              (unsigned)epoch, (unsigned)seq,
                              (unsigned)g_slots[s].seq,
                              (unsigned)g_slots[s].len);
            } else {
                /* 槽已被对端 ack 回收：无法重放，但必须给出可恢复的答复，
                 * 否则对端会一直悬等。语义：你的游标已过期 → 用 resync
                 * 告知当前纪元与响应序号，对端据此重建会话（不重复执行）。 */
                g_resyncs++;
                potato_printf("[POTATO][NETMCP] dup epoch=%u seq=%u "
                              "(slot acked) -> resync epoch=%u txseq=%u\r\n",
                              (unsigned)epoch, (unsigned)seq,
                              (unsigned)g_epoch, (unsigned)g_tx_seq);
                trace_push(TR_MCP_RESYNC, g_epoch, g_tx_seq);
                netmcp_send("resync", g_tx_seq, "", 0);
            }
            return;
        }
    }
    g_seen_head = (g_seen_head + 1u) % NETMCP_SEEN;
    g_seen_epoch[g_seen_head] = epoch;
    g_seen_seq[g_seen_head] = seq;
    g_seen_slot[g_seen_head] = -1;
    g_peer_seq = seq;
    g_reqs++;
    trace_push(TR_MCP_REQ, seq, epoch);

    /* 执行：把响应捕获进重放槽（抽头→netmcp_line_complete 负责发回） */
    uint32_t before = g_tx_seq;
    g_in_remote_req = 1;
    mcp_handle_line(body);
    g_in_remote_req = 0;
    if (g_tx_seq == before) {
        /* 该请求没有产生 {"jsonrpc"} 响应行：给出错误回执，保证对端不悬等 */
        char err[192];
        uint32_t o = 0;
        const char *pre = "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32601,"
                          "\"message\":\"no response for seq ";
        for (uint32_t i = 0; pre[i] != '\0'; i++) {
            err[o++] = pre[i];
        }
        char tmp[12];
        int n = 0;
        uint32_t v = seq;
        do {
            tmp[n++] = (char)('0' + (v % 10u));
            v /= 10u;
        } while (v != 0u);
        while (n > 0) {
            err[o++] = tmp[--n];
        }
        const char *post = "\"}}";
        for (uint32_t i = 0; post[i] != '\0'; i++) {
            err[o++] = post[i];
        }
        err[o] = '\0';
        uint32_t s = g_tx_seq + 1u;
        int slot = slot_store(err, o, s);
        if (slot >= 0) {
            g_tx_seq = s;
            g_seen_slot[g_seen_head] = (int8_t)slot;
            netmcp_send("resp", s, err, o);
        }
        potato_printf("[POTATO][NETMCP] req epoch=%u seq=%u no-resp\r\n",
                      (unsigned)epoch, (unsigned)seq);
        return;
    }
    g_seen_slot[g_seen_head] = (int8_t)((g_next_slot + NETMCP_SLOTS - 1u) %
                                        NETMCP_SLOTS);
    potato_printf("[POTATO][NETMCP] req epoch=%u seq=%u len=%u lines=%u "
                  "cursor=%u\r\n",
                  (unsigned)epoch, (unsigned)seq, (unsigned)blen,
                  (unsigned)(g_tx_seq - before), (unsigned)g_peer_seq);
}

/* ---------- 周期 ---------- */
void netmcp_udp_tick(void)
{
    uint32_t now = pot_sched_now();
    if (!g_link) {
        return;   /* 链路 down：不发探活（真机/PWR 关闭时省流量） */
    }
    if (!g_peer_known) {
        if ((now - g_last_hello_ms) >= NETMCP_HELLO_MS) {
            g_last_hello_ms = now;
            netmcp_send_hello();
        }
        return;
    }
    if (slots_pending() != 0u && (now - g_last_replay_ms) >= NETMCP_REPLAY_MS) {
        g_last_replay_ms = now;
        replay_pending("retry");
    }
}

void netmcp_on_link(int up)
{
    if (up == g_link) {
        return;
    }
    g_link = up;
    if (up) {
        g_epoch++;
        g_have_cursor = 0;
        potato_printf("[POTATO][NETMCP] link up epoch=%u pending=%u\r\n",
                      (unsigned)g_epoch, (unsigned)slots_pending());
        trace_push(TR_MCP_RESYNC, g_epoch, 1u);
        netmcp_send_hello();
        replay_pending("relink");
    } else {
        potato_printf("[POTATO][NETMCP] link down epoch=%u (will bump)\r\n",
                      (unsigned)g_epoch);
    }
}

void netmcp_init(void)
{
    for (uint32_t i = 0; i < NETMCP_SLOTS; i++) {
        g_slots[i].valid = 0;
    }
    for (uint32_t i = 0; i < NETMCP_SEEN; i++) {
        g_seen_slot[i] = -1;
    }
    g_epoch = 0;    /* 首次 link up 时才进入 epoch=1 会话 */
    g_tx_seq = 0;
    g_ack = 0;
    g_peer_seq = 0;
    g_have_cursor = 0;
    g_peer_known = 0;
    g_peer_configured = 1;   /* 默认对端 10.0.2.2:47654，可用 netmcp 命令改 */
}

int netmcp_udp_port(void)
{
    return (int)g_udp_port;
}

void netmcp_set_udp_port(uint16_t port)
{
    if (port != 0u) {
        g_udp_port = port;
    }
}

void netmcp_set_peer(uint32_t ip, uint16_t port)
{
    if (ip != 0u) {
        g_peer_ip = ip;
    }
    if (port != 0u) {
        g_peer_port = port;
    }
    g_peer_known = 0;
    g_peer_configured = 1;
    g_last_hello_ms = pot_sched_now() - NETMCP_HELLO_MS;
    potato_printf("[POTATO][NETMCP] peer set %u.%u.%u.%u:%u local_port=%u\r\n",
                  (unsigned)(g_peer_ip >> 24), (unsigned)((g_peer_ip >> 16) & 0xFFu),
                  (unsigned)((g_peer_ip >> 8) & 0xFFu), (unsigned)(g_peer_ip & 0xFFu),
                  (unsigned)g_peer_port, (unsigned)g_udp_port);
}

uint32_t netmcp_epoch(void)
{
    return g_epoch;
}

uint32_t netmcp_cursor(void)
{
    return g_peer_seq;
}

int netmcp_peer_known(void)
{
    return g_peer_known;
}

uint32_t netmcp_stat(int which)
{
    switch (which) {
    case 0: return g_rx_msg;
    case 1: return g_tx_msg;
    case 2: return g_reqs;
    case 3: return g_dups;
    case 4: return g_replays;
    case 5: return g_resyncs;
    case 6: return g_hellos;
    case 7: return g_bad;
    case 8: return g_gaps;
    case 9: return g_ack;
    case 10: return g_tx_seq;
    case 11: return slots_pending();
    case 12: return g_trunc;
    default: return 0;
    }
}

void netmcp_dump(void)
{
    char peer[24];
    peer[0] = '\0';
    net_ip_str(g_peer_ip, peer, sizeof(peer));
    potato_printf("[POTATO][NETMCP] transport=udp-json local=%u peer=%s:%u "
                  "known=%d link=%d epoch=%u\r\n",
                  (unsigned)g_udp_port, peer, (unsigned)g_peer_port,
                  g_peer_known, g_link, (unsigned)g_epoch);
    potato_printf("[POTATO][NETMCP] rx=%u tx=%u req=%u dup=%u replay=%u "
                  "resync=%u hello=%u bad=%u gaps=%u ack=%u cursor=%u "
                  "txseq=%u pending=%u trunc=%u\r\n",
                  (unsigned)g_rx_msg, (unsigned)g_tx_msg, (unsigned)g_reqs,
                  (unsigned)g_dups, (unsigned)g_replays, (unsigned)g_resyncs,
                  (unsigned)g_hellos, (unsigned)g_bad, (unsigned)g_gaps,
                  (unsigned)g_ack, (unsigned)g_peer_seq, (unsigned)g_tx_seq,
                  (unsigned)slots_pending(), (unsigned)g_trunc);
    potato_printf("[POTATO][NETMCP] tap lines=%u filtered=%u long=%u "
                  "sendfail=%u maxbody=%u maxdgram=%u\r\n",
                  (unsigned)g_resp_lines, (unsigned)g_tap_filtered,
                  (unsigned)g_tap_long, (unsigned)g_send_fail,
                  (unsigned)NETMCP_BODY_MAX, (unsigned)NETMCP_DGRAM_MAX);
}

/* 自检：信封构造 + 取值往返（不依赖网络） */
int netmcp_selftest(void)
{
    char body[64];
    const char *src = "{\"jsonrpc\":\"2.0\",\"id\":7}";
    uint32_t n = str_len(src);
    int e = jesc(src, n, body, sizeof(body));
    if (e <= 0) {
        return -1;
    }
    char envelope[256];
    uint32_t o = 0;
    const char *pre = "{\"pot\":1,\"epoch\":3,\"seq\":9,\"ack\":2,\"type\":\"resp\",\"body\":\"";
    for (uint32_t i = 0; pre[i] != '\0'; i++) {
        envelope[o++] = pre[i];
    }
    for (int i = 0; i < e; i++) {
        envelope[o++] = body[i];
    }
    envelope[o++] = '"';
    envelope[o++] = '}';
    envelope[o] = '\0';
    uint32_t epoch = 0, seq = 0, ack = 0;
    char type[16];
    char back[128];
    if (jget_u32(envelope, "epoch", &epoch) != 0 || epoch != 3u) {
        return -1;
    }
    if (jget_u32(envelope, "seq", &seq) != 0 || seq != 9u) {
        return -1;
    }
    if (jget_u32(envelope, "ack", &ack) != 0 || ack != 2u) {
        return -1;
    }
    if (jget_type(envelope, type, sizeof(type)) != 0 || type[0] != 'r') {
        return -1;
    }
    int bl = jget_body(envelope, back, sizeof(back));
    if (bl != (int)n) {
        return -1;
    }
    for (uint32_t i = 0; i < n; i++) {
        if (back[i] != src[i]) {
            return -1;
        }
    }
    return 0;
}
