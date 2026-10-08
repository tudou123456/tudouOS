/*
 * tudouOS 土豆内核 M5 链接脚本 — ESP32-S3，flash XIP
 *
 * M5 起内核主体搬出 RAM：
 *   IRAM  0x40380000  向量表 + 切换/异常汇编（必须在 RAM 且 VECBASE 0x400 对齐）
 *   DRAM  0x3FC88000  数据/BSS/栈/堆（不变）
 *   DROM  0x3C000020  只读数据（flash 经 MMU/cache 映射）
 *   IROM  0x42000020  主体代码（flash 映射执行，摆脱 RAM 容量限制）
 * ROM 加载镜像时对 VMA 落在映射窗口的段自动配置 MMU（bootloader 同机制）。
 *
 * 地址依据（保守子集，避开 ROM/引导加载器占用区，同 M1 结论）。
 */

ENTRY(_start)

MEMORY
{
    iram (rx)  : ORIGIN = 0x40380000, LENGTH = 0x40000  /* 256KB：向量+汇编 */
    dram (rwx) : ORIGIN = 0x3FC88000, LENGTH = 0x28000  /* 160KB：数据+栈+堆 */
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
