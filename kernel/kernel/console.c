#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "console.h"
#include "demo.h"
#include "driver.h"
#include "heap.h"
#include "intc.h"
#include "lib_shim.h"
#include "mcp.h"
#include "memdemo.h"
#include "msgdemo.h"
#include "net.h"
#include "obs.h"
#include "pal.h"
#include "potato.h"
#include "potfs.h"
#include "smp.h"
#include "task.h"
#include "tick.h"
#include "time.h"
#include "trace.h"
#include "uart.h"

#define CMD_BUF_LEN 264 /* MCP 行上限 = POT_MSG_INLINE(256) + JSON-RPC 包裹余量。
                           超长行显式报错（不静默截断，D-07）；json 释放已
                           迭代化，长行的树释放无栈深风险 */

/* ---------- mini printf ---------- */

/* LX7 无硬件除法指令，二进制长除法自实现，避免链接 libgcc（窗口 ABI）。
 * volatile 索引禁 loop 指令（该 QEMU 不支持，见 lib_shim.c 注释）。 */
static uint32_t udiv32(uint32_t n, uint32_t d, uint32_t *rem)
{
    uint32_t q = 0, r = 0;
    for (volatile int i = 31; i >= 0; i--) {
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
    volatile int i = 0;

    do {
        uint32_t r;
        v = udiv32(v, base, &r);
        buf[i] = digits[r];
        i++;
    } while (v != 0u && i < (int)sizeof(buf));

    while (i < width) {
        buf[i] = pad; /* buf 按 LSD 在前存放，填充符排在高位即输出在前 */
        i++;
    }
    while (i > 0) {
        i--;
        uart_putc(buf[i]);
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

static void cmd_help(void)
{
    uart_puts("commands: help, ping, echo <text>, info, tickstat, trace [n], "
              "tasks, taskdemo <mode>, sleep <ms>, msgdemo <mode> [n], "
              "msgstat, memstat, memtest <mode> [n], pal, drivers, "
              "fs <sub>, mcp on|off, flood <n>, crash, looptest [n]\r\n"
              "  fs: ls|write <n> <t>|cat <n>|rm <n>|mkfs|df\r\n"
              "  trace: [n]|jsonl [n]|imp [n]|stream on|off|pump|meta|health\r\n"
              "  health, obs [panic], inject <leak|hang|spin|off> [arg]\r\n"
              "  net [arp|link down|up|ip <a.b.c.d> [gw]], ping <a.b.c.d>\r\n"
              "  netmcp [dump|test|port <p>|peer <a.b.c.d> <port>]\r\n"
              "  smp [stat|start|ipi <n>|spawn <cpu|any> <n>|work|preempt on|off|"
              "selftest]  (M9 双核)\r\n"
              "  MCP: lines starting with '{' are JSON-RPC\r\n");
}

/* M9：SMP 控制台命令。
 *   smp               → 状态（两核 online/IPI/迁移/自旋计数）
 *   smp start         → 启动 CPU1（写 SYSTEM_CORE_1_CONTROL_1 走 ROM 桩）
 *   smp ipi <n>       → CPU0 向 CPU1 发 n 次核间中断（验证 IPI 通路）
 *   smp spawn <cpu|any> <n> → 创建 n 个双核压测任务（any=任意核）
 *   smp work          → 打印压测任务执行计数
 *   smp preempt on|off → CPU1 tick 抢占开关（默认 off）
 *   smp selftest      → 常量/身份自检（rc=0 为通过） */
static const char *smp_skip_space(const char *p)
{
    while (*p == ' ') {
        p++;
    }
    return p;
}

static unsigned smp_parse_unsigned(const char **pp, unsigned max)
{
    const char *p = *pp;
    unsigned n = 0;
    unsigned digits = 0;
    while (*p >= '0' && *p <= '9') {
        n = n * 10u + (unsigned)(*p - '0');
        p++;
        digits++;
        if (digits > 6u) {
            break;
        }
    }
    if (n > max) {
        n = max;
    }
    *pp = p;
    return n;
}

static void cmd_smp(const char *arg)
{
    if (*arg == '\0' || strcmp(arg, "stat") == 0) {
        pot_smp_dump();
        return;
    }
    if (strcmp(arg, "selftest") == 0) {
        potato_printf("[POTATO][SMP] selftest rc=%d\r\n", pot_smp_selftest());
        return;
    }
    if (strcmp(arg, "start") == 0) {
        int rc = pot_smp_start();
        potato_printf("[POTATO][SMP] start rc=%d online=%u\r\n", rc,
                      (unsigned)pot_smp_online());
        return;
    }
    if (strcmp(arg, "work") == 0) {
        pot_smp_work_dump();
        return;
    }
    if (strcmp(arg, "preempt") == 0) {
        potato_printf("[POTATO][SMP] cpu1_preempt=%d (usage: smp preempt on|off)\r\n",
                      pot_smp_cpu1_preempt());
        return;
    }
    if (strcmp(arg, "preempt on") == 0 || strcmp(arg, "preempt 1") == 0) {
        pot_smp_set_cpu1_preempt(1);
        potato_printf("[POTATO][SMP] cpu1_preempt=1\r\n");
        return;
    }
    if (strcmp(arg, "preempt off") == 0 || strcmp(arg, "preempt 0") == 0) {
        pot_smp_set_cpu1_preempt(0);
        potato_printf("[POTATO][SMP] cpu1_preempt=0\r\n");
        return;
    }
    if (strncmp(arg, "spawn ", 6) == 0) {
        const char *p = smp_skip_space(arg + 6);
        int aff = -1;
        if (p[0] == 'a' && p[1] == 'n' && p[2] == 'y') {
            aff = -1;
            p = smp_skip_space(p + 3);
        } else if (p[0] == '0' || (p[0] == '1' && (p[1] == ' ' || p[1] == '\0'))) {
            aff = (p[0] == '1') ? 1 : 0;
            p = smp_skip_space(p + 1);
        } else {
            potato_printf("[POTATO][SMP] spawn: bad cpu (0|1|any)\r\n");
            return;
        }
        unsigned n = smp_parse_unsigned(&p, 4u);
        int made = pot_smp_spawn(aff, n);
        potato_printf("[POTATO][SMP] spawn aff=%d n=%u made=%d\r\n",
                      aff, n, made);
        return;
    }
    if (strncmp(arg, "ipi ", 4) == 0) {
        const char *p = arg + 4;
        unsigned n = smp_parse_unsigned(&p, 10000u);
        for (unsigned i = 0; i < n; i++) {
            pot_smp_ipi_send(1, POT_IPI_RESCHED);
        }
        potato_printf("[POTATO][SMP] ipi sent=%u to cpu1\r\n", n);
        return;
    }
    potato_printf("[POTATO][SMP] unknown smp sub: %s "
                  "(stat|start|ipi <n>|spawn <cpu|any> <n>|work|"
                  "preempt on|off|selftest)\r\n", arg);
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
    uint32_t rdrop = 0, sdrop = 0, regdrop = 0;
    pot_sched_drop_diag(&rdrop, &sdrop);
    regdrop = pot_task_reg_overflow();
    int n = pot_task_count();
    for (int i = 0; i < n; i++) {
        const pot_task_t *t = pot_task_at(i);
        if (t == 0) {
            continue;
        }
        potato_printf("[POTATO][TS] %s cls=%s st=%d cpu=%u sw=%u stk=%uw "
                      "(unused %uw)\r\n",
                      t->name, pot_class_name(t->cls), t->state,
                      t->cpu_ticks, t->switches, t->stk_words,
                      pot_task_stack_unused(t));
    }
    potato_printf("[POTATO][TS] now=%u switches=%u bg_used=%u/%u "
                  "diag wake=%u prem=%u sw=%u idle=%u "
                  "drop ready=%u sleep=%u reg=%u\r\n",
                  pot_sched_now(), pot_sched_total_switches(),
                  pot_sched_bg_used(), (unsigned)POT_SCHED_BG_BUDGET,
                  d[0], d[1], d[2], d[3], rdrop, sdrop, regdrop);
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

/* 拆首词：把 arg 开头的一个词写进 out（含 '\0'），返回剩余参数指针 */
static const char *split_word(const char *arg, char *out, int cap)
{
    int i = 0;
    while (*arg != '\0' && *arg != ' ' && i < cap - 1) {
        out[i++] = *arg++;
    }
    out[i] = '\0';
    while (*arg == ' ') {
        arg++;
    }
    return arg;
}

static void cmd_trace(const char *arg)
{
    /* 子命令：trace [n] | jsonl [n] | imp [n] | stream on|off|pump | meta | health */
    char sub[10];
    const char *rest = split_word(arg, sub, (int)sizeof(sub));

    if (sub[0] == '\0') {
        trace_dump(16u);
        return;
    }
    if (strcmp(sub, "jsonl") == 0) {
        unsigned n = trace_export_jsonl(parse_dec(rest, 32u));
        potato_printf("[POTATO][TRJ] exported=%u\r\n", n);
        return;
    }
    if (strcmp(sub, "stream") == 0) {
        char w[8];
        const char *r2 = split_word(rest, w, (int)sizeof(w));
        (void)r2;
        if (strcmp(w, "on") == 0) {
            pot_obs_stream_set(1);
        } else if (strcmp(w, "off") == 0) {
            pot_obs_stream_set(0);
        } else if (strcmp(w, "pump") == 0) {
            unsigned n = pot_obs_pump();
            potato_printf("[POTATO][OBS] pump=%u pumped=%u lost=%u\r\n",
                          n, pot_obs_pumped(), pot_obs_lost());
        } else {
            uart_puts("[POTATO][OBS] usage: trace stream on|off|pump\r\n");
        }
        return;
    }
    if (strcmp(sub, "imp") == 0) {
        unsigned n = trace_export_important(parse_dec(rest, 0u));
        potato_printf("[POTATO][TRJ] exported_imp=%u total_imp=%u\r\n",
                      n, trace_important_total());
        return;
    }
    if (strcmp(sub, "meta") == 0) {
        static char mbuf[768];
        trace_meta_json(mbuf, sizeof(mbuf), pot_obs_stream_on());
        potato_printf("[POTATO][TRM] %s\r\n", mbuf);
        return;
    }
    if (strcmp(sub, "health") == 0) {
        pot_obs_health();
        return;
    }
    trace_dump(parse_dec(arg, 16u));
}

static void cmd_health(void)
{
    pot_obs_health();
}

static void cmd_inject(const char *arg)
{
    /* inject <leak|hang|spin|off> [arg] */
    char mode[10];
    const char *rest = split_word(arg, mode, (int)sizeof(mode));
    pot_inject_cmd(mode, parse_dec(rest, 0));
}

static void cmd_obs(const char *arg)
{
    /* obs panic → 崩溃诊断样例（快照 + 最近事件 JSONL） */
    if (strcmp(arg, "panic") == 0) {
        pot_obs_panic_dump();
        return;
    }
    potato_printf("[POTATO][OBS] stream=%d pumped=%u lost=%u calls=%u "
                  "health_n=%u imp_pumped=%u imp_lost=%u\r\n",
                  pot_obs_stream_on(), pot_obs_pumped(), pot_obs_lost(),
                  pot_obs_pump_calls(), pot_obs_health_count(),
                  pot_obs_imp_pumped(), pot_obs_imp_lost());
}

static void cmd_flood(const char *arg)
{
    unsigned n = parse_dec(arg, 200u);
    for (unsigned i = 0; i < n; i++) {
        potato_printf("[POTATO][F] line %u\r\n", i);
    }
    potato_printf("[POTATO][F] FLOOD-DONE %u\r\n", n);
}

static void cmd_msgdemo(const char *arg)
{
    /* 参数：<mode> [n]，手动拆首词 */
    char mode[12];
    int mi = 0;
    while (*arg != '\0' && *arg != ' ' && mi < (int)sizeof(mode) - 1) {
        mode[mi++] = *arg++;
    }
    mode[mi] = '\0';
    while (*arg == ' ') {
        arg++;
    }
    unsigned n = parse_dec(arg, 0);

    if (strcmp(mode, "off") == 0) {
        msgdemo_stop();
        uart_puts("[POTATO][M] msgdemo stop-req\r\n");
        return;
    }
    int r = msgdemo_start(mode, n);
    if (r == 0) {
        potato_printf("[POTATO][M] msgdemo %s n=%u started\r\n", mode, n);
    } else if (r == -1) {
        uart_puts("[POTATO][M] msgdemo busy (msgdemo off first)\r\n");
    } else {
        potato_printf("[POTATO][M] unknown mode (%s)\r\n", msgdemo_mode_help());
    }
}

static void cmd_memstat(void)
{
    pot_heap_stat_t st;
    pot_heap_getstat(&st);
    potato_printf("[POTATO][MEM] total=%u used=%u free=%u largest=%u "
                  "maxused=%u alloc=%u free_n=%u fail=%u faults=%u "
                  "fragpct=%u/255\r\n",
                  st.total, st.used, st.total - st.used, st.largest,
                  st.max_used, st.n_alloc, st.n_free, st.n_fail, st.n_fault,
                  st.total - st.used != 0u
                      ? ((st.total - st.used - st.largest) * 255u) /
                            (st.total - st.used)
                      : 0u);
}

static void cmd_memtest(const char *arg)
{
    /* <mode> [n] 手动拆首词 */
    char mode[12];
    int mi = 0;
    while (*arg != '\0' && *arg != ' ' && mi < (int)sizeof(mode) - 1) {
        mode[mi++] = *arg++;
    }
    mode[mi] = '\0';
    while (*arg == ' ') {
        arg++;
    }
    unsigned n = parse_dec(arg, 0);

    if (strcmp(mode, "off") == 0) {
        memdemo_stop();
        uart_puts("[POTATO][MEM] memtest stop-req\r\n");
        return;
    }
    if (strcmp(mode, "churn") == 0) {
        int r = memdemo_start("churn", 0);
        if (r == 0) {
            uart_puts("[POTATO][MEM] memtest churn started\r\n");
        } else {
            uart_puts("[POTATO][MEM] memtest busy\r\n");
        }
        return;
    }
    mem_cmd_dispatch(mode, n);
}

static void cmd_pal(void)
{
    potato_printf("[POTATO][PAL] platform=%s tick=%s cpu_hz=%u\r\n",
                  pot_pal_name(),
                  pot_pal_tick_mode() == POT_TICK_POLL ? "poll" : "irq",
                  pot_pal_cpu_hz());
}

static void cmd_drivers(void)
{
    int n = pot_driver_count();
    if (n == 0) {
        uart_puts("[POTATO][DRV] no drivers registered\r\n");
        return;
    }
    for (volatile int i = 0; i < n; i++) {
        potato_printf("[POTATO][DRV] %s status=%d\r\n",
                      pot_driver_name(i), pot_driver_status(i));
    }
}

static void cmd_fs(const char *arg)
{
    /* 子命令：ls | write <name> <text> | cat <name> | rm <name> | mkfs | df */
    char sub[10];
    int si = 0;
    while (*arg != '\0' && *arg != ' ' && si < (int)sizeof(sub) - 1) {
        sub[si++] = *arg++;
    }
    sub[si] = '\0';
    while (*arg == ' ') {
        arg++;
    }

    if (strcmp(sub, "mkfs") == 0) {
        potfs_format();
        uart_puts("[POTATO][FS] formatted\r\n");
        return;
    }
    if (strcmp(sub, "ls") == 0) {
        int n = potfs_count();
        potato_printf("[POTATO][FS] %d files, %u bytes\r\n", n,
                      potfs_total_bytes());
        for (volatile int i = 0; i < POTFS_MAX_FILES; i++) {
            const potfs_file_t *f = potfs_get(i);
            if (f != 0) {
                potato_printf("[POTATO][FS] %s %u\r\n", f->name, f->size);
            }
        }
        return;
    }
    if (strcmp(sub, "cat") == 0) {
        char buf[192];
        int n = potfs_read(arg, buf, sizeof(buf) - 1);
        if (n < 0) {
            potato_printf("[POTATO][FS] cat: %s not found\r\n", arg);
            return;
        }
        buf[n] = '\0';
        potato_printf("[POTATO][FS] %s %u bytes:\r\n%s\r\n", arg, n, buf);
        return;
    }
    if (strcmp(sub, "write") == 0) {
        /* name 与 text 拆分 */
        char name[POTFS_NAME_MAX];
        int ni = 0;
        while (*arg != '\0' && *arg != ' ' && ni < POTFS_NAME_MAX - 1) {
            name[ni++] = *arg++;
        }
        name[ni] = '\0';
        while (*arg == ' ') {
            arg++;
        }
        int r = potfs_write(name, arg, (uint32_t)strlen(arg));
        if (r < 0) {
            uart_puts("[POTATO][FS] write failed\r\n");
        } else {
            potato_printf("[POTATO][FS] wrote %s %d bytes\r\n", name, r);
        }
        return;
    }
    if (strcmp(sub, "rm") == 0) {
        if (potfs_delete(arg) == 0) {
            potato_printf("[POTATO][FS] removed %s\r\n", arg);
        } else {
            potato_printf("[POTATO][FS] rm: %s not found\r\n", arg);
        }
        return;
    }
    if (strcmp(sub, "df") == 0) {
        pot_heap_stat_t st;
        pot_heap_getstat(&st);
        potato_printf("[POTATO][FS] store=potfs(ram) files=%u/%u bytes=%u "
                      "heap_free=%u\r\n",
                      (unsigned)potfs_count(), (unsigned)POTFS_MAX_FILES,
                      potfs_total_bytes(), st.total - st.used);
        return;
    }
    uart_puts("[POTATO][FS] fs ls|write <n> <t>|cat <n>|rm <n>|mkfs|df\r\n");
}

static void cmd_mcp(const char *arg)
{
    if (strcmp(arg, "on") == 0) {
        if (mcp_start() == 0) {
            uart_puts("[POTATO][MCP] starting\r\n");
        } else {
            uart_puts("[POTATO][MCP] already running or no resource\r\n");
        }
        return;
    }
    if (strcmp(arg, "off") == 0) {
        mcp_stop();
        uart_puts("[POTATO][MCP] stop-req\r\n");
        return;
    }
    potato_printf("[POTATO][MCP] running=%d (mcp on|off; "
                  "JSON-RPC 行以 '{' 开头直接投递)\r\n", mcp_running());
}

static void cmd_preempt(const char *arg)
{
    if (strcmp(arg, "off") == 0) {
        pot_sched_set_preempt(0);
        uart_puts("[POTATO][SCH] preempt disabled (checkpoint only)\r\n");
        return;
    }
    if (strcmp(arg, "on") == 0) {
        pot_sched_set_preempt(1);
        uart_puts("[POTATO][SCH] preempt enabled (tick IRQ + rfe)\r\n");
        return;
    }
    potato_printf("[POTATO][SCH] preempt=%d (preempt on|off)\r\n",
                  pot_sched_preempt_enabled());
}

static void cmd_crash(void)
{
    uart_puts("[POTATO] crashing on purpose (ill) ...\r\n");
    asm volatile("ill"); /* 触发非法指令异常，走通用异常向量 */
}

/* ---------- E-02 判定实验：可控 rfe 恢复（defect-report.md） ----------
 * 质疑对象：docs/qemu-testing.md 旧结论"EPS1 不可访问 → rfe 无法可控恢复"。
 * 实验手法：记录恢复点 PC 后执行 ill；异常分发（exception.c）识别
 * 哨兵后把 EPC1 设为恢复点、把命令行指定的候选 SR 设为异常前健康 PS，
 * 走既有 RESTORE_CALL0_REGS+rfe 返回。候选：161（标准 EPS1 编号；
 * binutils 未导出 eps1 名但数字可用）与 194（binutils 命名为 eps2；
 * 193 在本配置非有效 SR，汇编器拒收）。 */

volatile int g_e2_active;
volatile uint32_t g_e2_resume_pc;
volatile uint32_t g_e2_ps_before;
volatile uint32_t g_e2_result;
volatile uint32_t g_e2_eps_sr;

static void cmd_rfetest(const char *arg)
{
    unsigned sr = parse_dec(arg, 161u);
    uint32_t ps_before;
    asm volatile("rsr.ps %0" : "=r"(ps_before));
    g_e2_ps_before = ps_before;
    g_e2_result = 0xA11CE2E2u; /* 哨兵：未被恢复点改写即失败 */
    uart_puts("[POTATO][E2] rfe test: ill -> handler sets EPC1/EPS -> rfe\r\n");
    potato_printf("[POTATO][E2] candidate EPS sr=%u ps_before=0x%x\r\n",
                  sr, ps_before);
    g_e2_eps_sr = sr;
    asm volatile(
        "movi    a8, 1f\n\t"      /* 恢复点标签地址 */
        "s32i    a8, %0, 0\n\t"
        "movi.n  a9, 1\n\t"
        "s32i    a9, %1, 0\n\t"   /* g_e2_active = 1 */
        "ill\n"                   /* 触发通用异常（Kernel 向量） */
        "1:\n\t"                  /* rfe 成功则回到这里 */
        "movi.n  a9, 7\n\t"
        "s32i    a9, %2, 0\n\t"   /* g_e2_result = 7 */
        :
        : "r"(&g_e2_resume_pc), "r"(&g_e2_active), "r"(&g_e2_result)
        : "a8", "a9", "memory");
    if (g_e2_result == 7u) {
        uint32_t ps_now;
        asm volatile("rsr.ps %0" : "=r"(ps_now));
        potato_printf("[POTATO][E2] RESUMED at label, ps_before=0x%x "
                      "ps_now=0x%x\r\n", g_e2_ps_before, ps_now);
        if (ps_now == g_e2_ps_before) {
            uart_puts("[POTATO][E2] PASS (rfe restored PS from candidate sr)\r\n");
        } else {
            uart_puts("[POTATO][E2] PARTIAL (resumed but PS not restored)\r\n");
        }
    } else {
        uart_puts("[POTATO][E2] FAIL (resume point not reached)\r\n");
    }
}

/* ---------- E-01 判定实验：loop 指令真相（defect-report.md） ----------
 * 非常量计数循环若编译为 Xtensa 零开销 loop 指令，其正确性依赖
 * LBEG/LEND/LCOUNT 在切换/异常路径的保存恢复。此处成对运行
 * 非 volatile 实现（生成 loop）与 volatile 参照实现（禁 loop），
 * 每轮 yield 一次覆盖切换路径，结果不一致即说明 loop 不可用。 */

/* ---------- E-01 判定实验：loop 指令真相（defect-report.md） ----------
 * 手写零开销循环（loopnez），与编译器为 cmd_fs/msgdemo 生成的 loop
 * 指令同一机制：循环体执行期间 LBEG/LEND/LCOUNT 依赖
 *   - 上下文切换路径（switch.S 保存/恢复 loop 寄存器）
 *   - UART level-2 中断路径（vectors.S SAVE/RESTORE_CALL0_REGS）
 * 任一路径恢复有误，结果即错。与 volatile 参照实现逐轮比对。
 * （编译器只为"常量上界+双条件扫描"形态发 loop，此处手写以保证覆盖） */

static uint32_t loopy_asm_sum(uint32_t n)
{
    uint32_t sum, i;
    /* lbeg/lend/lcount 对编译器透明（call0 ABI：编译器自生成 loop 指令时
     * 同样不建模这三个寄存器，中断/切换路径负责保存恢复），故无需 clobber */
    asm volatile(
        "movi.n  %[i], 0\n\t"
        "movi.n  %[sum], 0\n\t"
        "loopnez %[n], 2f\n"
        "1:\n\t"
        "add.n   %[sum], %[sum], %[i]\n\t"
        "addi.n  %[i], %[i], 1\n\t"
        "2:\n\t"
        : [sum] "=&r"(sum), [i] "=&r"(i)
        : [n] "r"(n));
    return sum;
}

/* volatile 参照实现：不走 loop 指令，逐次访存计数 */
static uint32_t loopy_sum_ref(uint32_t n)
{
    uint32_t sum = 0;
    for (volatile uint32_t i = 0; i < n; i++) {
        sum += i;
    }
    return sum;
}

/* ---------- D-02 回归探针：任务参数经切换链路送达 ----------
 * 首启蹦床从帧槽 +8 取 a3；历史上 pot_do_resume 曾借 a3 作暂存导致
 * 任务收到的是 PS 值。此处创建一个真正解引用参数的探测任务。 */

static const uint32_t g_arg_magic = 0xC0FFEE42u;
static volatile uint32_t g_arg_recv;
static pot_task_t g_arg_tcb;
static uint32_t g_arg_stack[256];

static void arg_probe_task(void *arg)
{
    g_arg_recv = *(const uint32_t *)arg; /* 解引用参数（PS 值当指针用会现形） */
    pot_task_exit();
}

static void cmd_argtest(void)
{
    g_arg_recv = 0;
    if (pot_task_create(&g_arg_tcb, "arg", POTCLS_UI, arg_probe_task,
                        (void *)&g_arg_magic, g_arg_stack,
                        sizeof(g_arg_stack) / sizeof(uint32_t)) != 0) {
        uart_puts("[POTATO][ARG] create failed\r\n");
        return;
    }
    for (volatile int i = 0; i < 200 && g_arg_recv == 0; i++) {
        pot_task_sleep(1); /* 等探测任务跑到（检查点调度） */
    }
    potato_printf("[POTATO][ARG] recv=0x%x expect=0x%x\r\n",
                  g_arg_recv, g_arg_magic);
    if (g_arg_recv == g_arg_magic) {
        uart_puts("[POTATO][ARG] PASS\r\n");
    } else {
        uart_puts("[POTATO][ARG] FAIL\r\n");
    }
}

static void cmd_looptest(const char *arg)
{
    unsigned rounds = parse_dec(arg, 200u);
    uint32_t mism = 0;
    potato_printf("[POTATO][E1] looptest start rounds=%u\r\n", rounds);
    for (volatile unsigned r = 0; r < rounds; r++) {
        uint32_t n = 5000u + (r % 50u) * 100u; /* 长循环：中途大概率被 RX IRQ 打断 */
        uint32_t got = loopy_asm_sum(n);
        uint32_t exp = loopy_sum_ref(n);
        if (got != exp || exp != n * (n - 1u) / 2u) {
            mism++;
        }
        pot_task_yield(); /* 每轮切换一次：覆盖多次切换后的 loop 状态 */
    }
    potato_printf("[POTATO][E1] looptest done rounds=%u mismatches=%u\r\n",
                  rounds, mism);
}

/* ---------- E-03 判定实验：MMU 未映射表项语义（defect-report.md） ----------
 * boot.S 只写表项 0.._irom_pages-1，其余保持复位值。判定问题：
 * 复位值 0 是"invalid"（访问即 fault）还是"有效映射物理页 0"
 * （野指针静默读到引导镜像字节）？另取证 boot.S 32 位步进写表
 * 是否把 entry[1] 顺手写成 0。 */

static void cmd_mmutest(const char *arg)
{
    volatile uint16_t *mmu = (volatile uint16_t *)0x600C5000u;
    potato_printf("[POTATO][E3] mmu[0]=0x%x [1]=0x%x [2]=0x%x [3]=0x%x "
                  "[32]=0x%x [511]=0x%x\r\n",
                  mmu[0], mmu[1], mmu[2], mmu[3], mmu[32], mmu[511]);
    if (strcmp(arg, "load") == 0) {
        /* 0x42020000 = 虚拟页 32：boot.S 从未写过该表项 */
        volatile uint32_t *p = (volatile uint32_t *)0x42020000u;
        uint32_t v = *p;
        potato_printf("[POTATO][E3] load 0x42020000 -> 0x%x (no fault)\r\n", v);
        return;
    }
    if (strcmp(arg, "jump") == 0) {
        uart_puts("[POTATO][E3] jumping to 0x42020000 ...\r\n"); /* TX 轮询逐字节排空，返回即基本发完 */
        uint32_t target = 0x42020000u;
        asm volatile("jx %0" :: "r"(target) : "memory");
        uart_puts("[POTATO][E3] returned?? (unexpected)\r\n");
        return;
    }
    uart_puts("[POTATO][E3] mmutest dump|load|jump\r\n");
}

/* ---------- M7：以太网 / 远程 MCP 通道 ---------- */

/* "a.b.c.d" → 主机字节序；1=成功 */
static int parse_ip4(const char *s, uint32_t *out)
{
    uint32_t v = 0;
    int parts = 0;
    while (parts < 4) {
        if (*s < '0' || *s > '9') {
            return 0;
        }
        uint32_t n = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9' && digits < 3) {
            n = n * 10u + (uint32_t)(*s - '0');
            s++;
            digits++;
        }
        if (n > 255u) {
            return 0;
        }
        v = (v << 8) | n;
        parts++;
        if (parts == 4) {
            break;
        }
        if (*s != '.') {
            return 0;
        }
        s++;
    }
    if (*s != '\0' && *s != ' ') {
        return 0;
    }
    *out = v;
    return 1;
}

static void cmd_net(const char *arg)
{
    /* net | net arp | net link down|up | net ip <a.b.c.d> [gw] */
    char sub[10];
    const char *rest = split_word(arg, sub, (int)sizeof(sub));

    if (sub[0] == '\0') {
        net_stats_dump();
        return;
    }
    if (strcmp(sub, "arp") == 0) {
        if (net_arp_count() == 0) {
            uart_puts("[POTATO][NET] arp empty\r\n");
        }
        net_arp_dump();
        return;
    }
    if (strcmp(sub, "link") == 0) {
        char w[10];
        split_word(rest, w, (int)sizeof(w));
        if (strcmp(w, "down") == 0) {
            net_sim_link(0);
        } else if (strcmp(w, "up") == 0) {
            net_sim_link(1);
        } else {
            uart_puts("[POTATO][NET] usage: net link down|up\r\n");
        }
        return;
    }
    if (strcmp(sub, "ip") == 0) {
        char w[18];
        const char *r2 = split_word(rest, w, (int)sizeof(w));
        uint32_t ip = 0;
        if (!parse_ip4(w, &ip)) {
            uart_puts("[POTATO][NET] usage: net ip <a.b.c.d> [gw]\r\n");
            return;
        }
        char g[18];
        split_word(r2, g, (int)sizeof(g));
        uint32_t gw = 0;
        if (g[0] != '\0' && !parse_ip4(g, &gw)) {
            gw = 0;
        }
        net_set_ip(ip, gw);
        char b1[16];
        char b2[16];
        net_ip_str(net_ip(), b1, sizeof(b1));
        net_ip_str(net_gw(), b2, sizeof(b2));
        potato_printf("[POTATO][NET] ip=%s gw=%s\r\n", b1, b2);
        return;
    }
    uart_puts("[POTATO][NET] usage: net [arp|link down|up|ip <a.b.c.d> [gw]]\r\n");
}

static void cmd_ping(const char *arg)
{
    uint32_t ip = 0;
    if (!parse_ip4(arg, &ip)) {
        uart_puts("[POTATO][NET] usage: ping <a.b.c.d>\r\n");
        return;
    }
    char b[16];
    net_ip_str(ip, b, sizeof(b));
    uint32_t rtt = 0;
    int r = net_ping(ip, 1000u, &rtt);
    potato_printf("[POTATO][NET] ping %s %s rtt=%ums\r\n", b, r == 0 ? "ok" : "fail",
                  (unsigned)rtt);
}

static void cmd_netmcp(const char *arg)
{
    /* netmcp | netmcp dump | netmcp port <p> | netmcp peer <ip> <port> | test */
    char sub[10];
    const char *rest = split_word(arg, sub, (int)sizeof(sub));

    if (sub[0] == '\0' || strcmp(sub, "dump") == 0) {
        netmcp_dump();
        return;
    }
    if (strcmp(sub, "test") == 0) {
        int r = netmcp_selftest();
        potato_printf("[POTATO][NETMCP] selftest %s\r\n", r == 0 ? "ok" : "FAIL");
        return;
    }
    if (strcmp(sub, "port") == 0) {
        char w[12];
        split_word(rest, w, (int)sizeof(w));
        netmcp_set_udp_port((uint16_t)parse_dec(w, 0));
        potato_printf("[POTATO][NETMCP] local port=%d\r\n", netmcp_udp_port());
        return;
    }
    if (strcmp(sub, "peer") == 0) {
        char w[18];
        const char *r2 = split_word(rest, w, (int)sizeof(w));
        uint32_t ip = 0;
        if (!parse_ip4(w, &ip)) {
            uart_puts("[POTATO][NETMCP] usage: netmcp peer <a.b.c.d> <port>\r\n");
            return;
        }
        char p[12];
        split_word(r2, p, (int)sizeof(p));
        netmcp_set_peer(ip, (uint16_t)parse_dec(p, 0));
        return;
    }
    uart_puts("[POTATO][NETMCP] usage: netmcp [dump|test|port <p>|"
              "peer <a.b.c.d> <port>]\r\n");
}

static void cmd_dispatch(char *line)
{
    /* MCP 传输路由：'{' 开头的行投递消息基座（M6） */
    if (line[0] == '{') {
        int r = mcp_submit_line(line, (uint32_t)strlen(line));
        if (r == 0) {
            uart_puts("[POTATO][MCP] submitted\r\n");
        } else if (r == 2) {
            uart_puts("{\"jsonrpc\":\"2.0\",\"id\":null,"
                      "\"error\":{\"code\":-32600,"
                      "\"message\":\"request too long (max 256)\"}}\r\n");
        } else {
            uart_puts("[POTATO][MCP] channel full\r\n");
        }
        return;
    }

    /* 就地把首词和参数拆开（volatile 索引禁 loop 指令，见 lib_shim.c） */
    volatile int k = 0;
    while (line[k] != '\0' && line[k] != ' ') {
        k++;
    }
    char *arg = line + k;
    if (*arg == ' ') {
        *arg = '\0';
        arg++;
        volatile int s = 0;
        while (arg[s] == ' ') {
            s++;
        }
        arg += s;
    }

    if (strcmp(line, "") == 0) {
        return;
    }
    if (strcmp(line, "help") == 0) {
        cmd_help();
        return;
    }
    if (strcmp(line, "ping") == 0) {
        /* 无参：M1 存活探针（回归锚点，勿改）；带参：M7 ICMP echo */
        if (arg[0] == '\0') {
            uart_puts("pong\r\n");
        } else {
            cmd_ping(arg);
        }
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
    if (strcmp(line, "msgdemo") == 0) {
        cmd_msgdemo(arg);
        return;
    }
    if (strcmp(line, "msgstat") == 0) {
        msgdemo_stat();
        return;
    }
    if (strcmp(line, "memstat") == 0) {
        cmd_memstat();
        return;
    }
    if (strcmp(line, "memtest") == 0) {
        cmd_memtest(arg);
        return;
    }
    if (strcmp(line, "pal") == 0) {
        cmd_pal();
        return;
    }
    if (strcmp(line, "drivers") == 0) {
        cmd_drivers();
        return;
    }
    if (strcmp(line, "fs") == 0) {
        cmd_fs(arg);
        return;
    }
    if (strcmp(line, "mcp") == 0) {
        cmd_mcp(arg);
        return;
    }
    if (strcmp(line, "crash") == 0) {
        cmd_crash();
        return;
    }
    if (strcmp(line, "looptest") == 0) {
        cmd_looptest(arg);
        return;
    }
    if (strcmp(line, "rfetest") == 0) {
        cmd_rfetest(arg);
        return;
    }
    if (strcmp(line, "mmutest") == 0) {
        cmd_mmutest(arg);
        return;
    }
    if (strcmp(line, "argtest") == 0) {
        cmd_argtest();
        return;
    }
    if (strcmp(line, "preempt") == 0) {
        cmd_preempt(arg);
        return;
    }
    if (strcmp(line, "health") == 0) {
        cmd_health();
        return;
    }
    if (strcmp(line, "obs") == 0) {
        cmd_obs(arg);
        return;
    }
    if (strcmp(line, "inject") == 0) {
        cmd_inject(arg);
        return;
    }
    if (strcmp(line, "net") == 0) {
        cmd_net(arg);
        return;
    }
    if (strcmp(line, "netmcp") == 0) {
        cmd_netmcp(arg);
        return;
    }
    if (strcmp(line, "smp") == 0) {
        cmd_smp(arg);
        return;
    }
    potato_printf("[POTATO] unknown command: %s (try 'help')\r\n", line);
}

/* ---------- M7 串口 MCP：帧接收（STX 引导，单行） ----------
 * 真机远程控制面走串口时，host 先发 0x02(STX) 再发 "@@MCP <crc> <json>\r\n"。
 * STX 把「人类命令行」与「机器帧」彻底分开：帧模式不回显、不派发本地命令、
 * 不受 CMD_BUF_LEN=264 限制（独立缓冲）。坏帧（超长/前缀/CRC/JSON）由
 * netmcp_serial_rx 静默丢弃并计数、不回包，等对端重发；半截帧超时后复位
 * 接收状态，避免吞掉后续本地命令。 */
#define MCP_SER_FRAME_MAX 1600u
static void console_mcp_frame(void)
{
    static char fr[MCP_SER_FRAME_MAX + 2u];
    uint32_t n = 0;
    int over = 0;
    uint32_t last = pot_sched_now();

    /* 进入帧模式前先收掉人类提示符半行（"potato> "）：否则输出抽头会把
     * 响应行拼在提示符后面（行首不是 '{'）导致镜像/重放判定失败。 */
    if (uart_tx_midline()) {
        uart_putc('\n');
    }

    for (;;) {
        int c = uart_getc();
        if (c < 0) {
            if ((pot_sched_now() - last) > 2000u) {
                potato_printf("[POTATO][MCP] serial frame rx timeout (%u B) "
                              "dropped\r\n", (unsigned)n);
                return;
            }
            netmcp_serial_tick();
            pot_task_sleep(1);
            continue;
        }
        last = pot_sched_now();
        if (c == '\r' || c == '\n') {
            break;
        }
        if (!over) {
            if (n < MCP_SER_FRAME_MAX) {
                fr[n++] = (char)c;
            } else {
                over = 1;
                n = MCP_SER_FRAME_MAX + 1u; /* 哨兵：超上限交给 netmcp 记 too_long */
            }
        }
    }
    if (over) {
        netmcp_serial_rx(fr, MCP_SER_FRAME_MAX + 1u);
        return;
    }
    if (n != 0u) {
        fr[n] = '\0';
        netmcp_serial_rx(fr, n);
    }
}

void console_run(void)
{
    static char buf[CMD_BUF_LEN];

    uart_puts("[POTATO] type 'help' for commands\r\n");
    for (;;) {
        uart_puts("potato> ");
        volatile int n = 0;
        volatile int overlong = 0;
        int framed = 0;
        for (;;) {
            int c = uart_getc();
            if (c < 0) {
                if (netmcp_serial_active()) {
                    netmcp_serial_tick(); /* 真机无 net 任务：hello/重放唯一 tick */
                }
                pot_task_sleep(1); /* 无输入让出 CPU（调度检查点） */
                continue;
            }
            if (c == 0x02) { /* STX：串口 MCP 帧引导（只认纯文本行首） */
                if (netmcp_serial_active()) {
                    n = 0;
                    overlong = 0;
                    console_mcp_frame();
                    framed = 1; /* 帧已处理：回外层直接重打提示符 */
                    break;
                }
                continue;
            }
            if (c == '\r' || c == '\n') {
                uart_putc('\n');
                break;
            }
            if (c == 0x7f || c == '\b') {
                if (n > 0) {
                    n--;
                    overlong = 0; /* 回退后重判超长 */
                    uart_puts("\b \b");
                }
                continue;
            }
            if (c < 0x20) {
                continue;
            }
            if (n < CMD_BUF_LEN - 1) {
                buf[n] = (char)c;
                n++;
                uart_putc((char)c); /* 回显，测试台日志可读 */
            } else {
                overlong = 1; /* 继续吃掉本行剩余字符，收行时统一报错 */
            }
        }
        if (framed) {
            continue;
        }
        if (overlong) {
            potato_printf("[POTATO] line too long (max %d), dropped\r\n",
                          CMD_BUF_LEN - 1);
            continue;
        }
        buf[n] = '\0';
        cmd_dispatch(buf);
    }
}
