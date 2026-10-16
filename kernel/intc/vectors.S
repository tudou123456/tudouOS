/*
 * 土豆内核异常/中断向量表 — ESP32-S3 (Xtensa LX7, XEA2)
 *
 * 向量表 0x400 字节，VECBASE 由 boot.S 指向本表的 pot_vectors 符号
 * （链接脚本保证 0x400 对齐）。布局依据 esp-idf esp32s3 core-isa.h：
 *   0x000-0x17F 窗口上溢/下溢向量 ×6（call0 内核不应触发，进来即报告挂起）
 *   0x180 IntLevel2  → UART0（CPU int19）
 *   0x1C0 IntLevel3  → SYSTIMER target0（CPU int22）
 *   0x200/0x240 IntLevel4/5、0x280 IntLevel6(Debug)、0x2C0 NMI —— 未用
 *   0x300 Kernel 向量（UM=0 时所有通用异常入口）
 *   0x340 User 向量（UM 恒 0，保险挂同处理）
 *   0x3C0 DoubleException
 *
 * 中断入口按 call0 ABI 保存 a0-a15 + SAR/LBEG/LEND/LCOUNT（80 字节），
 * 返回用 rfi N（自动从 EPCn/EPSn 恢复，无需手工处理 PS）。
 */

    .section .pot.vectors, "ax"

    .globl pot_vectors
    .align  4
pot_vectors:
    /* ---- 窗口异常（call0 内核不该出现） ---- */
    .org 0x000
    movi    a2, 0x000
    j       pot_unexpected_vector
    .org 0x040
    movi    a2, 0x040
    j       pot_unexpected_vector
    .org 0x080
    movi    a2, 0x080
    j       pot_unexpected_vector
    .org 0x0C0
    movi    a2, 0x0C0
    j       pot_unexpected_vector
    .org 0x100
    movi    a2, 0x100
    j       pot_unexpected_vector
    .org 0x140
    movi    a2, 0x140
    j       pot_unexpected_vector

    /* ---- 中断向量 ---- */
    .org 0x180
    j       pot_irq_level2
    .org 0x1C0
    j       pot_irq_level3

    /* ---- 未使用等级 / NMI ---- */
    .org 0x200
    movi    a2, 0x200
    j       pot_unexpected_vector
    .org 0x240
    movi    a2, 0x240
    j       pot_unexpected_vector
    .org 0x280
    movi    a2, 0x280
    j       pot_unexpected_vector
    .org 0x2C0
    movi    a2, 0x2C0
    j       pot_unexpected_vector

    /* ---- 通用异常 / 双重异常 ---- */
    .org 0x300
    j       pot_genex_entry
    .org 0x340
    j       pot_genex_entry
    .org 0x3C0
    j       pot_doubleexc_entry

    .org 0x400 /* 表长固定 0x400，向量表到此为止 */

/* ================= 以下为处理体（普通 .text，可超出 0x40 槽位） ================= */

    .text

/* 通用异常现场保存：帧 88 字节，与 switch.S 任务帧布局一致（含 epc1/eps1，
 * M3 抢占切换会把它整体当作 IRQ 帧记账到被抢占任务）：
 *   0..56 a0,a2..a15   60..72 sar/lbeg/lend/lcount   76 epc1   80 eps1 */
    .macro SAVE_CALL0_REGS
    addi    a1, a1, -88
    s32i    a0, a1, 0
    s32i    a2, a1, 4
    s32i    a3, a1, 8
    s32i    a4, a1, 12
    s32i    a5, a1, 16
    s32i    a6, a1, 20
    s32i    a7, a1, 24
    s32i    a8, a1, 28
    s32i    a9, a1, 32
    s32i    a10, a1, 36
    s32i    a11, a1, 40
    s32i    a12, a1, 44
    s32i    a13, a1, 48
    s32i    a14, a1, 52
    s32i    a15, a1, 56
    rsr.sar     a2
    s32i    a2, a1, 60
    rsr.lbeg    a2
    s32i    a2, a1, 64
    rsr.lend    a2
    s32i    a2, a1, 68
    rsr.lcount  a2
    s32i    a2, a1, 72
    rsr.epc1    a2
    s32i    a2, a1, 76
    rsr     a2, 194               /* 异常 PS 槽位（E-02 实证：rfe 从 SR194
                                     * 恢复 PS；binutils 名 eps2，193 非有效
                                     * SR，161 标准 EPS1 编号但不被 rfe 读） */
    s32i    a2, a1, 80
    .endm

    .macro RESTORE_CALL0_REGS
    l32i    a2, a1, 72
    wsr.lcount  a2
    l32i    a2, a1, 68
    wsr.lend    a2
    l32i    a2, a1, 64
    wsr.lbeg    a2
    l32i    a2, a1, 60
    wsr.sar     a2
    l32i    a15, a1, 56
    l32i    a14, a1, 52
    l32i    a13, a1, 48
    l32i    a12, a1, 44
    l32i    a11, a1, 40
    l32i    a10, a1, 36
    l32i    a9, a1, 32
    l32i    a8, a1, 28
    l32i    a7, a1, 24
    l32i    a6, a1, 20
    l32i    a5, a1, 16
    l32i    a4, a1, 12
    l32i    a3, a1, 8
    l32i    a2, a1, 4
    l32i    a0, a1, 0
    addi    a1, a1, 88
    .endm

/*
 * 中断公共处理体（a2=级别由进入前设置）。
 * 硬件在入口已把 PS.INTLEVEL 抬到本级别，同级不会重入。
 */
    .macro IRQ_HANDLER level
pot_irq_level\level:
    SAVE_CALL0_REGS
    movi    a2, \level
    call0   intc_irq_dispatch
    RESTORE_CALL0_REGS
    rfi     \level
    .endm

    IRQ_HANDLER 2
    IRQ_HANDLER 3

/* 通用异常（Kernel/User 向量）。EXCM 已置位，中断不会再进来。
 * exception_dispatch 返回 1 = level-1 中断已处理 → 恢复现场 RFE；
 * 返回 0 = 致命（内部挂起，不会返回） */
pot_genex_entry:
    SAVE_CALL0_REGS
    movi    a2, 0
    mov     a3, a1                  /* 帧基址传给 C（抢占切换用） */
    call0   exception_dispatch
    bnez    a2, .Lgenex_resume
1:
    j       1b                          /* 致命路径挂起（保险） */
.Lgenex_resume:
    RESTORE_CALL0_REGS
    rfe

pot_doubleexc_entry:
    SAVE_CALL0_REGS
    movi    a2, 1
    call0   exception_dispatch        /* 不返回 */
1:
    j       1b

/* 意外向量：a2 在向量槽位里已带上向量偏移 */
pot_unexpected_vector:
    SAVE_CALL0_REGS
    l32i    a2, a1, 4                  /* 取回槽位设置的向量偏移 */
    call0   intc_unexpected_vector     /* 不返回 */
1:
    j       1b
