#ifndef TUDOU_CONSOLE_H
#define TUDOU_CONSOLE_H

/* 内核自带 mini printf：支持 %c %s %d %u %x %p %% 及可选 0 填充宽度 */
void potato_printf(const char *fmt, ...);

/* 串口命令循环（ping/echo/info/help），M1 的内核主循环 */
void console_run(void);

#endif /* TUDOU_CONSOLE_H */
