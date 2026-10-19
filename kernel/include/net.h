#ifndef TUDOU_NET_H
#define TUDOU_NET_H

#include <stdint.h>

/*
 * 以太网子系统（M7，支柱1/2）
 * ------------------------------------------------------------------
 * 硬件后端（kernel/net/emac.c）：
 *   QEMU esp32s3 机器在真机 EMAC 基址上挂了 OpenCores MAC + DP83848C
 *   PHY 子集模型（hw/net/opencores_eth.c）：
 *     寄存器窗 0x600CD000（0x54B）、DMA 描述符窗 0x600CD400（1KB=128×8B）
 *     描述符 8B：{len_flags, buf_ptr}；TX_BD_NUM 默认 0x40 → 0..63 TX、
 *     64..127 RX；中断经中断矩阵源 0（ETS_ETH_MAC_INTR_SOURCE =
 *     ETS_WIFI_MAC_INTR_SOURCE）接到 CPU 中断（本内核用 int20，level2）。
 *   真机 S3 硅片没有内部 EMAC（IDF v5.3.1 的 ETH_USE_OPENETH 仅 QEMU，
 *   esp_hal_emac 无 esp32s3 子目录）；以太网只能经 SPI 扩展芯片（W5500 /
 *   DM9051 等，待硬件到货）。真机远程控制面因此走串口 MCP 通道
 *   （netmcp 的 serial transport，见 docs/serial-mcp.md）。
 *
 * 协议层（kernel/net/net.c）：ARP 缓存 / IPv4 / ICMP echo / UDP 单端口
 * 远程通道（kernel/net/netmcp.c，带 epoch+seq 纪元与去重重放）。
 */

#define NET_MAC_LEN    6
#define NET_FRAME_MAX  1518u  /* 14B 头 + 1500B 负载 + 4B FCS 余量 */
#define NET_MTU        1500u

/* ---- 统计 ---- */
typedef struct {
    uint32_t tx_ok;      /* 成功交给 MAC 的帧 */
    uint32_t tx_bytes;
    uint32_t tx_drop;    /* 无描述符/忙 */
    uint32_t tx_err;     /* 描述符未回收 */
    uint32_t rx_ok;
    uint32_t rx_bytes;
    uint32_t rx_drop;    /* 队列满/无缓冲 */
    uint32_t rx_bad;     /* 长度非法 */
    uint32_t irq_rx;     /* RX 中断次数 */
    uint32_t irq_tx;     /* TX 中断次数 */
    uint32_t kick;       /* TX 启动次数 */
} net_stats_t;

/* ================= 硬件后端：emac.c ================= */
int  emac_init(void);                  /* 0=ok 1=设备缺席 -1=失败（驱动框架用） */
int  emac_present(void);
const uint8_t *emac_mac(void);
int  emac_set_mac(const uint8_t *mac); /* 0=ok；同时写 MAC_ADDR0/1 */
int  emac_link_up(void);               /* 读 PHY 基本状态（MII，链路位） */
uint32_t emac_phy_bmsr(void);          /* 原始 BMSR（诊断） */
void emac_int_enable(void);            /* 中断矩阵 + INTENABLE（须在全局中断开后调用） */
void emac_irq_handler(void);
int  emac_send(const void *frame, uint32_t len); /* 0=ok -1=失败 */
int  emac_recv(void *buf, uint32_t cap);         /* >0=帧长 -1=无帧 */
uint32_t emac_rx_pending(void);
void emac_get_stats(net_stats_t *out);
void emac_set_promisc(int on);
int  emac_promisc(void);
void emac_set_fcs_trim(uint32_t n);    /* QEMU 模型在帧尾补 4B FCS：默认裁 4 */
uint32_t emac_fcs_trim(void);

/* ================= 协议层：net.c ================= */
int  net_start(void);       /* 调度器/堆起来后：起 net 任务、绑 UDP、开中断 */
int  net_present(void);
int  net_link_up(void);
const char *net_mac_str(void);
uint32_t net_ip(void);      /* 主机字节序，如 10.0.2.15 = 0x0A00020F */
uint32_t net_gw(void);
void net_set_ip(uint32_t ip, uint32_t gw);
void net_ip_str(uint32_t ip, char *out, uint32_t cap);  /* "a.b.c.d" */
int  net_link_transitions(void);      /* 掉线/恢复次数（测试锚点） */
int  net_ping(uint32_t ip, uint32_t timeout_ms, uint32_t *rtt_ms); /* 任务上下文 */
int  net_send_udp(uint32_t dst_ip, uint16_t dst_port, uint16_t src_port,
                  const void *data, uint32_t len);   /* 0=ok（内含 ARP 解析） */
int  net_arp_lookup(uint32_t ip, uint8_t mac[NET_MAC_LEN]);
int  net_arp_count(void);
void net_arp_dump(void);
void net_stats_dump(void);            /* console `net` 用 */
void net_task_step(void);             /* net 任务单步（测试/诊断） */
const char *net_lock_owner_name(void);/* 诊断："" = 空闲；否则持锁任务名 */
int net_lock_depth(void);
int  net_task_running(void);
void net_sim_link(int up);            /* 合成链路事件（验收用例触发纪元重同步） */

/* ================= 远程 MCP 通道：netmcp.c ================= */
void netmcp_init(void);
void netmcp_udp_rx(uint32_t src_ip, uint16_t src_port, const uint8_t *data, uint32_t len);
void netmcp_udp_tick(void);           /* 周期：hello/重放/超时 */
void netmcp_on_link(int up);
int  netmcp_udp_port(void);
void netmcp_set_udp_port(uint16_t port);
void netmcp_set_peer(uint32_t ip, uint16_t port);
void netmcp_uart_tap(char c);         /* UART 输出抽头：抓 MCP 响应行 */
void netmcp_dump(void);               /* console `net` 用 */
int  netmcp_selftest(void);           /* 内部一致性自检（0=ok） */
uint32_t netmcp_epoch(void);          /* 当前消息纪元（链路 down→up 递增） */
uint32_t netmcp_cursor(void);         /* 已处理请求序号游标 */
int  netmcp_peer_known(void);
uint32_t netmcp_stat(int which);      /* 0rx 1tx 2req 3dup 4replay 5resync
                                       * 6hello 7bad 8gaps 9ack 10txseq
                                       * 11pending 12trunc */

/* ---- 串口 MCP 传输（真机：无 EMAC 时由 net_start 启用） ----
 * 帧：host→device = 0x02 "@@MCP <crc16> <json>\r\n"
 *     device→host = "@@MCP <crc16> <json>\r\n"（单行，≤1600B） */
void netmcp_init_serial(void);        /* transport=serial，epoch=1，注册 UART 抽头 */
int  netmcp_serial_active(void);      /* 当前传输是否为串口 MCP */
void netmcp_serial_rx(const char *line, uint32_t len); /* 控制台交来的一整行帧 */
void netmcp_serial_tick(void);        /* console 空闲时驱动 hello/重放定时器 */

#endif /* TUDOU_NET_H */
