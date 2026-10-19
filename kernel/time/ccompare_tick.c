#include <stdint.h>

#include "console.h"
#include "io.h"
#include "intc.h"
#include "smp.h"
#include "tick.h"
#include "wdt.h"
#include "time.h"
#include "trace.h"
#include "uart.h"

/* 调度器记账/唤醒入口（sched.c；中断路径由 intc_l1_dispatch 调用，
   本文件的轮询/看门狗旁路必须自行调用，见 D-08） */
extern void pot_sched_tick(uint32_t elapsed);

/*
 * 内核 tick — Xtensa 核内 CCOMPARE0（中断 6，level-1，经通用异常
 * EXCCAUSE=4 路径投递，由 vectors.S 的 genex 入口分发，RFE 返回）。
 * 纯核内定时器：不经过乐鑫 QEMU 的任何外设中断/计数器模型，稳定可靠。
 *
 * 时基（D-26 双端分叉）：
 *   qemu —— ccount 频率取编译期常量 40MHz（QEMU 复位默认时钟，实测），
 *           除法全部常量折叠，热路径不引入运行期除法（QEMU 模型纪律）。
 *   hw   —— time_source_init 用 systimer（52bit@16MHz，只读不使能中断）
 *           标定 ccount 实际频率（ROM 引导遗留时钟配置不定），tick
 *           周期/微秒换算统一走标定值（pot_pal_cpu_hz 同源）。真机
 *           S3 LX7 有硬件 32 位除法（quou，D-30 实证），运行期除法可用。
 */

#define TICK_HZ      1000u
#define CCOMPARE0_INT 6

#if TUDOU_PLATFORM == 0 /* qemu：常量（本路径数值行为不得改动） */
#define POT_CPU_HZ   40000000u
#define TICK_PERIOD_CC   (POT_CPU_HZ / TICK_HZ)      /* 40000 */
#define CC_PER_US        (POT_CPU_HZ / 1000000u)     /* 40 */
#else /* hw：systimer 标定的运行期时基 */
static uint32_t g_cpu_hz;    /* 标定值（time_source_init 填） */
static uint32_t g_tick_cc;   /* g_cpu_hz / TICK_HZ */
#define TICK_PERIOD_CC   (g_tick_cc)
#define CC_PER_US        (g_cc_per_us())
#endif

/*
 * M8：tick 停摆实测门限。QEMU 空闲基线的触发间隔抖动上界实测约 10ms
 * （调试打印/单步仿真导致），真机应在数十微秒量级；取 20ms 作为
 * 「tick 确实被卡住（如 inject hang / 高 INTLEVEL 自旋）」的判据，
 * 作为重要事件记入追踪副环，供 AI 分析流水线在环被冲刷时仍能定位根因。
 */
#define POT_TICK_GAP_US 20000u

static volatile uint32_t g_ticks;
static volatile uint32_t g_last_elapsed;
static volatile uint32_t g_jit_min = 0xFFFFFFFFu;
static volatile uint32_t g_jit_max;
/* K1：首个触发间隔跨「tick_irq_arm 之前/之后的启动窗口」（真机实测
 * 一次性 ~79ms），不是稳态中断延迟；统计首个样本时跳过，避免把启动
 * 瞬态记进 jit_max。首样本可能来自 poll 或中断路径，二者共用该标记。 */
static volatile int g_jit_seeded;
static uint32_t g_next_cc;
static uint32_t g_last_fire_cc;
static volatile int g_irq_armed; /* D-04：中断驱动启用后停用轮询路径
                                    （两者同写 g_next_cc/g_now 会竞态） */
static volatile uint32_t g_wd_ticks; /* M8：看门狗自愈路径补记的 tick 数
                                      （int6 丢失证据；非 0 说明 IRQ 路线丢过） */

#if !POT_TIME_TIMG /* Windows QEMU 路线：ccount 兼任微秒时基 */
static uint32_t g_boot_cc;
#endif

static inline uint32_t ccount(void)
{
    uint32_t v;
    asm volatile("rsr.ccount %0" : "=r"(v));
    return v;
}

static inline void set_ccompare0(uint32_t v)
{
    asm volatile("wsr.ccompare0 %0\n\t"
                 "esync"
                 :: "r"(v) : "memory");
}

#if TUDOU_PLATFORM != 0 /* ---- 真机：systimer 标定（D-26）---- */

/* systimer @0x60023000 只作标定基准：读 UNIT0 计数低 32 位，不配置
 * target/中断（QEMU 对该外设的写会崩仿真，故整段仅真机构建编译；
 * 偏移依据 esp-idf esp32s3/systimer_reg.h，与 systimer.c 一致）。 */
#define SYST_CONF      (0x60023000u + 0x00u) /* bit31 CLK_EN, bit30 UNIT0_EN */
#define SYST_UNIT0_OP  (0x60023000u + 0x04u) /* bit30 UPDATE, bit29 VALID */
#define SYST_UNIT0_LO  (0x60023000u + 0x44u)

/* 标定窗口：400k ccount（80MHz 下 5ms ≈ 80k 个 systimer tick，
 * 量化粒度 1/16 MHz，误差 <0.1%）。 */
#define CAL_CC 400000u

static uint32_t syst_unit0_lo(void)
{
    mmio_write(SYST_UNIT0_OP, mmio_read(SYST_UNIT0_OP) | (1u << 30));
    while ((mmio_read(SYST_UNIT0_OP) & (1u << 29)) == 0u) {
    }
    return mmio_read(SYST_UNIT0_LO);
}

static uint32_t calibrate_cpu_hz(void)
{
    mmio_write(SYST_CONF, (1u << 31) | (1u << 30));
    (void)syst_unit0_lo(); /* 预热一次锁存读，排除首次读时序差异 */
    uint32_t lo0 = syst_unit0_lo();
    uint32_t c0 = ccount();
    while ((ccount() - c0) < CAL_CC) {
    }
    uint32_t dsys = syst_unit0_lo() - lo0; /* CAL_CC 个 ccount 内的 16MHz tick 数 */
    if (dsys == 0u) {
        return 80000000u; /* systimer 无计数（读法偏差/时钟未启）：
                             回退 ROM 引导常见默认 80MHz，保命不死机 */
    }

    /* cpu_hz = CAL_CC * 16MHz / dsys。直接乘 16e6 会溢出 u32，
     * 改为以 1/16 MHz 为粒度合成：q*16 + (r*16)/dsys 再 ×1e6。
     * u32 除法由 S3 硬件 quou 完成（无 libgcc 依赖，D-30 实证）。 */
    uint32_t q = CAL_CC / dsys;
    uint32_t r = CAL_CC % dsys;
    return (q * 16u + (r * 16u) / dsys) * 1000000u;
}

static uint32_t g_cc_per_us_v; /* 标定后 = g_cpu_hz / 1e6（≥16） */
static inline uint32_t g_cc_per_us(void)
{
    return g_cc_per_us_v;
}

#endif /* TUDOU_PLATFORM != 0 */

/* M8：tick 实测停摆超门限 → 重要事件（两处触发路径共用） */
static void tick_note_gap(uint32_t delta_us)
{
    if (delta_us >= POT_TICK_GAP_US) {
        trace_push(TR_TICK_GAP, delta_us, g_ticks);
    }
}

void time_source_init(void)
{
#if TUDOU_PLATFORM != 0
    g_cpu_hz = calibrate_cpu_hz();
    g_tick_cc = g_cpu_hz / TICK_HZ;
    g_cc_per_us_v = g_cpu_hz / 1000000u;
    /* 上电即打印标定结果（uart_init 已先行；真机带板核心诊断信息） */
    potato_printf("[POTATO] hw timebase calibrated: cpu=%uHz "
                  "tick_period=%ucc\r\n", g_cpu_hz, g_tick_cc);
#endif
#if !POT_TIME_TIMG
    g_boot_cc = ccount();
#endif
}

#if !POT_TIME_TIMG /* TIMG 路线时由 timg_timer.c 提供，避免重复定义 */
uint32_t time_now_us(void)
{
    return (ccount() - g_boot_cc) / CC_PER_US; /* 除数为编译期常量 */
}

uint32_t time_ticks_raw(void)
{
    return ccount();
}
#endif

void tick_init(void)
{
    g_last_fire_cc = ccount();
    g_next_cc = g_last_fire_cc + TICK_PERIOD_CC;
    /* v5（D-03）：比较器基准就绪。int6 的使能拆到 tick_irq_arm()——
       必须等 pot_sched_init 设好 pot_current 之后，否则首个 tick 的
       抢占路径会以 pot_current==NULL 运行。 */
}

void tick_irq_arm(void)
{
    /* K1：从使能时刻重新对齐比较基准——首个中断要等启动打印窗口结束
       才到（真机实测 ~79ms 一次性瞬态），否则首个 delta 会从 tick_init
       起算。抖动统计侧另有首样本跳过兜底。 */
    g_last_fire_cc = ccount();
    g_next_cc = g_last_fire_cc + TICK_PERIOD_CC;
    g_jit_seeded = 0;
    set_ccompare0(g_next_cc);
    g_irq_armed = 1;
    intc_enable_int(CCOMPARE0_INT); /* int6，level-1，经 genex EXCCAUSE=4 */
}

/* 轮询推进 tick：到点则补记并驱动调度器记账/唤醒。
 * v5（D-03/D-04）：int6 使能后作为**看门狗自愈**——QEMU esp32s3 模型
 * 在"genex 上下文切走 + CCOMPARE 活动"的组合下会随机丢 tick（M3
 * 时代三版设计冻结的同源问题，E-02 已排除寄存器用法因素）。idle 自旋
 * 与检查点高频调用本函数：发现错过 ≥4 个周期即就地把 tick 补上并
 * 重挂比较器，系统在数百 µs 内自愈。 */
void pot_tick_poll(void)
{
    uint32_t now;
    /* M9：CPU1 无全局时基（其自有 CCOMPARE0 tick 在 smp.c cpu1_tick），
       轮询补偿只属于 CPU0，避免 CPU1 误推进全局 g_now */
    if (pot_smp_cpu_id() != 0u) {
        return;
    }
    if (g_irq_armed) {
        uint32_t ps = irq_save(); /* 与真 tick 中断互斥（防补账交错） */
        now = ccount();
        if ((int32_t)(now - g_next_cc) >= (int32_t)(4 * TICK_PERIOD_CC)) {
            /* tick 中断丢失：走与 tick_irq_handler 相同的补账+重挂 */
            tick_irq_handler();
            /* D-08：补账必须同时驱动调度器。tick_irq_handler 只推进
               g_ticks（中断路径的 pot_sched_tick 由 intc_l1_dispatch 调），
               看门狗这条旁路若漏掉，就会出现「tick 计数在走、调度时钟
               g_now 停摆」的漂移：sleep 到期不唤醒、wake/sw 计数冻结。
               实测（QEMU 空闲 2.5s 后）g_ticks-g_now 漂移可达 3.6s。 */
            pot_sched_tick(tick_last_elapsed());
            g_wd_ticks++;
        }
        irq_restore(ps);
        return;
    }
    now = ccount();
    while ((int32_t)(now - g_next_cc) >= 0) {
        uint32_t elapsed = (now - (g_next_cc - TICK_PERIOD_CC)) / TICK_PERIOD_CC;
        if (elapsed == 0u) {
            elapsed = 1u;
        }
        g_ticks += elapsed;
        g_last_elapsed = elapsed;

        g_next_cc += elapsed * TICK_PERIOD_CC;
        if ((int32_t)(g_next_cc - now) < 0) {
            g_next_cc = now + TICK_PERIOD_CC;
        }

        /* 触发间隔统计（真实观测）。K1：首个样本跨启动窗口（一次性
           瞬态 ~79ms），跳过统计；TICK_GAP 仍照实记录。 */
        uint32_t delta_us = (now - g_last_fire_cc) / CC_PER_US;
        g_last_fire_cc = now;
        if (g_jit_seeded == 0) {
            g_jit_seeded = 1;
        } else if (g_ticks > 2u) {
            if (delta_us < g_jit_min) {
                g_jit_min = delta_us;
            }
            if (delta_us > g_jit_max) {
                g_jit_max = delta_us;
            }
        }
        tick_note_gap(delta_us);

        pot_sched_tick(elapsed);
    }
}

uint32_t tick_cpu_hz(void)
{
#if TUDOU_PLATFORM == 0
    return POT_CPU_HZ;
#else
    return g_cpu_hz; /* D-26：与 tick 周期/微秒换算同源（标定值） */
#endif
}

uint32_t tick_last_elapsed(void)
{
    return g_last_elapsed;
}

uint32_t time_tick_count(void)
{
    return g_ticks;
}

uint32_t time_tick_jitter_min_us(void)
{
    return g_jit_min;
}

uint32_t time_tick_jitter_max_us(void)
{
    return g_jit_max;
}

uint32_t time_tick_watchdog_count(void)
{
    return g_wd_ticks;
}

void tick_irq_handler(void)
{
    uint32_t now = ccount();

    /* QEMU 的 ccount 按翻译块大步推进，可能整段跳过比较点（实测触发率
       ~53%）。按实际跨过的周期数补记，保证 tick 计数与真实时间对齐；
       jitter 统计按真实触发间隔记录（这正是要观测的可观测性数据）。 */
    uint32_t elapsed = (now - (g_next_cc - TICK_PERIOD_CC)) / TICK_PERIOD_CC;
    if (elapsed == 0u) {
        elapsed = 1u;
    }
    g_ticks += elapsed;
    g_last_elapsed = elapsed;

    g_next_cc += elapsed * TICK_PERIOD_CC;
    if ((int32_t)(g_next_cc - now) < 0) {
        g_next_cc = now + TICK_PERIOD_CC; /* 落后过多则重新对齐 */
    }
    set_ccompare0(g_next_cc); /* 写 CCOMPARE 顺带清除 int6 挂起 */

#if TUDOU_PLATFORM != 0
    /* 真机 super watchdog 兜底：若能关掉则这次只是 1 次只读（见 wdt.c）。
       实测 rst:0x12 SUPER_WDT_RST 会在启动后数秒复位，必须喂或关。 */
    wdt_swd_feed();
#endif

    /* K1：首个样本跨启动窗口（一次性瞬态 ~79ms），跳过统计；TICK_GAP 照实记录 */
    uint32_t delta_us = (now - g_last_fire_cc) / CC_PER_US;
    g_last_fire_cc = now;
    if (g_jit_seeded == 0) {
        g_jit_seeded = 1;
    } else if (g_ticks > 2u) {
        if (delta_us < g_jit_min) {
            g_jit_min = delta_us;
        }
        if (delta_us > g_jit_max) {
            g_jit_max = delta_us;
        }
    }
    tick_note_gap(delta_us);
}

