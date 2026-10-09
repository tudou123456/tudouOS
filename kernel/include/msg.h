#ifndef TUDOU_MSG_H
#define TUDOU_MSG_H

#include <stdint.h>

#include "task.h"

/*
 * 土豆内核消息基座（M3.5，支柱1 地基；设计稿 docs/mcp-bus-design.md）
 *
 * 实时语义进内核、协议解析不进内核：消息是定长二进制结构，通道是有界
 * 环，一切容量静态、永不动态扩容（洪泛不 OOM）。
 *
 * QoS（队列满/超时行为）：
 *   0 尽力而为   满则丢新并计数；超时静默
 *   1 需回执     满按通道策略（丢最旧可配/阻塞）；未回执超时→重发
 *                （指数退避，上限 POT_MSG_MAX_RETRY 次）后上报失败
 *   2 关键       满则阻塞发送方（有超时）不丢弃；重发不退避直至达成/失败
 *
 * 去重：入队按 (发送方, id) 滑动窗口，重发不产生重复投递。
 * 唤醒：通道进度（入队/出队/回执）显式唤醒等待者，醒来重查条件
 * （条件变量模式，允许虚假唤醒）。
 */

#define POT_MSG_INLINE 192u /* MCP JSON-RPC 单行需较长内联负载 */
#define POT_MSG_QOS0 0
#define POT_MSG_QOS1 1
#define POT_MSG_QOS2 2

#define POT_MSG_MAX_RETRY 2
#define POT_MSG_RETRY_BASE_MS 50u

/* 消息（56 字节定长） */
typedef struct {
    uint32_t id;         /* 发送方单调递增，重发保号（去重键之一） */
    uint32_t seq;        /* 通道内时序号，入队时分配（丢失/乱序检测） */
    uint16_t type;       /* 应用自定义消息类型 */
    uint16_t qos;        /* POT_MSG_QOS0/1/2 */
    uint32_t deadline;   /* 绝对 tick（send 按 timeout 设置，含重发预算） */
    pot_task_t *sender;  /* 发送方（去重/回执寻址） */
    uint32_t trace_tag;  /* 链路追踪标签，透传进追踪环 */
    uint16_t len;        /* 负载长度 ≤ POT_MSG_INLINE */
    uint8_t data[POT_MSG_INLINE];
} pot_msg_t;

/* 等待者槽位（发送/接收混排，醒来各自重查） */
#define POT_CH_WAITERS 4

/* 通道（调用方静态提供环形缓冲） */
typedef struct {
    const char *name;
    pot_msg_t *ring;
    uint16_t cap;        /* 2 的幂 */
    uint16_t head;       /* 下一个出队槽 */
    uint16_t count;
    uint8_t drop_oldest; /* QoS1 满时丢最旧（0=阻塞） */
    uint32_t next_seq;

    /* 去重窗口（最近入队的 (sender,id)） */
    struct {
        pot_task_t *sender;
        uint32_t id;
    } dedup[8];
    uint8_t dedup_idx;

    /* 回执窗口（最近回执的 (sender,id)） */
    struct {
        pot_task_t *sender;
        uint32_t id;
    } acked[8];
    uint8_t acked_idx;

    pot_task_t *waiters[POT_CH_WAITERS];
    int n_waiters;

    /* 健康快照计数 */
    uint32_t c_enq, c_deq, c_drop_full, c_drop_deadline, c_drop_dedup;
    uint32_t c_retry, c_ack;
    uint16_t high_water;
} pot_channel_t;

/* 初始化通道（ring 容量须为 2 的幂） */
int pot_channel_init(pot_channel_t *ch, const char *name,
                     pot_msg_t *ring, uint16_t cap, int drop_oldest);

/*
 * 发送（QoS0：0=入队 1=满丢弃；QoS1/2：阻塞等回执，0=已回执 -1=失败）
 * timeout_ms 为整次操作（含重发）预算；QoS2 满时阻塞同受此约束。
 */
int pot_msg_send(pot_channel_t *ch, pot_msg_t *m, uint32_t timeout_ms);

/* 接收：0=取到消息；-1=空且超时。QoS≥1 的消息由接收方调 pot_msg_ack */
int pot_msg_recv(pot_channel_t *ch, pot_msg_t *out, uint32_t timeout_ms);

/* 回执（记录进通道回执窗口并唤醒发送方） */
void pot_msg_ack(pot_channel_t *ch, const pot_msg_t *m);

/* 当前 tick（deadline 计算用） */
static inline uint32_t pot_msg_now(void)
{
    return pot_sched_now();
}

#endif /* TUDOU_MSG_H */
