#include <stdint.h>

#include "io.h"
#include "intc.h"
#include "tick.h"
#include "time.h"

/*
 * 内核 tick — Xtensa 核内 CCOMPARE0（中断 6，level-1，经通用异常
 * EXCCAUSE=4 路径投递，由 vectors.S 的 genex 入口分发，RFE 返回）。
 * 纯核内定时器：不经过乐鑫 QEMU 的任何外设中断/计数器模型，稳定可靠。
 *
 * ccount 频率取编译期常量（QEMU 复位默认时钟下 CPU=40MHz XTAL；
 * 真机/其它时钟配置在 M5 时钟驱动落地后改为运行时标定）。
 * 注意：本文件避免对运行期变量做除法（无硬件除法且不链 libgcc）。
 */

#define POT_CPU_HZ   40000000u
#define TICK_HZ      1000u
#define CCOMPARE0_INT 6

#define TICK_PERIOD_CC   (POT_CPU_HZ / TICK_HZ)      /* 40000 */
#define CC_PER_US        (POT_CPU_HZ / 1000000u)     /* 40 */

static volatile uint32_t g_ticks;
static volatile uint32_t g_last_elapsed;
static volatile uint32_t g_jit_min = 0xFFFFFFFFu;
static volatile uint32_t g_jit_max;
static uint32_t g_next_cc;
static uint32_t g_last_fire_cc;

#if !POT_TIME_TIMG /* Windows QEMU 路线：ccount 兼任微秒时基 */
static uint32_t g_boot_cc;
#endif

static inline uint32_t ccount(void)
{
    uint32_t v;
    asm volatile("rsr.ccount %0" : "=r"(v));
    return v;
}

static inline void set_ccompare0(uint32_t v)
{
    asm volatile("wsr.ccompare0 %0\n\t"
                 "esync"
                 :: "r"(v) : "memory");
}

void time_source_init(void)
{
#if !POT_TIME_TIMG
    g_boot_cc = ccount();
#endif
}

#if !POT_TIME_TIMG /* TIMG 路线时由 timg_timer.c 提供，避免重复定义 */
uint32_t time_now_us(void)
{
    return (ccount() - g_boot_cc) / CC_PER_US; /* 除数为编译期常量 */
}

uint32_t time_ticks_raw(void)
{
    return ccount();
}
#endif

void tick_init(void)
{
    g_last_fire_cc = ccount();
    g_next_cc = g_last_fire_cc + TICK_PERIOD_CC;
    /* v4：不使能 int6——调度核心去中断化（该 QEMU 的异常×手动退出
       交互存在不可修复的竞态），tick 由 pot_tick_poll() 在任务检查点
       与空闲循环内联处理；真机阶段再恢复中断驱动。 */
}

/* 轮询推进 tick：到点则补记并驱动调度器记账/唤醒。
 * 在任务级上下文调用（yield/sleep 入口、空闲循环）。 */
void pot_tick_poll(void)
{
    uint32_t now = ccount();
    while ((int32_t)(now - g_next_cc) >= 0) {
        uint32_t elapsed = (now - (g_next_cc - TICK_PERIOD_CC)) / TICK_PERIOD_CC;
        if (elapsed == 0u) {
            elapsed = 1u;
        }
        g_ticks += elapsed;
        g_last_elapsed = elapsed;

        g_next_cc += elapsed * TICK_PERIOD_CC;
        if ((int32_t)(g_next_cc - now) < 0) {
            g_next_cc = now + TICK_PERIOD_CC;
        }

        /* 触发间隔统计（真实观测） */
        uint32_t delta_us = (now - g_last_fire_cc) / CC_PER_US;
        g_last_fire_cc = now;
        if (g_ticks > 2u) {
            if (delta_us < g_jit_min) {
                g_jit_min = delta_us;
            }
            if (delta_us > g_jit_max) {
                g_jit_max = delta_us;
            }
        }

        extern void pot_sched_tick(uint32_t);
        pot_sched_tick(elapsed);
    }
}

uint32_t tick_cpu_hz(void)
{
    return POT_CPU_HZ;
}

uint32_t tick_last_elapsed(void)
{
    return g_last_elapsed;
}

uint32_t time_tick_count(void)
{
    return g_ticks;
}

uint32_t time_tick_jitter_min_us(void)
{
    return g_jit_min;
}

uint32_t time_tick_jitter_max_us(void)
{
    return g_jit_max;
}

void tick_irq_handler(void)
{
    uint32_t now = ccount();

    /* QEMU 的 ccount 按翻译块大步推进，可能整段跳过比较点（实测触发率
       ~53%）。按实际跨过的周期数补记，保证 tick 计数与真实时间对齐；
       jitter 统计按真实触发间隔记录（这正是要观测的可观测性数据）。 */
    uint32_t elapsed = (now - (g_next_cc - TICK_PERIOD_CC)) / TICK_PERIOD_CC;
    if (elapsed == 0u) {
        elapsed = 1u;
    }
    g_ticks += elapsed;
    g_last_elapsed = elapsed;

    g_next_cc += elapsed * TICK_PERIOD_CC;
    if ((int32_t)(g_next_cc - now) < 0) {
        g_next_cc = now + TICK_PERIOD_CC; /* 落后过多则重新对齐 */
    }
    set_ccompare0(g_next_cc); /* 写 CCOMPARE 顺带清除 int6 挂起 */

    uint32_t delta_us = (now - g_last_fire_cc) / CC_PER_US;
    g_last_fire_cc = now;
    if (g_ticks > 2u) {
        if (delta_us < g_jit_min) {
            g_jit_min = delta_us;
        }
        if (delta_us > g_jit_max) {
            g_jit_max = delta_us;
        }
    }
}
