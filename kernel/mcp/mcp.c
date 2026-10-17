#include <stdint.h>

#include "console.h"
#include "heap.h"
#include "json.h"
#include "lib_shim.h"
#include "mcp.h"
#include "msg.h"
#include "potato.h"
#include "potfs.h"
#include "task.h"
#include "tick.h"
#include "uart.h"

/*
 * MCP 适配器实现：代理任务 + 消息基座通道 + JSON-RPC 工具分发。
 *
 * 协议面（D-20 后）：
 *   - initialize / notifications/initialized 握手（protocolVersion 2025-06-18）
 *   - tools/list 返回规范 {tools:[{name,description,inputSchema}]}
 *   - tools/call 按 {name, arguments} 分发，result 走 {content:[...],isError}
 *   - id 三态原样回显（数字/字符串），无 id = notification 不回包
 *   - 兼容旧直连形态：method 直接写工具名（params 即参数，result 为工具对象）
 *
 * 已知边界：UART 行传输与控制台共享串口，流里混有回显与 "potato> "
 * 提示符——严格按 stdio 逐行解析 JSON 的客户端需要过滤非 JSON 行
 * （D-20 ④，独占传输/帧标识留待后续里程碑）。
 */

#define MCP_CH_CAP 8u
#define MCP_RESP_MAX 512

static pot_channel_t g_ch;
static pot_msg_t g_ring[MCP_CH_CAP];
static int g_ch_ready;
static volatile int g_run;

static pot_task_t g_task;
static uint32_t g_stk[1024];

void mcp_init(void)
{
    if (!g_ch_ready) {
        pot_channel_init(&g_ch, "mcp", g_ring, MCP_CH_CAP, 0);
        g_ch_ready = 1;
    }
}

int mcp_running(void)
{
    return g_run;
}

int mcp_submit_line(const char *line, uint32_t len)
{
    if (!g_ch_ready) {
        return 1;
    }
    if (len > POT_MSG_INLINE) {
        return 2; /* 超长拒绝（显式报错由控制台转出，不再静默截断） */
    }
    static uint32_t seq = 0; /* 每行独立 id——去重窗口按 (发送方,id) 拦截重复 */
    seq++;
    pot_msg_t m;
    memset(&m, 0, sizeof(m));
    m.id = seq;
    m.type = 1;
    m.qos = POT_MSG_QOS0;
    m.len = (uint16_t)len;
    for (volatile uint32_t i = 0; i < len; i++) {
        m.data[i] = (uint8_t)line[i];
    }
    return pot_msg_send(&g_ch, &m, 50u) != 0 ? 1 : 0;
}

/* ---------- 工具实现：result 为 JSON 值的子节点链 ---------- */

typedef struct {
    pot_json_t *first;
    pot_json_t **tail;
    int ok;
} rbuild_t;

static pot_json_t *rnew(rbuild_t *b, const char *key)
{
    pot_json_t *v = (pot_json_t *)pot_heap_alloc(sizeof(pot_json_t), 0x4D43u);
    if (v == 0) {
        b->ok = 0;
        return 0;
    }
    memset(v, 0, sizeof(*v));
    if (key != 0) {
        uint32_t n = 0;
        while (key[n] != '\0') {
            n++;
        }
        v->key = (char *)pot_heap_alloc(n + 1, 0x4B45u);
        if (v->key == 0) {
            pot_heap_free(v);
            b->ok = 0;
            return 0;
        }
        for (volatile uint32_t i = 0; i <= n; i++) {
            v->key[i] = key[i];
        }
    }
    *b->tail = v;
    b->tail = &v->next;
    return v;
}

static void rstr(rbuild_t *b, const char *key, const char *val)
{
    pot_json_t *v = rnew(b, key);
    if (v != 0) {
        v->type = POT_JSON_STR;
        uint32_t n = 0;
        while (val[n] != '\0') {
            n++;
        }
        v->str = (char *)pot_heap_alloc(n + 1, 0x5354u);
        if (v->str == 0) {
            b->ok = 0;
            return;
        }
        for (volatile uint32_t i = 0; i <= n; i++) {
            v->str[i] = val[i];
        }
    }
}

static void rnum(rbuild_t *b, const char *key, long val)
{
    pot_json_t *v = rnew(b, key);
    if (v != 0) {
        v->type = POT_JSON_NUM;
        v->num = val;
    }
}

static void rbool(rbuild_t *b, const char *key, int val)
{
    pot_json_t *v = rnew(b, key);
    if (v != 0) {
        v->type = POT_JSON_BOOL;
        v->num = val != 0;
    }
}

static void tool_ping(rbuild_t *b, const pot_json_t *params)
{
    (void)params;
    rbool(b, "pong", 1);
    rnum(b, "tick", (long)pot_sched_now());
}

static void tool_echo(rbuild_t *b, const pot_json_t *params)
{
    const pot_json_t *e = pot_json_get(params, "text");
    rstr(b, "echo", (e != 0 && e->type == POT_JSON_STR && e->str != 0)
                        ? e->str : "");
}

static void tool_sysinfo(rbuild_t *b, const pot_json_t *params)
{
    (void)params;
    pot_heap_stat_t st;
    pot_heap_getstat(&st);
    rstr(b, "name", POTATO_NAME);
    rstr(b, "version", POTATO_VERSION);
    rnum(b, "uptime_ms", (long)pot_sched_now());
    rnum(b, "heap_free", (long)(st.total - st.used));
    rnum(b, "files", potfs_count());
}

static void tool_fs_list(rbuild_t *b, const pot_json_t *params)
{
    (void)params;
    rnum(b, "count", potfs_count());
    rnum(b, "bytes", (long)potfs_total_bytes());
}

static void tool_fs_read(rbuild_t *b, const pot_json_t *params)
{
    const pot_json_t *nm = pot_json_get(params, "name");
    if (nm == 0 || nm->type != POT_JSON_STR || nm->str == 0) {
        b->ok = 0;
        return;
    }
    char buf[144];
    int n = potfs_read(nm->str, buf, sizeof(buf) - 1);
    if (n < 0) {
        b->ok = 0; /* 由调用方转 error */
        return;
    }
    buf[n] = '\0';
    rstr(b, "name", nm->str);
    rnum(b, "size", n);
    rstr(b, "data", buf);
    rnum(b, "truncated", (long)((uint32_t)n == potfs_size(nm->str) ? 0 : 1));
}

static void tool_fs_write(rbuild_t *b, const pot_json_t *params)
{
    const pot_json_t *nm = pot_json_get(params, "name");
    const pot_json_t *dt = pot_json_get(params, "data");
    if (nm == 0 || nm->type != POT_JSON_STR || nm->str == 0 ||
        dt == 0 || dt->type != POT_JSON_STR || dt->str == 0) {
        b->ok = 0;
        return;
    }
    uint32_t n = 0;
    while (dt->str[n] != '\0') {
        n++;
    }
    int r = potfs_write(nm->str, dt->str, n);
    if (r < 0) {
        b->ok = 0;
        return;
    }
    rnum(b, "written", r);
}

typedef struct {
    const char *name;
    void (*fn)(rbuild_t *, const pot_json_t *);
    const char *desc;
    const char *props;    /* inputSchema.properties 内文（可为空串） */
    const char *required; /* required 数组内文（可为空串=无必填） */
} mcp_tool_t;

static const mcp_tool_t g_tools[] = {
    { "ping", tool_ping, "liveness probe: returns kernel tick",
      "", "" },
    { "echo", tool_echo, "echo back the text parameter",
      "\"text\":{\"type\":\"string\"}", "\"text\"" },
    { "sysinfo", tool_sysinfo, "kernel name/version/uptime/heap snapshot",
      "", "" },
    { "fs.list", tool_fs_list, "list potfs files: count and total bytes",
      "", "" },
    { "fs.read", tool_fs_read, "read a potfs file by name (144B window, truncated flag)",
      "\"name\":{\"type\":\"string\"}", "\"name\"" },
    { "fs.write", tool_fs_write, "write (replace) a potfs file by name",
      "\"name\":{\"type\":\"string\"},"
      "\"data\":{\"type\":\"string\"}", "\"name\",\"data\"" },
};
#define N_TOOLS ((int)(sizeof(g_tools) / sizeof(g_tools[0])))

/* ---------- id 三态与响应输出 ---------- */

typedef struct {
    int present;      /* 0 = notification（无 id，不回包） */
    int is_str;
    long num;
    const char *str;  /* 指向请求树内字符串，生命周期随请求树 */
} jid_t;

/* 把 JSON 字符串值转义嵌入（含控制字符） */
static void escape_into(char *dst, uint32_t cap, const char *s)
{
    uint32_t o = 0;
    for (uint32_t i = 0; s[i] != '\0' && o + 7 < cap; i++) {
        char ch = s[i];
        if (ch == '"' || ch == '\\') {
            dst[o++] = '\\';
            dst[o++] = ch;
        } else if ((unsigned char)ch < 0x20u) {
            static const char hex[] = "0123456789abcdef";
            dst[o++] = '\\';
            dst[o++] = 'u';
            dst[o++] = '0';
            dst[o++] = '0';
            dst[o++] = hex[((unsigned char)ch >> 4) & 0xF];
            dst[o++] = hex[(unsigned char)ch & 0xF];
        } else {
            dst[o++] = ch;
        }
    }
    dst[o] = '\0';
}

static void u32_to_dec(char *dst, uint32_t v)
{
    char tmp[10];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0u);
    for (int i = 0; i < n; i++) {
        dst[i] = tmp[n - 1 - i];
    }
    dst[n] = '\0';
}

/* 渲染 id 片段："null" / 数字 / 带引号转义字符串 */
static void render_id(const jid_t *id, char *out, uint32_t cap)
{
    if (id == 0 || !id->present) {
        out[0] = 'n';
        out[1] = 'u';
        out[2] = 'l';
        out[3] = 'l';
        out[4] = '\0';
        return;
    }    if (id->is_str) {
        out[0] = '"';
        escape_into(out + 1, cap - 2, id->str != 0 ? id->str : "");
        uint32_t n = 0;
        while (out[n] != '\0') {
            n++;
        }
        out[n] = '"';
        out[n + 1] = '\0';
        return;
    }
    uint32_t v = (id->num < 0) ? (uint32_t)(-(id->num + 1)) + 1u : (uint32_t)id->num;
    if (id->num < 0) {
        out[0] = '-';
        u32_to_dec(out + 1, v);
    } else {
        u32_to_dec(out, v);
    }
}

static void send_raw_result(const jid_t *id, const char *body_json)
{
    if (id != 0 && !id->present) {
        return; /* notification：不回包（id==0 仅用于 parse error 的 id:null） */
    }
    char ids[80];
    render_id(id, ids, sizeof(ids));
    potato_printf("{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":%s}\r\n",
                  ids, body_json);
}

static void send_raw_error(const jid_t *id, int code, const char *msg)
{
    if (id != 0 && !id->present) {
        return; /* notification：不回包 */
    }
    char ids[80];
    render_id(id, ids, sizeof(ids));
    potato_printf("{\"jsonrpc\":\"2.0\",\"id\":%s,"
                  "\"error\":{\"code\":%d,\"message\":\"%s\"}}\r\n",
                  ids, code, msg);
}

/* 兼容旧直连形态：result 是工具子节点链（对象） */
static void send_tree_result(const jid_t *id, pot_json_t *result_children)
{
    if (id != 0 && !id->present) {
        pot_json_free(result_children); /* notification：树释放责任在此 */
        return;
    }
    pot_json_t res = { POT_JSON_OBJ, 0, 0, 0, result_children, 0 };
    char body[MCP_RESP_MAX];
    size_t n = pot_json_write(&res, body, sizeof(body));
    if (n == 0) {
        send_raw_error(id, -32603, "result too large");
        return;
    }
    send_raw_result(id, body);
}

/* ---------- 协议方法 ---------- */

static void send_initialize(const jid_t *id)
{
    if (id != 0 && !id->present) {
        return;
    }
    char ids[80];
    render_id(id, ids, sizeof(ids));
    potato_printf("{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{"
                  "\"protocolVersion\":\"2025-06-18\","
                  "\"capabilities\":{\"tools\":{\"listChanged\":false}},"
                  "\"serverInfo\":{\"name\":\"%s\",\"version\":\"%s\"},"
                  "\"instructions\":\"tudouOS kernel MCP over UART lines; "
                  "filter non-JSON console lines.\"}}}\r\n",
                  ids, POTATO_NAME, POTATO_VERSION);
}

static void send_tools_list(const jid_t *id)
{
    if (id != 0 && !id->present) {
        return;
    }
    char ids[80];
    render_id(id, ids, sizeof(ids));
    potato_printf("{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{\"tools\":[",
                  ids);
    for (int i = 0; i < N_TOOLS; i++) {
        potato_printf("%s{\"name\":\"%s\",\"description\":\"%s\","
                      "\"inputSchema\":{\"type\":\"object\","
                      "\"properties\":{%s}%s%s%s}}",
                      i != 0 ? "," : "",
                      g_tools[i].name, g_tools[i].desc, g_tools[i].props,
                      g_tools[i].required[0] != '\0' ? ",\"required\":[" : "",
                      g_tools[i].required,
                      g_tools[i].required[0] != '\0' ? "]" : "");
    }
    potato_printf("]}}\r\n");
}

/* tools/call：工具结果序列化后按 MCP content 形态回传 */
static void send_tools_call(const jid_t *id, rbuild_t *b)
{
    if (id != 0 && !id->present) {
        return;
    }
    char body[MCP_RESP_MAX];
    if (b->ok) {
        pot_json_t res = { POT_JSON_OBJ, 0, 0, 0, b->first, 0 };
        if (pot_json_write(&res, body, sizeof(body)) == 0) {
            send_raw_error(id, -32603, "result too large");
            return;
        }
        char esc[2 * MCP_RESP_MAX];
        escape_into(esc, sizeof(esc), body);
        char ids[80];
        render_id(id, ids, sizeof(ids));
        potato_printf("{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{"
                      "\"content\":[{\"type\":\"text\",\"text\":\"%s\"}]}}\r\n",
                      ids, esc);
    } else {
        char ids[80];
        render_id(id, ids, sizeof(ids));
        potato_printf("{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":{"
                      "\"content\":[{\"type\":\"text\","
                      "\"text\":\"tool failed\"},"
                      "{\"type\":\"text\",\"text\":\"check params\"}],"
                      "\"isError\":true}}\r\n", ids);
    }
}

/* ---------- 请求分发 ---------- */

static void handle_line(const char *line)
{
    pot_json_t *req = pot_json_parse(line);
    if (req == 0 || req->type != POT_JSON_OBJ) {
        send_raw_error(0, -32700, "parse error"); /* 无法定位 id → id:null */
        if (req != 0) {
            pot_json_free(req);
        }
        return;
    }

    /* id 三态：数字/字符串原样回显；缺失 = notification，不回包 */
    jid_t jid = { 0, 0, 0, 0 };
    const pot_json_t *jid_node = pot_json_get(req, "id");
    if (jid_node != 0) {
        if (jid_node->type == POT_JSON_NUM) {
            jid.present = 1;
            jid.num = jid_node->num;
        } else if (jid_node->type == POT_JSON_STR && jid_node->str != 0) {
            jid.present = 1;
            jid.is_str = 1;
            jid.str = jid_node->str;
        } else {
            pot_json_free(req);
            send_raw_error(0, -32600, "invalid request: bad id"); /* id 非法 → id:null */
            return;
        }
    }

    const pot_json_t *jm = pot_json_get(req, "method");
    if (jm == 0 || jm->type != POT_JSON_STR || jm->str == 0) {
        pot_json_free(req);
        if (jid.present) {
            send_raw_error(&jid, -32600, "invalid request");
        }
        return;
    }
    const pot_json_t *params = pot_json_get(req, "params");

    /* ---- MCP 协议方法 ---- */
    if (strcmp(jm->str, "initialize") == 0) {
        pot_json_free(req);
        send_initialize(&jid);
        return;
    }
    if (strcmp(jm->str, "notifications/initialized") == 0) {
        pot_json_free(req);
        return; /* notification：静默（即使误带 id 也不回包） */
    }
    if (strcmp(jm->str, "tools/list") == 0) {
        pot_json_free(req);
        send_tools_list(&jid);
        return;
    }
    if (strcmp(jm->str, "tools/call") == 0) {
        const pot_json_t *jn = (params != 0) ? pot_json_get(params, "name") : 0;
        const pot_json_t *ja = (params != 0) ? pot_json_get(params, "arguments") : 0;
        if (jn == 0 || jn->type != POT_JSON_STR || jn->str == 0) {
            pot_json_free(req);
            send_raw_error(&jid, -32602, "invalid params: tools/call needs name");
            return;
        }
        int found = -1;
        for (int i = 0; i < N_TOOLS; i++) {
            if (strcmp(jn->str, g_tools[i].name) == 0) {
                found = i;
                break;
            }
        }
        if (found < 0) {
            pot_json_free(req);
            send_raw_error(&jid, -32602, "unknown tool");
            return;
        }
        rbuild_t b = { 0, 0, 1 };
        b.tail = &b.first;
        g_tools[found].fn(&b, ja); /* arguments 作为参数对象 */
        pot_json_free(req);
        send_tools_call(&jid, &b);
        pot_json_free(b.first);
        return;
    }

    /* ---- 兼容旧直连形态：method 即工具名，params 即参数 ---- */
    for (volatile int i = 0; i < N_TOOLS; i++) {
        if (strcmp(jm->str, g_tools[i].name) == 0) {
            rbuild_t b = { 0, 0, 1 };
            b.tail = &b.first;
            g_tools[i].fn(&b, params);
            pot_json_free(req);
            if (b.ok) {
                send_tree_result(&jid, b.first);
            } else {
                send_raw_error(&jid, -32000, "tool failed");
            }
            pot_json_free(b.first);
            return;
        }
    }

    pot_json_free(req);
    if (jid.present) {
        send_raw_error(&jid, -32601, "method not found");
    }
}

/* 同步处理一行 JSON-RPC（M7 远程通道直调：请求方当场拿到响应行）。
 * 与代理任务共用同一 handle_line，语义完全一致。 */
void mcp_handle_line(const char *line)
{
    handle_line(line);
}

static void mcp_agent(void *arg)
{
    (void)arg;
    potato_printf("[POTATO][MCP] agent started (transport=uart-line)\r\n");
    pot_msg_t m;
    while (!pot_task_self()->stop_req) {
        if (pot_msg_recv(&g_ch, &m, 2000u) == 0) {
            m.data[(m.len < POT_MSG_INLINE) ? m.len : POT_MSG_INLINE - 1] = '\0';
            handle_line((const char *)m.data);
            pot_msg_ack(&g_ch, &m);
        }
    }
    g_run = 0;
    potato_printf("[POTATO][MCP] agent stopped\r\n");
    pot_task_exit();
}

int mcp_start(void)
{
    if (g_run) {
        return -1;
    }
    if (g_task.name != 0 && g_task.state != POTTASK_DEAD) {
        return -1;
    }
    mcp_init();
    g_run = 1;
    int r = pot_task_create(&g_task, "mcp", POTCLS_AI_COMMS, mcp_agent, 0,
                            g_stk, 1024);
    if (r != 0) {
        g_run = 0;
        return -1;
    }
    return 0;
}

void mcp_stop(void)
{
    g_task.stop_req = 1;
}
