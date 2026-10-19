/*
 * tudouOS 土豆内核启动代码 — ESP32-S3 (Xtensa LX7)
 *
 * 进入点状态：片上 ROM 已把镜像各段加载到链接地址，随后跳到镜像头
 * 里的入口地址（即 _start）。
 *
 * M1 内核整体用 call0 ABI 编译（-mabi=call0）：函数调用不旋转寄存器
 * 窗口，因此不会触发窗口上溢/下溢异常，无需提前安装窗口异常向量。
 */

    .section .text.start, "ax"
    .align  4

    /* 真机启动分阶段标记（调试用：TUDOU_BOOT_TRACE=1 由 build.py 环境变量注入）。
       直接轮询 UART0 TXFIFO 计数后写 THR，不依赖 uart 驱动；ROM 已配好 UART0。 */
#if TUDOU_BOOT_TRACE != 0
    .macro BOOTCH ch
    movi    a7, 0x60000000
    movi    a11, \ch
99:
    l32i    a12, a7, 0x1c
    extui   a12, a12, 16, 10
    movi    a8, 63
    bltu    a8, a12, 99b
    s32i    a11, a7, 0
    memw
    .endm
#else
    .macro BOOTCH ch
    .endm
#endif

    .globl  _start
    .type   _start, @function
_start:
    BOOTCH  83                  /* 'S'：已进入 _start */
    /* 关中断：PS.INTLEVEL=15 屏蔽包括 NMI 在内的所有中断 */
    rsil    a2, 15

    /* M9：每核身份。THREADPTR 是每核私有寄存器（异常/上下文切换不保存），
       本核（CPU0）置 0；CPU1 在 cpu1_entry.S 里置 1。
       pot_smp_cpu_id() 读它区分核。 */
    movi    a2, 0
    wur     a2, threadptr
    BOOTCH  84                  /* 'T'：线程指针/身份就绪 */

    /* 换到内核自己的栈（ROM 栈在 DRAM 高处，避开） */
    movi    a1, _stack_top
    BOOTCH  85                  /* 'U'：栈已切换 */

    /* ===== flash 自举（QEMU XIP；或 hw --xip 实验布局）=====
     * ROM 只搬 RAM 段、无 flash 映射能力（实测）；该布局下全部 C 代码/
     * 只读数据在 flash 0x10020 起（LMA），映射到 0x42000020（VMA）执行。
     * MMU 表 @0x600C5000：32 位槽/槽内 16 位表项值（[13:0]物理页号
     * 64KB 单位 | [14]invalid | [15]type），16 位访问对表完全无效。
     *
     * 双端差异（真机实测）：
     *  - QEMU：写表 + ICACHE_CTRL bit0=1 即可取指。
     *  - 真机：0x42 窗口取指路径不可用（几何/SHUT/tag/预烫全试过，仅数据
     *    读触发的填充偶尔成功）——hw 默认改为全 RAM 布局（TUDOU_HW_XIP=0，
     *    本节整段不编译），只有 --xip 实验构建才走下面 XIP 自举链。
     * 全程 memw（ROM cache 驱动同款 MMIO 屏障纪律）。 */
#if TUDOU_PLATFORM != 0 && TUDOU_HW_XIP != 0
    /* 1. 关 icache + 清 SHUT 双总线 + 清三个功能配置 + tag 断电重启 */
    movi    a2, 0x600C4060          /* EXTMEM_ICACHE_CTRL */
    l32i    a3, a2, 0
    movi    a5, ~1
    and     a3, a3, a5
    memw
    s32i    a3, a2, 0
    movi    a2, 0x600C4064          /* ICACHE_CTRL1：清 SHUT_CORE0/1 */
    l32i    a3, a2, 0
    movi    a5, ~3
    and     a3, a3, a5
    memw
    s32i    a3, a2, 0
    movi    a3, 0
    movi    a2, 0x600C406C          /* ICACHE_PRELOCK_CTRL */
    memw
    s32i    a3, a2, 0
    movi    a2, 0x600C4094          /* ICACHE_PRELOAD_CTRL */
    memw
    s32i    a3, a2, 0
    movi    a2, 0x600C40A0          /* ICACHE_AUTOLOAD_CTRL */
    memw
    s32i    a3, a2, 0
    movi    a2, 0x600C4068          /* ICACHE_TAG_POWER_CTRL */
    movi    a3, 2                   /* FORCE_PD：tag 阵列断电 */
    memw
    s32i    a3, a2, 0
    movi    a6, 0x2000              /* ~10µs @80MHz */
1:
    addi    a6, a6, -1
    bnez    a6, 1b
    movi    a3, 4                   /* FORCE_PU：重新上电 */
    memw
    s32i    a3, a2, 0
    movi    a6, 0x2000
2:
    addi    a6, a6, -1
    bnez    a6, 2b
#endif
#if TUDOU_PLATFORM == 0 || TUDOU_HW_XIP != 0
    /* 2. 写 MMU 表：表项 i ← 物理页(_irom_flash_page + i)。全 RAM 布局
       （hw 默认）没有 flash 段，不需要映射，整块跳过。 */
    movi    a2, 0x600C5000          /* 表项 0 = 0x42000000 页 */
    movi    a3, _irom_pages
    movi    a4, _irom_flash_page
.Lmap_loop:
    beqz    a3, .Lmap_done
    s32i    a4, a2, 0
    memw
    addi    a2, a2, 4
    addi    a4, a4, 1
    addi    a3, a3, -1
    j       .Lmap_loop
.Lmap_done:
#endif
#if TUDOU_PLATFORM != 0 && TUDOU_HW_XIP != 0
    /* 3. 全量无效化 icache（S3 无 S2 的 SYNC_MAP；ADDR=0 + SIZE=最大触发，
       等 DONE，清 INVALIDATE_ENA；offsets 0x88/0x8C/0x90） */
    movi    a2, 0x600C4088          /* ICACHE_SYNC_CTRL */
    movi    a3, 1
    memw
    s32i    a3, a2, 0               /* INVALIDATE_ENA=1 */
    movi    a3, 0
    memw
    s32i    a3, a2, 4               /* SYNC_ADDR = 0 */
    movi    a3, 0x7FFFFF
    memw
    s32i    a3, a2, 8               /* SYNC_SIZE = 最大（触发） */
3:
    l32i    a3, a2, 0
    bbci    a3, 1, 3b               /* 等 SYNC_DONE(bit1) */
    l32i    a3, a2, 0
    movi    a5, ~1
    and     a3, a3, a5
    memw
    s32i    a3, a2, 0               /* 清 INVALIDATE_ENA */
#endif
#if TUDOU_PLATFORM != 0 && TUDOU_HW_XIP != 0
    /* 4. X-run 实证修复链：IBUS/DBUS flash 边界写全窗口、清 CACHE_ILG
       粘滞态、RMW 使能 DCACHE——icache 的 miss 填充疑似经 dcache/L2
       引擎（关着时 0x3C 数据读可直通、0x42 取指必死，实测行为吻合） */
    movi    a2, 0x600C40B4          /* IBUS_TO_FLASH_START/END_VADDR */
    movi    a3, 0x42000000
    memw
    s32i    a3, a2, 0
    movi    a3, 0x43FFFFFF
    memw
    s32i    a3, a2, 4
    movi    a3, 0x3C000000          /* DBUS 边界 */
    memw
    s32i    a3, a2, 8
    movi    a3, 0x3DFFFFFF
    memw
    s32i    a3, a2, 12
    movi    a2, 0x600C40E0          /* CACHE_ILG_INT_CLR：清全部粘滞 */
    movi    a3, 0x1FF
    memw
    s32i    a3, a2, 0
    movi    a2, 0x600C4000          /* DCACHE_CTRL：RMW 置 EN */
    l32i    a3, a2, 0
    movi    a5, 1
    or      a3, a3, a5
    memw
    s32i    a3, a2, 0
    /* 5. 使能 icache：几何按 0xF（32KB|8路|32B块|EN，'X' 实证） */
    movi    a2, 0x600C4060
    l32i    a3, a2, 0
    movi    a5, 0xF
    or      a3, a3, a5
    memw
    s32i    a3, a2, 0
#elif TUDOU_PLATFORM == 0
    movi    a2, 0x600C4060          /* EXTMEM_ICACHE_CTRL */
    movi    a3, 1
    s32i    a3, a2, 0               /* 使能 icache，0x42 窗口即刻可取指 */
#endif

    /* 尽早关看门狗：QEMU/真机复位后 TIMG0/TIMG1/RTC WDT 均为启用态。
       注意：此时 .bss 尚未清零，wdt_early_disable 内不得使用任何静态变量 */
#if TUDOU_PLATFORM != 0 && TUDOU_HW_XIP != 0
    /* 真机实证：数据读触发的 icache 填充可用、取指触发的填充路径异常
       （'X' 实验唯一成功场景 = 取指前有对同址的 0x42 数据读）。启动时
       用数据读预烫全部 IROM 行（每 16B 一读，~74KB 仅数千次 l32i）。 */
    movi    a6, _irom_start
    movi    a4, _irom_end
.Lprime_loop:
    l32i    a5, a6, 0               /* 数据读 → 填充 icache 行 */
    addi    a6, a6, 16
    bltu    a6, a4, .Lprime_loop
#endif
    call0   wdt_early_disable
    BOOTCH  87                  /* 'W'：看门狗已关 */

    /* 清零 .bss */
    movi    a2, _bss_start
    movi    a3, _bss_end
    movi    a4, 0
    BOOTCH  67                  /* 'C'：开始清 bss */
.Lclear_bss:
    bgeu    a2, a3, .Lbss_done
    s32i    a4, a2, 0
    addi    a2, a2, 4
    j       .Lclear_bss
.Lbss_done:
    BOOTCH  68                  /* 'D'：bss 清零完成 */

    /* 安装异常/中断向量表（链接脚本保证 .pot.vectors 0x400 对齐） */
    movi    a2, pot_vectors
    wsr.vecbase a2
    esync
    BOOTCH  86                  /* 'V'：向量表就绪 */

    movi    a0, 0
    call0   kernel_main
    BOOTCH  75                  /* 'K'：[异常路径] kernel_main 返回了 */

    /* kernel_main 不应返回；若返回则原地自旋，便于 GDB/QMP 定位 */
.Lhang:
    j       .Lhang
    .size   _start, . - _start

/* 真机取指诊断助手（IROM 侧 hwtest.c 回调；'X' 探针配套，可长期保留） */
    .globl  pot_hw_fetch_ok
    .type   pot_hw_fetch_ok, @function
pot_hw_fetch_ok:
    movi    a7, 0x60000000          /* UART0 THR */
    movi    a11, 88                 /* 'X' */
1:
    l32i    a12, a7, 0x1c
    extui   a12, a12, 16, 10
    movi    a8, 63
    bltu    a8, a12, 1b
    s32i    a11, a7, 0
    ret
    .size   pot_hw_fetch_ok, . - pot_hw_fetch_ok
