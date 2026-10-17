#include "potato.h"

#include "console.h"
#include "driver.h"
#include "heap.h"
#include "intc.h"
#include "mcp.h"
#include "net.h"
#include "obs.h"
#include "pal.h"
#include "potfs.h"
#include "smp.h"
#include "task.h"
#include "tick.h"
#include "time.h"
#include "trace.h"
#include "uart.h"
#include "wdt.h"

/*
 * 土豆内核入口（boot.S 在关看门狗、清 .bss、设栈、装向量表后 call0 过来）。
 * M2 初始化顺序有讲究：
 *   trace → uart 基础 → intc（映射源+INTENABLE，但全局仍关中断）
 *   → systimer（tick 开始计数但被 INTLEVEL=15 挡住）
 *   → uart RX 中断源 → 横幅 → 放行中断 → 控制台循环
 *
 * 真机带板调试：每一步之间 pot_boot_mark() 打一个字符（TUDOU_BOOT_TRACE=1
 * 构建才真正输出，见 kernel/kernel/boot_trace.c），卡死时串口最后一个字符
 * 即卡点。
 */
void pot_boot_mark(char c);

void kernel_main(void)
{
    pot_boot_mark('0');
    trace_init();
    pot_boot_mark('1');
    uart_init(); /* 控制台最先可用（PAL/驱动框架的输出通道） */
    pot_boot_mark('2');
#if TUDOU_PLATFORM != 0
    /* 带板诊断：super watchdog 配置是否已关（bit30=DISABLE）。0x04b00000
       说明仍使能——wdt.c 的关闭没生效，此时靠 tick 兜底喂。 */
    potato_printf("[POTATO] hw rtc-cntl: swd-conf=0x%x (want bit30)\r\n",
                  (unsigned)wdt_swd_conf());
#endif
    pot_pal_early_init();
    pot_boot_mark('3');
    pot_drivers_init(); /* M5：驱动框架（uart 已注册，幂等初始化） */
    pot_boot_mark('4');
    /* 先开全局中断再逐级使能设备源（INTENABLE 未置位时不放行任何源，安全）；
       若保持 INTLEVEL=15 期间置起设备中断，QEMU 会陷入活锁 */
    intc_enable_interrupts();
    intc_init();
    pot_boot_mark('5');
    time_source_init();
    tick_init();
    pot_boot_mark('6');
    pot_sched_init(); /* M3：调度器（main 即控制台任务，idle 兜底） */
    tick_irq_arm();   /* D-03 v5：tick 中断驱动抢占（E-02 实证 rfe 路线） */
    pot_smp_init();   /* M9：SMP 状态与 IPI 矩阵（默认单核，smp start 起 CPU1） */
    pot_boot_mark('7');
    pot_heap_init();  /* M4：轻量堆（消息基座大负载/动态栈的地基） */
    pot_boot_mark('8');
    potfs_format();   /* M6：内存文件系统（RAM 盘语义：上电即空盘） */
    pot_boot_mark('9');
    mcp_init();       /* M6：MCP 适配器通道（代理任务由 mcp on 启动） */
    pot_boot_mark('A');
    uart_irq_init();
    pot_boot_mark('B');
    pot_obs_init();   /* M8：可观测体系（流式 JSONL + 健康快照，按需启动任务） */
    pot_boot_mark('C');
    net_start();      /* M7：以太网 + 远程 MCP 通道（无 EMAC 设备时自动跳过） */
    pot_boot_mark('D');

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
    pot_boot_mark('E');

    console_run();
}
