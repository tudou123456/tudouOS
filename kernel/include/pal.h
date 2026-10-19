#ifndef TUDOU_PAL_H
#define TUDOU_PAL_H

#include <stdint.h>

/*
 * 平台抽象层（M5 成形，支柱2：仿真/硬件双端同源）
 *
 * 一份内核源码，两个平台后端：
 *   qemu —— 乐鑫 QEMU esp32s3 机器（开发/CI 主战场）
 *   hw   —— ESP32-S3 真机
 * 差异点集中在：时基驱动（QEMU 用 ccount 轮询制避开已知的定时器/
 * 中断仿真缺陷；真机可用中断驱动）、时钟源选择、外设可用性。
 * 编译期由 TUDOU_PLATFORM 选择后端（build.py 注入宏），
 * 运行期经 pot_pal_* 接口查询（console `pal` 命令）。
 */

typedef enum {
    POT_PAL_QEMU = 0,
    POT_PAL_HW,
} pot_platform_t;

typedef enum {
    POT_TICK_POLL = 0, /* 检查点轮询（QEMU 制） */
    POT_TICK_IRQ,       /* 中断驱动（真机制） */
} pot_tick_mode_t;

const char *pot_pal_name(void);      /* "qemu-esp32s3" / "hw-esp32s3" */
pot_platform_t pot_pal_platform(void);
pot_tick_mode_t pot_pal_tick_mode(void);
uint32_t pot_pal_cpu_hz(void);

/* 平台早期初始化（向量表/MMU/看门狗在 boot.S；这里做 C 级初始化） */
void pot_pal_early_init(void);

#endif /* TUDOU_PAL_H */
