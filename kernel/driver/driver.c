#include <stdint.h>

#include "console.h"
#include "driver.h"
#include "lib_shim.h"

/*
 * 驱动框架实现：链接期 .pot_drivers 段收集 + 启动期按优先级初始化。
 * 段只读——先拷贝到槽位，槽位内按优先级排序后初始化。
 */

typedef struct {
    const pot_driver_t *drv;
    int status; /* 0=ok -1=fail 2=pending */
} pot_drv_slot_t;

static pot_drv_slot_t g_slots[POT_DRV_MAX];
static int g_n;

extern const pot_driver_t __pot_drivers_start; /* 链接脚本符号 */
extern const pot_driver_t __pot_drivers_end;

void pot_drivers_init(void)
{
    const pot_driver_t *d = &__pot_drivers_start;
    int n = (int)(&__pot_drivers_end - &__pot_drivers_start);
    if (n > POT_DRV_MAX) {
        n = POT_DRV_MAX;
    }

    g_n = n;
    for (volatile int i = 0; i < n; i++) {
        g_slots[i].drv = &d[i];
        g_slots[i].status = 2;
    }
    /* 槽位选择排序（volatile 索引禁 loop 指令；驱动数 << 8） */
    for (volatile int i = 0; i < n; i++) {
        int min = i;
        for (volatile int j = i + 1; j < n; j++) {
            if (g_slots[j].drv->priority < g_slots[min].drv->priority) {
                min = j;
            }
        }
        pot_drv_slot_t tmp = g_slots[i];
        g_slots[i] = g_slots[min];
        g_slots[min] = tmp;
    }
    /* 按序初始化 */
    for (volatile int i = 0; i < n; i++) {
        int r = g_slots[i].drv->init();
        g_slots[i].status = (r == 0) ? 0 : -1;
        potato_printf("[POTATO][DRV] %s init %s\r\n",
                      g_slots[i].drv->name, r == 0 ? "ok" : "FAIL");
    }
}

int pot_driver_count(void)
{
    return g_n;
}

const char *pot_driver_name(int i)
{
    return (i >= 0 && i < g_n) ? g_slots[i].drv->name : 0;
}

int pot_driver_status(int i)
{
    return (i >= 0 && i < g_n) ? g_slots[i].status : 1;
}
