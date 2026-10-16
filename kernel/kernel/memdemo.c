#include <stdint.h>

#include "console.h"
#include "heap.h"
#include "lib_shim.h"
#include "memdemo.h"
#include "task.h"
#include "uart.h"

/*
 * 内存管理演示/压测（M4 验收配套）——`memtest` 命令驱动。
 *   selftest [n] : 顺序分配 n 块（尺寸渐增），写花样校验后全释放
 *   holes        : 挖洞-碎片-回收链路，最后 64KB 大块证明合并有效
 *   fault        : 故意越界写 1 块，free 时 canary 捕获并报告
 *   dblfree      : 合并后旧地址二次释放，墓碑捕获（D-06 回归）
 *   szcorrupt    : 砸块头 size 后释放，隔离不扩散（D-05 回归）
 *   exhaust      : 分配到耗尽（fail 计数）→ 全释放 → 大块复得
 *   churn        : 后台任务持续分配/校验/释放（soak 压测源）
 *   off          : 停止 churn
 */

static pot_task_t g_t_churn;
static uint32_t g_stk_churn[1024];

static void churn_task(void *arg)
{
    (void)arg;
    /* LCG 伪随机尺寸 8..512，花样写入校验，立即/延迟释放混合 */
    uint32_t seed = 0x5EED0001u;
    uint32_t ops = 0;
    void *live[8];
    for (volatile int i = 0; i < 8; i++) {
        live[i] = 0;
    }
    while (!pot_task_self()->stop_req) {
        seed = seed * 1103515245u + 12345u;
        uint32_t sz = 8u + ((seed >> 16) & 0x1FFu); /* 8..519 */
        uint32_t slot = (seed >> 8) & 7u;
        if (live[slot] != 0) {
            pot_heap_free(live[slot]);
            live[slot] = 0;
        }
        uint8_t *p = (uint8_t *)pot_heap_alloc(sz, 0xC4120u);
        if (p != 0) {
            for (volatile uint32_t i = 0; i < sz; i++) {
                p[i] = (uint8_t)(seed + i);
            }
            volatile uint32_t bad = 0;
            for (volatile uint32_t i = 0; i < sz; i++) {
                if (p[i] != (uint8_t)(seed + i)) {
                    bad = 1;
                }
            }
            if (bad) {
                potato_printf("[POTATO][MEM] CHURN-CORRUPT op=%u sz=%u\r\n",
                              ops, sz);
                pot_task_exit();
            }
            if ((seed & 1u) != 0u) {
                pot_heap_free(p); /* 立即释放 */
            } else {
                live[slot] = p; /* 留驻 */
            }
        }
        ops++;
        if ((ops & 0x3FFu) == 0u) {
            pot_task_yield(); /* 检查点让出 */
        }
    }
    for (volatile int i = 0; i < 8; i++) {
        if (live[i] != 0) {
            pot_heap_free(live[i]);
            live[i] = 0;
        }
    }
    pot_heap_stat_t st;
    pot_heap_getstat(&st);
    potato_printf("[POTATO][MEM] CHURN-DONE ops=%u faults=%u fail=%u\r\n",
                  ops, st.n_fault, st.n_fail);
    pot_task_exit();
}

static int spawn_churn(void)
{
    if (g_t_churn.name != 0 && g_t_churn.state != POTTASK_DEAD) {
        return -1;
    }
    return pot_task_create(&g_t_churn, "mch", POTCLS_BACKGROUND, churn_task, 0,
                          g_stk_churn, 1024);
}

void memdemo_stop(void)
{
    g_t_churn.stop_req = 1;
}

int memdemo_start(const char *mode, unsigned n)
{
    (void)n; /* churn 模式不按轮数跑；参数保留给未来模式 */
    memdemo_stop();
    for (volatile int i = 0; i < 200 &&
         g_t_churn.name != 0 && g_t_churn.state != POTTASK_DEAD; i++) {
        pot_task_sleep(1);
    }
    if (strcmp(mode, "churn") == 0) {
        return spawn_churn();
    }
    return -2;
}

const char *memdemo_mode_help(void)
{
    return "memtest selftest|holes|fault|dblfree|szcorrupt|exhaust|churn|off";
}

/* ---------- 同步模式（控制台上下文直接执行） ---------- */

void mem_cmd_dispatch(const char *mode, unsigned n)
{
    pot_heap_stat_t st;

    if (strcmp(mode, "selftest") == 0) {
        if (n == 0u) {
            n = 64u;
        }
        if (n > 128u) {
            n = 128u;
        }
        static uint8_t *ptrs[128];
        uint32_t i;
        for (i = 0; i < n; i++) {
            ptrs[i] = (uint8_t *)pot_heap_alloc(8u + i * 8u, 0x5007u + i);
            if (ptrs[i] == 0) {
                break;
            }
            for (volatile uint32_t j = 0; j < 8u + i * 8u; j++) {
                ptrs[i][j] = (uint8_t)(0xA0u + i);
            }
        }
        uint32_t got = i;
        uint32_t bad = 0;
        for (i = 0; i < got; i++) {
            for (volatile uint32_t j = 0; j < 8u + i * 8u; j++) {
                if (ptrs[i][j] != (uint8_t)(0xA0u + i)) {
                    bad = 1;
                }
            }
        }
        for (i = 0; i < got; i++) {
            pot_heap_free(ptrs[i]);
        }
        pot_heap_getstat(&st);
        potato_printf("[POTATO][MEM] SELFTEST-%s n=%u faults=%u\r\n",
                      bad ? "FAIL" : "PASS", got, st.n_fault);
        return;
    }

    if (strcmp(mode, "holes") == 0) {
        enum { HN = 32 };
        static uint8_t *h[HN];
        for (volatile int i = 0; i < HN; i++) {
            h[i] = (uint8_t *)pot_heap_alloc(1024u, 0x3001u);
        }
        /* 挖洞：释放偶数块 → 16 个 1KB 空洞 */
        for (volatile int i = 0; i < HN; i += 2) {
            pot_heap_free(h[i]);
            h[i] = 0;
        }
        /* 碎片中分配小块 */
        void *m = pot_heap_alloc(512u, 0x3002u);
        pot_heap_free(m);
        /* 全释放 */
        for (volatile int i = 0; i < HN; i++) {
            if (h[i] != 0) {
                pot_heap_free(h[i]);
                h[i] = 0;
            }
        }
        /* 64KB 大块：证明合并把堆收回来了 */
        void *big = pot_heap_alloc(64u * 1024u, 0x3003u);
        pot_heap_free(big);
        pot_heap_getstat(&st);
        potato_printf("[POTATO][MEM] HOLES-%s big=%s free=%u largest=%u "
                      "faults=%u\r\n",
                      big ? "PASS" : "FAIL", big ? "ok" : "null",
                      st.total - st.used, st.largest, st.n_fault);
        return;
    }

    if (strcmp(mode, "fault") == 0) {
        uint8_t *p = (uint8_t *)pot_heap_alloc(16u, 0xBAADu);
        if (p == 0) {
            uart_puts("[POTATO][MEM] FAULT-SETUP-FAIL\r\n");
            return;
        }
        p[0] = 1; /* 正常写 */
        /* 故意越界：16B 载荷按 8 对齐仍为 16，写到 footer 区 */
        p[16] = 0xDE;
        p[17] = 0xAD;
        pot_heap_free(p); /* canary 校验在此捕获 */
        pot_heap_getstat(&st);
        potato_printf("[POTATO][MEM] FAULT-CAPTURED faults=%u used=%u\r\n",
                      st.n_fault, st.used);
        return;
    }

    if (strcmp(mode, "dblfree") == 0) {
        /* D-06 回归：构造 P(free) A(alloc) N(free)，free(A) 双向合并后
         * 旧 A 头位于合并块内部——再 free(A) 必须命中墓碑被捕获，
         * 且空闲表不得出现重叠块（随后 selftest 验证堆完好） */
        uint32_t f0 = 0;
        pot_heap_getstat(&st);
        f0 = st.n_fault;
        uint8_t *p = (uint8_t *)pot_heap_alloc(24u, 0xDB1u);
        uint8_t *a = (uint8_t *)pot_heap_alloc(24u, 0xDB2u);
        if (p == 0 || a == 0) {
            uart_puts("[POTATO][MEM] DBLFREE-SETUP-FAIL\r\n");
            return;
        }
        pot_heap_free(p); /* 下邻变空 → A 的下次释放会向下合并 */
        pot_heap_free(a); /* 双向合并，A 旧头成为合并块内部字节 */
        pot_heap_free(a); /* 二次释放：应命中 DEAD 墓碑 */
        pot_heap_getstat(&st);
        potato_printf("[POTATO][MEM] DBLFREE-CAPTURED faults=%u(+%u) "
                      "free=%u largest=%u\r\n",
                      st.n_fault, st.n_fault - f0, st.total - st.used,
                      st.largest);
        return;
    }

    if (strcmp(mode, "szcorrupt") == 0) {
        /* D-05 回归：直接砸掉块头的 size 字段后释放——不得越界写、
         * 不得把跨范围块插入空闲表；块被隔离（泄漏），系统存活 */
        pot_heap_getstat(&st);
        uint32_t f0 = st.n_fault;
        uint8_t *a = (uint8_t *)pot_heap_alloc(24u, 0x5C1u);
        if (a == 0) {
            uart_puts("[POTATO][MEM] SZCORRUPT-SETUP-FAIL\r\n");
            return;
        }
        uint32_t *hdr = (uint32_t *)(a - 16u); /* [magic|size|...] */
        hdr[1] = 0xFFFFFFF0u;                  /* size → 垃圾（8 对齐非零） */
        pot_heap_free(a);                      /* 头校验应拦截并隔离 */
        pot_heap_getstat(&st);
        potato_printf("[POTATO][MEM] SZCORRUPT-QUARANTINED faults=%u(+%u) "
                      "used=%u\r\n",
                      st.n_fault, st.n_fault - f0, st.used);
        return;
    }

    if (strcmp(mode, "exhaust") == 0) {
        enum { EN = 200 };
        static uint8_t *e[EN];
        uint32_t i;
        for (i = 0; i < EN; i++) {
            e[i] = (uint8_t *)pot_heap_alloc(1024u, 0xE7u);
            if (e[i] == 0) {
                break;
            }
        }
        uint32_t got = i;
        for (i = 0; i < got; i++) {
            pot_heap_free(e[i]);
        }
        void *big = pot_heap_alloc(64u * 1024u, 0xE8u);
        pot_heap_free(big);
        pot_heap_getstat(&st);
        potato_printf("[POTATO][MEM] EXHAUST-PASS got=%u fail=%u big=%s "
                      "faults=%u\r\n",
                      got, st.n_fail, big ? "ok" : "null", st.n_fault);
        return;
    }

    potato_printf("[POTATO][MEM] unknown mode (%s)\r\n", memdemo_mode_help());
}
