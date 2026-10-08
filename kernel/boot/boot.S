/*
 * tudouOS 土豆内核启动代码 — ESP32-S3 (Xtensa LX7)
 *
 * 进入点状态：片上 ROM（QEMU 中为内置 ROM 代码）已把镜像各段加载到
 * 链接地址，随后跳到镜像头里的入口地址（即 _start）。
 *
 * M1 内核整体用 call0 ABI 编译（-mabi=call0）：函数调用不旋转寄存器
 * 窗口，因此不会触发窗口上溢/下溢异常，无需提前安装窗口异常向量。
 */

    .section .text.start, "ax"
    .align  4
    .globl  _start
    .type   _start, @function
_start:
    /* 关中断：PS.INTLEVEL=15 屏蔽包括 NMI 在内的所有中断（M1 未启中断，
       双保险），后续里程碑引入中断框架时再重设 */
    rsil    a2, 15

    /* 换到内核自己的栈（ROM 栈在 DRAM 高处，避开） */
    movi    a1, _stack_top

    /* ===== 自研二级映射引导（M5）=====
     * ROM 只搬 RAM 段、无 flash 映射能力（实测）；全部 C 代码/只读数据
     * 在 flash 0x10020 起（LMA），映射到 0x42000020（VMA）执行。
     * 这里在任何 C 调用之前配置外部内存 MMU：
     *   表项 i ← 物理页(_irom_flash_page + i)，0x42000000 页起
     *   表项格式：[13:0]物理页号 | [14]invalid=0 | [15]type=0(flash)
     * 寄存器（QEMU esp32s3_cache 实现与 S3 TRM 一致）：
     *   MMU 表 0x600C5000；ICACHE_CTRL 0x600C4060（bit0 使能） */
    movi    a2, 0x600C5000          /* MMU 表项 0 = 0x42000000 页 */
    movi    a3, _irom_pages
    movi    a4, _irom_flash_page
.Lmap_loop:
    beqz    a3, .Lmap_done
    s32i    a4, a2, 0
    addi    a2, a2, 4
    addi    a4, a4, 1
    addi    a3, a3, -1
    j       .Lmap_loop
.Lmap_done:
    movi    a2, 0x600C4060          /* EXTMEM_ICACHE_CTRL */
    movi    a3, 1
    s32i    a3, a2, 0               /* 使能 icache，0x42 窗口即刻可取指 */

    /* 尽早关看门狗：QEMU 的 esp32s3 机器复位后 TG0 WDT 是启用状态，
       真机上 ROM/引导期间也会启用看门狗。
       注意：此时 .bss 尚未清零，wdt_early_disable 内不得使用任何静态变量 */
    call0   wdt_early_disable

    /* 清零 .bss */
    movi    a2, _bss_start
    movi    a3, _bss_end
    movi    a4, 0
.Lclear_bss:
    bgeu    a2, a3, .Lbss_done
    s32i    a4, a2, 0
    addi    a2, a2, 4
    j       .Lclear_bss
.Lbss_done:

    /* 安装异常/中断向量表（链接脚本保证 .pot.vectors 0x400 对齐） */
    movi    a2, pot_vectors
    wsr.vecbase a2
    esync

    movi    a0, 0
    call0   kernel_main

    /* kernel_main 不应返回；若返回则原地自旋，便于 GDB/QMP 定位 */
.Lhang:
    j       .Lhang
    .size   _start, . - _start
