#include <stdint.h>

#include "console.h"
#include "heap.h"
#include "intc.h"
#include "obs.h"
#include "task.h"
#include "time.h"
#include "trace.h"
#include "uart.h"

/*
 * M8 验收配套：故障/负载注入（inject 命令）。
 *
 * 目的：让"AI 从导出日志定位根因"这条验收链路有可控输入。三种模式各自
 * 留下不同的、可从 JSONL + 健康快照客观区分的指纹：
 *
 *   leak [n]   后台任务每 50ms 分配 64B 且永不释放（n=0 直到分配失败）。
 *             指纹：健康快照 heap.used 单调上升、alloc-free_n 差值持续
 *             增大、追踪环 MEM_ALLOC 多于 MEM_FREE。
 *   hang [ms]  在任务态把 INTLEVEL 抬到 1（屏蔽 tick）后自旋 ms 毫秒
 *             （默认 200）。指纹：INJECT_BEGIN/END 之间的 IRQ_ENTER 空洞、
 *             jitmax 突增、后续 tick 补记（t 跳变）。不抬到 2 以上，避免
 *             文档记载的"高 INTLEVEL 期间设备中断源活锁"陷阱。
 *   spin [ms]  以最高类（POTCLS_AI_COMMS）自旋 ms 毫秒（默认 300）不让出。
 *             指纹：该任务 cpu_ticks 陡增、idle/background 任务 switches
 *             停滞、其余任务 last_run_tick 落后。
 *
 * 注入任务为单例（静态 TCB+栈，退出后复用）；off 停止无限模式。
 */

#define INJ_STACK_WORDS 512u
#define INJ_MODE_LEAK 1u
#define INJ_MODE_HANG 2u
#define INJ_MODE_SPIN 3u

static pot_task_t g_inj_tcb;
static uint32_t g_inj_stack[INJ_STACK_WORDS];
static volatile int g_inj_active;
static volatile uint32_t g_inj_mode;
static volatile uint32_t g_inj_arg;
static void *g_leak_keep[128];

int pot_inject_active(void)
{
    return g_inj_active;
}

static void inj_leak_loop(void)
{
    unsigned slot = 0;
    uint32_t blocks = 0;
    while (g_inj_active) {
        if (g_inj_arg != 0u && blocks >= g_inj_arg) {
            break;
        }
        void *p = pot_heap_alloc(64u, 0x1EACu /* tag: leak */);
        if (p == 0) {
            break; /* 堆耗尽：泄漏注入自然收尾 */
        }
        g_leak_keep[slot & 127u] = p; /* 保存引用：确实不可回收 */
        slot++;
        blocks++;
        pot_task_sleep(50u);
    }
    potato_printf("[POTATO][INJ] leak done blocks=%u arg=%u\r\n",
                  blocks, g_inj_arg);
    pot_heap_stat_t hs;
    pot_heap_getstat(&hs);
    potato_printf("[POTATO][INJ] heap used=%u free=%u alloc=%u free_n=%u fail=%u\r\n",
                  hs.used, hs.total - hs.used, hs.n_alloc, hs.n_free, hs.n_fail);
}

static void inj_hang_once(void)
{
    uint32_t ms = (g_inj_arg != 0u) ? g_inj_arg : 200u;
    uint32_t t0 = time_now_us();
    uint32_t ps;
    asm volatile("rsil %0, 1" : "=r"(ps) :: "memory"); /* 只屏蔽 level1(tick) */
    while ((uint32_t)(time_now_us() - t0) < ms * 1000u) {
        /* 自旋：tick 停摆，uart 仍可收（level2 未被屏蔽） */
    }
    asm volatile("wsr.ps %0\n\tesync" :: "r"(ps) : "memory");
    (void)ps;
}

static void inj_spin_once(void)
{
    uint32_t ms = (g_inj_arg != 0u) ? g_inj_arg : 300u;
    uint32_t t0 = time_now_us();
    while ((uint32_t)(time_now_us() - t0) < ms * 1000u) {
        /* 自旋不让出：同优先级以下任务全部挨饿 */
    }
}

static void inj_task(void *arg)
{
    (void)arg;
    uint32_t mode = g_inj_mode;
    trace_push(TR_INJECT_BEGIN, mode, g_inj_arg);
    if (mode == INJ_MODE_LEAK) {
        inj_leak_loop();
    } else if (mode == INJ_MODE_HANG) {
        inj_hang_once();
    } else {
        inj_spin_once();
    }
    trace_push(TR_INJECT_END, mode, g_inj_arg);
    g_inj_active = 0;
    potato_printf("[POTATO][INJ] mode=%u done arg=%u\r\n", mode, g_inj_arg);
    pot_task_exit();
}

void pot_inject_stop(void)
{
    if (g_inj_active) {
        g_inj_active = 0;
        uart_puts("[POTATO][INJ] stop-req\r\n");
    } else {
        uart_puts("[POTATO][INJ] idle\r\n");
    }
}

void pot_inject_cmd(const char *mode, unsigned arg)
{
    /* 首词已在 console 层拆出，这里 mode 为子命令、arg 为参数 */
    if (mode == 0 || mode[0] == '\0' || (mode[0] == 'o' && mode[1] == 'f')) {
        pot_inject_stop();
        return;
    }
    uint32_t m;
    if (mode[0] == 'l') {
        m = INJ_MODE_LEAK;
    } else if (mode[0] == 'h') {
        m = INJ_MODE_HANG;
    } else if (mode[0] == 's') {
        m = INJ_MODE_SPIN;
    } else {
        uart_puts("[POTATO][INJ] unknown mode (leak|hang|spin|off)\r\n");
        return;
    }
    if (g_inj_active) {
        uart_puts("[POTATO][INJ] busy (inject off first)\r\n");
        return;
    }
    g_inj_mode = m;
    g_inj_arg = arg;
    g_inj_active = 1;
    pot_task_class_t cls = (m == INJ_MODE_SPIN) ? POTCLS_AI_COMMS : POTCLS_BACKGROUND;
    int r = pot_task_create(&g_inj_tcb, "inject", cls, inj_task, 0,
                            g_inj_stack, INJ_STACK_WORDS);
    if (r != 0) {
        g_inj_active = 0;
        uart_puts("[POTATO][INJ] task create failed\r\n");
        return;
    }
    potato_printf("[POTATO][INJ] inject %s arg=%u started\r\n", mode, arg);
}
