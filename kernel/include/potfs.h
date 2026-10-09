#ifndef TUDOU_POTFS_H
#define TUDOU_POTFS_H

#include <stddef.h>
#include <stdint.h>

/*
 * 土豆文件系统（M6）——内存块设备 + 扁平命名空间。
 *
 * 设计：静态元数据表（名称/大小/数据指针）+ M4 堆承载数据。
 * 块设备抽象（potfs_bd_t）为 M7+ 的 flash 后端预留——现在只有
 * RAM 后端，接口不变。上电自动 format（RAM 盘语义）。
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

/* 整写（存在即替换）；返回写入长度，失败返回 -1（表满/名字长/分配失败） */
int potfs_write(const char *name, const char *buf, uint32_t len);

/* 读到 buf（至多 maxlen）；返回实际长度，不存在返回 -1 */
int potfs_read(const char *name, char *buf, uint32_t maxlen);

/* 追加一行（Shell write 命令用）；返回追加后总长，失败 -1 */
int potfs_append(const char *name, const char *buf, uint32_t len);

int potfs_delete(const char *name);           /* 0=成功 */
const potfs_file_t *potfs_get(int index);     /* 枚举，越界 NULL */
int potfs_count(void);
uint32_t potfs_total_bytes(void);

#endif /* TUDOU_POTFS_H */
