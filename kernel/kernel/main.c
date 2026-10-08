#include "potato.h"

#include "console.h"
#include "driver.h"
#include "heap.h"
#include "intc.h"
#include "pal.h"
#include "task.h"
#include "tick.h"
#include "time.h"
#include "trace.h"
#include "uart.h"

/*
 * 土豆内核入口（boot.S 在关看门狗、清 .bss、设栈、装向量表后 call0 过来）。
 * M2 初始化顺序有讲究：
 *   trace → uart 基础 → intc（映射源+INTENABLE，但全局仍关中断）
 *   → systimer（tick 开始计数但被 INTLEVEL=15 挡住）
 *   → uart RX 中断源 → 横幅 → 放行中断 → 控制台循环
 */
void kernel_main(void)
{
    trace_init();
    uart_init(); /* 控制台最先可用（PAL/驱动框架的输出通道） */
    pot_pal_early_init();
    pot_drivers_init(); /* M5：驱动框架（uart 已注册，幂等初始化） */
    /* 先开全局中断再逐级使能设备源（INTENABLE 未置位时不放行任何源，安全）；
       若保持 INTLEVEL=15 期间置起设备中断，QEMU 会陷入活锁 */
    intc_enable_interrupts();
    intc_init();
    time_source_init();
    tick_init();
    pot_sched_init(); /* M3：调度器（main 即控制台任务，idle 兜底） */
    pot_heap_init();  /* M4：轻量堆（消息基座大负载/动态栈的地基） */
    uart_irq_init();

    potato_printf("\r\n[POTATO] %s v%s (%s kernel) on %s\r\n",
                  POTATO_NAME, POTATO_VERSION, POTATO_KERNEL, POTATO_TARGET);
    potato_printf("[POTATO] build %s %s\r\n", __DATE__, __TIME__);
    {
        extern char _iram_start[], _iram_end[], _irom_start[], _irom_end[];
        potato_printf("[POTATO] mem iram=%p-%p irom(flash)=%p-%p "
                      "data=%p-%p bss=%p-%p stack=%p-%p heap=%p-%p\r\n",
                      _iram_start, _iram_end, _irom_start, _irom_end,
                      _data_start, _data_end,
                      _bss_start, _bss_end,
                      _stack_bottom, _stack_top,
                      _heap_start, _heap_end);
    }
    potato_printf("[POTATO] intc+tick(1ms)+trace ready\r\n");
    potato_printf("[POTATO] READY\r\n");

    console_run();
}
