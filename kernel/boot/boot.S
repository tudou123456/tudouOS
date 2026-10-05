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

    movi    a0, 0
    call0   kernel_main

    /* kernel_main 不应返回；若返回则原地自旋，便于 GDB/QMP 定位 */
.Lhang:
    j       .Lhang
    .size   _start, . - _start
