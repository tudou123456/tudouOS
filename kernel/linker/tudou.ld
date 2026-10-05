/*
 * tudouOS 土豆内核 M1 链接脚本 — ESP32-S3
 *
 * M1 阶段内核全 RAM 运行（不做 flash XIP），由片上 ROM 直接把镜像的
 * 各段加载到下面的地址，因此 LMA == VMA，.data 无需运行时拷贝。
 *
 * 地址依据（取保守子集，避开 ROM/引导加载器/缓存占用区）：
 *   IRAM: 0x40370000 - 0x403F0000（S3 SRAM 的指令视图，512KB）
 *   DRAM: 0x3FC88000 - 0x3FD08000（同一块 SRAM 的数据视图）
 *   ESP-IDF 应用可用窗口为 IRAM 0x40378000-0x403CB700 /
 *   DRAM 0x3FC88000-0x3FCBB700，本脚本取其中一段更保守的区间。
 */

ENTRY(_start)

MEMORY
{
    iram (rx)  : ORIGIN = 0x40380000, LENGTH = 0x40000  /* 256KB, 代码+只读数据 */
    dram (rwx) : ORIGIN = 0x3FC88000, LENGTH = 0x28000  /* 160KB, 数据+栈+堆 */
}

SECTIONS
{
    .text : ALIGN(4)
    {
        _text_start = .;
        KEEP(*(.text.start))          /* boot.S 的 _start 必须在最前 */
        *(.text .text.* .literal .literal.* .rodata .rodata.*)
        . = ALIGN(4);
        _text_end = .;
    } > iram

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
