# MCP 消息基座设计稿（土豆内核 v0 草案）

> 状态：设计稿（M2 期间评审，M3.5 实现内核基座，M6 实现 MCP 适配器）。
> 对应支柱1（内核原生 MCP AI 消息总线）与强化重点2（总线容错）。

## 1. 分层原则

```
┌───────────────────────────────────────────────┐
│ MCP 适配器（系统服务层，M6）                   │
│  JSON-RPC 编解码 / 传输绑定(UART·ETH·WS)       │
│  会话管理 / 能力协商 / 大 payload 分片          │
├───────────────────────────────────────────────┤
│ 消息基座（内核，M3.5）          ← 本文档核心    │
│  通道/消息/回执/超时/QoS/流控/追踪埋点          │
├───────────────────────────────────────────────┤
│ 内核 IPC 原语（M3：队列/事件/信号量）           │
└───────────────────────────────────────────────┘
```

**纪律：实时语义进内核，协议解析不进内核。** 基座消息是固定头 + 定长/引用
负载的二进制结构，JSON 只存在于适配器；任何解析复杂度不得进入实时路径。

## 2. 消息模型

```c
/* 固定消息头，16 字节，4 字节对齐 */
typedef struct {
    uint32_t id;        /* 发送方单调递增；重发保号，用于去重 */
    uint32_t seq;       /* 通道内时序号，接收端据此检测丢失/乱序 */
    uint16_t type;      /* 消息类型：命令/状态/事件/回执/控制 */
    uint16_t qos;       /* QoS 等级，见 §4 */
    uint32_t deadline;  /* 绝对时刻(us)；过期按 QoS 处置 */
    uint32_t trace_tag; /* 链路追踪标签，透传进追踪环 */
} pot_msg_hdr_t;

/* 小负载按值（≤ POT_MSG_INLINE=28B），大负载引用+拷贝策略（M4 有堆后支持零拷贝） */
```

- **回执（ACK）**：type=回执 消息引用被确认消息的 `id`；QoS≥1 的消息未收到
  回执且未过 deadline → 触发重发状态机。
- **trace_tag**：由发送方填充（可用发送方 id+局部计数），内核在
  enqueue/dequeue/drop/retry/ack 五个事件里原样写入追踪环，形成全链路流水。

## 3. 通道（Channel）

- 有界队列，容量创建时静态指定，**永不动态扩容**。
- 每通道独立：`seq` 计数、水位高/低线、丢弃策略、QoS 默认值。
- 满时按通道策略处置（见 §4），并记 `drop` 计数与追踪事件。

## 4. QoS 与容错（强化重点2）

| QoS | 语义 | 队列满时 | 超时(deadline)时 |
| --- | --- | --- | --- |
| 0 best-effort | 尽力而为 | 丢新，计数 | 静默丢弃 |
| 1 acknowledged | 需回执 | 丢最旧（可配），通知发送方 | 重发（指数退避，上限 N 次）后上报失败 |
| 2 critical | 需回执+不丢 | 阻塞发送方（有超时），不丢弃 | 重发不退避，直达成或失败上报 |

- **去重**：接收端按 (发送方, id) 滑动窗口去重，防重发造成的重复执行。
- **流控**：通道额度（credit）机制——接收方回执即归还额度，额度为 0 时
  QoS<2 的发送方限流等待（有超时），从机制上防"洪泛打爆内核"。
- **断线自愈**：连接（适配器层概念）带**纪元 epoch**（每次重建+1）与
  **游标 cursor**（已确认 seq）；重连后发送方从 cursor+1 重放
  QoS≥1 未确认消息，接收端靠去重窗口吸收重放。QEMU 测试用 QMP 复位/
  暂停模拟断线验证自愈。

## 5. 内核 API 草案（M3.5 实现时定稿）

```c
pot_chan_t  pot_channel_create(const pot_chan_cfg_t *cfg);   /* 静态容量 */
int         pot_msg_send(pot_chan_t ch, const pot_msg_t *m, uint32_t timeout_us);
int         pot_msg_recv(pot_chan_t ch, pot_msg_t *out, uint32_t timeout_us);
int         pot_msg_ack  (pot_chan_t ch, uint32_t msg_id);   /* 生成回执消息 */
uint32_t    pot_msg_now_us(void);                            /* deadline 用 */
/* 水位/丢弃/重试计数查询 —— 供健康快照与测试断言 */
```

阻塞语义依赖 M3 的任务模型；M3.5 之前先提供轮询版 API 供驱动/测试使用。

## 6. 追踪埋点约定（对应支柱3）

| 事件 | 参数 |
| --- | --- |
| MSG_ENQ | ch, id, seq, qos |
| MSG_DEQ | ch, id, seq |
| MSG_DROP | ch, id, reason(queue_full/deadline/dedup) |
| MSG_RETRY | ch, id, attempt |
| MSG_ACK | ch, id, rtt_us |

## 7. 测试策略（QMP 测试台用例清单，M3.5 起只增不减）

1. `test_msg_flood`：多发送方洪泛 > 队列容量 ×10，断言：不 OOM、不 panic、
   drop 计数准确、QoS2 零丢失。
2. `test_msg_retry_dedup`：制造回执延迟，断言重发次数与去重窗口行为。
3. `test_msg_timeout`：永不回执的接收端，断言退避节奏与失败上报。
4. `test_msg_disconnect_heal`：QMP `stop` 模拟断线→`cont`，断言 epoch 重建、
   cursor 重放、无重复执行。
5. `test_msg_order`：事件序 diff——消息流水（追踪环导出）顺序确定，
   QEMU 与真机一致。

## 8. 为什么这算"内核原生 MCP 能力"

适配器只做编解码与传输：所有**实时语义**——有界、回执、超时、重发、
去重、流控、纪元重同步、全链路追踪——全部由内核基座提供并保证。
上层 AI 框架（本地 Agent / 远程 MCP 主机 / QEMU 里的模拟 Agent）看到的
是一套统一、可观测、可压测的消息语义，而不是裸队列加一堆应用层补丁。
