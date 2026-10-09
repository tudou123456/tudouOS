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
 * 工具：ping / echo / sysinfo / tools/list / fs.list / fs.read / fs.write。
 */

#define MCP_CH_CAP 8u
#define MCP_LINE_MAX 192
#define MCP_RESP_MAX 400

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
    if (len > MCP_LINE_MAX) {
        len = MCP_LINE_MAX;
    }
    static uint32_t seq = 0; /* 每行独立 id——去重窗口按 (发送方,id) 拦截重复 */
    seq++;
    pot_msg_t m;
    memset(&m, 0, sizeof(m));
    m.id = seq;
    m.type = 1;
    m.qos = POT_MSG_QOS0;
    m.len = (uint16_t)len;
    for (volatile uint32_t i = 0; i < len && i < POT_MSG_INLINE; i++) {
        m.data[i] = (uint8_t)line[i];
    }
    /* 行超长走 QoS0 截断（通道投递语义） */
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
} mcp_tool_t;

static const mcp_tool_t g_tools[] = {
    { "ping", tool_ping },
    { "echo", tool_echo },
    { "sysinfo", tool_sysinfo },
    { "tools/list", 0 },   /* 特判 */
    { "fs.list", tool_fs_list },
    { "fs.read", tool_fs_read },
    { "fs.write", tool_fs_write },
};
#define N_TOOLS ((int)(sizeof(g_tools) / sizeof(g_tools[0])))

/* ---------- 响应输出 ---------- */

static void send_result(long id, pot_json_t *result_children)
{
    pot_json_t res = { POT_JSON_OBJ, 0, 0, 0, result_children, 0 };
    char body[MCP_RESP_MAX];
    size_t n = pot_json_write(&res, body, sizeof(body));
    if (n == 0) {
        potato_printf("{\"jsonrpc\":\"2.0\",\"id\":%d,"
                      "\"error\":{\"code\":-32603,\"message\":\"result too large\"}}\r\n",
                      (int)id);
        return;
    }
    potato_printf("{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":%s}\r\n",
                  (int)id, body);
}

static void send_error(long id, int code, const char *msg)
{
    potato_printf("{\"jsonrpc\":\"2.0\",\"id\":%d,"
                  "\"error\":{\"code\":%d,\"message\":\"%s\"}}\r\n",
                  (int)id, code, msg);
}

static void handle_line(const char *line)
{
    pot_json_t *req = pot_json_parse(line);
    if (req == 0 || req->type != POT_JSON_OBJ) {
        send_error(0, -32700, "parse error");
        if (req != 0) {
            pot_json_free(req);
        }
        return;
    }

    const pot_json_t *jid = pot_json_get(req, "id");
    long id = (jid != 0 && jid->type == POT_JSON_NUM) ? jid->num : 0;

    const pot_json_t *jm = pot_json_get(req, "method");
    if (jm == 0 || jm->type != POT_JSON_STR || jm->str == 0) {
        pot_json_free(req);
        send_error(id, -32600, "invalid request");
        return;
    }
    const pot_json_t *params = pot_json_get(req, "params");

    if (strcmp(jm->str, "tools/list") == 0) {
        rbuild_t b = { 0, 0, 1 };
        b.tail = &b.first;
        rstr(&b, "transport", "uart-line");
        rstr(&b, "substrate", "potato-msg-v1");
        char list[160];
        uint32_t li = 0;
        list[0] = '\0';
        for (volatile int i = 0; i < N_TOOLS; i++) {
            uint32_t k = 0;
            while (g_tools[i].name[k] != '\0' && li < sizeof(list) - 2) {
                list[li++] = g_tools[i].name[k++];
            }
            if (i < N_TOOLS - 1 && li < sizeof(list) - 2) {
                list[li++] = ',';
            }
        }
        list[li] = '\0';
        rstr(&b, "tools", list);
        if (b.ok) {
            send_result(id, b.first);
        }
        pot_json_free(b.first);
        pot_json_free(req);
        return;
    }

    for (volatile int i = 0; i < N_TOOLS; i++) {
        if (g_tools[i].fn != 0 && strcmp(jm->str, g_tools[i].name) == 0) {
            rbuild_t b = { 0, 0, 1 };
            b.tail = &b.first;
            g_tools[i].fn(&b, params);
            if (b.ok) {
                send_result(id, b.first);
            } else {
                send_error(id, -32000, "tool failed");
            }
            pot_json_free(b.first);
            pot_json_free(req);
            return;
        }
    }

    pot_json_free(req);
    send_error(id, -32601, "method not found");
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
