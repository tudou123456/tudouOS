/*
 * 启动阶段标记（真机带板调试专用）。
 *
 * 背景：真机上内核卡死时串口可能还没有任何输出（横幅在 init 末尾才打印），
 * 无法判断卡在哪个初始化步骤。本文件提供 pot_boot_mark(c)：绕过 uart 驱动
 * 直接写 UART0 THR（ROM 已把 UART0 初始化为可打印状态），在 kernel_main 的
 * 每个初始化步骤之间打一个字符，串口最后的字符即卡点。
 *
 * 仅在 TUDOU_BOOT_TRACE=1 构建时真正输出（tools/build.py 读取同名环境变量，
 * 见 BOOT_TRACE_VAL）；默认构建为空操作，不影响 QEMU 回归与产物行为。
 */
#include <stdint.h>

#ifndef TUDOU_BOOT_TRACE
#define TUDOU_BOOT_TRACE 0
#endif

#define UART0_THR_REG  (*(volatile uint32_t *)0x60000000u) /* FIFO 写口 */
#define UART0_STAT_REG (*(volatile uint32_t *)0x6000001cu) /* bit[21:16]=TXFIFO_CNT */

void pot_boot_mark(char c);

void pot_boot_mark(char c)
{
#if TUDOU_BOOT_TRACE
    while (((UART0_STAT_REG >> 16) & 0x3fu) != 0u) {
        /* 等 TX FIFO 有空位（与 boot.S 的 BOOTCH 一致） */
    }
    UART0_THR_REG = (uint32_t)(unsigned char)c;
#else
    (void)c;
#endif
}
