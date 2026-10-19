#ifndef TUDOU_POTFS_H
#define TUDOU_POTFS_H

#include <stddef.h>
#include <stdint.h>

/*
 * 土豆文件系统（M6）——RAM 扁平命名 blob 表（D-25 定位澄清）。
 *
 * 设计：静态元数据表（名称/大小/数据指针）+ M4 堆承载数据。
 * 12 文件 / 15 字符名 / 无目录 / 无持久化（RAM 盘语义，上电即空）。
 * 写采用写时复制语义：先分配新缓冲并拷贝，成功后才释放旧缓冲——
 * 分配失败时旧文件原样保留（"写失败"不再等于"删文件"）。
 *
 * 并发不变量（D-25）：所有入口内部无让出点（无 sleep/yield，堆分配
 * 永不阻塞），在当前协作式检查点调度下 console（UI）与 mcp（AI）
 * 任务天然互斥；引入真机抢占后必须补临界区（D-04 批次）。
 */

#define POTFS_MAX_FILES 12
#define POTFS_NAME_MAX 16

typedef struct {
    char name[POTFS_NAME_MAX];
    uint32_t size;
    uint8_t *data;  /* 堆分配 */
    uint8_t used;
} potfs_file_t;

void potfs_format(void);

/* 整写（存在即替换，写时复制）；返回写入长度，失败返回 -1（表满/名字长/
 * 分配失败——失败时旧内容原样保留） */
int potfs_write(const char *name, const char *buf, uint32_t len);

/* 读到 buf（至多 maxlen）；返回实际长度，不存在返回 -1 */
int potfs_read(const char *name, char *buf, uint32_t maxlen);

/* 查询文件实际大小（truncated 判定用）；不存在返回 0 */
uint32_t potfs_size(const char *name);

int potfs_delete(const char *name);           /* 0=成功 */
const potfs_file_t *potfs_get(int index);     /* 枚举，越界 NULL */
int potfs_count(void);
uint32_t potfs_total_bytes(void);

#endif /* TUDOU_POTFS_H */
