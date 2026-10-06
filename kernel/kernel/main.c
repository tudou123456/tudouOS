#include "potato.h"

#include "console.h"
#include "intc.h"
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
    uart_init();
    /* 先开全局中断再逐级使能设备源（INTENABLE 未置位时不放行任何源，安全）；
       若保持 INTLEVEL=15 期间置起设备中断，QEMU 会陷入活锁 */
    intc_enable_interrupts();
    intc_init();
    time_source_init();
    tick_init();
    uart_irq_init();

    potato_printf("\r\n[POTATO] %s v%s (%s kernel) on %s\r\n",
                  POTATO_NAME, POTATO_VERSION, POTATO_KERNEL, POTATO_TARGET);
    potato_printf("[POTATO] build %s %s\r\n", __DATE__, __TIME__);
    potato_printf("[POTATO] mem text=%p-%p data=%p-%p bss=%p-%p stack=%p-%p heap=%p-%p\r\n",
                  _text_start, _text_end,
                  _data_start, _data_end,
                  _bss_start, _bss_end,
                  _stack_bottom, _stack_top,
                  _heap_start, _heap_end);
    potato_printf("[POTATO] intc+tick(1ms)+trace ready\r\n");
    potato_printf("[POTATO] READY\r\n");

    console_run();
}
