#include <stdint.h>

#include "pal.h"
#include "tick.h"

/*
 * PAL qemu 后端（乐鑫 QEMU esp32s3）。
 * 真机后端（hw）：同接口，时基切中断驱动——真机阶段接入
 * （boot.S 的二级映射引导与 MMU 序列双端通用）。
 */

#if TUDOU_PLATFORM == 0 /* qemu */

const char *pot_pal_name(void)
{
    return "qemu-esp32s3";
}

pot_platform_t pot_pal_platform(void)
{
    return POT_PAL_QEMU;
}

pot_tick_mode_t pot_pal_tick_mode(void)
{
    return POT_TICK_POLL; /* QEMU 定时器/中断仿真缺陷规避（见 docs） */
}

uint32_t pot_pal_cpu_hz(void)
{
    return tick_cpu_hz();
}

void pot_pal_early_init(void)
{
    /* QEMU 侧 C 级初始化已在各模块完成，此处为接口完整性 */
}

#else /* hw */

const char *pot_pal_name(void)
{
    return "hw-esp32s3";
}

pot_platform_t pot_pal_platform(void)
{
    return POT_PAL_HW;
}

pot_tick_mode_t pot_pal_tick_mode(void)
{
    return POT_TICK_IRQ; /* 真机恢复中断驱动（EPS1 可用，M3 设计已留路径） */
}

uint32_t pot_pal_cpu_hz(void)
{
    return tick_cpu_hz(); /* D-26：真机=运行时标定值（与 tick 周期同源） */
}

void pot_pal_early_init(void)
{
    /* 真机 C 级初始化：真机阶段接入 */
}

#endif
