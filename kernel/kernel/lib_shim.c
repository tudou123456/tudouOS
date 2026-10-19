#include <stddef.h>
#include <stdint.h>

#include "lib_shim.h"

/*
 * 说明：循环用 volatile 索引源于一次误诊（曾认为该 QEMU 不支持零开销
 * loop 指令）。实测（console looptest + test_loop_instruction）证伪：
 * loopnez 长循环在任务切换与 RX 中断风暴下 2000+ 轮零失配，镜像中
 * 编译器生成的 loop 指令也长期正常执行；保留 volatile 写法无害。
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
