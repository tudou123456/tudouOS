/*
 * tudouOS 土豆内核 M9 链接脚本 — ESP32-S3
 *
 * 本文件由 tools/build.py 先用 cpp 预处理（-DTUDOU_PLATFORM / -DTUDOU_HW_XIP
 * 注入，-P 去行号），再交给链接器；#if 选择两套布局：
 *
 *   XIP 布局（QEMU 默认；或 hw --xip 实验）
 *     IRAM  0x40380000  向量表 + 切换/异常汇编 + 汇编字面量（必须在 RAM）
 *     DRAM  0x3FC88000  数据/BSS/栈/堆
 *     IROM  0x42000020  主体代码 + 只读数据（flash 映射执行）
 *
 *   全 RAM 布局（hw 默认）
 *     IRAM  0x40380000  向量表 + 汇编 + 全部 C 代码 + 只读数据
 *     DRAM  0x3FC88000  数据/BSS/栈/堆
 *     无 IROM：真机 0x42 flash 取指路径不可用（带板实证），ROM 把 RAM 段搬到链接地址后直接执行，
 *     不依赖 MMU/icache。QEMU 模型可取指，保留 XIP 以贴合 flash 语义。
 *
 * 地址依据（保守子集，避开 ROM/引导加载器占用区，同 M1 结论）。
 */

ENTRY(_start)

MEMORY
{
#if TUDOU_PLATFORM == 0 || TUDOU_HW_XIP != 0
    iram (rx)  : ORIGIN = 0x40380000, LENGTH = 0x40000  /* 256KB */
    /* 192KB：数据+栈+堆。M9 起放宽（原 0x28000=160KB）：esp32s3 的
     * D/IRAM(D) 区 0x3FC88000–0x3FCF0000 共 416KB，ESP-IDF 的
     * dram0_0_seg 到 0x3FCDC700（346KB）可用；M7 以太网 DMA 缓冲（~12KB）
     * 与 M9 CPU1 栈（4KB）使 160KB 布局下堆跌破 64KB 断言，故扩到
     * 192KB（堆 end 0x3FCB8000，仍在 D/IRAM 边界与 ESP-IDF 数据段之内）。 */
    dram (rwx) : ORIGIN = 0x3FC88000, LENGTH = 0x30000
#else
    /* 全 RAM 布局（hw 默认）：真机 SRAM 在指令/数据总线上是两个别名窗口，
     * 别名关系 instr = data + 0x6F0000（带板实证：bss 清零到 0x3FC904F0 时
     * 正好清掉正在执行的 0x403804F0 指令 → cause 0 IllegalInstruction）。
     * 因此数据段必须放在 IRAM 末端别名之上，两区间不得重叠：
     *   iram 0x40380000..0x40398000  ↔  data 0x3FC90000..0x3FCA8000（禁用）
     * 数据窗从 0x3FCA8000（别名 0x40398000）起，到 ESP-IDF dram0_0_seg
     * 末端 0x3FCDC700 止，共 0x34700=215KB（数据+BSS+栈+堆）。 */
    iram (rx)  : ORIGIN = 0x40380000, LENGTH = 0x18000  /* 96KB 代码 */
    dram (rwx) : ORIGIN = 0x3FCA8000, LENGTH = 0x34700  /* 215KB */
#endif
    irom (r)   : ORIGIN = 0x42000020, LENGTH = 0x100000 /* VMA：代码+只读数据 */
}

SECTIONS
{
    /* ---- IRAM：向量表必须最前且 0x400 对齐 ---- */
    .pot_vectors : ALIGN(0x400)
    {
        pot_vectors = .;
        KEEP(*(.pot.vectors))
        . = ALIGN(0x400); /* 表长固定 0x400 */
    } > iram

    /* ---- IRAM：汇编字面量（l32r 向后取，须在引用代码之前的低地址） ---- */
    .iram_literal : ALIGN(4)
    {
        *(.literal .literal.*)
        . = ALIGN(4);
    } > iram

#if TUDOU_PLATFORM == 0 || TUDOU_HW_XIP != 0
    /* ================= XIP 布局 ================= */

    /* ---- IRAM：启动代码 + 异常/切换汇编体（generic .text 只来自 .S 文件：
               C 全部 -ffunction-sections 进 .text.*，归 IROM） ---- */
    .iram_text : ALIGN(4)
    {
        _iram_start = .;
        KEEP(*(.text.start))          /* boot.S 的 _start 必须在最前 */
        *(.text)                      /* vectors.S/switch.S 处理体 */
        . = ALIGN(4);
        _iram_end = .;
    } > iram

    /* ---- IROM：全部 C 代码 + 只读数据（auto-litpools 字面量池内联随行）。
               VMA 在 0x42 映射窗口，LMA 固定落 flash 0x10020（64KB 页 1，
               与 VMA 页内偏移 0x20 对齐）；boot.S 据此映射 MMU ---- */
    .irom_text 0x42000020 : AT(0x00010020)
    {
        _irom_start = .;
        *(.text .text.*)
        *(.rodata .rodata.*)

        . = ALIGN(4);
        __pot_drivers_start = .;
        KEEP(*(.pot_drivers))
        . = ALIGN(4);
        __pot_drivers_end = .;

        . = ALIGN(4);
        _irom_end = .;
    } > irom

    /* 自研二级映射引导参数：映射页数（向上取整 64KB）与起始物理页号 */
    _irom_pages = ((_irom_end - _irom_start) + 0xFFFF) >> 16;
    _irom_flash_page = 1; /* LMA 0x10020 >> 16 */
#else
    /* ================= 全 RAM 布局（hw 默认） ================= */

    /* ---- IRAM：仅放代码（以及启动向量表/字面量池）。
               真机 S3 的 IRAM（0x4038xxxx，指令总线）不能用普通 load/store
               按数据访问——只有 l32r（PC 相对字面量读）与取指可读。因此
               .rodata 字符串/表、.pot_drivers 表这些“当数据读”的常量必须
               放 DRAM，否则第一次读就异常（带板实证：driver 表排序读
               priority 时挂死）。 ---- */
    .iram_text : ALIGN(4)
    {
        _iram_start = .;
        KEEP(*(.text.start))          /* boot.S 的 _start 必须在最前 */
        *(.text .text.*)
        . = ALIGN(4);
        _iram_end = .;
    } > iram

    /* ---- DRAM：只读数据 + 驱动注册表（数据总线可读） ---- */
    .rodata : ALIGN(16)
    {
        _rodata_start = .;
        *(.rodata .rodata.*)
        . = ALIGN(4);
        _rodata_end = .;
    } > dram

    .pot_drivers_seg : ALIGN(4)
    {
        __pot_drivers_start = .;
        KEEP(*(.pot_drivers))
        . = ALIGN(4);
        __pot_drivers_end = .;
    } > dram

    _irom_start = 0;      /* 全 RAM 布局无 flash 段（诊断打印占位） */
    _irom_end = 0;
    _irom_pages = 0;
    _irom_flash_page = 1;
#endif

    .data : ALIGN(4)
    {
        _data_start = .;
        *(.data .data.*)
        . = ALIGN(4);
        _data_end = .;
    } > dram

    /* NOLOAD: ROM 只搬运 .data，.bss 由 boot.S 清零 */
    .bss (NOLOAD) : ALIGN(4)
    {
        _bss_start = .;
        *(.bss .bss.* COMMON)
        . = ALIGN(8);
        _bss_end = .;
    } > dram

    .stack (NOLOAD) : ALIGN(16)
    {
        _stack_bottom = .;
        . += 0x4000;                  /* 16KB 启动栈 */
        _stack_top = .;
    } > dram

    _heap_start = _stack_top;
    _heap_end   = ORIGIN(dram) + LENGTH(dram);

    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
        *(.gnu.attributes)
        *(.xtensa.*)
    }
}
