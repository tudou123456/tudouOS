#ifndef TUDOU_UART_H
#define TUDOU_UART_H

void uart_init(void);
void uart_putc(char c);        /* '\n' 自动扩展为 "\r\n" */
void uart_puts(const char *s); /* 无格式字符串，不改写换行符 */
int  uart_getc(void);          /* 无数据返回 -1，不阻塞等待 */

#endif /* TUDOU_UART_H */
