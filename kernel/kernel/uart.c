#include <stdint.h>

#include "io.h"
#include "uart.h"

/*
 * UART0 轮询驱动 — ESP32-S3
 *   基址 0x60000000，寄存器偏移（esp-idf esp32s3/uart_reg.h）：
 *     FIFO    +0x00  写=发送字节 / 读=接收字节
 *     INT_CLR +0x10  写 1 清挂起中断
 *     STATUS  +0x1C  RXFIFO_CNT=bits[9:0]，TXFIFO_CNT=bits[25:16]
 *   波特率/引脚不做重配：ROM 引导时已把 UART0 配成 115200-8N1 并接到
 *   默认引脚（GPIO43/44），QEMU 的 UART 模型对波特率也不敏感。
 */

#define UART0_BASE   0x60000000u
#define UART_FIFO    (UART0_BASE + 0x00u)
#define UART_INT_CLR (UART0_BASE + 0x10u)
#define UART_STATUS  (UART0_BASE + 0x1Cu)

#define ST_RXFIFO_CNT(st) (((st) >> 0) & 0x3FFu)
#define ST_TXFIFO_CNT(st) (((st) >> 16) & 0x3FFu)

void uart_init(void)
{
    /* 清掉 ROM 阶段可能挂起的 UART 中断标志（M1 用轮询，不开中断源） */
    mmio_write(UART_INT_CLR, 0xFFFFFFFFu);
}

void uart_putc(char c)
{
    if (c == '\n') {
        uart_putc('\r');
    }
    /* 等 TX FIFO 排空再写，轮询实现里最稳妥（对 FIFO 深度不敏感） */
    while (ST_TXFIFO_CNT(mmio_read(UART_STATUS)) != 0u) {
    }
    mmio_write(UART_FIFO, (uint32_t)(uint8_t)c);
}

void uart_puts(const char *s)
{
    while (*s != '\0') {
        uart_putc(*s++);
    }
}

int uart_getc(void)
{
    uint32_t st = mmio_read(UART_STATUS);
    if (ST_RXFIFO_CNT(st) == 0u) {
        return -1;
    }
    return (int)(mmio_read(UART_FIFO) & 0xFFu);
}
