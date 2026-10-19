#ifndef TUDOU_UART_H
#define TUDOU_UART_H

#include <stdint.h>

void uart_init(void);
void uart_irq_init(void);      /* 配置 RX FIFO 中断（intc_init 之后调用） */
void uart_putc(char c);        /* '\n' 自动扩展为 "\r\n" */
void uart_puts_raw(const char *s); /* 同 uart_puts 但绕过输出抽头（M7 串口 MCP
                                    * 帧专用；仍维护行边界标记，防帧被 tap 抓取） */
void uart_line_begin(void);    /* K2：异步输出行首保护——中线未收行时先补 CRLF */
void uart_mark(char c);        /* 诊断探针：直写 FIFO，不查状态 */
void uart_puts(const char *s); /* 无格式字符串，不改写换行符 */
int  uart_getc(void);          /* 无数据返回 -1（从 RX 环形缓冲取） */

void uart0_irq_handler(void);  /* intc 分发调用，勿直接调用 */

uint32_t uart_rx_irq_count(void);
uint32_t uart_rx_bytes(void);
int uart_tx_midline(void);     /* D-03：TX 行组装中（调度器推迟抢占用） */

/* M7：输出抽头（远程 MCP 通道抓响应行用）。fn 必须不重入、不打印。 */
void uart_set_sink(void (*fn)(char));

#endif /* TUDOU_UART_H */
