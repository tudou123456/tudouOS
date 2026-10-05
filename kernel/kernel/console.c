#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "console.h"
#include "lib_shim.h"
#include "potato.h"
#include "uart.h"

/* ---------- mini printf ---------- */

/* LX7 无硬件除法指令，二进制长除法自实现，避免链接 libgcc（窗口 ABI） */
static uint32_t udiv32(uint32_t n, uint32_t d, uint32_t *rem)
{
    uint32_t q = 0, r = 0;
    for (int i = 31; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1u);
        if (r >= d) {
            r -= d;
            q |= 1u << i;
        }
    }
    if (rem != NULL) {
        *rem = r;
    }
    return q;
}

static void put_unsigned(uint32_t v, uint32_t base, int width, char pad)
{
    char buf[12]; /* 十进制最长 10 位，十六进制 8 位 */
    const char *digits = "0123456789abcdef";
    int i = 0;

    do {
        uint32_t r;
        v = udiv32(v, base, &r);
        buf[i++] = digits[r];
    } while (v != 0u && i < (int)sizeof(buf));

    while (i < width) {
        buf[i++] = pad; /* buf 按 LSD 在前存放，填充符排在高位即输出在前 */
    }
    while (i > 0) {
        uart_putc(buf[--i]);
    }
}

void potato_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);

    for (; *fmt != '\0'; fmt++) {
        if (*fmt != '%') {
            uart_putc(*fmt);
            continue;
        }
        fmt++;
        char pad = ' ';
        int width = 0;
        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }
        switch (*fmt) {
        case '%':
            uart_putc('%');
            break;
        case 'c':
            uart_putc((char)va_arg(ap, int));
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            uart_puts(s != NULL ? s : "(null)");
            break;
        }
        case 'd': {
            int v = va_arg(ap, int);
            if (v < 0) {
                uart_putc('-');
                put_unsigned((uint32_t)(-(long)v), 10, width, pad);
            } else {
                put_unsigned((uint32_t)v, 10, width, pad);
            }
            break;
        }
        case 'u':
            put_unsigned(va_arg(ap, uint32_t), 10, width, pad);
            break;
        case 'x':
            put_unsigned(va_arg(ap, uint32_t), 16, width, pad);
            break;
        case 'p':
            uart_puts("0x");
            put_unsigned((uint32_t)(uintptr_t)va_arg(ap, void *), 16, 8, '0');
            break;
        case '\0':
            va_end(ap);
            return;
        default:
            uart_putc('%');
            uart_putc(*fmt);
            break;
        }
    }
    va_end(ap);
}

/* ---------- 串口命令循环 ---------- */

#define CMD_BUF_LEN 96

static void cmd_help(void)
{
    uart_puts("commands: help, ping, echo <text>, info\r\n");
}

static void cmd_info(void)
{
    potato_printf("[POTATO] info: %s v%s kernel=%s target=%s\r\n",
                  POTATO_NAME, POTATO_VERSION, POTATO_KERNEL, POTATO_TARGET);
}

static void cmd_dispatch(char *line)
{
    /* 就地把首词和参数拆开 */
    char *arg = line;
    while (*arg != '\0' && *arg != ' ') {
        arg++;
    }
    if (*arg == ' ') {
        *arg++ = '\0';
        while (*arg == ' ') {
            arg++;
        }
    }

    if (strcmp(line, "") == 0) {
        return;
    }
    if (strcmp(line, "help") == 0) {
        cmd_help();
        return;
    }
    if (strcmp(line, "ping") == 0) {
        uart_puts("pong\r\n");
        return;
    }
    if (strcmp(line, "echo") == 0) {
        potato_printf("%s\r\n", arg);
        return;
    }
    if (strcmp(line, "info") == 0) {
        cmd_info();
        return;
    }
    potato_printf("[POTATO] unknown command: %s (try 'help')\r\n", line);
}

void console_run(void)
{
    static char buf[CMD_BUF_LEN];

    uart_puts("[POTATO] type 'help' for commands\r\n");
    for (;;) {
        uart_puts("potato> ");
        int n = 0;
        for (;;) {
            int c;
            do {
                c = uart_getc();
            } while (c < 0); /* M1 无调度器，收字节忙等 */
            if (c == '\r' || c == '\n') {
                uart_putc('\n');
                break;
            }
            if (c == 0x7f || c == '\b') {
                if (n > 0) {
                    n--;
                    uart_puts("\b \b");
                }
                continue;
            }
            if (c < 0x20) {
                continue;
            }
            if (n < CMD_BUF_LEN - 1) {
                buf[n++] = (char)c;
                uart_putc((char)c); /* 回显，测试台日志可读 */
            }
        }
        buf[n] = '\0';
        cmd_dispatch(buf);
    }
}
