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

void pot_json_free(pot_json_t *v)
{
    if (v == 0) {
        return;
    }
    pot_json_free(v->child);
    pot_json_free(v->next);
    if (v->key != 0) {
        pot_heap_free(v->key);
    }
    if (v->str != 0) {
        pot_heap_free(v->str);
    }
    pot_heap_free(v);
}

/* ---------- 解析 ---------- */

static pot_json_t *parse_value(jctx_t *c);

static char *parse_string_raw(jctx_t *c)
{
    if (*c->p != '"') {
        return 0;
    }
    c->p++;
    /* 先数长度（含转义折算） */
    uint32_t n = 0;
    const char *q = c->p;
    while (*q != '"' && *q != '\0') {
        if (*q == '\\') {
            q++;
            if (*q == '\0') {
                return 0;
            }
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
    uint32_t i = 0;
    while (*c->p != '"' && *c->p != '\0' && i < n) {
        char ch = *c->p;
        if (ch == '\\') {
            c->p++;
            switch (*c->p) {
            case 'n': ch = '\n'; break;
            case 't': ch = '\t'; break;
            case 'r': ch = '\r'; break;
            case '"': ch = '"'; break;
            case '\\': ch = '\\'; break;
            case '/': ch = '/'; break;
            default: ch = *c->p; break;
            }
        }
        s[i] = ch;
        i++;
        asm volatile("" ::: "memory"); /* 编译器屏障禁 doloop（QEMU 限制） */
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
