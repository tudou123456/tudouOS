#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "console.h"
#include "demo.h"
#include "intc.h"
#include "lib_shim.h"
#include "potato.h"
#include "task.h"
#include "tick.h"
#include "time.h"
#include "trace.h"
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
    /* v3 检查点调度下，切换只发生在 syscall（yield/sleep），printf 不会被
       中途切走，无需关中断——且 INTLEVEL=15 期间挂起 tick 会触发 QEMU
       活锁（M2 已知问题），坚决不在这里持有高 INTLEVEL */
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
    uart_puts("commands: help, ping, echo <text>, info, tickstat, trace [n], "
              "tasks, taskdemo <mode>, sleep <ms>, crash\r\n");
}

static void cmd_info(void)
{
    potato_printf("[POTATO] info: %s v%s kernel=%s target=%s\r\n",
                  POTATO_NAME, POTATO_VERSION, POTATO_KERNEL, POTATO_TARGET);
}

static void cmd_tasks(void)
{
    uint32_t d[4];
    pot_sched_diag(d);
    int n = pot_task_count();
    for (int i = 0; i < n; i++) {
        const pot_task_t *t = pot_task_at(i);
        if (t == 0) {
            continue;
        }
        potato_printf("[POTATO][TS] %s cls=%s st=%d cpu=%u sw=%u\r\n",
                      t->name, pot_class_name(t->cls), t->state,
                      t->cpu_ticks, t->switches);
    }
    potato_printf("[POTATO][TS] now=%u switches=%u bg_used=%u/%u "
                  "diag wake=%u prem=%u sw=%u idle=%u\r\n",
                  pot_sched_now(), pot_sched_total_switches(),
                  pot_sched_bg_used(), (unsigned)POT_SCHED_BG_BUDGET,
                  d[0], d[1], d[2], d[3]);
}

static void cmd_taskdemo(const char *arg)
{
    if (strcmp(arg, "off") == 0) {
        demo_stop();
        uart_puts("[POTATO][D] demo stop-req\r\n");
        return;
    }
    int r = demo_start(arg);
    if (r == 0) {
        potato_printf("[POTATO][D] demo %s started\r\n", arg);
    } else if (r == -1) {
        uart_puts("[POTATO][D] demo busy (taskdemo off first)\r\n");
    } else {
        potato_printf("[POTATO][D] unknown mode (%s)\r\n", demo_mode_help());
    }
}

static unsigned parse_dec(const char *s, unsigned fallback)
{
    unsigned v = 0;
    int seen = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10u + (unsigned)(*s - '0');
        s++;
        seen = 1;
    }
    return seen ? v : fallback;
}

static void cmd_sleep(const char *arg)
{
    unsigned ms = parse_dec(arg, 0);
    if (ms == 0u) {
        uart_puts("[POTATO][D] sleep <ms>\r\n");
        return;
    }
    potato_printf("[POTATO][D] SLEEP %u\r\n", ms);
    pot_task_sleep(ms);
    potato_printf("[POTATO][D] SLEPT %u\r\n", ms);
}

static void cmd_tickstat(void)
{
    potato_printf("[POTATO][TICK] ticks=%u cpu_hz=%u now_us=0x%x jit_min=%uus "
                  "jit_max=%uus uart_irq=%u rx_bytes=%u\r\n",
                  time_tick_count(), tick_cpu_hz(), time_now_us(),
                  time_tick_jitter_min_us(), time_tick_jitter_max_us(),
                  uart_rx_irq_count(), uart_rx_bytes());
}

static void cmd_trace(const char *arg)
{
    trace_dump(parse_dec(arg, 16u));
}

static void cmd_flood(const char *arg)
{
    unsigned n = parse_dec(arg, 200u);
    for (unsigned i = 0; i < n; i++) {
        potato_printf("[POTATO][F] line %u\r\n", i);
    }
    potato_printf("[POTATO][F] FLOOD-DONE %u\r\n", n);
}

static void cmd_crash(void)
{
    uart_puts("[POTATO] crashing on purpose (ill) ...\r\n");
    asm volatile("ill"); /* 触发非法指令异常，走通用异常向量 */
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
    if (strcmp(line, "tickstat") == 0) {
        cmd_tickstat();
        return;
    }
    if (strcmp(line, "trace") == 0) {
        cmd_trace(arg);
        return;
    }
    if (strcmp(line, "tasks") == 0) {
        cmd_tasks();
        return;
    }
    if (strcmp(line, "taskdemo") == 0) {
        cmd_taskdemo(arg);
        return;
    }
    if (strcmp(line, "sleep") == 0) {
        cmd_sleep(arg);
        return;
    }
    if (strcmp(line, "flood") == 0) {
        cmd_flood(arg);
        return;
    }
    if (strcmp(line, "crash") == 0) {
        cmd_crash();
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
            int c = uart_getc();
            if (c < 0) {
                pot_task_sleep(1); /* 无输入让出 CPU（调度检查点） */
                continue;
            }
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
