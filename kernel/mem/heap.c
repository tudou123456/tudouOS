#include <stdint.h>

#include "heap.h"
#include "potato.h"
#include "trace.h"

/*
 * 轻量堆实现（M4）。
 *
 * 块布局（总尺寸为 8 的倍数，最小 32）：
 *   [头 16B: magic|size|tag|next] [用户数据（8 对齐）] [尾 8B: canary|self]
 * 空闲链：按块总尺寸 2 的幂分 15 桶（覆盖 16B..128K+），位图定位首个
 * 非空桶，桶内首适配；分裂大块、邻块即时双向合并。
 */

extern char _heap_start[], _heap_end[];

#define H_MAGIC_ALLOC 0xA110CA7Eu
#define H_MAGIC_FREE  0xF7EE810Cu
#define H_CANARY_BASE 0xC0FFEE42u

#define H_ALIGN     8u
#define H_HDR       16u
#define H_FOOT      8u
#define H_MIN_BLOCK 32u
#define H_NBUCKETS  15u /* 桶 i 覆盖 [16<<i, 16<<(i+1)) */

typedef struct hblock {
    uint32_t magic;
    uint32_t size; /* 含头尾，8 的倍数 */
    uint32_t tag;
    struct hblock *next; /* 空闲链（分配态为 NULL） */
} hblock_t;

typedef struct {
    uint32_t canary;
    hblock_t *self;
} hfoot_t; /* 8B，位于块尾 */

static hblock_t *g_bucket[H_NBUCKETS];
static uint32_t g_bitmap; /* bit i = 桶 i 非空 */

static uint8_t *g_heap_lo, *g_heap_hi;

static pot_heap_stat_t g_stat;

static inline hfoot_t *foot_of(hblock_t *b)
{
    return (hfoot_t *)((uint8_t *)b + b->size - H_FOOT);
}

static inline uint32_t canary_of(hblock_t *b)
{
    return H_CANARY_BASE ^ (uint32_t)b ^ b->size;
}

/* 桶号：floor(log2(size))-4，钳到最大桶（volatile 循环禁 loop 指令） */
static uint32_t bucket_of(uint32_t size)
{
    uint32_t idx = 0;
    uint32_t bound = 32u; /* 桶 0 上界 16<<1 */
    while (idx < H_NBUCKETS - 1u && size > bound) {
        idx++;
        bound <<= 1;
    }
    return idx;
}

/* 位图循环移除干扰位后再扫描（volatile 索引禁 loop 指令） */
static void bitmap_set(uint32_t i)
{
    g_bitmap |= (UINT32_C(1) << i);
}

static void bitmap_update(uint32_t i)
{
    if (g_bucket[i] == 0) {
        g_bitmap &= ~(UINT32_C(1) << i);
    } else {
        bitmap_set(i);
    }
}

static void free_insert(hblock_t *b)
{
    uint32_t i = bucket_of(b->size);
    b->next = g_bucket[i];
    g_bucket[i] = b;
    bitmap_set(i);
}

static void free_remove(hblock_t *b)
{
    uint32_t i = bucket_of(b->size);
    hblock_t **pp = &g_bucket[i];
    while (*pp != 0) {
        if (*pp == b) {
            *pp = b->next;
            b->next = 0;
            bitmap_update(i);
            return;
        }
        pp = &(*pp)->next;
    }
}

/* 首适配：从 need 桶起向上找首个足够块 */
static hblock_t *find_block(uint32_t need)
{
    uint32_t start = bucket_of(need);
    for (volatile uint32_t i = start; i < H_NBUCKETS; i++) {
        if ((g_bucket[i]) == 0) {
            continue;
        }
        hblock_t *b = g_bucket[i];
        while (b != 0) {
            if (b->size >= need) {
                return b;
            }
            b = b->next;
        }
    }
    return 0;
}

/* 大块分裂：尾部成新空闲块 */
static void split_block(hblock_t *b, uint32_t need)
{
    if (b->size - need < H_MIN_BLOCK + H_ALIGN) {
        return; /* 残块太小不分裂 */
    }
    hblock_t *tail = (hblock_t *)((uint8_t *)b + need);
    tail->size = b->size - need;
    tail->tag = 0;
    tail->magic = H_MAGIC_FREE;
    foot_of(tail)->self = tail;
    foot_of(tail)->canary = canary_of(tail);
    b->size = need;
    free_insert(tail);
}

void pot_heap_init(void)
{
    uint32_t lo = ((uint32_t)_heap_start + H_ALIGN - 1u) & ~(H_ALIGN - 1u);
    uint32_t hi = (uint32_t)_heap_end & ~(H_ALIGN - 1u);

    for (volatile int i = 0; i < H_NBUCKETS; i++) {
        g_bucket[i] = 0;
    }
    g_bitmap = 0;
    g_stat.total = hi - lo;
    g_stat.used = 0;
    g_stat.largest = g_stat.total;
    g_stat.max_used = 0;
    g_stat.n_alloc = 0;
    g_stat.n_free = 0;
    g_stat.n_fail = 0;
    g_stat.n_fault = 0;
    g_heap_lo = (uint8_t *)lo;
    g_heap_hi = (uint8_t *)hi;

    hblock_t *b = (hblock_t *)lo;
    b->magic = H_MAGIC_FREE;
    b->size = hi - lo;
    b->tag = 0;
    foot_of(b)->self = b;
    foot_of(b)->canary = canary_of(b);
    free_insert(b);
}

void *pot_heap_alloc(uint32_t size, uint32_t tag)
{
    if (size == 0u) {
        size = 1u;
    }
    uint32_t payload = (size + H_ALIGN - 1u) & ~(H_ALIGN - 1u);
    uint32_t need = H_HDR + payload + H_FOOT;
    if (need < H_MIN_BLOCK) {
        need = H_MIN_BLOCK;
    }

    hblock_t *b = find_block(need);
    if (b == 0) {
        g_stat.n_fail++;
        return 0;
    }
    free_remove(b);
    split_block(b, need);

    b->magic = H_MAGIC_ALLOC;
    b->tag = tag;
    b->next = 0;
    foot_of(b)->self = b;
    foot_of(b)->canary = canary_of(b);

    g_stat.used += b->size;
    if (g_stat.used > g_stat.max_used) {
        g_stat.max_used = g_stat.used;
    }
    g_stat.n_alloc++;
    trace_push(TR_MEM_ALLOC, b->size, tag);
    return (void *)((uint8_t *)b + H_HDR);
}

void pot_heap_free(void *ptr)
{
    if (ptr == 0) {
        return;
    }
    hblock_t *b = (hblock_t *)((uint8_t *)ptr - H_HDR);

    if (b->magic != H_MAGIC_ALLOC) {
        g_stat.n_fault++;
        trace_push(TR_MEM_FAULT, 2 /* bad-free */, b->tag);
        return; /* 非法/重复释放：吞掉并计数 */
    }

    hfoot_t *f = foot_of(b);
    if (f->canary != canary_of(b) || f->self != b) {
        g_stat.n_fault++;
        trace_push(TR_MEM_FAULT, 1 /* overflow */, b->tag);
        /* footer 被踩：头信息仍可信（尺寸来自头），继续回收 */
    }

    uint32_t asize = b->size; /* 原块尺寸：记账用（合并前） */
    uint32_t bsize = b->size;

    /* 合并下一个空闲邻块（头信息判定，不依赖被踩的 footer） */
    hblock_t *n = (hblock_t *)((uint8_t *)b + bsize);
    if ((uint8_t *)n + H_MIN_BLOCK <= g_heap_hi &&
        n->magic == H_MAGIC_FREE && n->size >= H_MIN_BLOCK &&
        (uint8_t *)n + n->size <= g_heap_hi) {
        free_remove(n);
        bsize += n->size;
    }

    /* 合并上一个空闲邻块（上块的 footer 自指校验） */
    if ((uint8_t *)b - H_FOOT >= g_heap_lo) {
        hfoot_t *pf = (hfoot_t *)((uint8_t *)b - H_FOOT);
        hblock_t *pb = pf->self;
        if ((uint8_t *)pb >= g_heap_lo && pb->magic == H_MAGIC_FREE &&
            foot_of(pb)->self == pb && (uint8_t *)pb + pb->size == (uint8_t *)b) {
            free_remove(pb);
            pb->size += bsize;
            b = pb;
            bsize = pb->size;
        }
    }

    b->magic = H_MAGIC_FREE;
    b->size = bsize;
    b->tag = 0;
    foot_of(b)->self = b;
    foot_of(b)->canary = canary_of(b);
    free_insert(b);

    g_stat.used -= asize;
    g_stat.n_free++;
    trace_push(TR_MEM_FREE, bsize, b->tag);
}

void pot_heap_getstat(pot_heap_stat_t *out)
{
    *out = g_stat;
    /* largest 实时重算：扫位图取最大非空桶首块近似（够用于度量） */
    out->largest = 0;
    for (volatile int i = H_NBUCKETS - 1; i >= 0; i--) {
        if (g_bucket[i] != 0) {
            out->largest = g_bucket[i]->size;
            break;
        }
    }
}
