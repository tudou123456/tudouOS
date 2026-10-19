#ifndef TUDOU_HEAP_H
#define TUDOU_HEAP_H

#include <stdint.h>

/*
 * 土豆内核轻量堆（长期运行耐用性）
 *
 * 分级空闲表 + 首适配 + 双向合并（工程简化的 TLSF 类）：
 *   - 尺寸分级空闲表：按块总尺寸的 2 的幂分桶（15 桶）+ 位图定位
 *     首个非空桶，桶内首适配；大块分裂、邻块即时双向合并
 *   - 护栏：块尾 8 字节 footer = canary + 回指头；free 时校验，
 *     越界即捕获并计数（不致命）
 *   - 对抗性路径防护：
 *     头部 size 完整性校验不过关 → 块置墓碑隔离（永久泄漏，不入表
 *     不合并）；正常回收前置 DEAD 墓碑，被吞并块的旧地址二次释放
 *     会被捕获计数；超大尺寸请求直接拒绝（防圆整回绕）；上邻合并
 *     的 footer->self 指针做双端范围校验后才解引用
 *   - 度量：used/free/largest、最大使用水位、alloc/free/fail/fault 计数
 *   - 追踪：TR_MEM_ALLOC/FREE/FAULT 事件入追踪环（含来源标签）
 *
 * 约束继承：计数循环 volatile 索引为历史纪律（E-01 已证伪
 * "QEMU 不支持 loop 指令"，见 lib_shim.c 注释，新代码无需沿用）；
 * 热路径无除法。largest 为近似值（最高非空桶首块，非精确最大块）。
 *
 * API 语义：alloc 失败返回 NULL 并计 fail（永不挂起）；free 非法指针
 * 或 canary 破坏计 fault 并吞掉（不崩溃）；size 0 按 1 处理。
 */

void pot_heap_init(void);

void *pot_heap_alloc(uint32_t size, uint32_t tag);
void pot_heap_free(void *ptr);

typedef struct {
    uint32_t total;      /* 堆总字节（8 对齐后） */
    uint32_t used;       /* 当前占用（含头尾开销） */
    uint32_t largest;    /* 最大可用块 */
    uint32_t max_used;   /* 历史最大使用水位 */
    uint32_t n_alloc;
    uint32_t n_free;
    uint32_t n_fail;     /* 分配失败（耗尽/无合适块） */
    uint32_t n_fault;    /* canary/坏块捕获 */
} pot_heap_stat_t;

void pot_heap_getstat(pot_heap_stat_t *out);

#endif /* TUDOU_HEAP_H */
