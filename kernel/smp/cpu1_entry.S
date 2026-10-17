/*
 * 土豆内核 CPU1 入口（M9）——从 ROM 轮询桩跳入内核世界。
 *
 * 进入条件（QEMU 实证）：CPU0 写 SYSTEM_CORE_1_CONTROL_1_REG=入口地址后，
 * ROM 桩执行 `callx8 a2`（窗口化调用，窗口寄存器是 ROM 现场）；此时：
 *   - a1 = ROM 栈（不可用），a0/a2..a15 = ROM 现场
 *   - PS = ROM 现场（UM=1 用户态！INTLEVEL=0、CALLINC=1）
 *   - VECBASE = 0x40000000（ROM 向量表）
 *   - INTENABLE = 0（无中断使能）
 * 因此第一件事是关中断、换栈、装内核向量表、把 PS 换成内核任务常态 PS
 *（UM=0 内核态、CALLINC=1 call0 ABI），再 call0 C 主函数。
 *
 * 每核身份用 THREADPTR（Xtensa 用户寄存器，每核私有、上下文切换不保存）
 * 保存：CPU0 在 _start 置 0，CPU1 在此置 1。
 */

    .section .bss.cpu1_stack, "aw", @nobits
    .align  16
    .globl  pot_cpu1_stack
pot_cpu1_stack:
    .space  4096
    .globl  pot_cpu1_stack_top
pot_cpu1_stack_top:

    /* 代码放 IRAM（裸 .text）：链接脚本把 *(.text) 归入 IRAM，与
     * *(.literal .literal.*) 同区，保证 l32r 字面量在 ±128KB 内；
     * 若用 .text.cpu1 会被 *(.text .text.*) 收进 IROM，而字面量仍在
     * IRAM，链接报 "l32r: literal target out of range"。 */
    .section .text, "ax"
    .align  4
    .globl  _cpu1_entry
    .type   _cpu1_entry, @function
_cpu1_entry:
    /* 1) 关中断（ROM 现场 INTLEVEL=0，先压到 15） */
    rsil    a2, 15

    /* 2) 每核身份 → THREADPTR=1 */
    movi    a2, 1
    wur     a2, threadptr

    /* 3) 换到内核给 CPU1 的独立栈（ROM 栈不可信） */
    movi    a1, pot_cpu1_stack_top

    /* 真机带板标记 'a'：栈切换完成（TUDOU_BOOT_TRACE=1 才真正输出） */
    movi    a2, 0x61
    call0   pot_boot_mark

    /* 4) 窗口状态归零：ROM 用 callx8 进入，WINDOWBASE/START 是 ROM 现场；
     *    内核全程 call0 ABI，只用 a0-a15（不旋转窗口），归零避免窗口
     *    上溢/下溢异常。 */
    movi    a2, 1
    wsr     a2, windowstart
    movi    a2, 0
    wsr     a2, windowbase

    /* 5) 装内核异常/中断向量表（CPU0 在 boot.S 里装同一张） */
    movi    a2, pot_vectors
    wsr.vecbase a2
    esync

    /* 标记 'b'：内核向量表已装 */
    movi    a2, 0x62
    call0   pot_boot_mark

    /* 6) PS 换成内核任务常态 PS（pot_sched_init 捕获：UM=0、EXCM=0、
     *    INTLEVEL=0、CALLINC=1）。注意：INTENABLE 仍为 0，无中断能进来；
     *    中断使能由 pot_smp_cpu1_main 在调度状态就绪后打开。 */
    movi    a2, pot_task_ps
    l32i    a2, a2, 0
    movi    a3, 15
    or      a2, a2, a3          /* 保险：PS.INTLEVEL=15，直到主函数自己放开 */
    wsr     a2, ps
    esync

    /* 标记 'c'：即将进 C 主函数 */
    movi    a2, 0x63
    call0   pot_boot_mark

    /* 7) 进 C 主函数（不返回） */
    movi    a0, 0
    call0   pot_smp_cpu1_main

.Lcpu1_hang:
    j       .Lcpu1_hang
    .size   _cpu1_entry, . - _cpu1_entry

