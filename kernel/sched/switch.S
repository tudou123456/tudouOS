/*
 * 土豆内核上下文切换 — Xtensa call0 ABI，syscall 统一制（M3 v4）
 *
 * 设计沿革：v1 双帧制、v2 rfe 统一制当年因"无可访问的 EPS1"废弃——
 * E-02 判定实验（2026-10-16）已证伪该前提（rfe 实际从 EPC1+SR194
 * 可控恢复，eps1 只是 binutils 未导出名字）。现行 v4 检查点制稳定
 * 在用；rfe 路线保留给真机中断驱动抢占（D-03 接线时启用）。
 *   - 一切切换点都是 syscall（任务级：yield/sleep/exit/调度检查点）
 *   - 帧由异常入口 SAVE（88 字节），帧的 a0 槽 = syscall 的 C 返回地址
 *   - 恢复 = 恢复寄存器 + wsr.ps 常态 PS + ret.n（不需要 epc1/eps1/rfe）
 *   - tick 只做唤醒/老化/预算记账，不在中断上下文切换
 *   - "抢占"语义 = 检查点调度：任务在 beat 循环调用 pot_task_yield()，
 *     更高优先级就绪时在检查点立即切换（QEMU 下粒度为 beat 级 ~毫秒）
 *
 * 帧布局（与 vectors.S SAVE 一致）：
 *   +0 a0  +4 a2  +8 a3 ... +56 a15
 *   +60 sar +64 lbeg +68 lend +72 lcount
 *   +76 epc1  +80 eps1（v4 不使用后两槽，保留布局一致性）
 */

#include "task.h"

#define TCB_SP_OFF    4

/* 阻塞意图魔数：syscall 前放入 a2，异常侧从帧槽 +4 识别 */
#define POT_SCHED_MAGIC_YIELD 0x50534C01u
#define POT_SCHED_MAGIC_SLEEP 0x50534C02u
#define POT_SCHED_MAGIC_EXIT  0x50534C03u

    .text

/* ---------- 任务级阻塞入口（syscall 的下一条即恢复续体） ---------- */
    .globl pot_sched_syscall_yield
pot_sched_syscall_yield:
    movi    a2, POT_SCHED_MAGIC_YIELD
    syscall
    ret.n

    .globl pot_sched_syscall_sleep
pot_sched_syscall_sleep:
    /* C 参数 a2=until → 挪到 a3，a2 放魔数（异常侧从帧 +4/+8 取） */
    mov     a3, a2
    movi    a2, POT_SCHED_MAGIC_SLEEP
    syscall
    ret.n

    .globl pot_sched_syscall_exit
pot_sched_syscall_exit:
    movi    a2, POT_SCHED_MAGIC_EXIT
    syscall
1:
    j       1b                      /* 退出任务不会被恢复，保险 */

/* ---------- 任务首启蹦床（初始帧 a2=fn a3=arg） ---------- */
    .globl pot_task_trampoline
pot_task_trampoline:
    mov     a6, a2
    mov     a2, a3
    callx0  a6                     /* fn(arg) */
    call0   pot_sched_syscall_exit  /* 任务函数返回即自尽 */
1:
    j       1b

/* ---------- 统一恢复（异常上下文调用，a2 = tcb） ----------
 * a0 槽即恢复续体（syscall 帧=C 返回地址；初始帧=蹦床），
 * 恢复常态 PS 后 ret.n 离开异常上下文（手动退出，QEMU 实测可靠——
 * v1 失败根因是链表环与 EPS1，不是本路径）。 */
    .globl pot_do_resume
pot_do_resume:
    l32i    a1, a2, TCB_SP_OFF
    l32i    a0, a1, 0
    l32i    a3, a1, 8
    l32i    a4, a1, 12
    l32i    a5, a1, 16
    l32i    a6, a1, 20
    l32i    a7, a1, 24
    l32i    a8, a1, 28
    l32i    a9, a1, 32
    l32i    a10, a1, 36
    l32i    a11, a1, 40
    l32i    a12, a1, 44
    l32i    a13, a1, 48
    l32i    a14, a1, 52
    l32i    a15, a1, 56
    l32i    a3, a1, 60
    wsr.sar a3
    l32i    a3, a1, 64
    wsr.lbeg a3
    l32i    a3, a1, 68
    wsr.lend a3
    l32i    a3, a1, 72
    wsr.lcount a3
    movi    a3, pot_task_ps         /* 常态 PS 直接恢复（v4 无 tick 中断，
                                       退出窗口无挂起源） */
    l32i    a3, a3, 0
    wsr.ps  a3
    esync
    /* D-02：a3 在上面被借作 sar/lbeg/lend/lcount/PS 的暂存，此处必须
       从帧槽 +8 重载任务参数（帧基址 a1 在下一条 addi 前仍有效）。
       同理 a2 也在此重载（帧 +4）。退出窗口（wsr.ps 之后、ret 之前）
       若落 level-2 IRQ：ISR 的 SAVE/RESTORE 自带对称 88B 帧，
       call0 下 a1=帧基址仍 4 字节对齐，机械上安全（真机启用 level-1
       抢占前需按 D-14 复核此窗口）。 */
    l32i    a3, a1, 8
    l32i    a2, a1, 4
    addi    a1, a1, 88
    ret.n
