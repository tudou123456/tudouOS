#include <stdint.h>

#include "driver.h"
#include "io.h"
#include "uart.h"

/*
 * UART0 驱动 — ESP32-S3（M2 起 RX 走中断）
 *   基址 0x60000000，寄存器偏移（esp-idf esp32s3/uart_reg.h）：
 *     FIFO    +0x00  写=发送字节 / 读=接收字节
 *     INT_RAW +0x04  INT_ST +0x08  INT_ENA +0x0C  INT_CLR +0x10
 *     CLKDIV  +0x14  STATUS +0x1C（RXFIFO_CNT=[9:0]，TXFIFO_CNT=[25:16]）
 *     CONF1   +0x24（RXFIFO_FULL_THRHD=[9:0]）
 *   波特率/引脚不做重配：ROM 引导时已把 UART0 配成 115200-8N1。
 * TX 保持轮询（M2 无调度器，轮询最简单可靠）；RX 由 rxfifo_full 中断
 * （阈值 1 字节）搬入环形缓冲，控制台从缓冲取数 —— 事件驱动打样。
 */

#define UART0_BASE   0x60000000u
#define UART_FIFO    (UART0_BASE + 0x00u)
#define UART_INT_RAW (UART0_BASE + 0x04u)
#define UART_INT_ST  (UART0_BASE + 0x08u)
#define UART_INT_ENA (UART0_BASE + 0x0Cu)
#define UART_INT_CLR (UART0_BASE + 0x10u)
#define UART_STATUS  (UART0_BASE + 0x1Cu)
#define UART_CONF1   (UART0_BASE + 0x24u)

#define ST_RXFIFO_CNT(st) (((st) >> 0) & 0x3FFu)
#define ST_TXFIFO_CNT(st) (((st) >> 16) & 0x3FFu)

#define UART_RXFIFO_FULL_INT (1u << 0)

/* RX 环形缓冲：长度必须是 2 的幂 */
#define RX_RING_LEN 256u
static volatile char g_rx_ring[RX_RING_LEN];
static volatile uint16_t g_rx_head;
static volatile uint16_t g_rx_tail;
static volatile uint32_t g_rx_irq_count;
static volatile uint32_t g_rx_bytes;
static volatile int g_tx_midline; /* D-03：TX 行组装中标记 */

void uart_init(void)
{
    /* 清掉 ROM 阶段可能挂起的 UART 中断标志（此时尚未开 RX 中断源） */
    mmio_write(UART_INT_CLR, 0xFFFFFFFFu);
}

void uart_irq_init(void)
{
    g_rx_head = 0;
    g_rx_tail = 0;
    g_rx_irq_count = 0;
    g_rx_bytes = 0;

    /* RX FIFO 达 1 字节即触发 rxfifo_full 中断 */
    mmio_write(UART_CONF1, 1u & 0x3FFu);
    mmio_write(UART_INT_CLR, 0xFFFFFFFFu);
    mmio_write(UART_INT_ENA, UART_RXFIFO_FULL_INT);
}

/* intc.c 在 level2 调用 */
void uart0_irq_handler(void)
{
    if ((mmio_read(UART_INT_ST) & UART_RXFIFO_FULL_INT) != 0u) {
        g_rx_irq_count++;
        while (ST_RXFIFO_CNT(mmio_read(UART_STATUS)) != 0u) {
            char c = (char)(mmio_read(UART_FIFO) & 0xFFu);
            uint16_t next = (uint16_t)((g_rx_head + 1) & (RX_RING_LEN - 1u));
            if (next != g_rx_tail) { /* 满则丢字节（128B 对控制台足够） */
                g_rx_ring[g_rx_head] = c;
                g_rx_head = next;
                g_rx_bytes++;
            }
        }
        mmio_write(UART_INT_CLR, UART_RXFIFO_FULL_INT);
    }
}

/* M7：输出抽头——非空时每个输出字节额外送一份（远程 MCP 通道用它抓
 * {"jsonrpc"} 响应行）。抽头不得打印，否则自激。 */
static void (*g_uart_sink)(char);

void uart_set_sink(void (*fn)(char))
{
    g_uart_sink = fn;
}

void uart_putc(char c)
{
    if (c == '\n') {
        uart_putc('\r');
    }
    if (g_uart_sink != 0) {
        g_uart_sink(c);
    }
    /* 等 TX FIFO 排空再写，对 FIFO 深度不敏感 */
    while (ST_TXFIFO_CNT(mmio_read(UART_STATUS)) != 0u) {
    }
    mmio_write(UART_FIFO, (uint32_t)(uint8_t)c);
    /* D-03：行组装标记——自上一个换行起有字节发出且尚未收行。
     * 调度器据此把 tick 抢占推迟到行边界，保住逐行输出锚点。 */
    if (c == '\n') {
        g_tx_midline = 0;
    } else {
        g_tx_midline = 1;
    }
}

int uart_tx_midline(void)
{
    return g_tx_midline;
}

/* 诊断探针：直接写 FIFO 一个字符，不查状态（vectors.S 用） */
void uart_mark(char c)
{
    mmio_write(UART_FIFO, (uint32_t)(uint8_t)c);
}

void uart_puts(const char *s)
{
    while (*s != '\0') {
        uart_putc(*s++);
    }
}

/* M7：绕过输出抽头的发送（串口 MCP 帧用）。帧字节若再被 tap 抓取会自激
 * 重发，所以不得调用 g_uart_sink；但行边界语义必须照旧维护：调度器靠
 * g_tx_midline 把 tick 抢占推迟到行边界，否则帧会被任务切换切成两截。 */
static void uart_putc_raw(char c)
{
    if (c == '\n') {
        uart_putc_raw('\r');
    }
    while (ST_TXFIFO_CNT(mmio_read(UART_STATUS)) != 0u) {
    }
    mmio_write(UART_FIFO, (uint32_t)(uint8_t)c);
    if (c == '\n') {
        g_tx_midline = 0;
    } else {
        g_tx_midline = 1;
    }
}

void uart_puts_raw(const char *s)
{
    while (*s != '\0') {
        uart_putc_raw(*s++);
    }
}

/* K2：异步输出（obs 后台流式 JSONL 等）的行首保护：控制台提示符打印后
 * 会处于中线（g_tx_midline=1）；若后台任务此时插行，直接写会把
 * "potato> [POTATO][TRJ] {...}" 粘成一行，行级解析器认不出行首前缀。
 * 先补 CRLF 再输出即可保证行首干净。 */
void uart_line_begin(void)
{
    if (g_tx_midline != 0) {
        uart_puts("\r\n");
    }
}

int uart_getc(void)
{
    if (g_rx_head != g_rx_tail) {
        char c = g_rx_ring[g_rx_tail];
        g_rx_tail = (uint16_t)((g_rx_tail + 1) & (RX_RING_LEN - 1u));
        return (int)(unsigned char)c;
    }
    return -1;
}

uint32_t uart_rx_irq_count(void)
{
    return g_rx_irq_count;
}

uint32_t uart_rx_bytes(void)
{
    return g_rx_bytes;
}

/* ---- 驱动框架注册（M5）：UART 为首个注册驱动 ---- */

static int uart_drv_init(void)
{
    uart_init();
    return 0;
}

POT_DRIVER_REGISTER(uart, uart_drv_init, 10);
