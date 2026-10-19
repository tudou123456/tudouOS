#ifndef TUDOU_SB_H
#define TUDOU_SB_H

#include <stddef.h>
#include <stdint.h>

/*
 * 极小字符串构造器（M8）：给 JSONL 导出器/健康快照/网络统计复用。
 * 有界写入，永不越界；写满后静默截断（len 停在 cap-1），调用方可查
 * truncated() 判断是否溢出。裸机无 libc，全部 static inline。
 */

typedef struct {
    char *p;
    uint32_t cap; /* 含结尾 '\0' 的容量 */
    uint32_t len;
} pot_sb_t;

static inline void pot_sb_init(pot_sb_t *b, char *buf, uint32_t cap)
{
    b->p = buf;
    b->cap = cap;
    b->len = 0;
    if (cap > 0u) {
        buf[0] = '\0';
    }
}

static inline int pot_sb_truncated(const pot_sb_t *b)
{
    return (b->len + 1u) >= b->cap;
}

static inline void pot_sb_ch(pot_sb_t *b, char c)
{
    if (b->len + 1u < b->cap) {
        b->p[b->len++] = c;
        b->p[b->len] = '\0';
    }
}

static inline void pot_sb_str(pot_sb_t *b, const char *s)
{
    if (s == NULL) {
        return;
    }
    for (uint32_t i = 0; s[i] != '\0'; i++) {
        pot_sb_ch(b, s[i]);
    }
}

/* 只做最小转义：引号与反斜杠（内核内部字符串不含控制字符） */
static inline void pot_sb_jstr(pot_sb_t *b, const char *s)
{
    pot_sb_ch(b, '"');
    if (s != NULL) {
        for (uint32_t i = 0; s[i] != '\0'; i++) {
            char c = s[i];
            if (c == '"' || c == '\\') {
                pot_sb_ch(b, '\\');
            }
            pot_sb_ch(b, c);
        }
    }
    pot_sb_ch(b, '"');
}

static inline void pot_sb_u32(pot_sb_t *b, uint32_t v)
{
    char tmp[10];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0u && n < (int)sizeof(tmp));
    while (n > 0) {
        pot_sb_ch(b, tmp[--n]);
    }
}

static inline void pot_sb_i32(pot_sb_t *b, int32_t v)
{
    if (v < 0) {
        pot_sb_ch(b, '-');
        pot_sb_u32(b, (uint32_t)(-(int64_t)v));
    } else {
        pot_sb_u32(b, (uint32_t)v);
    }
}

static inline void pot_sb_hex(pot_sb_t *b, uint32_t v)
{
    static const char d[] = "0123456789abcdef";
    pot_sb_ch(b, '0');
    pot_sb_ch(b, 'x');
    for (int i = 28; i >= 0; i -= 4) {
        pot_sb_ch(b, d[(v >> i) & 0xFu]);
    }
}

static inline void pot_sb_kv_u32(pot_sb_t *b, const char *k, uint32_t v, int comma)
{
    if (comma) {
        pot_sb_ch(b, ',');
    }
    pot_sb_jstr(b, k);
    pot_sb_ch(b, ':');
    pot_sb_u32(b, v);
}

static inline void pot_sb_kv_str(pot_sb_t *b, const char *k, const char *v, int comma)
{
    if (comma) {
        pot_sb_ch(b, ',');
    }
    pot_sb_jstr(b, k);
    pot_sb_ch(b, ':');
    pot_sb_jstr(b, v);
}

#endif /* TUDOU_SB_H */
