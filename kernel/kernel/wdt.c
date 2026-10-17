#include <stdint.h>

#include "io.h"
#include "wdt.h"

/*
 * ESP32-S3 看门狗寄存器（偏移依据 esp-idf esp32s3 register 头文件）：
 *   TIMG0 0x6001F000 / TIMG1 0x60020000
 *     WDTCONFIG0  +0x48（bit31 WDT_EN）   WDTWPROTECT +0x64
 *   RTC_CNTL 0x60008000
 *     WDTCONFIG0  +0x98（bit31 WDT_EN）   WDTWPROTECT +0xB0
 *   写保护键值 0x50D83AA1（TIMG 与 RTC 相同）
 */

#define TIMG0_BASE           0x6001F000u
#define TIMG1_BASE           0x60020000u
#define TIMG_WDTCONFIG0_OFF  0x48u
#define TIMG_WDTWPROTECT_OFF 0x64u

#define RTC_CNTL_BASE           0x60008000u
#define RTC_CNTL_WDTCONFIG0_OFF 0x98u
#define RTC_CNTL_WDTWPROTECT_OFF 0xB0u

#define WDT_WKEY 0x50D83AA1u

#define RTC_CNTL_SWD_CONF_OFF     0xB4u
#define RTC_CNTL_SWD_WPROTECT_OFF 0xB8u
#define SWD_WKEY              0x8F1D312Au
#define SWD_CONF_DISABLE      (1u << 30)
#define SWD_CONF_FEED         (1u << 29)
#define SWD_CONF_RST_FLAG_CLR (1u << 28)
#define SWD_CONF_AUTO_FEED_EN (1u << 31)

static void timg_wdt_disable(uint32_t timg_base)
{
    mmio_write(timg_base + TIMG_WDTWPROTECT_OFF, WDT_WKEY);
    mmio_write(timg_base + TIMG_WDTCONFIG0_OFF, 0u);
    mmio_write(timg_base + TIMG_WDTWPROTECT_OFF, 0u);
}

void wdt_swd_disable(void)
{
    uint32_t v = mmio_read(RTC_CNTL_BASE + RTC_CNTL_SWD_CONF_OFF);
    mmio_write(RTC_CNTL_BASE + RTC_CNTL_SWD_WPROTECT_OFF, SWD_WKEY);
    mmio_write(RTC_CNTL_BASE + RTC_CNTL_SWD_CONF_OFF,
               v | SWD_CONF_DISABLE | SWD_CONF_RST_FLAG_CLR);
    mmio_write(RTC_CNTL_BASE + RTC_CNTL_SWD_WPROTECT_OFF, 0u);
}

uint32_t wdt_swd_conf(void)
{
    return mmio_read(RTC_CNTL_BASE + RTC_CNTL_SWD_CONF_OFF);
}

/* 关不掉时的兜底：tick 里周期喂。已关闭/自动喂时是 1 次只读，几乎零开销 */
void wdt_swd_feed(void)
{
    uint32_t v = mmio_read(RTC_CNTL_BASE + RTC_CNTL_SWD_CONF_OFF);
    if ((v & SWD_CONF_DISABLE) != 0u || (v & SWD_CONF_AUTO_FEED_EN) != 0u) {
        return;
    }
    mmio_write(RTC_CNTL_BASE + RTC_CNTL_SWD_WPROTECT_OFF, SWD_WKEY);
    mmio_write(RTC_CNTL_BASE + RTC_CNTL_SWD_CONF_OFF,
               v | SWD_CONF_FEED | SWD_CONF_RST_FLAG_CLR);
    mmio_write(RTC_CNTL_BASE + RTC_CNTL_SWD_WPROTECT_OFF, 0u);
}

void wdt_early_disable(void)
{
    timg_wdt_disable(TIMG0_BASE);
    timg_wdt_disable(TIMG1_BASE);

    mmio_write(RTC_CNTL_BASE + RTC_CNTL_WDTWPROTECT_OFF, WDT_WKEY);
    mmio_write(RTC_CNTL_BASE + RTC_CNTL_WDTCONFIG0_OFF, 0u);
    mmio_write(RTC_CNTL_BASE + RTC_CNTL_WDTWPROTECT_OFF, 0u);

    wdt_swd_disable(); /* 真机 super watchdog：默认使能，必须显式关 */
}

