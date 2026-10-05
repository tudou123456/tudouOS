#include "potato.h"

#include "console.h"
#include "uart.h"

/*
 * 土豆内核入口（boot.S 在关看门狗、清 .bss、设好栈后 call0 过来）。
 * 启动横幅的每一行都是测试台的断言锚点：
 *   [POTATO] tudouOS vX.Y.Z ...   — 版本
 *   [POTATO] build ...            — 构建时间
 *   [POTATO] mem ...              — 内存布局
 *   [POTATO] READY                — 就绪标记
 */
void kernel_main(void)
{
    uart_init();

    potato_printf("\r\n[POTATO] %s v%s (%s kernel) on %s\r\n",
                  POTATO_NAME, POTATO_VERSION, POTATO_KERNEL, POTATO_TARGET);
    potato_printf("[POTATO] build %s %s\r\n", __DATE__, __TIME__);
    potato_printf("[POTATO] mem text=%p-%p data=%p-%p bss=%p-%p stack=%p-%p heap=%p-%p\r\n",
                  _text_start, _text_end,
                  _data_start, _data_end,
                  _bss_start, _bss_end,
                  _stack_bottom, _stack_top,
                  _heap_start, _heap_end);
    potato_printf("[POTATO] READY\r\n");

    console_run();
}
