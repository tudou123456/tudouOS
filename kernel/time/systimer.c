#if 0 /* 保留待真机阶段启用：QEMU 的 esp32s3 systimer 使能中断会把仿真
         wedges 住（乐鑫 fork 实现缺陷），M2 在 QEMU 用 timg_timer.c。
         真机上 systimer（0x60023000, 52bit@16MHz）仍是一等选择。 */

#include <stdint.h>

#include "io.h"
#include "time.h"

/*
 * ESP32-S3 SYSTIMER @ 0x60023000（esp-idf reg_base.h 与乐鑫 QEMU 一致）。
 * 52 位计数器 @16MHz；target0 配周期模式每 1ms 触发，经中断矩阵 int22
 * （level3）进 tick 处理。
 * 偏移依据 esp-idf esp32s3/systimer_reg.h：
 *   CONF +0x00（bit31 CLK_EN, bit30 UNIT0_EN, bit24 TARGET0_EN）
 *   UNIT0_OP +0x04（bit30 UPDATE 锁存, bit29 VALUE_VALID）
 *   TARGET0_HI/LO +0x1C/+0x20（绝对模式目标值）
 *   TARGET0_CONF +0x34（bit31 单元选择, bit30 周期模式, [25:0] 周期）
 *   INT_ENA/RAW/CLR +0x64/+0x68/+0x6C（bit0 = target0）
 *   UNIT0_VALUE_HI/LO +0x40/+0x44
 */

#define SYSTIMER_BASE     0x60023000u
#define SYST_CONF         (SYSTIMER_BASE + 0x00u)
#define SYST_UNIT0_OP     (SYSTIMER_BASE + 0x04u)
#define SYST_T0_HI        (SYSTIMER_BASE + 0x1Cu)
#define SYST_T0_LO        (SYSTIMER_BASE + 0x20u)
#define SYST_T0_CONF      (SYSTIMER_BASE + 0x34u)
#define SYST_UNIT0_HI     (SYSTIMER_BASE + 0x40u)
#define SYST_UNIT0_LO     (SYSTIMER_BASE + 0x44u)
#define SYST_INT_ENA      (SYSTIMER_BASE + 0x64u)
#define SYST_INT_RAW      (SYSTIMER_BASE + 0x68u)
#define SYST_INT_CLR      (SYSTIMER_BASE + 0x6Cu)

#define SYST_CLK_EN          (1u << 31)
#define SYST_UNIT0_EN        (1u << 30)
#define SYST_T0_EN           (1u << 24)
#define SYST_UPDATE          (1u << 30)
#define SYST_VALID           (1u << 29)
#define SYST_T0_PERIOD_MODE  (1u << 30)
#define SYST_T0_PERIOD_MASK  0x03FFFFFFu
#define SYST_T0_INT          (1u << 0)

#define SYSTIMER_CLK_HZ   16000000u
#define TICK_PERIOD_TICKS (SYSTIMER_CLK_HZ / 1000u) /* 16000 tick = 1ms */

/* QEMU 的周期模式（PERIOD_MODE）会在使能中断时把整个仿真 wedges 住
   （乐鑫 fork 的实现缺陷，IDF/Zephyr 均只用绝对目标模式）——用单次重挂 */
#define POT_SYSTIMER_ONE_SHOT 1
/* 诊断开关：0 = 不置 INT_ENA（定时器照跑，中断不使能） */
#define POT_SYSTIMER_INT_ENA 1

static volatile uint32_t g_ticks;
static volatile uint32_t g_jit_min = 0xFFFFFFFFu;
static volatile uint32_t g_jit_max;
static uint32_t g_last_lo;

#if POT_SYSTIMER_ONE_SHOT
static void t0_arm_next(void)
{
    uint32_t lo = mmio_read(SYST_UNIT0_LO) + TICK_PERIOD_TICKS;
    mmio_write(SYST_T0_LO, lo);
    mmio_write(SYST_T0_HI, 0);
}
#endif

void time_init(void)
{
    mmio_write(SYST_T0_CONF, 0); /* 先停比较器再配置 */

    uint32_t conf = mmio_read(SYST_CONF) | SYST_CLK_EN | SYST_UNIT0_EN | SYST_T0_EN;
    mmio_write(SYST_CONF, conf);

#if POT_SYSTIMER_ONE_SHOT
    mmio_write(SYST_T0_CONF, 0); /* 绝对目标模式，单元 0 */
    t0_arm_next();
#else
    /* 周期模式：每 TICK_PERIOD_TICKS 触发一次 target0 */
    mmio_write(SYST_T0_CONF, SYST_T0_PERIOD_MODE |
                 (TICK_PERIOD_TICKS & SYST_T0_PERIOD_MASK));
#endif

    mmio_write(SYST_INT_CLR, SYST_T0_INT);
    if (POT_SYSTIMER_INT_ENA) {
        mmio_write(SYST_INT_ENA, SYST_T0_INT);
    }

    g_last_lo = mmio_read(SYST_UNIT0_LO);
}

uint32_t time_ticks_raw(void)
{
    return mmio_read(SYST_UNIT0_LO);
}

uint32_t time_now_us(void)
{
    mmio_write(SYST_UNIT0_OP, SYST_UPDATE); /* 锁存一致快照 */
    while ((mmio_read(SYST_UNIT0_OP) & SYST_VALID) == 0u) {
    }
    uint32_t lo = mmio_read(SYST_UNIT0_LO);
    uint32_t hi = mmio_read(SYST_UNIT0_HI) & 0x0000000Fu; /* 36 位 us 足够 */
    return (lo >> 4) | (hi << 28);
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

void systimer_irq_handler(void)
{
    if ((mmio_read(SYST_INT_RAW) & SYST_T0_INT) != 0u) {
        mmio_write(SYST_INT_CLR, SYST_T0_INT);
#if POT_SYSTIMER_ONE_SHOT
        t0_arm_next();
#endif
        g_ticks++;

        uint32_t now = mmio_read(SYST_UNIT0_LO);
        uint32_t delta_us = (now - g_last_lo) >> 4;
        g_last_lo = now;
        if (g_ticks > 2u) { /* 跳过首拍（含初始化间隔） */
            if (delta_us < g_jit_min) {
                g_jit_min = delta_us;
            }
            if (delta_us > g_jit_max) {
                g_jit_max = delta_us;
            }
        }
    }
}

#endif /* 真机阶段再启用 */
