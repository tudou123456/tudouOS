#include <stdint.h>

#include "console.h"
#include "exception.h"
#include "intc.h"
#include "potato.h"
#include "tick.h"
#include "time.h"
#include "trace.h"

/*
 * 异常处理（M2）：
 *  - EXCCAUSE=4（LEVEL1_INTERRUPT）：核内 level-1 中断（CCOMPARE0 tick），
 *    分发处理后返回 1，由 vectors.S 恢复现场 RFE。
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
    default: return "?";
    }
}

static void halt_forever(void)
{
    for (;;) {
    }
}

unsigned exception_dispatch(unsigned vec_flag)
{
    uint32_t cause = sr_exccause();

    if (vec_flag == 0u && cause == 4u) {
        return intc_l1_dispatch() != 0u ? 1u : 0u;
    }

    uint32_t epc = sr_epc1();
    uint32_t vaddr = sr_excvaddr();
    uint32_t ps = sr_ps();

    trace_push(TR_EXCEPTION, cause, epc);

    potato_printf("\r\n[POTATO][EXC] %s\r\n",
                  vec_flag != 0u ? "DOUBLE_EXCEPTION" : "EXCEPTION");
    potato_printf("[POTATO][EXC] cause=%u(%s) epc=0x%x vaddr=0x%x ps=0x%x\r\n",
                  cause, cause_name(cause), epc, vaddr, ps);
    potato_printf("[POTATO][EXC] ticks=%u trace_dropped=%u\r\n",
                  time_tick_count(), trace_dropped());
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
