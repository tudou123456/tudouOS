# 以太网与远程 MCP 通道（M7）

M7 给内核接上「一根网线」：MAC 硬件后端（QEMU 下是 OpenCores `open_eth` 模型；
**ESP32-S3 硅片没有内部以太网 MAC**，见 §8）、ARP/IPv4/ICMP/UDP 协议层、以及一条
**UDP 上的远程 MCP 通道**——让主机侧的 AI/脚本不必抢串口，也能像本地 `mcp on`
一样调用内核工具。

与 M8 文档一样，本文只写**代码里真实存在的行为**；QEMU 与真机的差异、以及
「哪些是 QEMU 模型专有现象」都明确标出，避免把测试台行为当成硬件规格。
**真机没有以太网控制器**：真机远程 MCP 改走串口 MCP 传输
（[serial-mcp.md](serial-mcp.md)），以太网协议栈（ARP/ping/UDP）当前只在 QEMU
`open_eth` 下验证，详见 §8。

## 1. 分层

| 层 | 文件 | 职责 |
|---|---|---|
| MAC 硬件后端 | `kernel/net/emac.c` | OpenCores MAC + DP83848C PHY 子集：寄存器/描述符、TX/RX 环、中断、探测（**仅 QEMU `open_eth` 测试外设**；真机 S3 无内部 EMAC，见 §8） |
| 协议层 | `kernel/net/net.c` | ARP、IPv4、ICMP echo、UDP；net 后台任务；可重入协作锁；统计 |
| 远程 MCP | `kernel/net/netmcp.c` | 远程 MCP 信封/会话纪元/去重/重放/重同步（UDP 传输 + 真机串口传输）、UART 抽头、计数 |
| 控制台 | `kernel/kernel/console.c` | `net` / `ping` / `netmcp` 命令族 |
| MCP 适配器 | `kernel/mcp/*`（M6） | 被远程请求复用：`mcp_handle_line()` 解析并执行 JSON-RPC 行 |
| 测试台 | `test/net_peer.py`、`test/cases/test_m7*.py` | 主机侧 UDP 对端 + QMP 用例 |

数据面有两条入口，最终都走同一个 MCP 适配器：

```
UART 行 ──► mcp 代理任务 ──► mcp_handle_line() ──► UART 响应行
UDP 报文 ─► net 任务 ──► netmcp_udp_rx() ──► mcp_handle_line() ──► UART tap ──► UDP 响应
```

内核主动发出的报文只有两类：`hello`（探活/让对端学习）和 `resp`（响应/重放），
都从 `netmcp_send()` 出网。远程请求是「主机 → 内核」方向唯一的触发源。

> UDP 这条入口只在 QEMU（`open_eth`）下可用：ESP32-S3 硅片没有内部以太网 MAC，
> 真机远程 MCP 走串口 MCP 传输（[serial-mcp.md](serial-mcp.md)），不经过 UDP。
> 见 §8。

## 2. MAC 硬件后端（`kernel/net/emac.c`）

### 2.1 QEMU 挂载点

QEMU `esp32s3` 机器在 `esp32s3_init_openeth()`（`build/_qemu_src/hw/xtensa/esp32s3.c`）
里调用 `qemu_create_nic_device("open_eth", true, NULL)` 创建模型，并把它的两个
sysbus MMIO 区分别叠到 0x600CD000 一带（旧文档按「EMAC 窗口」记录的地址）：

* 寄存器区 → `DR_REG_EMAC_BASE`（**0x600CD000**）；
* 描述符区 → `DR_REG_EMAC_BASE + 0x400`（**0x600CD400**，1KB = 128 × 8B）；
* 中断 → 中断矩阵 `ETS_ETH_MAC_INTR_SOURCE`（源 0）。

因此 `kernel/net/emac.c` 的地址常量就是 QEMU 模型挂载地址；它驱动的是
**OpenCores 寄存器语义**。这个控制器**只存在于 QEMU 机器模型**：ESP32-S3 硅片
没有内部以太网 MAC（事实核查见 §8）。

> 测试台注意：`esp32s3` 机器在**不传 `-nic` 时也会自动创建默认网卡**。要验证
> 「无网卡」路径必须显式 `-nic none`（`test/cases/test_m7_nodev.py` 就是这么做的）。

### 2.2 寄存器窗（偏移自 0x600CD000）

| 偏移 | 寄存器 | 用途 |
|---|---|---|
| 0x00 | `MODER` | 模式：`RST`=0x800 复位、`TXEN`=0x2、`RXEN`=0x1、`PRO`=0x20 混杂、`BRO`=0x8 收广播、`PAD`=0x8000、`HUGEN`=0x4000、`LOOP`=0x80、`IAM`=0x10 |
| 0x04 | `INT_SOURCE` | 中断源（写 1 清）：`TXB`=0x1、`RXB`=0x4、`BUSY`=0x10 |
| 0x08 | `INT_MASK` | 中断使能 |
| 0x18 | `PACKETLEN` | 帧长：`MINFL`=[31:16]=64，`MAXFL`=[15:0]=1518 |
| 0x20 | `TX_BD_NUM` | TX 描述符个数（模型复位值 0x40 = 64） |
| 0x2C | `MIICOMMAND` | MII 读命令（`RSTAT`=0x2） |
| 0x30 | `MIIADDRESS` | `RGAD`=[12:8] 寄存器号、`FIAD`=[4:0] PHY 地址 |
| 0x38 | `MIIRX_DATA` | MII 读回数据 |
| 0x3C | `MIISTATUS` | `LINKFAIL`=bit0 |
| 0x40 | `MAC_ADDR0` | MAC 字节 2..5（byte2 最高） |
| 0x44 | `MAC_ADDR1` | MAC 字节 0..1 |
| 0x50 | `TXCTRL` | QEMU 模型 enum 中存在，驱动未使用 |

PHY 用 MII 管理接口读 `MII_BMSR`（寄存器 1）的 `LINK`=bit2 判断链路；
`emac_link_up()` 还要求 `MIISTATUS.LINKFAIL` 为 0。

### 2.3 描述符窗（0x600CD400，128 × 8B）

每个描述符 `{len_flags, buf_ptr}`，`len_flags` 高位 [31:16] 是帧长。驱动只用
`TX_BD_NUM=0x40` 的默认布局：**0..63 是 TX 环、64..127 是 RX 环**；实际只用
2 个 TX 描述符、4 个 RX 描述符（缓冲都是静态内存）。

| 标志 | 值 | 含义 |
|---|---|---|
| `TXD_RD` | 0x8000 | 描述符就绪（**发送的唯一闸门**） |
| `TXD_IRQ` | 0x4000 | 完成时置中断源 TXB |
| `TXD_WR` | 0x2000 | 环回卷 |
| `TXD_PAD` | 0x1000 | 短帧补齐 |
| `TXD_CRC` | 0x0800 | 追加 CRC |
| `RXD_E` | 0x8000 | 软件已备好缓冲（设备填包后清掉，需每帧重置） |
| `RXD_IRQ` | 0x4000 | 收包置中断源 RXB |
| `RXD_WRAP` | 0x2000 | RX 环回卷 |

**关键坑（M7 已踩）**：QEMU 模型在「写描述符」这个动作里就启动发送
（`open_eth_desc_write → check_start_xmit`），所以写顺序必须是
「先写不带 `TXD_RD` 的长度 → 写 `buf_ptr` → 最后写带 `TXD_RD` 的长度」；
漏掉 `TXD_RD` 模型永远不发包，`RD` 也永远不会被清，驱动会卡在自旋里。
`emac_send()` 把顺序固化成代码注释，并用有界自旋（200000 次）等 `RD` 清零，
超时记 `tx_err`（有界自旋是防御性设计，避免模型异常时永久卡死；S3 真机没有
这个 MAC，见 §8）。

发送侧：帧长必须在 14..1518 之间；物理层无链路直接 `tx_drop`；短帧由驱动补零到
64B（`PAD` 不交给模型，见下）。

接收侧：设备的 `rx_desc` 从 `TX_BD_NUM` 开始走，`RXD_E=1` 表示软件已备缓冲；
设备填包后清 `RXD_E`、置长度、在 `RXD_IRQ` 时抬 `INT_SOURCE.RXB`。软件每取走一帧
都要重新 `RXD_E` 武装该槽，环尾描述符带 `RXD_WRAP`。

**FCS 裁剪**：模型在帧尾补 4 字节零 FCS（`hw/net/opencores_eth.c` 中
`fcsl=4` 的零填充逻辑），帧长字段含这 4B，所以驱动默认 `fcs_trim=4` 裁掉
（`emac_fcs_trim()` / `emac_set_fcs_trim()`，上限 8）。不把 `PAD` 交给模型是
有意的：置 `PAD` 时模型会把短帧补到 `MINFL` 且**不加** FCS，长度语义反而变脏。

中断：源 0（`ETS_ETH_MAC_INTR_SOURCE`，与 `ETS_WIFI_MAC_INTR_SOURCE` 同值）→
CPU **int20（level2）**。ISR 只做三件事：读 `INT_SOURCE`、按 `RXB/TXB/BUSY`
写 1 清并计数；**不碰调度器**——level2 会抢占 level1 的 tick 路径，在 ISR 里动
就绪链会和 `pot_sched_tick` 竞争。收包由 net 任务轮询描述符完成，中断只负责
把 `RXB` 记成统计。

### 2.4 探测与「无网卡」路径

探测**只在 QEMU 构建执行**（hw 构建在探测前直接按「无此控制器」跳过，见 §8），
判据是 QEMU 模型语义的读写往返校验：

1. 写 `MODER.RST`，读回 `MODER` 应为 `0x800`（模型先复位再把 `MODER` 覆盖为
   写入值）；
2. 读 `TX_BD_NUM` 应为 `0x40`；
3. 两条判据任一不成立 → 视为设备缺席：打印
   `[POTATO][NET] EMAC probe: moder=0x... txbd=0x... (expect 0x800/0x40) -> absent`，
   推一条 `TR_NET_LINK`（a=0），**返回 1** 让驱动框架记为 `skipped`
   （`[POTATO][DRV] net init skipped`）。设备缺席时读未映射 MMIO 得到毒值
   `0xdeadbeef`，判据自然不成立。

`net_start()` 随后看到 `emac_present()==0`，打印
`[POTATO][NET] no EMAC device: ethernet disabled` 并返回成功：**没有以太网
不是启动失败**，控制台与本地 MCP 不受影响。这条路径被
`test_m7_nodev_absent_is_explicit_and_harmless` 覆盖（`-nic none`）。真机构建在
同一分支里额外启用**串口 MCP 传输**（具体锚点与行为见
[serial-mcp.md](serial-mcp.md)），见 §8.2。

驱动注册：`POT_DRIVER_REGISTER(net, emac_drv_init, 20)`，在 .pot_drivers 段里
按优先级初始化，UART 之后。

## 3. ARP / IPv4 / ICMP / UDP（`kernel/net/net.c`）

### 3.1 支持范围

| 协议 | 做了什么 | 明确没做 |
|---|---|---|
| ARP | 请求/应答收发、学习、缓存（4 项，TTL 60s）、解析重试 | 代理 ARP、免费 ARP、ARP probe |
| IPv4 | 版本/IHL/总长/首部校验和校验；目的地址必须是本机或受限广播 | 分片/重组、IP 选项、转发、IGMP、IPv6 |
| ICMP | 收 echo request 回 reply；`ping` 发 echo request（id=0x504F，64B 负载） | 其他 ICMP 类型（仅计数） |
| UDP | 校验和校验（0 = 未校验，按 RFC 允许）；目的端口 == netmcp 端口才上送 | 分片、端口复用、其他 UDP 服务 |

帧缓冲是两块静态 `1518B`（`g_tx_frame` / `g_rx_frame`），不做零拷贝；以太网头
14B，EtherType 只认 0x0806(ARP)/0x0800(IPv4)，其它计 `l2bad` 或忽略。

### 3.2 默认地址与帧构造

* 默认 `ip = 10.0.2.15`、`gw = 10.0.2.2`——QEMU SLIRP 用户态网络的固定租约；
  `net ip <a.b.c.d> [gw]` 可在运行时改。
* MAC 默认 `02:50:54:4f:53:00`（本地管理位，不撞真实厂商）。
* `net_send_udp(dst_ip, dst_port, src_port, data, len)`：加锁 → ARP 解析
  （租约 600ms，请求每 200ms 重发，同时驱动 `emac_recv` 泵包）→ 组以太网头 +
  IPv4 头 + UDP 头 → 计算**带伪首部的 UDP 校验和** → `emac_send()` → 解锁。
  长度超过 `1518-14-28` 直接返回 -1。
* `net_ping(ip, timeout, &rtt)`：加锁解析 ARP（800ms；失败打印
  `ping <ip> arp-timeout`）→ 发 64B ICMP echo → 解锁后短临界区轮询收包 →
  成功打印 `[POTATO][NET] ping <ip> id=<id> seq=<n> rtt=<ms>ms`，超时打印
  `[POTATO][NET] ping <ip> timeout`。控制台 `ping` 给的预算是 1000ms。

### 3.3 net 后台任务与可重入协作锁

`net_task` 是 `POTCLASS_BACKGROUND` 任务（静态栈 640 words），循环体：

```
net_task_step():
  net_lock()
    net_step_locked():  link_poll(100ms 一次 PHY) → 最多收 4 帧 → net_input() → netmcp_udp_tick()
  net_unlock()
  没收到帧才 pot_task_sleep(3ms)      ← 休眠必须在锁外
```

锁是**可重入协作锁**（`g_net_lock`/`g_net_owner`/`g_net_depth`）：同一任务重入
只加深度；别的任务自旋时用 `pot_task_sleep(1)` 让出。`net_lock_owner_name()` /
`net_lock_depth()` 暴露给 `net` 统计行。

**M7 实测踩过的坑**：早期版本在持锁状态下休眠（或发送路径忘解锁），net 任务
攥着锁睡 3ms，控制台 `ping` 每次醒来都看到锁被占 → 永久饥饿（`net` 行显示
`lock=net/1`、控制台卡死、hello 停发）。修复就是把 sleep 移到 `net_unlock()`
之后。所以 `net` 统计里的 `lock=free/0` 是一个**测试断言锚点**，不是装饰。

### 3.4 收包分发与链路事件

`net_input()`：长度过短 → `l2bad`；ARP → `arp_input`（学习并回请求）；IPv4 →
校验版本/IHL/总长/首部校验和 → 目的地址非本机且非广播则静默丢弃（不转发）→
按协议分 ICMP / UDP（UDP 再校验长度与校验和，目的端口是 netmcp 端口才调
`netmcp_udp_rx`）。

链路事件：每 100ms 轮询 `emac_link_up()`，变化时打印
`[POTATO][NET] link up|down (transitions=<n>)`、推 `TR_NET_LINK`，并通知
`netmcp_on_link()`。`net link down|up` 命令是**内核合成事件**（`net_sim_link`），
不是真的把 QEMU 网卡 set_link——测试台里后者对 usernet 不稳定，断线重连用例
因此走合成路径。

## 4. 远程 MCP 通道（`kernel/net/netmcp.c`）

### 4.1 信封

每条 UDP 报文是一个手写 JSON 信封（不引第三方库，解析用最小 `jfind/jget_*`）：

```json
{"pot":1,"epoch":1,"seq":3,"ack":2,"type":"resp","body":"{\"jsonrpc\":\"2.0\",...}"}
```

| 字段 | 含义 |
|---|---|
| `pot` | 魔数，固定 1 |
| `epoch` | 会话纪元：链路首次 up 后为 1，之后每重连 +1 |
| `seq` | 本方向序号：请求由对端分配，响应由内核 `g_tx_seq` 递增分配 |
| `ack` | **捎带确认**：内核发出的每条报文都带 `ack=g_peer_seq`（已处理到的对端请求序号）；对端发 `ack` 类型报文时用它确认内核响应 |
| `type` | `hello` / `req` / `ack` / `resync` / `resp` |
| `body` | 一行 MCP JSON-RPC 文本（`hello` 是 `{"mac":"..."}`；`ack`/部分 `resync` 为空串）；转义后长度受限 |

报文类型与方向：

| type | 方向 | 语义 |
|---|---|---|
| `hello` | 内核→对端（对端→内核时仅用于地址学习） | 对端未知时每 500ms 发一次，body 带 MAC |
| `req` | 对端→内核 | body 一行 JSON-RPC，内核执行后回 `resp` |
| `resp` | 内核→对端 | body 为响应行，同时存入重放槽；未确认时 400ms 重发 |
| `ack` | 对端→内核 | `ack=n` 累积确认响应序号 ≤n，内核回收重放槽 |
| `resync` | 双向 | 会话游标失效：内核回 `resync` 让对端跟随新纪元重发；对端发 `resync` 表示它进入新纪元 |

> 内核**没有独立的 `ack` 发送路径**：确认靠每条报文里的 `ack` 字段捎带。
> 主机侧 `test/net_peer.py` 会显式发 `ack` 类型，所以协议表里保留这一行。

### 4.2 会话纪元与重同步

* 启动：`netmcp_init()`（epoch=0）→ `net_start()` 读到链路 up → `netmcp_on_link(1)`：
  `epoch=1`、游标清零、打印 `[POTATO][NETMCP] link up epoch=1 pending=0`、
  推 `TR_MCP_RESYNC`、发 hello、重放未确认响应。随后 net 打印 ready 行。
* 断线：`netmcp_on_link(0)` 打印 `link down epoch=N (will bump)`，停发 hello/重放。
* 重连：epoch+1，清游标与去重窗口，hello + 重放（`replay relink`）。
* 收到**更高纪元**的请求：接受并清旧状态，打印 `epoch N->M (peer new generation)`，
  `resync` 计数 +1，推 `TR_MCP_RESYNC`。
* 收到**更低纪元**的残包：`bad` +1，打印
  `[POTATO][NETMCP] stale epoch=<E> (cur=<N>) seq=<S> drop`，回一条
  `resync`（带当前 epoch 与 `g_tx_seq`），**不执行**请求。
* 收到对端 `resync`：若纪元不同则整体切到对端纪元并清游标，然后重放未确认
  响应（`replay resync`）。

### 4.3 去重、重放与确认

* **请求去重窗口 8 项**：环上记 `(epoch, seq, slot)`；同 `(epoch,seq)` 再次到达：
  * 槽仍有效 → 直接重放缓存响应（`dup`、`replay` 计数），**不重复执行**；
  * 槽已被 ack 回收 → 无法重放，回 `resync`（`resync` 计数 +1）告知对端
    「游标过期，按当前纪元重来」，**不重复执行**。
* **响应重放槽 8 项**：每条响应原文存槽（`seq`、`epoch`、`body`）；未确认时
  `netmcp_udp_tick()` 每 400ms 重发（`replay retry`），重连时立即重发
  （`replay relink`）；收到 ack 后 `slot_drop_upto(ack)` 回收。
* 游标 `g_peer_seq` = 已处理的对端请求序号；`seq > cursor+1` 的缺口计入 `gaps`
  （UDP 乱序/丢包可观测）。
* 请求执行后若适配器**没有产生任何响应行**（例如 notification），内核补一条
  JSON-RPC 错误回执 `-32601 "no response for seq N"`，保证对端不悬等。

### 4.4 UART 抽头（响应抓取）

远程请求最终调 `mcp_handle_line(body)`，响应由 M6 适配器打印成 UART 行。内核在
`net_start()` 里 `uart_set_sink(netmcp_uart_tap)` 注册一个**全局 UART 输出抽头**：

* 只在「正在处理远程请求」上下文（`g_in_remote_req`）里抓行，且行必须以
  `{"jsonrpc` 开头；本地串口发起的响应不会被镜像到网络；
* 抓到的行存进重放槽并以 `resp` 发出；行长度上限 1200B（`NETMCP_BODY_MAX`），
  转义后信封上限 1400B（`NETMCP_DGRAM_MAX`）；
* 启动时会看到 `tap filtered=24` 之类数字——那是开机横幅/日志行被过滤的计数，
  属正常现象。

### 4.5 限制

| 限制 | 值 | 说明 |
|---|---|---|
| 单条 MCP 行 | ≤1200B | 超长记 `trunc`/`sendfail`，不截断伪造 |
| 单条 UDP 报文 | ≤1400B | 信封（含转义）超限则发送失败并计数 |
| UDP 端口 | 默认 47654 | `netmcp port <p>` 可改 |
| 对端 | 默认 `10.0.2.2:47654` | 内核把它视为「已配置」，上电即尝试 hello；任何合法入站报文都会学习源地址并打印 `peer <ip>:<port> epoch=<n>` |
| 传输 | 仅 UDP | 无 TCP；无认证/加密（测试台内网使用） |

**为什么内核主动 hello**：QEMU SLIRP 的 `hostfwd` 只支持 TCP，主机无法主动向
guest 的 UDP 端口发起连接；所以由内核周期发 hello/响应，主机侧 `test/net_peer.py`
绑定自己的 UDP 端口，先收包学习 guest 地址（10.0.2.15:47654），之后反向发包。

### 4.6 时序示意

```
对端 (host)                                  内核 net 任务
   |                                            link up: epoch=1
   |<------------ hello (每 500ms 直到 known) ----|
   |--- req(epoch=1, seq=1, body) ------------->| 去重/cursor → mcp_handle_line
   |                                            | tap 抓响应 → 存槽 → resp
   |<------------ resp(seq=1, ack=1?) -----------|
   |--- ack(ack=1) ----------------------------->| 回收槽 seq≤1
   |--- 同 seq=1 重发 ------------------------->| 重放缓存响应（dup），不重复执行
   |--- 同 seq=1（槽已 ack）------------------->| 回 resync（epoch=1）
   |--- net link down / up（内核合成）----------->| epoch=2，游标清零
   |--- req(epoch=1, …) ----------------------->| 丢弃，打印 stale，回 resync(epoch=2)
   |--- req(epoch=2, seq=1) ------------------->| 执行
```

## 5. 控制台命令

`help` 里的原文：

```
  net [arp|link down|up|ip <a.b.c.d> [gw]], ping <a.b.c.d>
  netmcp [dump|test|port <p>|peer <a.b.c.d> <port>]
```

| 命令 | 行为 |
|---|---|
| `net` | 4 行 NET 统计 + ARP 缓存（每条一行）+ 3 行 NETMCP 统计 |
| `net arp` | 缓存空打印 `[POTATO][NET] arp empty`；否则逐条 `[POTATO][NET] arp <ip> <mac> age=<ms>ms` |
| `net link down\|up` | **内核合成**链路事件（走 `net_sim_link`），用于断线重连测试；状态未变时是 no-op |
| `net ip <a.b.c.d> [gw]` | 运行时改 IP/网关，打印 `[POTATO][NET] ip=<ip> gw=<gw>` |
| `ping <a.b.c.d>` | 1000ms 预算的 ARP+ICMP echo，打印 `[POTATO][NET] ping <ip> ok|fail rtt=<ms>ms` |
| `netmcp` / `netmcp dump` | 打印 3 行远程通道统计（transport/rx…/tap…） |
| `netmcp test` | 离线自检：信封构造+字段/正文往返（不依赖网络），打印 `selftest ok|FAIL` |
| `netmcp port <p>` | 改 UDP 本地端口，打印 `local port=<p>` |
| `netmcp peer <ip> <port>` | 改对端并立即 hello，打印 `peer set <ip>:<port> local_port=<p>` |

> 没有裸 `arp` 命令——ARP 是 `net` 的子命令（M7 用例踩过这个坑，发送裸
> `arp` 会得到 `[POTATO] unknown command: arp`）。

## 6. 观测锚点与统计行

所有输出以 `[POTATO]` 开头，测试台/脚本直接断言。网卡就绪行：

```
[POTATO][NET] ready mac=02:50:54:4f:53:00 ip=10.0.2.15 gw=10.0.2.2 udp=47654 link=up
```

`net` 命令（`net_stats_dump()`）完整输出示例：

```
[POTATO][NET] present=1 link=1 trans=0 mac=02:50:54:4f:53:00 promisc=1 fcs_trim=4 bmsr=0xffff
[POTATO][NET] ip=10.0.2.15 gw=10.0.2.2 arp=1 lock=free/0
[POTATO][NET] mac-tx=3/273B drop=0 err=0 kick=3 mac-rx=3/323B drop=0 bad=0 irq=3/3
[POTATO][NET] l3-tx=3 rx=3 l2bad=0 arp(tx=1 rep_tx=0 rep_rx=1 miss=0) icmp(req=0 rep_rx=1 rep_tx=0) udp(rx=0 tx=1 bad=0) ipbad=0
[POTATO][NET] arp 10.0.2.2 52:55:0a:00:02:02 age=29ms
[POTATO][NETMCP] transport=udp-json local=47654 peer=10.0.2.2:47654 known=0 link=1 epoch=1
[POTATO][NETMCP] rx=0 tx=1 req=0 dup=0 replay=0 resync=0 hello=1 bad=0 gaps=0 ack=0 cursor=0 txseq=0 pending=0 trunc=0
[POTATO][NETMCP] tap lines=0 filtered=24 long=0 sendfail=0 maxbody=1200 maxdgram=1400
```

字段速查：

| 行 | 字段 | 含义 |
|---|---|---|
| 1 | `present/link/trans` | EMAC 是否存在 / 当前链路 / 链路翻转次数 |
| 1 | `promisc/fcs_trim/bmsr` | 混杂模式、FCS 裁剪字节、最近一次 PHY BMSR |
| 2 | `ip/gw/arp` | 本机地址、网关、ARP 缓存项数 |
| 2 | `lock` | **协作锁当前持有者/深度**，`free/0` 为健康 |
| 3 | `mac-tx=ok/bytes drop err kick` | 发送成功帧/字节、丢弃、自旋超时、踢描述符次数 |
| 3 | `mac-rx=ok/bytes drop bad`, `irq=rx/tx` | 收帧统计与中断计数 |
| 4 | `l3-tx/rx/l2bad/ipbad` | 协议层收发与坏帧计数 |
| 4 | `arp(tx rep_tx rep_rx miss)` | ARP 请求/应答/学习/未命中 |
| 4 | `icmp(req rep_rx rep_tx)` | echo request 收到/echo reply 收到/自己回的 reply |
| 4 | `udp(rx tx bad)` | UDP 收发与坏包 |
| mcp 1 | `transport/local/peer/known/link/epoch` | UDP 通道状态与对端学习状态 |
| mcp 2 | `rx tx req dup replay resync hello bad gaps ack cursor txseq pending trunc` | 见 §4.3/§4.5 |
| mcp 3 | `tap lines/filtered/long/sendfail/maxbody/maxdgram` | 抽头抓到的响应行、被过滤行、超长行、发送失败、上限常量 |

> 计数口径已知缺口（M7 测试发现，内核未改）：`g_resyncs` 只在「收到 resync /
> 对端新纪元 / 去重命中但槽已 ack（回 resync）」时 +1；**因陈旧纪元而主动发
> resync 时只记 `bad` 并打印锚点**，不涨 `resync`。文档按实际行为记录，测试
> 断言也按实际行为写（断线重连用例断言 `bad` 与 `stale epoch=` 锚点）。

追踪事件：`TR_NET_TX`(128)、`TR_NET_RX`(129)、`TR_NET_DROP`(130)、
`TR_NET_LINK`(131)、`TR_MCP_REQ`(160)、`TR_MCP_RESP`(161)、`TR_MCP_DUP`(162)、
`TR_MCP_RESYNC`(163)、`TR_MCP_REPLAY`(164)——其中 LINK/RESYNC/REPLAY 等进
重要事件副环，便于 M8 导出根因。

## 7. 测试用例（`test/cases/test_m7*.py`）

用例依赖**模块级** `QEMU_ARGS`（`runner.py` 会把它追加到该模块每个用例的 QEMU
命令行）：

* `test/cases/test_m7.py`：`QEMU_ARGS = ["-nic", "user,model=open_eth,id=n0"]`
* `test/cases/test_m7_nodev.py`：`QEMU_ARGS = ["-nic", "none"]`

| 用例 | 断言要点 |
|---|---|
| `test_m7_net_ready_and_link` | ready 行正则**必须锚 `\r`**（半行陷阱，见下）；mac/ip/gw 合法；`present=1 link=1`；`lock=free/0` |
| `test_m7_net_arp_and_ping` | `net arp` 有缓存行；`ping 10.0.2.2` 回来；统计含 `arp(tx=… rep_rx=…)`、`icmp(req=0 rep_rx=…)`、`mac-tx=`；缓存记录网关 |
| `test_m7_net_remote_mcp_roundtrip_and_dedup` | hello 学习对端；`tools/list` 经 UDP 往返（响应 >700B，含 tools/ping）；同 seq 重发命中重放；ack 后同 seq 得到 resync；计数 `req/dup/resync` |
| `test_m7_net_reconnect_epoch_resync` | `net link down` 打印 epoch 1（will bump）；`net link up` epoch=2；旧纪元请求被丢弃并回 resync(epoch=2)；对端跟随新纪元后请求可执行；dump 含 `epoch=2`、`bad≥1`、`stale epoch=1 (cur=2)` |
| `test_m7_net_link_events_and_trace` | `trace imp 0` 导出副环：`NET_LINK ≥2`、`MCP_RESYNC ≥1` |
| `test_m7_nodev_absent_is_explicit_and_harmless` | `-nic none`：EMAC probe 判缺席、`[DRV] net init skipped`、无 ready 行、`net` 报 `present=0`、控制台本地 MCP 仍可用（不卡死） |

**半行陷阱（M7 已知坑）**：`SerialTap.expect()` 在半行到达时就会匹配成功，整行
断言必须要求行尾 `\r`，否则 `link=up` 会在读到 `link=u` 时就匹配，取出 `"u"`
导致后续断言错乱。`NET_READY` 正则因此以 `link=(\w+)\r` 结尾。

**用例命名**：`runner.py --filter` 只匹配**函数名**（`fn.__name__` 子串），不
匹配文件名。所以 M7 用例函数统一带 `m7` 前缀，`--filter m7` 才能选中全部 6 条；
全量回归按发现规则跑，不受命名影响。

**测试台跑法**（在目录联接 `D:\tudouOS` 下）：

```powershell
& $PY tools/build.py
& $PY -m test.runner --filter m7
& $PY -m test.runner            # 全量
```

M7 用例需要 `-nic user,model=open_eth`（模块级 `QEMU_ARGS` 已配置）。
每用例新起一台 QEMU，日志在 `test/logs/`。

## 8. 真机没有「以太网后端」：ESP32-S3 无内部 EMAC

> **结论**：ESP32-S3 硅片**没有内部以太网 MAC**，也没有
> (R)MII/外部 PHY 接口。此前设想的「真机 DesignWare GMAC 后端
> （`emac_dw.c`，配 RTL8211 类 PHY）」前提不成立，真机以太网需外挂
> SPI 模块（W5500/DM9051 类），尚未实现。

证据（按可信度排序）：

* **ESP-IDF v5.3.1 `components/esp_eth/Kconfig`**：`ETH_USE_OPENETH` 的 help
  原文「This driver is not supported when running on a real chip」——OpenCores
  MAC 驱动只用于 QEMU
  （[Kconfig](https://github.com/espressif/esp-idf/blob/v5.3.1/components/esp_eth/Kconfig)）。
* **ESP-IDF master `components/esp_hal_emac/`** 只有 `esp32`、`esp32p4`、
  `esp32s31` 三个子目录，**没有 `esp32s3`**
  （[目录树](https://github.com/espressif/esp-idf/tree/master/components/esp_hal_emac)）。
* **Espressif GitHub issue #17606**（IDFGH-16485，closed—Won't Do）：乐鑫明确
  回复 ESP32-S3 不支持 (R)MII 连接外部 PHY（ESP32 / ESP32-P4 支持），要接有线
  网络请用 SPI-Ethernet 适配器
  （[issue](https://github.com/espressif/esp-idf/issues/17606)）。
* **ESP-IDF v5.3.1 ESP32-S3 文档 `api-reference/network/esp_eth.html`**：esp32s3
  只讲 external SPI-Ethernet modules（DM9051 / W5500 / KSZ8851SNL 一类）
  （[esp_eth](https://docs.espressif.com/projects/esp-idf/en/v5.3.1/esp32s3/api-reference/network/esp_eth.html)）。
* **本仓库 QEMU 侧**：`build/_qemu_src/hw/xtensa/esp32s3.c:324-349` 的
  `esp32s3_init_openeth()` 由 QEMU 机器模型自己调用
  `qemu_create_nic_device("open_eth", ...)`，把 MMIO 叠到 `DR_REG_EMAC_BASE`
  （`build/_qemu_src/include/hw/misc/esp32s3_reg.h:77`，0x600CD000）。这是
  **QEMU 测试外设**：机器模型为了测试以太网协议栈自建的设备；真机硅片上没有
  这个控制器，0x600CD000 也不是可用外设。

### 8.1 当前实现（真机）

`kernel/net/emac.c` 在 hw 构建里**直接跳过**（`g_present=0`，且不碰 0x600CD000：
带板实证盲读该地址会因 EMAC 时钟未开而挂死总线）；`net_start()` 看到
`emac_present()==0` 后打印 `[POTATO][NET] no EMAC device: ethernet disabled`，
随后在 hw 构建启用串口 MCP 传输（当前代码打印
`[POTATO][NET] remote MCP over serial enabled (transport=serial-json)`，锚点以
[serial-mcp.md](serial-mcp.md) 为准）——**没有以太网不是启动失败**，控制台与
本地 MCP 不受影响（见 §2.4）。

> 说明：M7 早期文档里的「真机 DW GMAC 后端未实现」「`emac_dw.c`」措辞已随
> 串口 MCP 改造从 `kernel/net/emac.c` 清理；真机分支的行为（跳过 + 优雅降级 +
> 启用串口传输）语义未变。

### 8.2 真机网络路线：串口 MCP 传输

真机没有以太网，但**远程 MCP 通道仍然可用**：把 M7 远程 MCP 的同一套信封/
会话语义（epoch、seq、ack、去重、重放、hello、resync）承载在 **UART 帧**上，
不再依赖 UDP。主机侧脚本直接经 COM 口对话，不必经过 QEMU 的 SLIRP。

* 协议细节、主机工具用法与真机验收证据只维护在
  [serial-mcp.md](serial-mcp.md)，本文不复述。
* 传输层与信封/会话语义分层：QEMU 用 UDP 传输、真机用串口帧传输，上层
  去重/重放/纪元语义共用一套实现。
* ARP / IPv4 / ICMP / UDP 帧链路（`kernel/net/net.c`）是**以太网专用**的，
  真机不跑；真机上 `net` / `ping` 在无 EMAC 时不可用（优雅降级，见 §2.4）。

### 8.3 可选路线：外挂 SPI 以太网模块（未实现）

若将来真机要跑真正的 IP 网络（ARP/ping/UDP），S3 只能**外挂 SPI 以太网模块**
（W5500 / DM9051 / KSZ8851SNL 等）：

* 需要新写一个 **SPI MAC 后端**：与 `emac.c` 的 OpenCores 后端并列，实现同一套
  `emac_*` 接口（`emac_present/init/send/recv/link_up/int_enable/set_mac/
  set_promisc/set_fcs_trim/get_stats`），把 SPI 收发映射成「收帧/发帧」；
* 上层 ARP/IPv4/ICMP/UDP（`net.c`）与远程 MCP 信封（`netmcp.c`）与 MAC 后端
  无关，可原样复用；
* 现状：**模块未到货、后端未实现、无验收数据**——不要把它当成已就绪能力。

### 8.4 被推翻的旧说法（供检索与避免复燃）

* ~~真机 ESP32-S3 的以太网控制器是 DW GMAC，需另写 `emac_dw.c`~~：S3 没有
  内部 MAC，该后端文件不需要也不存在。
* ~~真机需要 RTL8211 类外部 PHY、MDIO 时钟分频、DMA 描述符环、cache 对齐~~
  ：这些是「带内部 GMAC 的芯片（如 ESP32 / ESP32-P4）」的问题，S3 不适用。
* ~~M7 的「真机」部分 = DW GMAC 带板工作~~：M7 的真机部分 = **串口 MCP
  传输**；以太网部分只在 QEMU 验证。

> 另按 `docs/architecture.md` 的 M7 行：**WiFi 只能真机调**，不在 M7 范围；
> 外挂 SPI 以太网同样不在 M7 验收范围内。

## 9. 已知限制

1. 只实现 ARP/IPv4/ICMP echo/UDP 的最小闭环；无 TCP/IPv6/ICMPv6/IGMP、无分片
   重组、无 IP 选项、无转发。
2. ARP 缓存只有 4 项（60s TTL）；MAC 默认值固定，可 `emac_set_mac()` 改。
3. MAC 环固定 2 个 TX / 4 个 RX 描述符，发送有界自旋；高吞吐场景需要扩环或
   改异步完成（QEMU 模型语义；S3 真机无此 MAC）。
4. FCS 裁剪默认 4 是 **QEMU 模型**行为（模型在帧尾补 4B 零 FCS）；协议层不
   依赖这个固定值。
5. 远程通道单端口、单对端、无认证加密，面向测试台/内网；`body`/报文有
   1200B/1400B 上限，超出显式失败并计数，不静默截断。
6. `g_resyncs` 计数口径见 §6 的说明（陈旧纪元主动回 resync 不计数）。
7. **以太网链路只在 QEMU 验证**：真机 S3 无内部 EMAC；无网卡探测（`MODER/
   TX_BD_NUM` 往返）是 QEMU 模型专有判据，真机构建不探测、直接优雅降级。
8. **真机远程 MCP 走串口传输**（[serial-mcp.md](serial-mcp.md)，与 UDP 通道
   同信封语义）；真机没有 UDP 通道，除非后续外挂 SPI 以太网模块（§8.3）。

## 10. 文件索引

| 文件 | 说明 |
|---|---|
| `kernel/net/emac.c` | OpenCores MAC 后端（仅 QEMU open_eth 测试外设）；真机 S3 无内部 EMAC，直接跳过 |
| `kernel/net/net.c` | ARP/IPv4/ICMP/UDP、net 任务、协作锁、统计与 `net` 输出 |
| `kernel/net/netmcp.c` | 远程 MCP 通道：信封、纪元、去重/重放、tap、selftest；UDP 传输 + 串口帧传输抽象 |
| `docs/serial-mcp.md` | 真机串口 MCP 传输：帧格式、会话语义、主机工具、真机证据（真机网络现行路线，见 §8.2） |
| `tools/serial_mcp.py` | 主机侧串口 MCP 工具（selftest / tools-list / call / raw / dump / `--stdio` 桥，`--port COM3`） |
| `kernel/include/net.h` | 唯一接口头：`emac_*` 硬件抽象、`net_*` 协议层、`netmcp_*` 远程通道与统计结构 |
| `kernel/kernel/console.c` | `net` / `ping` / `netmcp` 命令实现 |
| `test/net_peer.py` | 主机侧 UDP 对端（信封编解码、ack、重同步跟随） |
| `test/cases/test_m7.py`、`test/cases/test_m7_nodev.py` | 6 条 M7 验收用例 |
| `build/_qemu_src/hw/net/opencores_eth.c` | QEMU open_eth 模型（行为参考） |
| `build/_qemu_src/hw/xtensa/esp32s3.c` | QEMU 机器挂载点（地址/中断/默认 NIC） |
