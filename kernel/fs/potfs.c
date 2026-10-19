#include <stdint.h>

#include "heap.h"
#include "intc.h"
#include "lib_shim.h"
#include "potfs.h"

/*
 * potfs 实现：静态元数据表 + 堆数据。M7+ 换 flash 后端时保持接口。
 */

#define FS_TAG 0xF511u

static potfs_file_t g_files[POTFS_MAX_FILES];

/* P0-3/P0-4：文件表 + 数据缓冲生命周期是一个整体，读写两侧都必须跨核
 * 互斥。此前读侧无锁，写侧在临界区内释放旧 data —— 控制台 fs cat /
 * MCP fs.read 与并发写交错时读已释放内存。锁序：fs → heap。 */
static pot_lock_t g_fs_lock;

void potfs_format(void)
{
    uint32_t ps = pot_lock_acquire(&g_fs_lock);
    for (volatile int i = 0; i < POTFS_MAX_FILES; i++) {
        if (g_files[i].used != 0u && g_files[i].data != 0) {
            pot_heap_free(g_files[i].data);
        }
        g_files[i].used = 0u;
        g_files[i].data = 0;
        g_files[i].size = 0u;
        g_files[i].name[0] = '\0';
    }
    pot_lock_release(&g_fs_lock, ps);
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

    /* D-25 写时复制：先备好新缓冲，任何失败路径都不动旧文件状态。
     * P0-3：整个写入序列跨核互斥（console/mcp/另一核并发改写） */
    uint32_t ps = pot_lock_acquire(&g_fs_lock);

    uint8_t *nd = 0;
    if (len != 0u) {
        nd = (uint8_t *)pot_heap_alloc(len, FS_TAG);
        if (nd == 0) {
            pot_lock_release(&g_fs_lock, ps);
            return -1; /* 分配失败：旧内容原样保留 */
        }
        for (volatile uint32_t i = 0; i < len; i++) {
            nd[i] = (uint8_t)buf[i];
        }
    }

    potfs_file_t *f = find_slot(name);
    if (f == 0) {
        f = find_free();
        if (f == 0) {
            if (nd != 0) {
                pot_heap_free(nd);
            }
            pot_lock_release(&g_fs_lock, ps);
            return -1; /* 表满：旧内容不受影响 */
        }
        f->used = 1u;
        for (volatile uint32_t i = 0; i <= n; i++) {
            f->name[i] = name[i];
        }
        f->data = 0;
    }
    if (f->data != 0) {
        pot_heap_free(f->data); /* 持 fs 锁：读侧不会看到悬空 data */
    }
    f->data = nd;
    f->size = len;
    pot_lock_release(&g_fs_lock, ps);
    return (int)len;
}

int potfs_read(const char *name, char *buf, uint32_t maxlen)
{
    uint32_t ps = pot_lock_acquire(&g_fs_lock); /* P0-4：复制期间 data 不得被释放 */
    potfs_file_t *f = find_slot(name);
    if (f == 0) {
        pot_lock_release(&g_fs_lock, ps);
        return -1;
    }
    uint32_t n = (f->size < maxlen) ? f->size : maxlen;
    for (volatile uint32_t i = 0; i < n; i++) {
        buf[i] = (char)f->data[i];
    }
    pot_lock_release(&g_fs_lock, ps);
    return (int)n;
}

uint32_t potfs_size(const char *name)
{
    uint32_t ps = pot_lock_acquire(&g_fs_lock);
    potfs_file_t *f = find_slot(name);
    uint32_t n = (f != 0) ? f->size : 0u;
    pot_lock_release(&g_fs_lock, ps);
    return n;
}

int potfs_delete(const char *name)
{
    uint32_t ps = pot_lock_acquire(&g_fs_lock); /* P0-3 */
    potfs_file_t *f = find_slot(name);
    if (f == 0) {
        pot_lock_release(&g_fs_lock, ps);
        return -1;
    }
    if (f->data != 0) {
        pot_heap_free(f->data);
    }
    f->data = 0;
    f->size = 0u;
    f->used = 0u;
    f->name[0] = '\0';
    pot_lock_release(&g_fs_lock, ps);
    return 0;
}

/* 注意：返回内部表项指针（控制台只读 name/size）。data 指针可能在返回后
 * 被并发写释放——调用方不得解引用 data，读内容请走 potfs_read。 */
const potfs_file_t *potfs_get(int index)
{
    if (index < 0 || index >= POTFS_MAX_FILES) {
        return 0;
    }
    return (g_files[index].used != 0u) ? &g_files[index] : 0;
}

int potfs_count(void)
{
    uint32_t ps = pot_lock_acquire(&g_fs_lock);
    int n = 0;
    for (volatile int i = 0; i < POTFS_MAX_FILES; i++) {
        if (g_files[i].used != 0u) {
            n++;
        }
    }
    pot_lock_release(&g_fs_lock, ps);
    return n;
}

uint32_t potfs_total_bytes(void)
{
    uint32_t ps = pot_lock_acquire(&g_fs_lock);
    uint32_t t = 0;
    for (volatile int i = 0; i < POTFS_MAX_FILES; i++) {
        if (g_files[i].used != 0u) {
            t += g_files[i].size;
        }
    }
    pot_lock_release(&g_fs_lock, ps);
    return t;
}
