#include <stdint.h>

#include "console.h"
#include "demo.h"
#include "lib_shim.h"
#include "task.h"

/*
 * 调度演示/压测任务（M3 验收配套）——`taskdemo` 命令驱动。
 *   preempt: 后台忙烧心跳 + UI 周期睡醒 → 验证优先级抢占时序（检查点粒度）
 *   stress : AI/UI/后台 三路忙烧 → 验证严格优先级 + 老化防饿死
 *   bg     : 仅后台忙烧 → 验证后台预算限速（约 ≤70% CPU）
 *   off    : 请求全部停止（任务在下一个检查点自尽）
 * 重生成纪律：先请求停止并等旧任务真正退出（DEAD）再创建，
 * 绝不覆盖活任务的 TCB。
 */

#define DEMO_BURN 120000u /* 一次忙烧迭代，QEMU 实测约 8ms/拍 */

static pot_task_t g_t_ai, g_t_ui, g_t_bg;
static uint32_t g_stk_ai[1024], g_stk_ui[1024], g_stk_bg[1024];

static void burn(void)
{
    volatile uint32_t x = 0;
    for (uint32_t i = 0; i < DEMO_BURN; i++) {
        x = x + i;
    }
    (void)x;
}

/* 检查点：忙烧节奏里让出（更高优先级就绪时在此切换——检查点调度） */
static void beat(void)
{
    burn();
    pot_task_yield();
}

static void ai_task(void *arg)
{
    (void)arg;
    for (int i = 0; i < 120 && !pot_task_self()->stop_req; i++) {
        beat();
        potato_printf("[POTATO][D] A %d\r\n", i);
    }
    potato_printf("[POTATO][D] AI-DONE\r\n");
    pot_task_exit();
}

static void ui_task_preempt(void *arg)
{
    (void)arg;
    for (int i = 0; i < 5 && !pot_task_self()->stop_req; i++) {
        pot_task_sleep(100);
        potato_printf("[POTATO][D] U %d\r\n", i);
    }
    potato_printf("[POTATO][D] UI-DONE\r\n");
    pot_task_exit();
}

static void ui_task_stress(void *arg)
{
    (void)arg;
    for (int i = 0; i < 120 && !pot_task_self()->stop_req; i++) {
        beat();
        potato_printf("[POTATO][D] U %d\r\n", i);
    }
    pot_task_exit();
}

static void bg_task(void *arg)
{
    (void)arg;
    for (int i = 0; i < 60 && !pot_task_self()->stop_req; i++) {
        beat();
        potato_printf("[POTATO][D] G %d\r\n", i);
    }
    potato_printf("[POTATO][D] BG-DONE\r\n");
    pot_task_exit();
}

static int spawn(pot_task_t *t, uint32_t *stk, const char *name,
                 pot_task_class_t cls, pot_task_fn_t fn)
{
    if (t->name != 0 && t->state != POTTASK_DEAD) {
        return -1; /* 活任务不许覆盖 */
    }
    return pot_task_create(t, name, cls, fn, 0, stk, 1024);
}

void demo_stop(void)
{
    g_t_ai.stop_req = 1;
    g_t_ui.stop_req = 1;
    g_t_bg.stop_req = 1;
}

static int all_dead(void)
{
    if (g_t_ai.name != 0 && g_t_ai.state != POTTASK_DEAD) {
        return 0;
    }
    if (g_t_ui.name != 0 && g_t_ui.state != POTTASK_DEAD) {
        return 0;
    }
    if (g_t_bg.name != 0 && g_t_bg.state != POTTASK_DEAD) {
        return 0;
    }
    return 1;
}

int demo_start(const char *mode)
{
    demo_stop(); /* 幂等：先请旧的退场 */
    for (int i = 0; i < 200 && !all_dead(); i++) {
        pot_task_sleep(1); /* 等旧任务在检查点自尽（最多 ~200ms） */
    }
    if (!all_dead()) {
        return -1; /* 旧任务未退，拒绝 */
    }

    if (strcmp(mode, "preempt") == 0) {
        if (spawn(&g_t_bg, g_stk_bg, "dbg", POTCLS_BACKGROUND, bg_task) != 0) {
            return -1;
        }
        return spawn(&g_t_ui, g_stk_ui, "dui", POTCLS_UI, ui_task_preempt);
    }
    if (strcmp(mode, "stress") == 0) {
        if (spawn(&g_t_ai, g_stk_ai, "dai", POTCLS_AI_COMMS, ai_task) != 0) {
            return -1;
        }
        if (spawn(&g_t_ui, g_stk_ui, "dui", POTCLS_UI, ui_task_stress) != 0) {
            return -1;
        }
        return spawn(&g_t_bg, g_stk_bg, "dbg", POTCLS_BACKGROUND, bg_task);
    }
    if (strcmp(mode, "bg") == 0) {
        return spawn(&g_t_bg, g_stk_bg, "dbg", POTCLS_BACKGROUND, bg_task);
    }
    return -2; /* 未知模式 */
}

/* console 命令表使用 */
const char *demo_mode_help(void)
{
    return "taskdemo preempt|stress|bg|off";
}
