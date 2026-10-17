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

/* 真机带板调试用启动标记（TUDOU_BOOT_TRACE=1 时才真正输出，
 * 见 kernel/kernel/boot_trace.c） */
void pot_boot_mark(char c);

void pot_drivers_init(void)
{
    pot_boot_mark('d');
    const pot_driver_t *d = &__pot_drivers_start;
    int n = (int)(&__pot_drivers_end - &__pot_drivers_start);
    pot_boot_mark('j');
    if (n > POT_DRV_MAX) {
        n = POT_DRV_MAX;
    }
    pot_boot_mark('k');
    g_n = n;
    for (volatile int i = 0; i < n; i++) {
        pot_boot_mark('l');
        g_slots[i].drv = &d[i];
        g_slots[i].status = 2;
    }
    pot_boot_mark('m');
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
    pot_boot_mark('e');
    for (volatile int i = 0; i < n; i++) {
        pot_boot_mark('f');
        int r = g_slots[i].drv->init();
        pot_boot_mark('g');
        /* 0=ok 1=设备缺席（skipped） 其它=fail —— 与 driver.h 约定一致 */
        g_slots[i].status = (r == 0) ? 0 : ((r == 1) ? 1 : -1);
        pot_boot_mark('h');
        potato_printf("[POTATO][DRV] %s init %s\r\n", g_slots[i].drv->name,
                      r == 0 ? "ok" : (r == 1 ? "skipped" : "FAIL"));
    }
    pot_boot_mark('i');
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
