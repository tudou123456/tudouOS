#if POT_TIME_TIMG /* 真机/WSL 路线：TIMG0 1MHz 被动时基（QEMU Windows 崩，禁用） */

#include <stdint.h>

#include "io.h"
#include "time.h"

/*
 * 被动微秒时基 — ESP32-S3 TIMG0 定时器0 计数器（1MHz）。
 * 只读计数，不使能其任何中断。
 * APB 80MHz / (79+1) = 1MHz；T0UPDATE 锁存后读 T0LO。
 * 偏移依据 esp-idf esp32s3/timer_group_reg.h。
 */

#define TIMG0_BASE       0x6001F000u
#define T0_CONFIG        (TIMG0_BASE + 0x00u)
#define T0_LO            (TIMG0_BASE + 0x04u)
#define T0_UPDATE        (TIMG0_BASE + 0x0Cu)
#define T0_ALARM_LO      (TIMG0_BASE + 0x10u)
#define T0_ALARM_HI      (TIMG0_BASE + 0x14u)

#define T0_EN            (1u << 31)
#define T0_INCREASE      (1u << 30)
#define T0_UPDATE_BIT    (1u << 31) /* TIMG_T0UPDATE：写 bit31 触发锁存（P2-4） */
#define T0_DIV_S         13u
#define T0_DIV_VAL       79u /* 80MHz/(79+1) = 1MHz */

void time_source_init(void)
{
    mmio_write(T0_CONFIG, 0); /* 先停 */
    mmio_write(T0_ALARM_LO, 0xFFFFFFFFu); /* 告警永不触发（不用它） */
    mmio_write(T0_ALARM_HI, 0x7FFFFFFFu);
    mmio_write(T0_CONFIG, T0_EN | T0_INCREASE |
                 (T0_DIV_VAL << T0_DIV_S)); /* 不开 ALARM_EN */
    mmio_write(T0_UPDATE, T0_UPDATE_BIT);
}

uint32_t time_now_us(void)
{
    mmio_write(T0_UPDATE, T0_UPDATE_BIT); /* 锁存后读，保证一致 */
    return mmio_read(T0_LO);
}

uint32_t time_ticks_raw(void)
{
    mmio_write(T0_UPDATE, T0_UPDATE_BIT);
    return mmio_read(T0_LO);
}

#endif /* POT_TIME_TIMG */
