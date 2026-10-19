# tudouOS 架构设计

本文描述当前内核的结构与关键约束。实现细节以 `kernel/` 源码和头文件注释为准。

## 1. 目标与边界

- 目标平台：ESP32-S3（Xtensa LX7，双核，无 MMU 进程隔离）。
- 从零实现：C11 + Xtensa 汇编，call0 ABI，不链接 libc/libgcc。
- 同一份源码运行在 Espressif QEMU 分支与真实硬件。
- 面向 AI/Agent 的控制面：内核原生消息基座 + MCP 工具，而不是把协议栈
  塞进中断/调度热路径。
- 非目标：POSIX、桌面级网络协议栈、多地址空间进程模型。

## 2. 启动链

```
ROM → _start (boot.S) → kernel_main → console_run
```

`boot.S` 负责：

1. 关中断、设置 THREADPTR（CPU 号 0）、切换到内核栈。
2. （QEMU/XIP 构建）配置外部内存 MMU 表并启用 icache；真机默认全 RAM
   布局，不依赖 flash XIP。
3. 尽早关闭看门狗、清零 `.bss`、安装向量表。
4. 进入 `kernel_main`。

`kernel_main` 的顺序：trace → UART → 驱动/PAL → 调度器/SMP 状态 →
放行中断 → 时基/tick → 堆 → potfs → MCP → UART RX → 可观测 → 网络 →
控制台。调度器与 SMP 状态在中断放行前就绪，避免首个 tick 踩空。

## 3. 异常与中断

- 向量表布局遵循 ESP32-S3：level-2 向量（UART0/EMAC/IPI）、level-3
  （预留 SYSTIMER）、通用异常/内核向量、双重异常。
- 所有入口保存统一的 88 字节 call0 帧（a0/a2–a15、SAR/LBEG/LEND/LCOUNT、
  EPC1/PS 槽），与上下文切换帧一致。
- tick 使用核内 CCOMPARE0（int6，level-1），经通用异常路径分发；
  按实际跨过的周期补记，避免仿真/忙循环丢拍。
- 中断上下文只做记账/唤醒/必要设备处理；任务切换在异常返回前完成
  （IRQ 帧用 `rfe`，syscall 帧用 `rfe` + 统一恢复路径）。
- level-1 异常没有可写 EPS1；恢复以 EPC1 为准，PS 的 INTLEVEL/EXCM
  由 `rfe` 清回。

## 4. 调度器

- 三分类优先级域：AI（0）> UI（1）> 后台（2）> idle（3）。
- 每核独立就绪队列（定宽数组）、当前任务、时间片与切换计数；全局
  睡眠表与毫秒时基由 CPU0 推进。
- 同级轮转；连续 200ms 未运行的 UI/后台任务临时提升；后台在 1000 tick
  窗口内最多消耗 700 tick。
- tick 可抢占（QEMU 默认关、真机默认开）；检查点 syscall（yield/sleep/
  exit）始终可用。
- 上下文切换只发生在异常上下文（EXCM=1）；任务首启使用伪造帧
  （a0=蹦床，a2=fn，a3=arg）。
- 就绪/睡眠表溢出会回滚任务状态并计数，不静默丢任务；栈底有哨兵，
  切换时校验。

## 5. 内存

- 单堆，块头 16B（magic/size/tag/next）+ 用户区 + 8B 尾（canary/self）。
- 15 个 2 的幂桶 + 位图定位，桶内首适配；大块分裂、邻块双向合并。
- canary 键在启动时由 ccount/堆范围派生；free 先做地址范围/对齐校验，
  头损坏块墓碑隔离，重复释放被捕获并计数。
- 堆/统计/消息/文件/追踪等非重入结构在 SMP 下使用跨核原子锁
  （`pot_lock_acquire/release`，rsil 15 + S32C1I）。
- 锁顺序约定：`fs → heap → trace`、`sched → trace`；持锁不得睡眠。

## 6. 消息基座与 MCP

- 通道是静态有界环（容量 2 的幂），消息定长内联负载 256B；永不动态扩容。
- QoS0 满丢新；QoS1 可丢最旧/阻塞，超时指数退避重发；QoS2 阻塞到成功
  或预算耗尽。
- 去重/回执窗口按发送方分桶；回执按入队 seq 配对，QoS0 不占窗口。
- MCP 适配器运行在独立任务：控制台/串口帧/网络把 JSON-RPC 行投递到
  通道，代理解析后分发工具，响应按单行 JSON 输出并回显 id。
- 串口帧 `@@MCP <crc16> <json>` 用 CRC16/CCITT-FALSE 校验；UART 行传输
  与控制台共享串口，严格客户端需过滤非 JSON 行。

## 7. potfs

- 静态元数据表 + 堆数据；写时复制：先分配新缓冲，成功后再替换并释放旧块。
- 读写删/统计都持文件系统锁；读侧复制期间数据不会被并发写释放。
- 返回的 `potfs_file_t *` 只保证表项结构有效，调用方不得解引用 `data`；
  读内容走 `potfs_read`。

## 8. 网络

- 协议栈：ARP / IPv4 / ICMP echo / UDP；EMAC 驱动面向 QEMU open_eth。
- 远程 MCP：UDP 信封（`pot`/`epoch`/`seq`/`ack`/`type`/`body`），
  固定 peer、ack 纪元/上界校验、请求去重与响应重放。
- 真机 ESP32-S3 无内部 EMAC，远程 MCP 走串口帧传输；以太网需外挂
  SPI 模块（未实现）。

## 9. SMP

- CPU1 由 CPU0 写 `SYSTEM_CORE_1_CONTROL_1` 入口地址启动；入口重建窗口、
  栈、向量表、PS 与 THREADPTR。
- 每核独立 CCOMPARE0 tick；CPU1 不推进全局时间。
- IPI 经中断矩阵源 79/80 → int21（level-2）；ISR 先清源，再原子换出
  pending 位图。
- 负载均衡在 CPU0 tick 中限频决策，只迁移可迁移任务；迁移有 20ms 冷却。
- 跨核共享结构用原子锁；CPU1 的追踪事件用计数替代写环。

## 10. 可观测

- 主追踪环（256 事件）+ 重要事件副环（64），事件含时间、类型、参数。
- JSONL 导出、`trace meta` 累计计数、健康快照、`tasks`/`tickstat`/
  `memstat`/`smp stat` 诊断命令。
- 故障注入：leak / hang / spin；`tools/ai_analysis.py` 用规则从日志定位
  根因。

## 11. 构建与测试

- `tools/build.py`：交叉编译 → 链接 → elf2image → flash 镜像自检；
  `--platform hw` 产出全 RAM 单镜像。
- `tools/fetch_deps.py`：固定 URL + SHA256 下载工具链/QEMU。
- QEMU 测试台：QMP 控制、每用例独立 VM、动态端口、串口全量落盘。
- 真机回归台：per-case 复位、115200 串口、SKIP 必须写明平台原因。
- `tools/release_gate.py`：干净构建 + QEMU 连续 N 轮全绿 + 可选真机。

## 12. 设计限制

- 单地址空间、无 MMU 隔离；共享结构依赖锁而不是硬件一致性。
- 固定容量容器（任务/通道/文件/追踪环）以可预测性换灵活性。
- 串口控制面与日志共享 UART，输出行需要协议层过滤。
- 以太网仅 QEMU 验证；真机外挂模块尚未实现。
