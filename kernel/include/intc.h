#ifndef TUDOU_INTC_H
#define TUDOU_INTC_H

#include <stdint.h>

/*
 * 土豆内核 M2 中断框架 — ESP32-S3 (Xtensa LX7)
 *
 * S3 的外设中断经中断矩阵(0x600C2000)映射到 Xtensa CPU 中断号，
 * CPU 中断号的优先级由核心配置固定（无软件可编程优先级）：
 *   int 0-18,27..: level1（无专用向量，走通用异常，M2 不用）
 *   int 19/20/21:  level2（向量 VECBASE+0x180）
 *   int 22/23:     level3（向量 VECBASE+0x1C0）
 * 本内核占用：UART0→int19(level2)，EMAC→int20(level2)，
 * SYSTIMER_TARGET0→int22(level3)。
 * 矩阵寄存器布局：源 N 的映射字在 0x600C2000 + 4*N（值 = CPU 中断号，
 * 0 无效；CPU1 的映射区从 ESP32S3_INT_MATRIX_INPUTS*4 起）。
 */

#define INTC_UART0_CPU_INT     19
#define INTC_EMAC_CPU_INT      20
#define INTC_IPI_CPU_INT       21  /* M9：核间中断（level2，与 19/20 共用向量） */
#define INTC_UART0_SOURCE      27u  /* ETS_UART0_INTR_SOURCE */
#define INTC_EMAC_SOURCE       0u   /* ETS_ETH_MAC_INTR_SOURCE = ETS_WIFI_MAC_INTR_SOURCE */
/* M9 核间中断源：SYSTEM_CPU_INTR_FROM_CPU_0/1（0x600C0030/0x600C0034）
 * → 中断矩阵源 ETS_FROM_CPU_INTR0=79 / INTR1=80（QEMU esp32s3_intc.h 枚举计数） */
#define INTC_FROM_CPU0_SOURCE  79u  /* CPU0 写 FROM_CPU_0 → CPU1 接收 */
#define INTC_FROM_CPU1_SOURCE  80u  /* CPU1 写 FROM_CPU_1 → CPU0 接收 */
/* IPI 的"对核接收"映射目标：level-1 外部中断号 4，INTENABLE 永不置位
 *（只抬矩阵线不响应）——避免矩阵默认映射 6（CCOMPARE0 tick）误触发 */
#define INTC_IPI_MASK_INT      4
/* 中断矩阵每 CPU 区大小：ESP32S3_INT_MATRIX_INPUTS(512) * 4 */
#define INTC_MATRIX_CPU_STRIDE 0x800u

/* 映射外设源到 CPU 中断号并置 INTENABLE（在开全局中断前调用） */
void intc_init(void);

/* 通用源路由（M7：EMAC 等新增外设用；source 为中断矩阵源号，CPU0 视角） */
void intc_route_source(unsigned source, unsigned cpu_int);

/* M9：按 CPU 路由（cpu=0/1；CPU1 的映射区在 0x600C2800 起） */
void intc_route_source_cpu(unsigned cpu, unsigned source, unsigned cpu_int);

/* 使能单个 CPU 中断号（如 tick_init 再开 int6） */
void intc_enable_int(unsigned int_num);

/* vectors.S 胶水在关中断现场保存后调用（汇编侧入口，勿直接调用）。
 * frame_base = 被打断上下文的 IRQ 帧基址（M9 IPI 重调度要用） */
void intc_irq_dispatch(unsigned level, void *frame_base);

/* 通用异常(EXCCAUSE=4)路径的 level-1 分发；1=已处理可恢复。
 * frame_base 为被打断上下文的帧基址（抢占切换用，切换时不返回） */
unsigned intc_l1_dispatch(void *frame_base);

/* 意外向量（窗口异常等 call0 内核不应出现的入口）的处理，不返回 */
void intc_unexpected_vector(unsigned vector_offset);

/* ---- 全局中断开关（PS.INTLEVEL） ---- */

static inline void intc_enable_interrupts(void)
{
    uint32_t ps;
    asm volatile("rsil %0, 0" : "=r"(ps) :: "memory");
    (void)ps;
}

/* D-04：临界区只屏蔽 level-1（tick）——不是 15。INTLEVEL=15 期间挂起的
 * 设备源会触发 QEMU 活锁（M2 已知问题），且任务态唯一竞态对象是
 * level-1 tick 会触碰的调度器结构；UART(level-2) 不改调度器状态。 */
static inline uint32_t irq_save(void)
{
    uint32_t ps;
    asm volatile("rsil %0, 1" : "=r"(ps) :: "memory");
    return ps;
}

static inline void irq_restore(uint32_t ps)
{
    asm volatile("wsr.ps %0\n\t"
                 "esync"
                 :: "r"(ps) : "memory");
}

/* ---- 跨核互斥锁（M9/P0-3） ----
 * rsil 15（本核全屏蔽）+ S32C1I 原子交换。heap/msg/potfs/trace 等
 * "本核 rsil1 假锁"在 CPU1 上线后必须换成它：PS.INTLEVEL 是每核私有的，
 * 两核可同时进入 rsil1 区间，复合容器更新会静默损坏。
 *
 * 约束：不可在 NMI 里使用；持锁期间不得睡眠；锁是全局地址，加锁顺序
 * heap < trace、fs < heap（本仓库实际调用图只有这两种嵌套）。
 * 每次加锁都重设 SCOMPARE1（每核私有），不依赖 smp_init 的初始化时序。 */
typedef struct {
    volatile uint32_t v;
} pot_lock_t;

uint32_t pot_lock_acquire(pot_lock_t *lock);
void pot_lock_release(pot_lock_t *lock, uint32_t ps);

/* 原子清零并返回旧值（IPI pending 等"读-清"不能丢并发置位） */
uint32_t pot_atomic_swap_zero(volatile uint32_t *p);

#endif /* TUDOU_INTC_H */
