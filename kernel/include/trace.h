#ifndef TUDOU_TRACE_H
#define TUDOU_TRACE_H

#include <stdint.h>

/*
 * 土豆内核最小追踪环（支柱3起步）：
 * 静态定容 RAM 环形缓冲，事件定长 16 字节，写满覆盖最旧（有丢弃计数）。
 * 时间戳来源为 ccount 低 32 位（40MHz 假设，约 107s 回绕；D-27：仅用于
 * 顺序与差值，不用于绝对时刻）。M8 前导出为文本行，事件结构已按
 * JSONL 导出器的字段设计。
 */

#define POT_TRACE_CAPACITY 256 /* 必须是 2 的幂 */

enum {
    TR_IRQ_ENTER = 1,   /* a=level                b=0 */
    TR_IRQ_EXIT  = 2,   /* a=level                b=0 */
    TR_EXCEPTION = 3,   /* a=EXCCAUSE             b=EPC */
    TR_TICK_GAP  = 4,   /* a=本次 tick 实测间隔 us  b=tick 计数（M8：停摆实测） */
    TR_SCHED_SWITCH = 16, /* a=from TCB 指针       b=to TCB 指针 */
    TR_SCHED_DROP  = 17,  /* a=TCB 指针            b=槽位(0..3=ready,0x10=sleep) */
    TR_SCHED_AGING = 18,  /* a=TCB 指针            b=等待 ms */
    TR_MSG_ENQ   = 32,  /* a=通道指针             b=消息 id */
    TR_MSG_DEQ   = 33,  /* a=通道指针             b=消息 id */
    TR_MSG_DROP  = 34,  /* a=通道指针             b=原因(1full/2deadline/3dedup) */
    TR_MSG_RETRY = 35,  /* a=通道指针             b=消息 id */
    TR_MSG_ACK   = 36,  /* a=通道指针             b=消息 id */
    TR_MEM_ALLOC = 64,  /* a=块尺寸               b=tag */
    TR_MEM_FREE  = 65,  /* a=块尺寸               b=tag */
    TR_MEM_FAULT = 66,  /* a=原因(1overflow/2badfree) b=tag */
    /* M7：以太网/远程 MCP 通道 */
    TR_NET_TX    = 128, /* a=帧长                 b=协议(1arp/2ipv4/3icmp/4udp) */
    TR_NET_RX    = 129, /* a=帧长                 b=协议 */
    TR_NET_DROP  = 130, /* a=原因                 b=协议 */
    TR_NET_LINK  = 131, /* a=0 down / 1 up        b=0 */
    TR_MCP_REQ   = 160, /* a=epoch                b=seq */
    TR_MCP_RESP  = 161, /* a=epoch                b=seq */
    TR_MCP_DUP   = 162, /* a=epoch                b=seq（去重抑制的重放） */
    TR_MCP_RESYNC = 163,/* a=epoch                b=补发条数 */
    TR_MCP_REPLAY = 164,/* a=epoch                b=重放条数 */
    /* M9：多核 */
    TR_CPU_UP    = 144, /* a=cpu id               b=入口地址 */
    TR_IPI_SEND  = 145, /* a=cpu id               b=原因 */
    TR_IPI_RECV  = 146, /* a=cpu id               b=原因 */
    TR_SMP_MIGRATE = 147, /* a=TCB 指针           b=目标 cpu |
                           * bit31=steal(主动窃取) 否则 push */
    /* M8 验收：故障/负载注入（inject.c） */
    TR_INJECT_BEGIN = 176, /* a=模式(1leak 2hang 3spin) b=参数 */
    TR_INJECT_END   = 177, /* a=模式                b=参数 */
};

/* 事件名（JSONL/文本导出共用）；未知返回 "UNKNOWN" */
const char *trace_event_name(uint16_t type);

void trace_init(void);
void trace_push(uint16_t type, uint32_t a, uint32_t b);
uint32_t trace_dropped(void);
uint32_t trace_total(void);            /* 累计事件数（含已被覆盖的） */
uint32_t trace_oldest_seq(void);       /* 环内最旧可读序号（== total 表示空） */
uint32_t trace_count_of(uint16_t type);/* 按类型累计计数（BSS 1KB） */
void trace_dump(unsigned max_n); /* 打印最近 max_n 条（console 命令） */

/*
 * M8 JSONL 导出：每行 `[POTATO][TRJ] {"seq":..,"t":..,"ev":"..","a":..,"b":..}`
 * seq 为 32 位全量序号（由环位置与累计数反推，不依赖 16 位截断字段）。
 * 返回实际导出行数；max_n=0 视作 POT_TRACE_CAPACITY。
 */
unsigned trace_export_jsonl(unsigned max_n);
/* 流式游标导出：从 *cursor 起（已被覆盖则从最旧可读处）导出至多 max_n 条，
 * 并把 *cursor 推进到已导出末尾。返回行数。 */
unsigned trace_export_since(uint32_t *cursor, unsigned max_n);
/* 汇总（总量/丢弃/流状态/各类型计数）写入 buf，返回写入长度 */
uint32_t trace_meta_json(char *buf, uint32_t cap, int stream_on);

/*
 * M8 重要事件副环：故障类事件（异常/tick 停摆/丢弃/泄漏/网络丢包/注入/
 * 迁移）另存一份 64 条、不被 sched 噪声冲刷的历史，导出带 "imp":1。
 * 普通环绕一圈仅约 93ms（实测 2763 事件/s），副环保证根因现场可导出。
 */
#define POT_TRACE_IMP_CAPACITY 64 /* 必须是 2 的幂 */
int trace_is_important(uint16_t type);
unsigned trace_export_important(unsigned max_n);
unsigned trace_export_important_since(uint32_t *cursor, unsigned max_n);
uint32_t trace_important_total(void); /* 副环累计写入条数 */

#endif /* TUDOU_TRACE_H */
