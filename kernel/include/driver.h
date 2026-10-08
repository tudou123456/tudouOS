#ifndef TUDOU_DRIVER_H
#define TUDOU_DRIVER_H

#include <stdint.h>

/*
 * 土豆驱动框架（M5，支柱2 驱动单测的地基）
 *
 * 极简注册模型：驱动静态声明 → 链接期收集 → 启动期按序初始化。
 * console `drivers` 命令枚举（名称/状态），为后续单测框架提供挂载点。
 * 驱动初始化返回 0 成功；框架记录状态（0=ok -1=fail 1=skipped）。
 */

#define POT_DRV_MAX 8

#define POT_DRIVER_REGISTER(name_, init_, priority_) \
    static const pot_driver_t pot_drv_##name_         \
        __attribute__((used, section(".pot_drivers"))) = { \
        .name = #name_, .init = (init_), .priority = (priority_), \
    }

typedef struct pot_driver {
    const char *name;
    int (*init)(void);
    uint8_t priority; /* 小者先初始化 */
    /* 状态由框架运行期填充（注册期 const）——存到影子表 */
} pot_driver_t;

void pot_drivers_init(void);
int pot_driver_count(void);
const char *pot_driver_name(int i);
int pot_driver_status(int i);

#endif /* TUDOU_DRIVER_H */
