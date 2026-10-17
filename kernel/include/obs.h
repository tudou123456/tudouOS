#ifndef TUDOU_OBS_H
#define TUDOU_OBS_H

#include <stdint.h>

/*
 * 土豆内核可观测体系（M8，支柱3：事件导出为 JSONL + 健康快照 + AI 分析）
 *
 * 三层：
 *   1) 事件层  trace.c 环形缓冲（16 字节/条，256 条）+ 类型计数
 *              + 重要事件副环（64 条：异常/tick 停摆/丢弃/泄漏/注入/迁移，
 *                不被 2763 事件/s 的调度噪声冲刷，导出带 "imp":1）
 *   2) 导出层  trace_export_jsonl / trace_export_since —— 每行一条 JSONL
 *              流式模式（本模块后台任务）按游标持续导出新增事件，
 *              不重复导出、不静默丢（被环覆盖的条数计入 lost）
 *   3) 快照层  pot_obs_health() 单行 JSON 健康快照（任务/调度/堆/消息/
 *              中断/追踪/网络/SMP），供 AI 分析流水线逐帧比对
 *
 * 与 AI 流水线的契约（见 tools/ai_analysis.py）：
 *   事件行   [POTATO][TRJ] {"seq":N,"t":N,"ev":"NAME","a":N,"b":N}
 *   健康行   [POTATO][HEALTH] {...}
 *   崩溃行   [POTATO][PANIC] {...} + 其后最近事件 JSONL
 * 分析器只依赖以上三类前缀 + 字段名，不依赖顺序与数量。
 *
 * 线程模型：流式导出跑在独立 BACKGROUND 任务（obs），每 100ms 导出至多
 * OBS_PUMP_LINES 条；health/panic 在调用者上下文同步执行，使用模块内静态
 * 缓冲（不可重入，console 单线程调用已足够）。
 */

#define OBS_PUMP_LINES      4u
#define OBS_PUMP_PERIOD_MS  100u

void pot_obs_init(void);

int  pot_obs_stream_on(void);
void pot_obs_stream_set(int on);   /* 1=开始/继续流式导出 0=停止（任务保留） */
unsigned pot_obs_pump(void);       /* 立即导出新增事件，返回行数 */
uint32_t pot_obs_pumped(void);     /* 累计导出行数 */
uint32_t pot_obs_lost(void);       /* 环覆盖导致无法导出的条数（应为 0） */
uint32_t pot_obs_pump_calls(void);
uint32_t pot_obs_imp_pumped(void); /* 重要事件副环导出行数（M8 根因现场） */
uint32_t pot_obs_imp_lost(void);   /* 副环被覆盖条数（应为 0） */

void pot_obs_health(void);         /* 打印单行健康快照 JSON */
uint32_t pot_obs_health_count(void);
void pot_obs_panic_dump(void);     /* 崩溃诊断：快照 + 最近事件 JSONL */

/* 注入开关（M8 验收：卡死/泄漏场景）——kernel/trace/inject.c 实现 */
void pot_inject_cmd(const char *mode, unsigned arg);
void pot_inject_stop(void);
int  pot_inject_active(void);

#endif /* TUDOU_OBS_H */
