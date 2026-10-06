#ifndef TUDOU_UART_H
#define TUDOU_UART_H

#include <stdint.h>

void uart_init(void);
void uart_irq_init(void);      /* 配置 RX FIFO 中断（intc_init 之后调用） */
void uart_putc(char c);        /* '\n' 自动扩展为 "\r\n" */
void uart_mark(char c);        /* 诊断探针：直写 FIFO，不查状态 */
void uart_puts(const char *s); /* 无格式字符串，不改写换行符 */
int  uart_getc(void);          /* 无数据返回 -1（从 RX 环形缓冲取） */

void uart0_irq_handler(void);  /* intc 分发调用，勿直接调用 */

uint32_t uart_rx_irq_count(void);
uint32_t uart_rx_bytes(void);

#endif /* TUDOU_UART_H */
