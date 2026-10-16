#include <stddef.h>
#include <stdint.h>

#include "heap.h"
#include "json.h"
#include "lib_shim.h"

/*
 * 迷你 JSON 实现：递归下降解析 + 转义序列化。
 * 所有节点/字符串经 pot_heap_alloc 分配（tag 高 16 位标识来源便于追踪）。
 */

#define JSON_TAG 0x4501u

typedef struct {
    const char *p;
    int depth;
} jctx_t;

#define JSON_MAX_DEPTH 8

static void skip_ws(const char **pp)
{
    while (**pp == ' ' || **pp == '\t' || **pp == '\r' || **pp == '\n') {
        (*pp)++;
    }
}

static pot_json_t *jnew(uint8_t type)
{
    pot_json_t *v = (pot_json_t *)pot_heap_alloc(sizeof(pot_json_t), JSON_TAG);
    if (v != 0) {
        memset(v, 0, sizeof(*v));
        v->type = type;
    }
    return v;
}

/* 迭代化（D-07/D-19）：next 兄弟链循环处理（长度只受输入行限制），
 * child 深度由解析侧 JSON_MAX_DEPTH=8 封顶，递归栈深有界 */
void pot_json_free(pot_json_t *v)
{
    while (v != 0) {
        pot_json_t *next = v->next;
        pot_json_free(v->child);
        if (v->key != 0) {
            pot_heap_free(v->key);
        }
        if (v->str != 0) {
            pot_heap_free(v->str);
        }
        pot_heap_free(v);
        v = next;
    }
}

/* ---------- 解析 ---------- */

static pot_json_t *parse_value(jctx_t *c);

/* 读 4 位十六进制（\uXXXX 用），成功时前移 *pp 并返回码点 */
static int hex4(const char **pp, uint32_t *out)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        char ch = (*pp)[i];
        uint32_t d;
        if (ch >= '0' && ch <= '9') {
            d = (uint32_t)(ch - '0');
        } else if (ch >= 'a' && ch <= 'f') {
            d = (uint32_t)(ch - 'a') + 10u;
        } else if (ch >= 'A' && ch <= 'F') {
            d = (uint32_t)(ch - 'A') + 10u;
        } else {
            return 0;
        }
        v = (v << 4) | d;
    }
    *pp += 4;
    *out = v;
    return 1;
}

/* UTF-8 编码码点，返回写入字节数（1..4；调用方保证容量） */
static int utf8_put(char *dst, uint32_t cp)
{
    if (cp < 0x80u) {
        dst[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800u) {
        dst[0] = (char)(0xC0u | (cp >> 6));
        dst[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u) {
        dst[0] = (char)(0xE0u | (cp >> 12));
        dst[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        dst[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    dst[0] = (char)(0xF0u | (cp >> 18));
    dst[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    dst[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    dst[3] = (char)(0x80u | (cp & 0x3Fu));
    return 4;
}

static int utf8_len(uint32_t cp)
{
    if (cp < 0x80u) {
        return 1;
    }
    if (cp < 0x800u) {
        return 2;
    }
    if (cp < 0x10000u) {
        return 3;
    }
    return 4;
}

/* 解析 \uXXXX（含代理对合成）。失败返回 0（非法转义按 RFC 8259 拒绝）。
 * 成功时 *pp 前移、*cp 输出码点。 */
static int parse_unicode_escape(const char **pp, uint32_t *cp)
{
    if (!hex4(pp, cp)) {
        return 0;
    }
    if (*cp >= 0xD800u && *cp <= 0xDBFFu) { /* 高代理：必须跟 \uDC00-DFFF */
        const char *r = *pp; /* 此处指向低代理转义的 '\\' */
        if (r[0] == '\\' && r[1] == 'u') {
            const char *h = r + 2; /* 跳过 \u 前缀再读 4 位十六进制 */
            uint32_t lo;
            if (hex4(&h, &lo) && lo >= 0xDC00u && lo <= 0xDFFFu) {
                *cp = 0x10000u + ((*cp - 0xD800u) << 10) + (lo - 0xDC00u);
                *pp = h;
                return 1;
            }
        }
        return 0; /* 孤立高代理（或低代理非法） */
    }
    if (*cp >= 0xDC00u && *cp <= 0xDFFFu) {
        return 0; /* 孤立低代理 */
    }
    return 1;
}

/* 转义字符集合（\u 单独处理） */
static int is_known_escape(char ch)
{
    return ch == 'b' || ch == 'f' || ch == 'n' || ch == 'r' || ch == 't' ||
           ch == '"' || ch == '\\' || ch == '/';
}

static char escape_value(char ch)
{
    switch (ch) {
    case 'b': return '\b';
    case 'f': return '\f';
    case 'n': return '\n';
    case 'r': return '\r';
    case 't': return '\t';
    default:  return ch; /* " \\ / */
    }
}

static char *parse_string_raw(jctx_t *c)
{
    if (*c->p != '"') {
        return 0;
    }
    c->p++;
    /* 第一趟：折算转义后的 UTF-8 字节数，并整体校验转义合法性 */
    uint32_t n = 0;
    const char *q = c->p;
    while (*q != '"' && *q != '\0') {
        if (*q == '\\') {
            q++;
            if (*q == '\0') {
                return 0;
            }
            if (*q == 'u') {
                q++;
                uint32_t cp;
                if (!parse_unicode_escape(&q, &cp)) {
                    return 0;
                }
                n += (uint32_t)utf8_len(cp);
                continue;
            }
            if (!is_known_escape(*q)) {
                return 0; /* 未知转义：RFC 8259 拒绝 */
            }
            n++;
            q++;
            continue;
        }
        q++;
        n++;
    }
    if (*q != '"') {
        return 0;
    }
    char *s = (char *)pot_heap_alloc(n + 1, JSON_TAG);
    if (s == 0) {
        return 0;
    }
    /* 第二趟：按第一趟的折算规则拷贝 */
    uint32_t i = 0;
    while (*c->p != '"' && i < n) {
        char ch = *c->p;
        if (ch == '\\') {
            c->p++;
            if (*c->p == 'u') {
                c->p++;
                uint32_t cp;
                if (!parse_unicode_escape(&c->p, &cp)) {
                    pot_heap_free(s);
                    return 0; /* 理论不可达：第一趟已校验 */
                }
                i += (uint32_t)utf8_put(&s[i], cp);
                continue;
            }
            ch = escape_value(*c->p);
        }
        s[i] = ch;
        i++;
        c->p++;
    }
    s[i] = '\0';
    if (*c->p != '"') {
        pot_heap_free(s);
        return 0;
    }
    c->p++;
    return s;
}

static pot_json_t *parse_object(jctx_t *c)
{
    c->p++; /* { */
    pot_json_t *obj = jnew(POT_JSON_OBJ);
    if (obj == 0) {
        return 0;
    }
    pot_json_t **tail = &obj->child;
    skip_ws(&c->p);
    if (*c->p == '}') {
        c->p++;
        return obj;
    }
    for (;;) {
        skip_ws(&c->p);
        char *key = parse_string_raw(c);
        if (key == 0) {
            pot_json_free(obj);
            return 0;
        }
        skip_ws(&c->p);
        if (*c->p != ':') {
            pot_heap_free(key);
            pot_json_free(obj);
            return 0;
        }
        c->p++;
        skip_ws(&c->p);
        pot_json_t *val = parse_value(c);
        if (val == 0) {
            pot_heap_free(key);
            pot_json_free(obj);
            return 0;
        }
        val->key = key;
        *tail = val;
        tail = &val->next;
        skip_ws(&c->p);
        if (*c->p == ',') {
            c->p++;
            continue;
        }
        if (*c->p == '}') {
            c->p++;
            return obj;
        }
        pot_json_free(obj);
        return 0;
    }
}

static pot_json_t *parse_array(jctx_t *c)
{
    c->p++; /* [ */
    pot_json_t *arr = jnew(POT_JSON_ARR);
    if (arr == 0) {
        return 0;
    }
    pot_json_t **tail = &arr->child;
    skip_ws(&c->p);
    if (*c->p == ']') {
        c->p++;
        return arr;
    }
    for (;;) {
        skip_ws(&c->p);
        pot_json_t *val = parse_value(c);
        if (val == 0) {
            pot_json_free(arr);
            return 0;
        }
        *tail = val;
        tail = &val->next;
        skip_ws(&c->p);
        if (*c->p == ',') {
            c->p++;
            continue;
        }
        if (*c->p == ']') {
            c->p++;
            return arr;
        }
        pot_json_free(arr);
        return 0;
    }
}

static pot_json_t *parse_value(jctx_t *c)
{
    if (c->depth >= JSON_MAX_DEPTH) {
        return 0;
    }
    c->depth++;
    skip_ws(&c->p);
    pot_json_t *v = 0;
    const char *start = c->p;

    if (*c->p == '{') {
        v = parse_object(c);
    } else if (*c->p == '[') {
        v = parse_array(c);
    } else if (*c->p == '"') {
        char *s = parse_string_raw(c);
        if (s != 0) {
            v = jnew(POT_JSON_STR);
            if (v != 0) {
                v->str = s;
            } else {
                pot_heap_free(s);
            }
        }
    } else if (c->p[0] == 't' && c->p[1] == 'r' && c->p[2] == 'u' &&
               c->p[3] == 'e') {
        v = jnew(POT_JSON_BOOL);
        if (v != 0) {
            v->num = 1;
        }
        c->p += 4;
    } else if (c->p[0] == 'f' && c->p[1] == 'a' && c->p[2] == 'l' &&
               c->p[3] == 's' && c->p[4] == 'e') {
        v = jnew(POT_JSON_BOOL);
        c->p += 5;
    } else if (c->p[0] == 'n' && c->p[1] == 'u' && c->p[2] == 'l' &&
               c->p[3] == 'l') {
        v = jnew(POT_JSON_NULL);
        c->p += 4;
    } else if (*c->p == '-' || (*c->p >= '0' && *c->p <= '9')) {
        int neg = 0;
        if (*c->p == '-') {
            neg = 1;
            c->p++;
        }
        if (!(*c->p >= '0' && *c->p <= '9')) {
            c->depth--;
            return 0;
        }
        long n = 0;
        while (*c->p >= '0' && *c->p <= '9') {
            n = n * 10 + (*c->p - '0');
            c->p++;
        }
        v = jnew(POT_JSON_NUM);
        if (v != 0) {
            v->num = neg ? -n : n;
        }
    }

    if (v == 0 && c->p == start) {
        c->depth--;
        return 0; /* 无进展 = 非法 */
    }
    c->depth--;
    return v;
}

pot_json_t *pot_json_parse(const char *s)
{
    jctx_t c = { s, 0 };
    pot_json_t *v = parse_value(&c);
    if (v == 0) {
        return 0;
    }
    skip_ws(&c.p);
    if (*c.p != '\0') {
        pot_json_free(v);
        return 0; /* 尾部垃圾 */
    }
    return v;
}

const pot_json_t *pot_json_get(const pot_json_t *obj, const char *key)
{
    if (obj == 0 || obj->type != POT_JSON_OBJ) {
        return 0;
    }
    for (const pot_json_t *m = obj->child; m != 0; m = m->next) {
        if (m->key != 0 && strcmp(m->key, key) == 0) {
            return m;
        }
    }
    return 0;
}

const pot_json_t *pot_json_at(const pot_json_t *arr, int index)
{
    if (arr == 0 || arr->type != POT_JSON_ARR) {
        return 0;
    }
    int i = 0;
    for (const pot_json_t *m = arr->child; m != 0; m = m->next) {
        if (i == index) {
            return m;
        }
        i++;
    }
    return 0;
}

/* ---------- 序列化 ---------- */

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    int overflow;
} wctx_t;

static void wputc(wctx_t *w, char ch)
{
    if (w->len + 1 < w->cap) {
        w->buf[w->len] = ch;
    } else {
        w->overflow = 1;
    }
    w->len++;
}

static void wputs(wctx_t *w, const char *s)
{
    while (*s != '\0') {
        wputc(w, *s++);
    }
}

static void wnum(wctx_t *w, long n)
{
    char tmp[12];
    volatile int i = 0;
    unsigned long u = (n < 0) ? (unsigned long)(-n) : (unsigned long)n;
    if (n < 0) {
        wputc(w, '-');
    }
    do {
        tmp[i] = (char)('0' + (int)(u % 10));
        u /= 10;
        i++;
    } while (u != 0 && i < 11);
    while (i > 0) {
        i--;
        wputc(w, tmp[i]);
    }
}

static void wstr_escaped(wctx_t *w, const char *s)
{
    wputc(w, '"');
    while (*s != '\0') {
        if (*s == '"' || *s == '\\') {
            wputc(w, '\\');
            wputc(w, *s);
        } else if (*s == '\n') {
            wputs(w, "\\n");
        } else if ((unsigned char)*s < 0x20) {
            wputs(w, "\\u");
            wputc(w, '0');
            wputc(w, '0');
            wputc(w, "0123456789abcdef"[(*s >> 4) & 0xF]);
            wputc(w, "0123456789abcdef"[*s & 0xF]);
        } else {
            wputc(w, *s);
        }
        s++;
    }
    wputc(w, '"');
}

static void write_value(wctx_t *w, const pot_json_t *v)
{
    if (v == 0) {
        wputs(w, "null");
        return;
    }
    switch (v->type) {
    case POT_JSON_NULL:
        wputs(w, "null");
        break;
    case POT_JSON_BOOL:
        wputs(w, v->num != 0 ? "true" : "false");
        break;
    case POT_JSON_NUM:
        wnum(w, v->num);
        break;
    case POT_JSON_STR:
        wstr_escaped(w, v->str != 0 ? v->str : "");
        break;
    case POT_JSON_ARR:
        wputc(w, '[');
        for (const pot_json_t *m = v->child; m != 0; m = m->next) {
            write_value(w, m);
            if (m->next != 0) {
                wputc(w, ',');
            }
        }
        wputc(w, ']');
        break;
    case POT_JSON_OBJ:
        wputc(w, '{');
        for (const pot_json_t *m = v->child; m != 0; m = m->next) {
            wstr_escaped(w, m->key != 0 ? m->key : "");
            wputc(w, ':');
            write_value(w, m);
            if (m->next != 0) {
                wputc(w, ',');
            }
        }
        wputc(w, '}');
        break;
    default:
        wputs(w, "null");
        break;
    }
}

size_t pot_json_write(const pot_json_t *v, char *buf, size_t bufsz)
{
    wctx_t w = { buf, bufsz, 0, 0 };
    if (bufsz == 0) {
        return 0;
    }
    write_value(&w, v);
    if (w.overflow || w.len >= w.cap) {
        return 0;
    }
    buf[w.len] = '\0';
    return w.len;
}
