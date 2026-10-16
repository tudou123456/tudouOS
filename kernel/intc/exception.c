#include <stdint.h>

#include "console.h"
#include "exception.h"
#include "intc.h"
#include "potato.h"
#include "task.h"
#include "tick.h"
#include "time.h"
#include "trace.h"

/*
 * 异常处理（M3）：
 *  - EXCCAUSE=4（LEVEL1_INTERRUPT）：tick/核内中断 → 分发 + 调度记账，
 *    若需抢占则把当前异常帧转为被抢占任务的 IRQ 帧并切换（不返回）。
 *  - EXCCAUSE=1（SYSCALL）：任务级恢复 IRQ 帧任务的跳板落地。
 *  - 其余：致命，打印现场 + 追踪环摘要后挂起（QMP 复位/GDB 恢复）。
 */

static inline uint32_t sr_exccause(void)
{
    uint32_t v;
    asm volatile("rsr.exccause %0" : "=r"(v));
    return v;
}

static inline uint32_t sr_epc1(void)
{
    uint32_t v;
    asm volatile("rsr.epc1 %0" : "=r"(v));
    return v;
}

static inline uint32_t sr_excvaddr(void)
{
    uint32_t v;
    asm volatile("rsr.excvaddr %0" : "=r"(v));
    return v;
}

static inline uint32_t sr_ps(void)
{
    uint32_t v;
    asm volatile("rsr.ps %0" : "=r"(v));
    return v;
}

static const char *cause_name(uint32_t cause)
{
    switch (cause) {
    case 0:  return "ILLEGAL_INSTRUCTION";
    case 1:  return "SYSCALL";
    case 2:  return "INSTRUCTION_FETCH_ERROR";
    case 3:  return "LOAD_STORE_ERROR";
    case 4:  return "LEVEL1_INTERRUPT";
    case 5:  return "ALLOCA";
    case 6:  return "INTEGER_DIVIDE_BY_ZERO";
    case 9:  return "UNALIGNED";
    case 12: return "INSTR_PROHIBITED";
    case 28: return "INSTR_PIF_DATA_ERROR"; /* E-03：invalid MMU 页取指 */
    default: return "?";
    }
}

static void halt_forever(void)
{
    for (;;) {
    }
}

/* ---------- E-02 判定实验：可控 rfe 恢复（console.c cmd_rfetest 配套） ----------
 * 旧结论"EPS1 不可访问 → rfe 无法可控恢复"疑似误诊（SR194 实为 EPS2，
 * eps1 名字不存在可能只是 binutils 未导出）。此处打印异常现场各候选
 * SR 现值，随后把 EPC1 设为恢复点、候选 SR 设为异常前健康 PS，经
 * .Lgenex_resume（RESTORE+rfe）返回，验证 rfe 路线是否可控。 */

extern volatile int g_e2_active;
extern volatile uint32_t g_e2_resume_pc;
extern volatile uint32_t g_e2_ps_before;
extern volatile uint32_t g_e2_eps_sr;

static uint32_t rsr_sr161(void)
{
    uint32_t v;
    asm volatile("rsr %0, 161" : "=r"(v));
    return v;
}

static uint32_t rsr_sr194(void)
{
    uint32_t v;
    asm volatile("rsr %0, 194" : "=r"(v));
    return v;
}

static int e2_try_resume(uint32_t cause)
{
    potato_printf("[POTATO][E2][H] cause=%u(%s) epc1=0x%x sr161=0x%x "
                  "sr194=0x%x ps=0x%x\r\n",
                  cause, cause_name(cause), sr_epc1(),
                  rsr_sr161(), rsr_sr194(), sr_ps());

    uint32_t ps = g_e2_ps_before;
    uint32_t sr = g_e2_eps_sr;
    if (sr == 161u || sr == 194u || sr == 1611u) {
        if (sr == 1611u) {
            /* 区分变体：写 161 一个 INTLEVEL=1 的 PS（安全：只屏蔽
             * level-1，UART level-2 不受影响）。若 rfe 读 161，恢复后
             * ps 低位=1；若读 194，ps 维持硬件填入的原值 */
            uint32_t ps_il1 = (ps & ~0xFu) | 1u;
            asm volatile("wsr %0, 161" :: "r"(ps_il1));
        } else if (sr == 161u) {
            asm volatile("wsr %0, 161" :: "r"(ps));
        } else {
            asm volatile("wsr %0, 194" :: "r"(ps));
        }
    } else {
        potato_printf("[POTATO][E2][H] unsupported sr=%u "
                      "(161/1611/194；193 在本配置非有效 SR，汇编器拒收)\r\n", sr);
        g_e2_active = 0;
        return 0; /* 走致命路径 */
    }
    asm volatile("wsr.epc1 %0" :: "r"(g_e2_resume_pc));
    asm volatile("esync");
    g_e2_active = 0;
    return 1; /* → vectors.S .Lgenex_resume：RESTORE + rfe */
}

unsigned exception_dispatch(unsigned vec_flag, void *frame_base)
{
    uint32_t cause = sr_exccause();

    if (vec_flag == 0u && g_e2_active) {
        return e2_try_resume(cause); /* E-02 实验：可控 rfe 恢复 */
    }

    if (vec_flag == 0u) {
        if (cause == 4u) { /* level-1 中断（tick）：记账+唤醒，不在中断上下文切换 */
            intc_l1_dispatch();
            return 1;
        }
        if (cause == 1u) { /* syscall：任务级切换（yield/sleep/exit/检查点） */
            if (pot_sched_try_syscall(frame_base)) {
                /* 不可达：切换在异常上下文完成 */
            }
            /* 非调度 syscall（M3 无其它用途）→ 落入致命路径 */
        }
    }

    uint32_t epc = sr_epc1();
    uint32_t vaddr = sr_excvaddr();
    uint32_t ps = sr_ps();

    trace_push(TR_EXCEPTION, cause, epc);

    potato_printf("\r\n[POTATO][EXC] %s\r\n",
                  vec_flag != 0u ? "DOUBLE_EXCEPTION" : "EXCEPTION");
    potato_printf("[POTATO][EXC] cause=%u(%s) epc=0x%x vaddr=0x%x ps=0x%x\r\n",
                  cause, cause_name(cause), epc, vaddr, ps);
    potato_printf("[POTATO][EXC] task=%s ticks=%u trace_dropped=%u\r\n",
                  pot_current->name, time_tick_count(), trace_dropped());
    potato_printf("[POTATO][EXC] HALTED (QMP system_reset 可复位重启)\r\n");

    halt_forever();
    return 0;
}

void intc_unexpected_vector(unsigned vector_offset)
{
    potato_printf("\r\n[POTATO][EXC] unexpected vector offset=0x%x "
                  "(call0 kernel shouldn't hit this)\r\n", vector_offset);
    potato_printf("[POTATO][EXC] HALTED\r\n");
    halt_forever();
}
