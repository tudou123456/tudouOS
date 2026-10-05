#ifndef TUDOU_WDT_H
#define TUDOU_WDT_H

/*
 * 尽早关闭所有看门狗（TIMG0/TIMG1 MWDT + RTC WDT）。
 * 会在 boot.S 清 .bss 之前被调用，实现里禁止使用静态变量。
 */
void wdt_early_disable(void);

#endif /* TUDOU_WDT_H */
