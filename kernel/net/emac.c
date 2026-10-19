#include <stdint.h>

#include "console.h"
#include "driver.h"
#include "intc.h"
#include "io.h"
#include "net.h"
#include "trace.h"

/*
 * EMAC 硬件后端（M7）——OpenCores MAC + DP83848C PHY 子集
 * ------------------------------------------------------------------
 * 目标：QEMU esp32s3 机器在「真机 EMAC 基址」0x600CD000 上挂的
 * open_eth 设备（build/_qemu_src/hw/net/opencores_eth.c）—— 该外设是
 * QEMU 机器模型自建的测试设备，不是 ESP32-S3 硅片功能。S3 没有内部
 * EMAC（IDF v5.3.1 的 ETH_USE_OPENETH 仅 QEMU；esp_hal_emac 无
 * esp32s3 子目录；Espressif issue #17606 官方确认不支持 (R)MII），
 * 真机以太网只能经 SPI 扩展芯片（W5500/DM9051 等，待硬件到货）。
 * 真机远程控制面走串口 MCP 通道（netmcp serial transport）。
 *
 * 寄存器布局（偏移自基址，模型 enum 顺序，0x54B 窗）：
 *   MODER 0x00  INT_SOURCE 0x04  INT_MASK 0x08  PACKETLEN 0x18
 *   TX_BD_NUM 0x20  MIICOMMAND 0x2C  MIIADDRESS 0x30  MIIRX_DATA 0x38
 *   MIISTATUS 0x3C  MAC_ADDR0 0x40  MAC_ADDR1 0x44  TXCTRL 0x50
 * 描述符窗 0x600CD400（1KB = 128 × 8B）：
 *   {len_flags, buf_ptr}；TX_BD_NUM=64 → 0..63 TX、64..127 RX。
 *   写描述符本身即触发 TX（open_eth_desc_write → check_start_xmit），
 *   故必须「先写长度(清 RD) → 写 buf_ptr → 最后写带 RD 的长度」。
 *
 * 收包路径：设备按 rx_desc 从 TX_BD_NUM 起走，描述符 RXD_E=1 表示软件
 * 已备好缓冲；设备填包后清 RXD_E 并在 RXD_IRQ 置位时抬 INT_SOURCE.RXB。
 * 因此软件每收一帧都要重新置 RXD_E。RXD_WRAP 置在环尾实现回卷。
 * 设备在帧尾补 4B FCS（QEMU 模型行为），emac_fcs_trim 默认裁掉。
 *
 * 中断：中断矩阵源 0（ETS_ETH_MAC_INTR_SOURCE = ETS_WIFI_MAC_INTR_SOURCE，
 * 见 build/_qemu_src/include/hw/xtensa/esp32s3_intc.h:150）→ CPU int20
 * （level2，core-esp32s3/core-isa.h:495 XCHAL_EXTINT15_NUM 20 intlevel 2）。
 * ISR 只清中断源并计数，不碰调度器（level2 会抢占 level1 的 tick 路径，
 * 在 ISR 里动就绪链会与 pot_sched_tick 竞争）；收包由 net 任务轮询描述符。
 */

#define EMAC_BASE       0x600CD000u
#define EMAC_DESC_BASE  0x600CD400u

#define R_MODER      (EMAC_BASE + 0x00u)
#define R_INT_SOURCE (EMAC_BASE + 0x04u)
#define R_INT_MASK   (EMAC_BASE + 0x08u)
#define R_PACKETLEN  (EMAC_BASE + 0x18u)
#define R_TX_BD_NUM  (EMAC_BASE + 0x20u)
#define R_MIICOMMAND (EMAC_BASE + 0x2Cu)
#define R_MIIADDRESS (EMAC_BASE + 0x30u)
#define R_MIIRX_DATA (EMAC_BASE + 0x38u)
#define R_MIISTATUS  (EMAC_BASE + 0x3Cu)
#define R_MAC_ADDR0  (EMAC_BASE + 0x40u)
#define R_MAC_ADDR1  (EMAC_BASE + 0x44u)

/* MODER */
#define MODER_PAD    0x8000u
#define MODER_HUGEN  0x4000u
#define MODER_RST    0x0800u
#define MODER_LOOP   0x0080u
#define MODER_PRO    0x0020u
#define MODER_IAM    0x0010u
#define MODER_BRO    0x0008u
#define MODER_TXEN   0x0002u
#define MODER_RXEN   0x0001u

/* INT_SOURCE / INT_MASK */
#define INT_BUSY 0x10u
#define INT_RXB  0x04u
#define INT_TXB  0x01u

/* PACKETLEN：MINFL=[31:16] MAXFL=[15:0] */
#define PKTLEN_MINFL 64u
#define PKTLEN_MAXFL NET_FRAME_MAX

/* 描述符标志 */
#define TXD_RD   0x8000u
#define TXD_IRQ  0x4000u
#define TXD_WR   0x2000u
#define TXD_PAD  0x1000u
#define TXD_CRC  0x0800u
#define RXD_E    0x8000u
#define RXD_IRQ  0x4000u
#define RXD_WRAP 0x2000u

#define MII_BMSR       1u
#define MII_BMSR_LINK  0x0004u
#define MII_RSTAT      0x0002u
#define MIISTATUS_LINKFAIL 0x0001u

/* TX_BD_NUM 默认 64：0..63 TX，64..127 RX */
#define EMAC_TX_BD_DEFAULT 0x40u
#define EMAC_TX_DESC_COUNT 2u   /* 只用 2 个 TX 描述符（描述符窗固定 128 个） */
#define EMAC_RX_DESC_COUNT 4u
#define EMAC_RX_BUF_SIZE   1600u

/* 中断矩阵：源 0 → CPU int20（level2） */
#define EMAC_INT_SOURCE_ID 0u
#define EMAC_CPU_INT       20u

static int g_present;
static uint8_t g_mac[NET_MAC_LEN] = { 0x02, 0x50, 0x54, 0x4F, 0x53, 0x00 }; /* 本地产 MAC */
static uint32_t g_phy_bmsr;      /* 最近一次 PHY 状态（诊断） */
static int g_promisc = 1;        /* 默认混杂：测试网卡，避免组播哈希表配置 */
static uint32_t g_fcs_trim = 4;  /* QEMU 模型在帧尾补 4B 零 FCS */
static uint32_t g_tx_head;       /* 下一个 TX 描述符下标（模 TX_DESC_COUNT） */
static uint32_t g_rx_tail;       /* 下一个待取 RX 描述符下标 */

static net_stats_t g_stats;

static uint8_t g_tx_buf[EMAC_TX_DESC_COUNT][NET_FRAME_MAX] __attribute__((aligned(16)));
static uint8_t g_rx_buf[EMAC_RX_DESC_COUNT][EMAC_RX_BUF_SIZE] __attribute__((aligned(16)));

/* ---- 描述符访问（QEMU 把描述符窗做成独立 MMIO 区） ----
 * 真机后端用普通内存描述符时，把 desc_* 换成指针即可。 */
static uint32_t desc_addr(uint32_t idx)
{
    return EMAC_DESC_BASE + idx * 8u;
}

static void desc_write(uint32_t idx, uint32_t len_flags, uint32_t buf_ptr)
{
    mmio_write(desc_addr(idx), len_flags);
    mmio_write(desc_addr(idx) + 4u, buf_ptr);
}

static uint32_t desc_flags(uint32_t idx)
{
    return mmio_read(desc_addr(idx));
}

static uint32_t desc_bufptr(uint32_t idx)
{
    return mmio_read(desc_addr(idx) + 4u);
}

/* ---- PHY（MII 管理接口） ---- */
static uint32_t mii_read(unsigned rgad)
{
    mmio_write(R_MIIADDRESS, (rgad << 8) & 0x1F00u);
    mmio_write(R_MIICOMMAND, MII_RSTAT);
    return mmio_read(R_MIIRX_DATA) & 0xFFFFu;
}

uint32_t emac_phy_bmsr(void)
{
    return g_phy_bmsr;
}

int emac_link_up(void)
{
    if (g_present == 0) {
        return 0;
    }
    g_phy_bmsr = mii_read(MII_BMSR);
    uint32_t st = mmio_read(R_MIISTATUS);
    if ((st & MIISTATUS_LINKFAIL) != 0u) {
        return 0;
    }
    return (g_phy_bmsr & MII_BMSR_LINK) != 0u ? 1 : 0;
}

/* ---- MAC 地址 ---- */
const uint8_t *emac_mac(void)
{
    return g_mac;
}

int emac_set_mac(const uint8_t *mac)
{
    if (mac == 0 || g_present == 0) {
        return -1;
    }
    for (int i = 0; i < NET_MAC_LEN; i++) {
        g_mac[i] = mac[i];
    }
    /* MAC_ADDR1 = 字节 0..1（byte0 在高字节），MAC_ADDR0 = 字节 2..5 */
    mmio_write(R_MAC_ADDR1, ((uint32_t)g_mac[0] << 8) | (uint32_t)g_mac[1]);
    mmio_write(R_MAC_ADDR0, ((uint32_t)g_mac[2] << 24) | ((uint32_t)g_mac[3] << 16) |
                            ((uint32_t)g_mac[4] << 8) | (uint32_t)g_mac[5]);
    return 0;
}

void emac_set_promisc(int on)
{
    g_promisc = on ? 1 : 0;
    if (g_present == 0) {
        return;
    }
    uint32_t m = mmio_read(R_MODER);
    if (g_promisc) {
        m |= MODER_PRO | MODER_BRO;   /* 混杂 + 收广播（ARP 必需） */
    } else {
        m = (m & ~MODER_PRO) | MODER_BRO;
    }
    mmio_write(R_MODER, m);
}

int emac_promisc(void)
{
    return g_promisc;
}

void emac_set_fcs_trim(uint32_t n)
{
    g_fcs_trim = (n > 8u) ? 8u : n;
}

uint32_t emac_fcs_trim(void)
{
    return g_fcs_trim;
}

/* ---- RX 环 ---- */
static void rx_arm(uint32_t i)
{
    uint32_t idx = EMAC_TX_BD_DEFAULT + i;
    uint32_t flags = RXD_E | RXD_IRQ;
    if (i + 1u == EMAC_RX_DESC_COUNT) {
        flags |= RXD_WRAP;   /* 环尾回卷到 TX_BD_NUM */
    }
    desc_write(idx, flags, (uint32_t)(uintptr_t)&g_rx_buf[i][0]);
}

uint32_t emac_rx_pending(void)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < EMAC_RX_DESC_COUNT; i++) {
        uint32_t flags = desc_flags(EMAC_TX_BD_DEFAULT + i);
        if ((flags & RXD_E) == 0u && ((flags >> 16) & 0xFFFFu) != 0u) {
            n++;
        }
    }
    return n;
}

int emac_recv(void *buf, uint32_t cap)
{
    if (g_present == 0 || buf == 0) {
        return -1;
    }
    uint32_t idx = EMAC_TX_BD_DEFAULT + g_rx_tail;
    uint32_t flags = desc_flags(idx);
    if ((flags & RXD_E) != 0u) {
        return -1;   /* 软件已备好，设备还没填 */
    }
    uint32_t len = (flags >> 16) & 0xFFFFu;
    if (len == 0u) {
        rx_arm(g_rx_tail);
        return -1;
    }
    if (len > g_fcs_trim) {
        len -= g_fcs_trim;
    }
    if (len > EMAC_RX_BUF_SIZE) {
        g_stats.rx_bad++;
        rx_arm(g_rx_tail);
        return -1;
    }
    if (len > cap) {
        g_stats.rx_drop++;
        rx_arm(g_rx_tail);
        return -1;
    }
    const uint8_t *src = (const uint8_t *)(uintptr_t)desc_bufptr(idx);
    for (uint32_t i = 0; i < len; i++) {
        ((uint8_t *)buf)[i] = src[i];
    }
    g_stats.rx_ok++;
    g_stats.rx_bytes += len;
    g_rx_tail = (g_rx_tail + 1u) % EMAC_RX_DESC_COUNT;
    rx_arm((g_rx_tail + EMAC_RX_DESC_COUNT - 1u) % EMAC_RX_DESC_COUNT);
    return (int)len;
}

/* ---- TX ---- */
int emac_send(const void *frame, uint32_t len)
{
    if (g_present == 0 || frame == 0) {
        return -1;
    }
    if (len < 14u || len > NET_FRAME_MAX) {
        g_stats.tx_drop++;
        return -1;
    }
    if (!emac_link_up()) {
        g_stats.tx_drop++;   /* 物理层无链路：不发送 */
        return -1;
    }

    uint32_t slot = g_tx_head;
    uint8_t *dst = &g_tx_buf[slot][0];
    for (uint32_t i = 0; i < len; i++) {
        dst[i] = ((const uint8_t *)frame)[i];
    }
    if (len < PKTLEN_MINFL) {
        for (uint32_t i = len; i < PKTLEN_MINFL; i++) {
            dst[i] = 0;
        }
    }

    /* RD=ready 是「可以发送」的唯一闸门，漏了它模型不会发包（RD 也就永远不会被清） */
    uint32_t flags = (len << 16) | TXD_RD | TXD_PAD | TXD_CRC | TXD_IRQ;
    if (slot + 1u == EMAC_TX_DESC_COUNT) {
        flags |= TXD_WR;
    }
    /* 顺序要求：先无 RD 的长度 → buf_ptr → 带 RD 的长度（写描述符即启动 TX） */
    desc_write(slot, len << 16, (uint32_t)(uintptr_t)dst);
    mmio_write(desc_addr(slot), flags);
    g_stats.kick++;

    /* 模型在写描述符的同一调用里完成发送并清 RD；真机为异步，给出有界等待 */
    int done = 0;
    for (uint32_t spin = 0; spin < 200000u; spin++) {
        if ((desc_flags(slot) & TXD_RD) == 0u) {
            done = 1;
            break;
        }
    }
    if (!done) {
        g_stats.tx_err++;
        return -1;
    }
    /* 完成中断：清掉我们自己看到的那一位（写 1 清） */
    uint32_t isrc = mmio_read(R_INT_SOURCE);
    if ((isrc & INT_TXB) != 0u) {
        g_stats.irq_tx++;
        mmio_write(R_INT_SOURCE, INT_TXB);
    }
    g_tx_head = (g_tx_head + 1u) % EMAC_TX_DESC_COUNT;
    g_stats.tx_ok++;
    g_stats.tx_bytes += len;
    return 0;
}

/* ---- 中断 ---- */
void emac_irq_handler(void)
{
    uint32_t isrc = mmio_read(R_INT_SOURCE);
    if ((isrc & INT_RXB) != 0u) {
        g_stats.irq_rx++;
        mmio_write(R_INT_SOURCE, INT_RXB);
    }
    if ((isrc & INT_TXB) != 0u) {
        g_stats.irq_tx++;
        mmio_write(R_INT_SOURCE, INT_TXB);
    }
    if ((isrc & INT_BUSY) != 0u) {
        /* 软件未及时补缓冲：清标志并记录（应保持为 0） */
        g_stats.rx_drop++;
        mmio_write(R_INT_SOURCE, INT_BUSY);
    }
}

void emac_int_enable(void)
{
    if (g_present == 0) {
        return;
    }
    mmio_write(R_INT_MASK, INT_RXB | INT_TXB | INT_BUSY);
    mmio_write(R_INT_SOURCE, INT_RXB | INT_TXB | INT_BUSY); /* 清历史挂起 */
    intc_route_source(EMAC_INT_SOURCE_ID, EMAC_CPU_INT);
    intc_enable_int(EMAC_CPU_INT);
}

/* ---- 初始化/探测 ---- */
int emac_present(void)
{
    return g_present;
}

void emac_get_stats(net_stats_t *out)
{
    if (out != 0) {
        *out = g_stats;
    }
}

int emac_init(void)
{
#if TUDOU_PLATFORM != 0
    /* 真机（hw）构建：ESP32-S3 硅片没有内部 EMAC（见文件头事实说明）。
     * 0x600CD000 是 QEMU open_eth 测试外设的地址，真机无此硬件；盲读还会
     * 因 EMAC 时钟未开而挂死总线（带板实证：启动卡在 pot_drivers_init 的
     * net 驱动）。故判为设备缺席、驱动框架记 skipped；真机远程控制面由
     * net_start 启用串口 MCP 通道（transport=serial-json）。 */
    g_present = 0;
    potato_printf("[POTATO][NET] EMAC: ESP32-S3 has no internal MAC, skip "
                  "(remote MCP over serial)\r\n");
    trace_push(TR_NET_LINK, 0, 0);
    return 1;
#else
    /* 设备探测：写 MODER.RST 触发模型复位，然后读回做一次读写往返校验。
     * 模型语义（hw/net/opencores_eth.c:603-619）：写 RST 会先 open_eth_reset()
     * 但紧接着把 MODER 覆盖为写入值 → 读回应为 0x800；TX_BD_NUM 复位为 0x40。
     * 设备缺席（未传 -nic，区域未映射）时数据读返回毒值 0xdeadbeef，两个判据
     * 都不成立 → 视为 skipped（QEMU 无 -nic 的测试场景）。 */
    mmio_write(R_MODER, MODER_RST);
    uint32_t moder = mmio_read(R_MODER);
    uint32_t txbd = mmio_read(R_TX_BD_NUM);
    if (moder != MODER_RST || txbd != EMAC_TX_BD_DEFAULT) {
        g_present = 0;
        potato_printf("[POTATO][NET] EMAC probe: moder=0x%x txbd=0x%x "
                      "(expect 0x%x/0x%x) -> absent\r\n",
                      (unsigned)moder, (unsigned)txbd, (unsigned)MODER_RST,
                      (unsigned)EMAC_TX_BD_DEFAULT);
        trace_push(TR_NET_LINK, 0, 0);   /* a=0：无 MAC 设备 */
        return 1;                        /* 驱动框架：skipped */
    }
    g_present = 1;
    g_tx_head = 0;
    g_rx_tail = 0;
    g_stats = (net_stats_t){ 0 };

    /* PACKETLEN：MINFL=64 MAXFL=1518 */
    mmio_write(R_PACKETLEN, (PKTLEN_MINFL << 16) | PKTLEN_MAXFL);
    mmio_write(R_TX_BD_NUM, EMAC_TX_BD_DEFAULT);

    emac_set_mac(g_mac);   /* 同时写 MAC_ADDR0/1 */

    /* RX 环备好缓冲 */
    for (uint32_t i = 0; i < EMAC_RX_DESC_COUNT; i++) {
        rx_arm(i);
    }
    /* TX 描述符清干净（避免上次运行的残留 RD 触发误发） */
    for (uint32_t i = 0; i < EMAC_TX_DESC_COUNT; i++) {
        desc_write(i, 0, 0);
    }
    mmio_write(R_INT_SOURCE, INT_RXB | INT_TXB | INT_BUSY);

    /* 使能 MAC：RXEN 上升沿会把设备 rx_desc 归位到 TX_BD_NUM。
     * 不置 PAD：模型在收包时会「短帧补齐到 MINFL 且不加 FCS」，反而让长度
     * 语义变脏；不置 PAD 时短帧按原长补齐 4B 零 FCS（裁剪后长度精确，见
     * hw/net/opencores_eth.c:459-478）。TX 侧短帧由驱动自己补齐到 64B。 */
    uint32_t m = MODER_TXEN | MODER_RXEN | MODER_BRO;
    if (g_promisc) {
        m |= MODER_PRO;
    }
    mmio_write(R_MODER, m);
    for (uint32_t i = 0; i < EMAC_RX_DESC_COUNT; i++) {
        rx_arm(i);
    }

    /* PHY 自协商结果（模型直接给 100M 全双工） */
    (void)emac_link_up();
    trace_push(TR_NET_LINK, 1, (uint32_t)emac_link_up());
    return 0;
#endif /* TUDOU_PLATFORM != 0 */
}

/* ---- 驱动框架注册（M5）：EMAC 为第二个注册驱动 ---- */
static int emac_drv_init(void)
{
    return emac_init();
}

POT_DRIVER_REGISTER(net, emac_drv_init, 20);
