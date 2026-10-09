#ifndef TUDOU_JSON_H
#define TUDOU_JSON_H

#include <stddef.h>
#include <stdint.h>

/*
 * 迷你 JSON（M6，MCP 适配器的编解码层）
 *
 * 设计约束：裸机无 libc；节点经 M4 堆分配；解析为递归下降；
 * 数字只支持 32 位整数（MCP 控制面够用）；计数循环 volatile 索引。
 */

enum {
    POT_JSON_NULL = 0,
    POT_JSON_BOOL,
    POT_JSON_NUM,
    POT_JSON_STR,
    POT_JSON_ARR,
    POT_JSON_OBJ,
};

typedef struct pot_json {
    uint8_t type;
    char *key;                /* 对象成员名（非成员为 NULL） */
    long num;                 /* BOOL/NUM */
    char *str;                /* STR */
    struct pot_json *child;   /* OBJ/ARR 首个子节点 */
    struct pot_json *next;    /* 兄弟节点 */
} pot_json_t;

/* 解析失败返回 NULL（整棵树已释放） */
pot_json_t *pot_json_parse(const char *s);
void pot_json_free(pot_json_t *v);

/* 对象取成员 / 数组取下标 */
const pot_json_t *pot_json_get(const pot_json_t *obj, const char *key);
const pot_json_t *pot_json_at(const pot_json_t *arr, int index);

/* 序列化（含转义）；返回写入长度，buf 不足返回 0 */
size_t pot_json_write(const pot_json_t *v, char *buf, size_t bufsz);

#endif /* TUDOU_JSON_H */
