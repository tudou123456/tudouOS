#include <stdint.h>

#include "heap.h"
#include "lib_shim.h"
#include "potfs.h"

/*
 * potfs 实现：静态元数据表 + 堆数据。M7+ 换 flash 后端时保持接口。
 */

#define FS_TAG 0xF511u

static potfs_file_t g_files[POTFS_MAX_FILES];

void potfs_format(void)
{
    for (volatile int i = 0; i < POTFS_MAX_FILES; i++) {
        if (g_files[i].used != 0u && g_files[i].data != 0) {
            pot_heap_free(g_files[i].data);
        }
        g_files[i].used = 0u;
        g_files[i].data = 0;
        g_files[i].size = 0u;
        g_files[i].name[0] = '\0';
    }
}

static potfs_file_t *find_slot(const char *name)
{
    for (volatile int i = 0; i < POTFS_MAX_FILES; i++) {
        if (g_files[i].used != 0u && strcmp(g_files[i].name, name) == 0) {
            return &g_files[i];
        }
    }
    return 0;
}

static potfs_file_t *find_free(void)
{
    for (volatile int i = 0; i < POTFS_MAX_FILES; i++) {
        if (g_files[i].used == 0u) {
            return &g_files[i];
        }
    }
    return 0;
}

int potfs_write(const char *name, const char *buf, uint32_t len)
{
    uint32_t n = 0;
    while (name[n] != '\0') {
        n++;
    }
    if (n == 0u || n >= POTFS_NAME_MAX) {
        return -1;
    }

    potfs_file_t *f = find_slot(name);
    if (f == 0) {
        f = find_free();
        if (f == 0) {
            return -1; /* 表满 */
        }
        f->used = 1u;
        for (volatile uint32_t i = 0; i <= n; i++) {
            f->name[i] = name[i];
        }
        f->data = 0;
        f->size = 0u;
    }
    if (f->data != 0) {
        pot_heap_free(f->data);
        f->data = 0;
    }
    if (len != 0u) {
        f->data = (uint8_t *)pot_heap_alloc(len, FS_TAG);
        if (f->data == 0) {
            f->used = 0u;
            f->name[0] = '\0';
            return -1;
        }
        for (volatile uint32_t i = 0; i < len; i++) {
            f->data[i] = (uint8_t)buf[i];
        }
    }
    f->size = len;
    return (int)len;
}

int potfs_append(const char *name, const char *buf, uint32_t len)
{
    potfs_file_t *f = find_slot(name);
    if (f == 0) {
        return potfs_write(name, buf, len);
    }
    uint32_t nsz = f->size + len;
    uint8_t *nd = 0;
    if (nsz != 0u) {
        nd = (uint8_t *)pot_heap_alloc(nsz, FS_TAG);
        if (nd == 0) {
            return -1;
        }
        for (uint32_t i = 0; i < f->size; i++) {
            nd[i] = f->data[i];
        }
        for (volatile uint32_t i = 0; i < len; i++) {
            nd[f->size + i] = (uint8_t)buf[i];
        }
    }
    if (f->data != 0) {
        pot_heap_free(f->data);
    }
    f->data = nd;
    f->size = nsz;
    return (int)nsz;
}

int potfs_read(const char *name, char *buf, uint32_t maxlen)
{
    potfs_file_t *f = find_slot(name);
    if (f == 0) {
        return -1;
    }
    uint32_t n = (f->size < maxlen) ? f->size : maxlen;
    for (volatile uint32_t i = 0; i < n; i++) {
        buf[i] = (char)f->data[i];
    }
    return (int)n;
}

int potfs_delete(const char *name)
{
    potfs_file_t *f = find_slot(name);
    if (f == 0) {
        return -1;
    }
    if (f->data != 0) {
        pot_heap_free(f->data);
    }
    f->data = 0;
    f->size = 0u;
    f->used = 0u;
    f->name[0] = '\0';
    return 0;
}

const potfs_file_t *potfs_get(int index)
{
    if (index < 0 || index >= POTFS_MAX_FILES) {
        return 0;
    }
    return (g_files[index].used != 0u) ? &g_files[index] : 0;
}

int potfs_count(void)
{
    int n = 0;
    for (volatile int i = 0; i < POTFS_MAX_FILES; i++) {
        if (g_files[i].used != 0u) {
            n++;
        }
    }
    return n;
}

uint32_t potfs_total_bytes(void)
{
    uint32_t t = 0;
    for (volatile int i = 0; i < POTFS_MAX_FILES; i++) {
        if (g_files[i].used != 0u) {
            t += g_files[i].size;
        }
    }
    return t;
}
