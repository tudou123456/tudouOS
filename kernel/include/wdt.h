#ifndef TUDOU_WDT_H
#define TUDOU_WDT_H

#include <stdint.h>

/*
 * 尽早关闭所有看门狗（TIMG0/TIMG1 MWDT + RTC WDT + super WDT）。
 * 会在 boot.S 清 .bss 之前被调用，实现里禁止使用静态变量。
 */
void wdt_early_disable(void);

/*
 * 真机 super watchdog（RTC_CNTL SWD，rst:0x12 SUPER_WDT_RST）。
 * 硬件复位默认使能且不自动喂，ESP-IDF 由 bootloader/HAL 关闭或喂；
 * 我们裸跑必须自己处理：wdt_early_disable() 会调用 wdt_swd_disable()，
 * 若关不掉则 tick 里用 wdt_swd_feed() 兜底。
 */
void wdt_swd_disable(void);
void wdt_swd_feed(void);
uint32_t wdt_swd_conf(void);

#endif /* TUDOU_WDT_H */
