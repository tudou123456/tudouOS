#include <stddef.h>
#include <stdint.h>

#include "lib_shim.h"

/*
 * 历史说明：循环用 volatile 索引源于 M3.5 的一次误诊（当时认定该 QEMU
 * 不支持零开销 loop 指令）。2026-10-16 判定实验（E-01，console 的
 * looptest 命令 + test_loop_instruction 用例）证伪：loopnez 长循环在
 * 数百次任务切换与 RX 中断风暴下 2000+ 轮零失配，镜像中编译器生成
 * 的 3 条 loop 指令也在测试路径上长期正常执行。M2 strcmp 崩溃 /
 * M3.5 ack 窗口误命中的真因未明（已排除 loop），留待复发时重查。
 * 现存 volatile 写法正确无害，保持不动；新代码无需沿用。
 */

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    for (volatile size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dst;
}

void *memset(void *s, int c, size_t n)
{
    unsigned char *p = (unsigned char *)s;
    for (volatile size_t i = 0; i < n; i++) {
        p[i] = (unsigned char)c;
    }
    return s;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d < s) {
        for (volatile size_t i = 0; i < n; i++) {
            d[i] = s[i];
        }
    } else if (d > s) {
        for (volatile size_t i = n; i > 0; i--) {
            d[i - 1] = s[i - 1];
        }
    }
    return dst;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (volatile size_t i = 0; i < n; i++) {
        if (a[i] == '\0' || a[i] != b[i]) {
            return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
        }
    }
    return 0;
}
