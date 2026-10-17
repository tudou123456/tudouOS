#ifndef TUDOU_TIME_H
#define TUDOU_TIME_H

#include <stdint.h>

/*
 * 土豆内核时间服务：
 *  - tick：核内 CCOMPARE0 1kHz（ccompare_tick.c，纯 Xtensa 核路径）
 *  - 时基 time_now_us：
 *      POT_TIME_TIMG=1 → TIMG0 计数器（1MHz，无中断）——真机/WSL Linux QEMU 用
 *      POT_TIME_TIMG=0 → ccount 推算（固定编译期 CPU 频率）——Windows QEMU 用
 *        （乐鑫 QEMU Windows 构建 bug：TIMG0 计数器一启用（T0_CONFIG 置 EN）
 *          约 1s 内进程崩溃 0xC0000409，任何中断使能与否无关；详见
 *          docs/qemu-testing.md 的已知问题节）
 */

#define POT_TIME_TIMG 0

void time_source_init(void);
uint32_t time_now_us(void);
uint32_t time_ticks_raw(void);

uint32_t time_tick_count(void);
uint32_t time_tick_jitter_min_us(void);
uint32_t time_tick_jitter_max_us(void);
/* M8/D-08：看门狗自愈路径（int6 丢失补偿）补记的 tick 累计次数 */
uint32_t time_tick_watchdog_count(void);

#endif /* TUDOU_TIME_H */
