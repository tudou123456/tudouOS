#include <stdint.h>

#include "console.h"
#include "net.h"
#include "potato.h"
#include "task.h"
#include "time.h"
#include "trace.h"
#include "uart.h"

/*
 * 以太网协议层（M7 支柱1）——与 MAC 后端无关，QEMU/真机同源。
 *   ARP（缓存 + 请求/应答）、IPv4（无分片/无选项）、ICMP echo、UDP 单端口
 *   （远程 MCP 通道，kernel/net/netmcp.c）。
 * 默认地址 10.0.2.15/24，网关 10.0.2.2（QEMU SLIRP 用户态网络默认值）。
 * 收包在 net 任务里从 emac_recv 取帧；发送走 emac_send。
 * 控制台 `net`（状态/统计/ARP/地址）与 `ping <ip>`（本端发起，验收锚点）。
 *
 * 栈约束：net 任务栈 640 字（2560B）——本文件所有收发路径一律不放大
 * 局部缓冲：帧在静态 g_tx_frame 里组装，ICMP 回显就地改写整帧。
 * 并发约束：net 任务与控制台任务共享收发路径，用协作式可重入锁串行化。
 */

#define NET_ETH_HDR    14u
#define NET_TYPE_ARP   0x0806u
#define NET_TYPE_IP    0x0800u
#define NET_IP_ICMP    1u
#define NET_IP_UDP     17u

#define NET_ARP_CACHE  4u
#define NET_ARP_TTL_MS 60000u

#define NET_POLL_MS    3u     /* net 任务轮询周期 */
#define NET_LINK_MS    100u   /* PHY 轮询周期（掉线检测） */

#define NET_IP_DEFAULT 0x0A00020Fu /* 10.0.2.15 */
#define NET_GW_DEFAULT 0x0A000202u /* 10.0.2.2  */

#define NET_PING_ID    0x504Fu

/* 帧入口（net_input 在 arp_resolve 里被复用，先声明） */
void net_input(const uint8_t *frame, uint32_t len);

typedef struct {
    uint32_t ip;
    uint8_t mac[NET_MAC_LEN];
    uint32_t stamp_ms;
    int valid;
} arp_entry_t;

static arp_entry_t g_arp[NET_ARP_CACHE];
static uint32_t g_ip = NET_IP_DEFAULT;
static uint32_t g_gw = NET_GW_DEFAULT;

static char g_mac_str[18];
static int g_mac_str_done;

static uint32_t g_rx_frames;      /* L2 收帧（含非 IP） */
static uint32_t g_tx_frames;      /* L2 发帧 */
static uint32_t g_l2_bad;         /* 太短/不支持的类型 */
static uint32_t g_arp_req_tx, g_arp_rep_tx, g_arp_rep_rx, g_arp_miss;
static uint32_t g_icmp_req_rx, g_icmp_rep_rx, g_icmp_rep_tx;
static uint32_t g_udp_rx, g_udp_tx, g_udp_bad;
static uint32_t g_ip_bad;
static int g_link_up;
static int g_link_transitions;
static int g_task_running;

static uint8_t g_tx_frame[NET_FRAME_MAX] __attribute__((aligned(16)));
static uint8_t g_rx_frame[NET_FRAME_MAX] __attribute__((aligned(16)));

static pot_task_t g_net_task_tcb;
static uint32_t g_net_stack[640];

/* ---- 协作式可重入锁（单核；tick ISR 不触碰网络路径） ---- */
static volatile int g_net_lock;
static pot_task_t *g_net_owner;
static int g_net_depth;

static void net_lock(void)
{
    pot_task_t *self = pot_task_self();
    if (g_net_owner != 0 && g_net_owner == self) {
        g_net_depth++;
        return;
    }
    while (g_net_lock != 0) {
        pot_task_sleep(1);
    }
    g_net_lock = 1;
    g_net_owner = self;
    g_net_depth = 1;
}

static void net_unlock(void)
{
    if (g_net_depth > 1) {
        g_net_depth--;
        return;
    }
    g_net_depth = 0;
    g_net_owner = 0;
    g_net_lock = 0;
}

/* 诊断：锁当前归谁（"" = 空闲）。M7 实测踩过「发送阶段忘放锁 →
 * net 任务永久饥饿 → hello 停发且控制台 ping 卡死」的坑，故暴露给
 * net 命令/测试用例断言。 */
const char *net_lock_owner_name(void)
{
    if (g_net_lock == 0 || g_net_owner == 0) {
        return "";
    }
    return g_net_owner->name ? g_net_owner->name : "?";
}

int net_lock_depth(void)
{
    return g_net_depth;
}

/* ---------- 字节序/校验和（全部零栈开销） ---------- */
static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFFu);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* wr16/wr32/rd16/rd32 一律按网络字节序（大端）读写：写 IPv4 字段时直接传
 * 主机字节序的地址值（如 net_ip() 的返回值），不要再手工翻转，否则线上
 * 会得到字节反序的地址。 */

static uint32_t csum_fold(uint32_t sum)
{
    while ((sum >> 16) != 0u) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    return sum & 0xFFFFu;
}

static uint32_t csum_run(uint32_t sum, const uint8_t *p, uint32_t len)
{
    uint32_t i = 0;
    for (; i + 1u < len; i += 2u) {
        sum += (uint32_t)rd16(p + i);
    }
    if (i < len) {
        sum += (uint32_t)p[i] << 8;
    }
    return sum;
}

static uint16_t csum16(const uint8_t *p, uint32_t len)
{
    return (uint16_t)(~csum_fold(csum_run(0, p, len)) & 0xFFFFu);
}

static uint32_t csum_add_bytes(uint32_t sum, const uint8_t *p, uint32_t len)
{
    return csum_run(sum, p, len);
}

/* ---------- ARP ---------- */
static arp_entry_t *arp_slot(uint32_t ip, int create)
{
    uint32_t now = pot_sched_now();
    arp_entry_t *free_slot = 0;
    arp_entry_t *oldest = &g_arp[0];
    for (uint32_t i = 0; i < NET_ARP_CACHE; i++) {
        if (g_arp[i].valid && (now - g_arp[i].stamp_ms) > NET_ARP_TTL_MS) {
            g_arp[i].valid = 0;
        }
        if (g_arp[i].stamp_ms < oldest->stamp_ms) {
            oldest = &g_arp[i];
        }
        if (g_arp[i].valid && g_arp[i].ip == ip) {
            return &g_arp[i];
        }
        if (!g_arp[i].valid && free_slot == 0) {
            free_slot = &g_arp[i];
        }
    }
    if (!create) {
        return 0;
    }
    if (free_slot == 0) {
        free_slot = oldest;
    }
    free_slot->ip = ip;
    free_slot->valid = 0;
    return free_slot;
}

int net_arp_count(void)
{
    int n = 0;
    for (uint32_t i = 0; i < NET_ARP_CACHE; i++) {
        if (g_arp[i].valid) {
            n++;
        }
    }
    return n;
}

int net_arp_lookup(uint32_t ip, uint8_t mac[NET_MAC_LEN])
{
    arp_entry_t *e = arp_slot(ip, 0);
    if (e == 0 || !e->valid) {
        return -1;
    }
    for (int i = 0; i < NET_MAC_LEN; i++) {
        mac[i] = e->mac[i];
    }
    return 0;
}

void net_arp_dump(void)
{
    char ipbuf[16];
    for (uint32_t i = 0; i < NET_ARP_CACHE; i++) {
        if (!g_arp[i].valid) {
            continue;
        }
        net_ip_str(g_arp[i].ip, ipbuf, sizeof(ipbuf));
        potato_printf("[POTATO][NET] arp %s %02x:%02x:%02x:%02x:%02x:%02x age=%ums\r\n",
                      ipbuf, g_arp[i].mac[0], g_arp[i].mac[1], g_arp[i].mac[2],
                      g_arp[i].mac[3], g_arp[i].mac[4], g_arp[i].mac[5],
                      (unsigned)(pot_sched_now() - g_arp[i].stamp_ms));
    }
}

static void arp_send_request(uint32_t ip)
{
    const uint8_t *mac = emac_mac();
    uint8_t *f = g_tx_frame;
    for (int i = 0; i < NET_MAC_LEN; i++) {
        f[i] = 0xFFu;
        f[NET_MAC_LEN + i] = mac[i];
    }
    wr16(f + 12, NET_TYPE_ARP);
    uint8_t *a = f + NET_ETH_HDR;
    wr16(a + 0, 1);
    wr16(a + 2, NET_TYPE_IP);
    a[4] = 6;
    a[5] = 4;
    wr16(a + 6, 1);
    for (int i = 0; i < NET_MAC_LEN; i++) {
        a[8 + i] = mac[i];
        a[18 + i] = 0;
    }
    wr32(a + 14, g_ip);
    wr32(a + 24, ip);
    if (emac_send(f, NET_ETH_HDR + 28u) == 0) {
        g_arp_req_tx++;
        g_tx_frames++;
    }
}

static void arp_reply_to(const uint8_t *req)
{
    const uint8_t *mac = emac_mac();
    uint8_t *f = g_tx_frame;
    for (int i = 0; i < NET_MAC_LEN; i++) {
        f[i] = req[8 + i];
        f[NET_MAC_LEN + i] = mac[i];
    }
    wr16(f + 12, NET_TYPE_ARP);
    uint8_t *a = f + NET_ETH_HDR;
    wr16(a + 0, 1);
    wr16(a + 2, NET_TYPE_IP);
    a[4] = 6;
    a[5] = 4;
    wr16(a + 6, 2);
    for (int i = 0; i < NET_MAC_LEN; i++) {
        a[8 + i] = mac[i];
        a[18 + i] = req[8 + i];
    }
    wr32(a + 14, g_ip);
    wr32(a + 24, rd32(req + 14));
    if (emac_send(f, NET_ETH_HDR + 28u) == 0) {
        g_arp_rep_tx++;
        g_tx_frames++;
        trace_push(TR_NET_TX, (NET_TYPE_ARP << 8) | 2u, NET_ETH_HDR + 28u);
    }
}

static void arp_input(const uint8_t *frame, uint32_t len)
{
    if (len < NET_ETH_HDR + 28u) {
        g_l2_bad++;
        return;
    }
    const uint8_t *a = frame + NET_ETH_HDR;
    uint16_t op = rd16(a + 6);
    uint32_t spa = rd32(a + 14);
    uint32_t tpa = rd32(a + 24);
    arp_entry_t *e = arp_slot(spa, 1);
    if (e != 0) {
        for (int i = 0; i < NET_MAC_LEN; i++) {
            e->mac[i] = a[8 + i];
        }
        e->valid = 1;
        e->stamp_ms = pot_sched_now();
    }
    if (op == 1u) {
        if (tpa == g_ip) {
            arp_reply_to(frame);
        }
    } else if (op == 2u) {
        g_arp_rep_rx++;
        trace_push(TR_NET_RX, (NET_TYPE_ARP << 8) | 2u, len);
    }
}

/* 解析（必要时先请求）目标 MAC：0=ok（调用者须持锁） */
static int arp_resolve(uint32_t ip, uint8_t mac[NET_MAC_LEN], uint32_t timeout_ms)
{
    if (net_arp_lookup(ip, mac) == 0) {
        return 0;
    }
    uint32_t start = pot_sched_now();
    uint32_t last_req = start - 1000u;
    while ((pot_sched_now() - start) < timeout_ms) {
        uint32_t now = pot_sched_now();
        if ((now - last_req) >= 200u) {
            arp_send_request(ip);
            last_req = now;
        }
        int n = emac_recv(g_rx_frame, sizeof(g_rx_frame));
        if (n > 0) {
            net_input(g_rx_frame, (uint32_t)n);
        }
        if (net_arp_lookup(ip, mac) == 0) {
            return 0;
        }
        pot_task_sleep(1);
    }
    g_arp_miss++;
    return -1;
}

/* ---------- 发送：帧在 g_tx_frame 内组装 ---------- */
/* 写 eth + ip 头，返回 IP 头起始指针；payload 由调用者紧随其后写入 */
static uint8_t *frame_ip_begin(const uint8_t *dst_mac, uint32_t dst_ip,
                               uint8_t proto, uint32_t payload_len)
{
    const uint8_t *mac = emac_mac();
    uint8_t *f = g_tx_frame;
    for (int i = 0; i < NET_MAC_LEN; i++) {
        f[i] = dst_mac[i];
        f[NET_MAC_LEN + i] = mac[i];
    }
    wr16(f + 12, NET_TYPE_IP);
    uint8_t *ip = f + NET_ETH_HDR;
    uint32_t total = 20u + payload_len;
    ip[0] = 0x45u;
    ip[1] = 0;
    wr16(ip + 2, (uint16_t)total);
    wr16(ip + 4, (uint16_t)(g_tx_frames & 0xFFFFu));
    wr16(ip + 6, 0);
    ip[8] = 64;
    ip[9] = proto;
    wr16(ip + 10, 0);
    wr32(ip + 12, g_ip);
    wr32(ip + 16, dst_ip);
    return ip;
}

static int frame_ip_finish(uint8_t *ip, uint32_t payload_len, uint32_t proto)
{
    wr16(ip + 10, csum16(ip, 20));
    uint32_t total = NET_ETH_HDR + 20u + payload_len;
    if (emac_send(g_tx_frame, total) != 0) {
        return -1;
    }
    g_tx_frames++;
    trace_push(TR_NET_TX, ((uint32_t)proto << 8) | (total & 0xFFu), total);
    return 0;
}

int net_send_udp(uint32_t dst_ip, uint16_t dst_port, uint16_t src_port,
                 const void *data, uint32_t len)
{
    if (len > (NET_FRAME_MAX - NET_ETH_HDR - 28u)) {
        return -1;
    }
    uint8_t mac[NET_MAC_LEN];
    net_lock();
    int rc = arp_resolve(dst_ip, mac, 600u);
    if (rc == 0) {
        uint8_t *ip = frame_ip_begin(mac, dst_ip, NET_IP_UDP, 8u + len);
        uint8_t *udp = ip + 20;
        wr16(udp + 0, src_port);
        wr16(udp + 2, dst_port);
        wr16(udp + 4, (uint16_t)(8u + len));
        wr16(udp + 6, 0);   /* IPv4 允许 0 = 不校验 */
        const uint8_t *src = (const uint8_t *)data;
        for (uint32_t i = 0; i < len; i++) {
            udp[8 + i] = src[i];
        }
        /* 伪首部校验和（QEMU/真机对端都会校验，必须正确） */
        uint32_t sum = 0;
        sum = csum_add_bytes(sum, ip + 12, 8);          /* src+dst ip */
        sum += (uint32_t)NET_IP_UDP;
        sum += (uint32_t)(8u + len);
        sum = csum_add_bytes(sum, udp, 8u + len);
        uint16_t ck = (uint16_t)(~csum_fold(sum) & 0xFFFFu);
        if (ck == 0u) {
            ck = 0xFFFFu;
        }
        wr16(udp + 6, ck);
        rc = frame_ip_finish(ip, 8u + len, NET_IP_UDP);
        if (rc == 0) {
            g_udp_tx++;
        }
    }
    net_unlock();
    return rc;
}

/* ---------- ICMP ---------- */
static uint32_t g_ping_t0_us;
static uint32_t g_ping_rtt_ms;
static uint16_t g_ping_seq_wait;
static int g_ping_got;

/* 回显应答：就地改写整帧（零局部缓冲），源/目的互换 */
static void icmp_echo_reply(const uint8_t *frame, uint32_t len, uint32_t ihl)
{
    if (len > sizeof(g_tx_frame) || len < NET_ETH_HDR + ihl + 8u) {
        return;
    }
    for (uint32_t i = 0; i < len; i++) {
        g_tx_frame[i] = frame[i];
    }
    uint8_t *f = g_tx_frame;
    for (int i = 0; i < NET_MAC_LEN; i++) {
        uint8_t t = f[i];
        f[i] = f[NET_MAC_LEN + i];
        f[NET_MAC_LEN + i] = t;
    }
    uint8_t *ip = f + NET_ETH_HDR;
    uint8_t tmp[4];
    for (int i = 0; i < 4; i++) {
        tmp[i] = ip[12 + i];
        ip[12 + i] = ip[16 + i];
        ip[16 + i] = tmp[i];
    }
    ip[8] = 64;
    wr16(ip + 10, 0);
    wr16(ip + 10, csum16(ip, ihl));
    uint8_t *icmp = ip + ihl;
    icmp[0] = 0;   /* echo reply */
    icmp[1] = 0;
    wr16(icmp + 2, 0);
    wr16(icmp + 2, csum16(icmp, len - NET_ETH_HDR - ihl));
    if (emac_send(f, len) == 0) {
        g_icmp_rep_tx++;
        g_tx_frames++;
        trace_push(TR_NET_TX, ((uint32_t)NET_IP_ICMP << 8) | 0u, len);
    }
}

static void icmp_input(const uint8_t *frame, uint32_t len, uint32_t ihl,
                       uint32_t src_ip, const uint8_t *p, uint32_t plen)
{
    (void)src_ip;
    if (plen < 8u) {
        g_ip_bad++;
        return;
    }
    if (p[0] == 8u) {
        g_icmp_req_rx++;
        icmp_echo_reply(frame, len, ihl);
    } else if (p[0] == 0u) {
        g_icmp_rep_rx++;
        if (rd16(p + 4) == NET_PING_ID &&
            rd16(p + 6) == (uint16_t)g_ping_seq_wait) {
            g_ping_rtt_ms = (uint32_t)(time_now_us() / 1000u) -
                            (uint32_t)(g_ping_t0_us / 1000u);
            g_ping_got = 1;
        }
    }
}

int net_ping(uint32_t ip, uint32_t timeout_ms, uint32_t *rtt_ms)
{
    uint8_t mac[NET_MAC_LEN];
    char ipbuf[16];
    net_ip_str(ip, ipbuf, sizeof(ipbuf));
    net_lock();
    if (arp_resolve(ip, mac, 800u) != 0) {
        net_unlock();
        potato_printf("[POTATO][NET] ping %s arp-timeout\r\n", ipbuf);
        return -1;
    }
    static uint32_t seq;
    seq++;
    uint8_t icmp[64];
    icmp[0] = 8;
    icmp[1] = 0;
    wr16(icmp + 2, 0);
    wr16(icmp + 4, NET_PING_ID);
    wr16(icmp + 6, (uint16_t)seq);
    for (uint32_t i = 8; i < sizeof(icmp); i++) {
        icmp[i] = (uint8_t)('a' + (i % 26u));
    }
    wr16(icmp + 2, csum16(icmp, sizeof(icmp)));

    g_ping_seq_wait = (uint16_t)seq;
    g_ping_got = 0;
    g_ping_rtt_ms = 0;
    g_ping_t0_us = time_now_us();
    uint8_t *ip_hdr = frame_ip_begin(mac, ip, NET_IP_ICMP, sizeof(icmp));
    for (uint32_t i = 0; i < sizeof(icmp); i++) {
        ip_hdr[20 + i] = icmp[i];
    }
    if (frame_ip_finish(ip_hdr, sizeof(icmp), NET_IP_ICMP) != 0) {
        net_unlock();
        return -1;
    }
    net_unlock();   /* 发送阶段结束即放锁；等待阶段每轮短持锁（下方） */

    int rc = -1;
    uint32_t t0 = pot_sched_now();
    while ((pot_sched_now() - t0) < timeout_ms) {
        /* 每轮短持锁取包，等待期间释放锁——否则后台 net 任务被饿死 */
        net_lock();
        int n = emac_recv(g_rx_frame, sizeof(g_rx_frame));
        if (n > 0) {
            net_input(g_rx_frame, (uint32_t)n);
        }
        int got = g_ping_got;
        net_unlock();
        if (got) {
            rc = 0;
            break;
        }
        pot_task_sleep(1);
    }
    if (rc == 0) {
        potato_printf("[POTATO][NET] ping %s id=%u seq=%u rtt=%ums\r\n", ipbuf,
                      (unsigned)NET_PING_ID, (unsigned)seq,
                      (unsigned)g_ping_rtt_ms);
    } else {
        potato_printf("[POTATO][NET] ping %s timeout\r\n", ipbuf);
    }
    if (rtt_ms != 0) {
        *rtt_ms = g_ping_rtt_ms;
    }
    return rc;
}

/* ---------- IPv4 ---------- */
static int udp_csum_ok(const uint8_t *ip, const uint8_t *udp, uint32_t ulen)
{
    uint32_t sum = 0;
    sum = csum_add_bytes(sum, ip + 12, 8);
    sum += (uint32_t)NET_IP_UDP;
    sum += ulen;
    sum = csum_add_bytes(sum, udp, ulen);
    return csum_fold(sum) == 0xFFFFu;
}

void net_input(const uint8_t *frame, uint32_t len)
{
    if (len < NET_ETH_HDR) {
        g_l2_bad++;
        return;
    }
    uint16_t type = rd16(frame + 12);
    if (type == NET_TYPE_ARP) {
        g_rx_frames++;
        arp_input(frame, len);
        return;
    }
    if (type != NET_TYPE_IP) {
        g_rx_frames++;   /* 其它协议（IPv6 等）不实现，仅计数 */
        return;
    }
    const uint8_t *ip = frame + NET_ETH_HDR;
    uint32_t iplen = len - NET_ETH_HDR;
    if (iplen < 20u || (ip[0] >> 4) != 4u) {
        g_ip_bad++;
        return;
    }
    uint32_t hlen = (uint32_t)(ip[0] & 0x0Fu) * 4u;
    if (hlen < 20u || iplen < hlen) {
        g_ip_bad++;
        return;
    }
    uint32_t total = rd16(ip + 2);
    if (total > iplen || total < hlen) {
        g_ip_bad++;
        return;
    }
    if (csum16(ip, hlen) != 0u) {
        g_ip_bad++;
        return;
    }
    uint32_t dst = rd32(ip + 16);
    if (dst != g_ip && dst != 0xFFFFFFFFu) {
        return;   /* 非本机（不做转发） */
    }
    uint32_t src = rd32(ip + 12);
    const uint8_t *payload = ip + hlen;
    uint32_t plen = total - hlen;

    g_rx_frames++;
    trace_push(TR_NET_RX, ((uint32_t)ip[9] << 8) | (plen & 0xFFu), len);

    if (ip[9] == NET_IP_ICMP) {
        icmp_input(frame, len, hlen, src, payload, plen);
    } else if (ip[9] == NET_IP_UDP) {
        if (plen < 8u) {
            g_udp_bad++;
            return;
        }
        uint16_t sport = rd16(payload + 0);
        uint16_t dport = rd16(payload + 2);
        uint32_t ulen = rd16(payload + 4);
        if (ulen < 8u || ulen > plen) {
            g_udp_bad++;
            return;
        }
        uint16_t ck = rd16(payload + 6);
        if (ck != 0u && !udp_csum_ok(ip, payload, ulen)) {
            g_udp_bad++;
            return;
        }
        g_udp_rx++;
        if (dport == netmcp_udp_port()) {
            netmcp_udp_rx(src, sport, payload + 8, ulen - 8u);
        }
    }
}

/* ---------- 周期：链路与任务 ---------- */
static void net_link_event(int up)
{
    g_link_up = up;
    g_link_transitions++;
    trace_push(TR_NET_LINK, up ? 1u : 2u, (uint32_t)g_link_transitions);
    potato_printf("[POTATO][NET] link %s (transitions=%d)\r\n", up ? "up" : "down",
                  g_link_transitions);
    netmcp_on_link(up);
}

void net_sim_link(int up)
{
    if ((up != 0) == (g_link_up != 0)) {
        return;   /* 状态未变：不合成事件 */
    }
    net_link_event(up);
}

static void net_link_poll(void)
{
    static uint32_t last_ms;
    uint32_t now = pot_sched_now();
    if ((now - last_ms) < NET_LINK_MS) {
        return;
    }
    last_ms = now;
    int up = emac_link_up();
    if (up == g_link_up) {
        return;
    }
    net_link_event(up);
}

/* 持锁执行一轮收发（调用者须持锁；**绝不在锁内休眠**） */
static int net_step_locked(void)
{
    net_link_poll();
    int drained = 0;
    for (int i = 0; i < 4; i++) {
        int n = emac_recv(g_rx_frame, sizeof(g_rx_frame));
        if (n <= 0) {
            break;
        }
        net_input(g_rx_frame, (uint32_t)n);
        drained++;
    }
    netmcp_udp_tick();
    return drained;
}

/* M7：加锁→干活→解锁→（无帧时）在锁外休眠。
 * 教训：休眠必须发生在 net_unlock() 之后——否则持有锁的后台任务会
 * 在休眠期间把锁一直攥住，而 net_lock() 的等待者（控制台 ping 等）
 * 每次醒来都看到锁被占，形成永久饥饿（实测：g_net_owner=net、
 * g_net_depth=1、控制台任务卡在 net_ping 的 net_lock()，idle 在跑）。 */
void net_task_step(void)
{
    net_lock();
    int drained = net_step_locked();
    net_unlock();
    if (drained == 0) {
        pot_task_sleep(NET_POLL_MS);
    }
}

static void net_task(void *arg)
{
    (void)arg;
    g_task_running = 1;
    for (;;) {
        net_task_step();
    }
}

int net_task_running(void)
{
    return g_task_running;
}

int net_start(void)
{
    if (!emac_present()) {
        /* 保留原锚点行（测试/文档按此匹配）。真机 S3 无内部 EMAC，远程
         * 控制面改走串口 MCP；QEMU 无 -nic 场景不启用（UDP 通道是回归对象）。 */
        potato_printf("[POTATO][NET] no EMAC device: ethernet disabled\r\n");
#if TUDOU_PLATFORM != 0
        netmcp_init_serial();
        potato_printf("[POTATO][NET] remote MCP over serial enabled "
                      "(transport=serial-json)\r\n");
#endif
        return 0;
    }
    emac_int_enable();
    netmcp_init();
    uart_set_sink(netmcp_uart_tap);
    g_link_up = emac_link_up();
    netmcp_on_link(g_link_up);
    char ipbuf[16];
    char gwbuf[16];
    net_ip_str(g_ip, ipbuf, sizeof(ipbuf));
    net_ip_str(g_gw, gwbuf, sizeof(gwbuf));
    potato_printf("[POTATO][NET] ready mac=%s ip=%s gw=%s udp=%u link=%s\r\n",
                  net_mac_str(), ipbuf, gwbuf, (unsigned)netmcp_udp_port(),
                  g_link_up ? "up" : "down");
    if (pot_task_create(&g_net_task_tcb, "net", POTCLS_BACKGROUND, net_task, 0,
                        g_net_stack, sizeof(g_net_stack) / 4u) != 0) {
        return -1;
    }
    return 0;
}

int net_present(void)
{
    return emac_present();
}

int net_link_up(void)
{
    return g_link_up;
}

int net_link_transitions(void)
{
    return g_link_transitions;
}

const char *net_mac_str(void)
{
    const uint8_t *m = emac_mac();
    static const char hex[] = "0123456789abcdef";
    if (!g_mac_str_done) {
        int o = 0;
        for (int i = 0; i < NET_MAC_LEN; i++) {
            if (i != 0) {
                g_mac_str[o++] = ':';
            }
            g_mac_str[o++] = hex[(m[i] >> 4) & 0xFu];
            g_mac_str[o++] = hex[m[i] & 0xFu];
        }
        g_mac_str[o] = '\0';
        g_mac_str_done = 1;
    }
    return g_mac_str;
}

uint32_t net_ip(void)
{
    return g_ip;
}

uint32_t net_gw(void)
{
    return g_gw;
}

void net_set_ip(uint32_t ip, uint32_t gw)
{
    g_ip = ip;
    if (gw != 0u) {
        g_gw = gw;
    }
}

void net_ip_str(uint32_t ip, char *out, uint32_t cap)
{
    uint32_t o = 0;
    uint32_t parts[4];
    parts[0] = (ip >> 24) & 0xFFu;
    parts[1] = (ip >> 16) & 0xFFu;
    parts[2] = (ip >> 8) & 0xFFu;
    parts[3] = ip & 0xFFu;
    for (int i = 0; i < 4; i++) {
        if (i != 0) {
            if (o + 1u >= cap) {
                break;
            }
            out[o++] = '.';
        }
        uint32_t v = parts[i];
        char tmp[4];
        int n = 0;
        do {
            tmp[n++] = (char)('0' + (v % 10u));
            v /= 10u;
        } while (v != 0u && n < 3);
        while (n > 0) {
            if (o + 1u >= cap) {
                break;
            }
            out[o++] = tmp[--n];
        }
    }
    out[o] = '\0';
}

void net_stats_dump(void)
{
    char ipbuf[16];
    char gwbuf[16];
    net_stats_t mac_stats;
    emac_get_stats(&mac_stats);
    net_ip_str(g_ip, ipbuf, sizeof(ipbuf));
    net_ip_str(g_gw, gwbuf, sizeof(gwbuf));
    potato_printf("[POTATO][NET] present=%d link=%d trans=%d mac=%s promisc=%d "
                  "fcs_trim=%u bmsr=0x%04x\r\n",
                  emac_present(), g_link_up, g_link_transitions, net_mac_str(),
                  emac_promisc(), (unsigned)emac_fcs_trim(),
                  (unsigned)emac_phy_bmsr());
    potato_printf("[POTATO][NET] ip=%s gw=%s arp=%d lock=%s/%d\r\n", ipbuf, gwbuf,
                  net_arp_count(),
                  net_lock_owner_name()[0] ? net_lock_owner_name() : "free",
                  net_lock_depth());
    potato_printf("[POTATO][NET] mac-tx=%u/%uB drop=%u err=%u kick=%u "
                  "mac-rx=%u/%uB drop=%u bad=%u irq=%u/%u\r\n",
                  mac_stats.tx_ok, mac_stats.tx_bytes, mac_stats.tx_drop,
                  mac_stats.tx_err, mac_stats.kick, mac_stats.rx_ok,
                  mac_stats.rx_bytes, mac_stats.rx_drop, mac_stats.rx_bad,
                  mac_stats.irq_rx, mac_stats.irq_tx);
    potato_printf("[POTATO][NET] l3-tx=%u rx=%u l2bad=%u "
                  "arp(tx=%u rep_tx=%u rep_rx=%u miss=%u) "
                  "icmp(req=%u rep_rx=%u rep_tx=%u) udp(rx=%u tx=%u bad=%u) "
                  "ipbad=%u\r\n",
                  g_tx_frames, g_rx_frames, g_l2_bad, g_arp_req_tx, g_arp_rep_tx,
                  g_arp_rep_rx, g_arp_miss, g_icmp_req_rx, g_icmp_rep_rx,
                  g_icmp_rep_tx, g_udp_rx, g_udp_tx, g_udp_bad, g_ip_bad);
    net_arp_dump();
    netmcp_dump();
}
