#ifndef TUDOU_IO_H
#define TUDOU_IO_H

#include <stdint.h>

/* MMIO 读写内联：内核中所有外设寄存器访问统一走这两个函数 */
static inline void mmio_write(uint32_t addr, uint32_t val)
{
    *(volatile uint32_t *)addr = val;
}

static inline uint32_t mmio_read(uint32_t addr)
{
    return *(volatile uint32_t *)addr;
}

#endif /* TUDOU_IO_H */
