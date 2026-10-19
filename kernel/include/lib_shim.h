#ifndef TUDOU_LIB_SHIM_H
#define TUDOU_LIB_SHIM_H

#include <stddef.h>

/*
 * freestanding 环境自带的极小 libc 子集。
 * GCC 可能对结构体拷贝等生成 memcpy/memset 隐式调用，必须由我们提供，
 * 否则会链接到窗口 ABI 编译的预编译库，与 call0 ABI 冲突。
 */

void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *s, int c, size_t n);
void *memmove(void *dst, const void *src, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);

#endif /* TUDOU_LIB_SHIM_H */
