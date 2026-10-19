# 串口 MCP 传输（serial-json）

真机（普通 ESP32-S3 开发板，无网口）在 M7 远程控制面的传输层。
一句话：**把与 UDP 通道完全相同的 netmcp 信封（epoch/seq/ack/去重/重放）
架在 UART0 的单行串口帧上**，主机侧用 `tools/serial_mcp.py` 当 MCP 客户端。

为什么不是以太网：ESP32-S3 硅片没有内部 EMAC（ESP-IDF v5.3.1
`components/esp_eth/Kconfig` 的 `ETH_USE_OPENETH` 明确 QEMU only；IDF master
`components/esp_hal_emac/` 无 `esp32s3` 子目录；Espressif issue #17606 官方
答复 S3 不支持 (R)MII）。仓库 `kernel/net/emac.c` 的 0x600CD000 OpenCores
实现是 **QEMU open_eth 测试外设**，仅 QEMU 有意义。真机上以太网只能等
SPI 扩展芯片（W5500/DM9051）；在那之前，真机远程控制面 = MCP over UART。
（详见 [ethernet.md](ethernet.md) 与 [architecture.md](architecture.md)。）

## 1. 帧格式

UART0 115200 8N1，单行，最大 1600 字节：

```
device → host :  @@MCP <crc16> <json>\r\n
host → device :  STX(0x02) + @@MCP <crc16> <json>\r\n
```

- `<crc16>`：4 位小写十六进制，**CRC-16/CCITT-FALSE**
  （poly 0x1021，init 0xFFFF，不反射，无终异或），覆盖 `<json>` 的原始字节
  （不含前缀、不含 CRLF）。
- `<json>`：netmcp 信封，与 UDP 通道完全一致：

```json
{"pot":1,"epoch":1,"seq":7,"ack":3,"type":"req","body":"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}"}
```

  `body` 是**一行** JSON-RPC 文本（作为 JSON 字符串转义）；`type` 取
  `hello | req | resp | ack | resync`。设备不回 NUL，不换行（唯一换行是帧尾 CRLF）。

帧示例（CRC 随内容变化）：

```
@@MCP 5e0e {"pot":1,"epoch":1,"seq":0,"ack":0,"type":"hello","body":"{\"mac\":\"02:50:54:4f:53:00\"}"}
```

## 2. 为什么要 STX

UART0 同时是**人类控制台**（`potato>` 提示符）。不加区分时，机器帧会
被控制台的字符回显、行编辑和命令派发一起吞掉/污染。约定：

1. host→device 发帧前先发一个 `0x02`（STX）。设备控制台读到 STX 后进入
   **帧模式**：不回声、不进 CMD_BUF 行缓冲、不派发人类命令，改用独立的
   1602 字节静态缓冲累积到 `\r`/`\n`，然后交给 `netmcp_serial_rx()`。
2. device→host 方向不需要 STX：设备只把完整合法的 `@@MCP ` 行发给主机，
   人类可读输出照旧（host 侧按行过滤，只认 `@@MCP ` 前缀）。
3. 设备坏帧（前缀/CRC/JSON/超长）**静默丢弃并计数**，不回包；host 等不到
   响应就重发。帧模式 2000ms 无后续字节会自动放弃并打印
   `[POTATO][MCP] serial frame rx timeout`，控制台恢复命令模式（不吞后续命令）。

## 3. 转发语义（与 UDP 通道共用代码）

`kernel/net/netmcp.c` 把「信封解析 → ack 回收 → hello/resync/req 分发 →
去重/重放」抽成 `netmcp_handle_envelope()`，UDP 与串口两个入口共用，因此
语义逐条一致：

| 语义 | 行为 |
| --- | --- |
| 响应配对 | host 以 `resp.seq >= req.seq` 且 `resp.ack == req.seq` 配对（设备可能因丢 ack 而重放旧响应，host 按序丢弃） |
| 请求去重 | 设备保留最近 8 个对端请求 seq（`NETMCP_SEEN`）；同 seq 重发只重放缓存响应、**不重复执行** |
| 响应重放 | 设备缓存最近 8 个已发响应（`NETMCP_SLOTS`），未收到 host ack 时每 400ms 重放 |
| ack 累积回收 | host 每收到响应回 `type:"ack"` 帧（body 空，ack=已处理的设备 seq），设备据此回收重放槽 |
| epoch | 设备每个启动周期 epoch=1；host 从 hello/resp 学习并跟随。设备收到 epoch 更旧的帧会回 `resync`，host 跟随新 epoch 重发 |
| hello | 设备未学对端前每 500ms 发 hello（body 带 MAC）；host 收到后回 ack，设备学到对端即停止广播 |
| 对端学习 | 设备收到**首个合法帧**即记住对端（串口只有一条链路，无需地址） |

串口与 UDP 的差别只有：没有 IP/端口/网卡概念；hello 不发往固定对端地址；
tick 定时器由控制台主循环空闲路径驱动（`netmcp_serial_tick()`，真机没有
net 任务）；`g_transport` 为 SERIAL 时 `netmcp_send()` 走串口帧编码。

## 4. 与 UDP 通道的关系

- QEMU（`-nic` 有网卡）继续走 UDP 通道，`test/cases/test_m7.py` 的
  UDP 语义、信封、去重/重放用例全部不变。
- 真机（无 EMAC）在 `net_start()` 里自动启用串口传输；QEMU `-nic none`
  不启用串口传输（保持「无网卡即停用」语义，`test_m7_nodev` 锚点不变）。
- 两条通道同一份信封/状态机代码；主机侧 `test/net_peer.py`（UDP Peer）与
  `tools/serial_mcp.py` 是两个传输实现，信封格式相同。

## 5. 主机工具用法

依赖：`py -3.14` + `pyserial`（仓库环境已装 3.5）。默认 `--port COM3 --baud 115200`。

```powershell
# 传输 + MCP 自检（握手 hello → tools/list → ping → sysinfo → netmcp dump）
py -3.14 tools/serial_mcp.py --port COM3 selftest

# 列出 MCP 工具（stdout 只输出 JSON-RPC 响应行，机器可读）
py -3.14 tools/serial_mcp.py --port COM3 tools/list

# 调用工具；参数是 JSON 对象
py -3.14 tools/serial_mcp.py --port COM3 call ping
py -3.14 tools/serial_mcp.py --port COM3 call sysinfo
py -3.14 tools/serial_mcp.py --port COM3 call fs.write '{"name":"n1","data":"hello"}'
py -3.14 tools/serial_mcp.py --port COM3 call fs.read '{"name":"n1"}'

# 任意 JSON-RPC 行
py -3.14 tools/serial_mcp.py --port COM3 raw '{"jsonrpc":"2.0","id":1,"method":"ping"}'

# 设备侧 netmcp 统计（human-readable）
py -3.14 tools/serial_mcp.py --port COM3 dump

# 板复位后自动恢复：脉冲复位 → 等 hello → 重新握手
py -3.14 tools/serial_mcp.py --port COM3 --reset selftest

# 重放/去重测试：--seq 强制请求序号、--no-ack 故意不回 ack 保留设备重放槽。
# 第一次发 seq=100 后再发一次同样的 seq，设备应打印 dup/replay 并返回
# 与第一次逐字节相同的缓存响应（不重复执行）。
py -3.14 tools/serial_mcp.py --port COM3 --seq 100 --no-ack raw '{"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"ping","arguments":{}}}'

# MCP 客户端桥：stdin 逐行 JSON-RPC → 设备，响应写 stdout
py -3.14 tools/serial_mcp.py --port COM3 --stdio
```

注意：每个 CLI 进程的起始 seq 由启动时刻派生（时间基准高位起点），避免
「上一次会话的 seq 仍在设备 8 槽去重窗口内」导致新请求被误判为 dup。
副作用是设备 `netmcp dump` 的 `gaps=` 计数可能很大（主机序号跳变），这是
纯诊断计数，不影响去重/重放语义。

打开 COM 口**不会复位设备**：`SerialPeer` 在 `open()` 之前先把 DTR/RTS 预置为
False（与 esptool 复位时序相反），因此多个 CLI 进程可连续操作而状态不丢；
需要复位时显式用 `--reset`。`--stdio` 对 notification（无 `id` 或
`notifications/*`）只发送不等待响应，符合 MCP 客户端握手语义。

退出码：`0` 成功；`2` 传输/握手/超时失败；`3` 设备返回 MCP error 响应。
`--verbose` 把帧级诊断打到 stderr；stdout 保持机器可读。
host 侧 CRC、超长、配对逻辑在 `tools/serial_mcp.py` 的 `SerialPeer` 中实现，
可直接 `import serial_mcp` 复用。

## 6. 设备侧实现位置

| 位置 | 内容 |
| --- | --- |
| `kernel/kernel/uart.c` / `kernel/include/uart.h` | `uart_puts_raw()`：绕过输出抽头（sink）的原始输出，但仍维护 `g_tx_midline` 行边界语义（调度器 D-03 只在行边界抢占）；帧须用它发，否则自己的帧会被 tap 再抓一遍 |
| `kernel/kernel/console.c` | 控制台主循环：STX→帧模式累积（1602B 静态缓冲）→ `netmcp_serial_rx()`；空闲路径调 `netmcp_serial_tick()` |
| `kernel/net/netmcp.c` | 传输抽象 `g_transport`（NONE/UDP/SERIAL）、`netmcp_send()` 路由、`netmcp_handle_envelope()` 公共入口、`netmcp_init_serial()`、`netmcp_serial_rx()`、`netmcp_serial_tick()`、`netmcp_serial_active()`、串口分支的 `netmcp_dump()`/`netmcp_selftest()` |
| `kernel/net/net.c` | `net_start()` 无 EMAC 时 `netmcp_init_serial()`，打印 `ethernet disabled` + `remote MCP over serial enabled (transport=serial-json)` |
| `kernel/net/emac.c` | 真机分支不再声称「DW GMAC 后端未实现」，改为事实描述（S3 无内部 MAC，跳过） |
| `tools/serial_mcp.py` | 主机侧帧编解码 + SerialPeer（握手/req/ack/重放配对/复位恢复）+ CLI |

真机 `netmcp dump` 串口分支输出示例：

```
[POTATO][NETMCP] transport=serial-json known=1 link=1 epoch=1
[POTATO][NETMCP] rx=... tx=... req=... dup=... replay=... resync=... hello=... bad=... gaps=...
[POTATO][NETMCP] serial tx=... rx=... crc_bad=0 too_long=0 bad_prefix=0 deferred=... pending_now=0 maxframe=1600 maxbody=1200 maxdgram=1400
```

`netmcp_selftest()` 在串口模式做本地回环：编码一条合成 ping req（epoch=1，
不污染的 seq）→ 用与 rx 相同的解析函数校验前缀/CRC/字段 → 派发 ping 处理，
打印 `selftest ok`；真机 `netmcp test` 命令可验证。

## 7. 限制

- **继承 UDP 通道的常量**：单条 MCP 响应 body 上限 `NETMCP_BODY_MAX=1200`，
  netmcp 信封上限 `NETMCP_DGRAM_MAX=1400`，串口帧上限 1600 字节。
  JSON 字符串转义会让 body 里的 `"` 变 `\"`（长度近 2 倍），超限的响应会被
  设备侧截断/发送失败——`tools/list` 的响应转义后实测 943B 仍在 1400B 内，
  实测可用；更大的响应需要同时放宽上述常量。
- 串口是**单主机独占**链路：同一时刻只能有一个 `serial_mcp.py` 打开 COM 口
  （烧录用 esptool 时也要先关掉它）。烧录会触发 DTR/RTS，设备复位后
  host 侧用 `--reset` 或重跑命令恢复（epoch 跟随 + 自动重握手）。
- 没有认证/加密：串口是本地物理链路，控制台本身也允许人类命令；
  安全边界与 UDP MCP 相同（本机串口）。
- 帧模式依赖控制台主循环轮询：host 一帧必须一次性连续发送（工具已这样做），
  中途长时间停顿会触发 2000ms 超时丢弃。
- **并发输出**：`trace stream on` / obs 的后台 JSONL 与 console 共用 UART。
  设备侧已加行首保护 `uart_line_begin()`（中线未收行时先补 CRLF），串口帧
  发送前也会先收行；实测 `stream on` 期间 TRJ 行首干净、`@@MCP` 帧解析
  0 失败（见 §8.1）。但 UART 本身无行锁，极高输出频率下仍建议先
  `trace stream off` 再做长响应工具调用。
- 串口带宽约 11.5KB/s（115200 8N1），大响应（如带长文本的 `fs.read`）的
  往返时延明显高于 UDP；`tools/serial_mcp.py --timeout` 默认 5s 足够。

## 8. 真机实测记录（2026-10-19，普通 ESP32-S3 开发板 / CH340 / COM3）

固件：`py -3.14 tools/build.py --platform hw` → `build/potato_boot_hw.bin`
（85744 字节，全 RAM 布局）；启动身份行
`[POTATO] build Oct 19 2026 19:43:33`，mem 行：
`data=0x3fcabdb0-0x3fcabe18 bss=0x3fcabe20-0x3fcc71b0 stack=0x3fcc71b0-0x3fccb1b0 heap=0x3fccb1b0-0x3fcdc700`。
烧录：`py -3.14 -m esptool --chip esp32s3 --port COM3 --baud 921600 write-flash 0x0 build/potato_boot_hw.bin`（Hash verified）。

| 验收项 | 日志 | 结果 |
| --- | --- | --- |
| a) hw 构建/烧录/启动 banner/READY/heap + netmcp serial banner | `build/_serial_mcp_boot_net.log` | `[POTATO] tudouOS v0.7.0`、`[POTATO] READY`、`[POTATO][NET] EMAC: ESP32-S3 has no internal MAC, skip (remote MCP over serial)`、`[POTATO][NET] no EMAC device: ethernet disabled`、`[POTATO][NETMCP] transport=serial-json epoch=1`、`[POTATO][NET] remote MCP over serial enabled (transport=serial-json)` |
| b) MCP 自检（hello→tools/list→ping→sysinfo→dump） | `build/_serial_mcp_selftest_final.log` | PASS，`rx_frames=4 bad_frames=0`；6 个工具；sysinfo `{"name":"tudouOS","version":"0.7.0","uptime_ms":20524,"heap_free":70352,"files":0}` |
| c) fs.write + fs.read 往返（两次独立进程，不复位） | `build/_serial_mcp_fs_roundtrip_final.log` | write `{"written":19}`，read `{"name":"m7test","size":19,"data":"hello-serial-mcp-42","truncated":0}`，两次 exit 0 |
| d) 同 seq 重放不重复执行 | `build/_serial_mcp_dup_final.log` | 设备打印 `[POTATO][NETMCP] req epoch=1 seq=100 ...`，重发同 seq 打印 `[POTATO][NETMCP] dup epoch=1 seq=100 replay seq=1 bytes=101`，两次响应 body 完全相同（`tick` 不变）；dump `dup=1 replay≥1` |
| e) 板复位后自动恢复 | `build/_serial_mcp_selftest_reset_final.log`、`build/_serial_mcp_dup_final.log` | `--reset selftest` PASS；重放测试第 1 轮 `--reset` 后第 2 轮自动重新握手并命中 dup |
| f) `netmcp dump` 串口分支 | `build/_serial_mcp_dump_final.log` | `[POTATO][NETMCP] transport=serial-json known=1 link=1 epoch=1` + `serial tx=.. rx=.. crc_bad=0 too_long=0 bad_prefix=0 deferred=.. pending_now=0 maxframe=1600 maxbody=1200 maxdgram=1400` |
| g) 真机 `net` 优雅降级 | `build/_serial_mcp_boot_net.log` | `[POTATO][NET] present=0 link=0 trans=0 mac=02:50:54:4f:53:00`，随后 netmcp dump |
| 附加：设备本地自检 `netmcp test` | `build/_serial_mcp_netmcp_test.log` | `[POTATO][NETMCP] selftest ok`，合成 ping 响应 `{"pong":true,"tick":3633}` |
| 附加：`--stdio` MCP 客户端桥 | `build/_serial_mcp_stdio.log` | initialize / notifications/initialized / tools/list / ping 全通，响应 id `[1,2,3]`，exit 0 |
| 附加：QEMU 回归（kernel 改动后，UDP 语义不回归） | `build/_qemu_regress.log` | 67/67 通过（M7 UDP 去重/重放/epoch/链路事件 + `-nic none` 降级用例全绿） |

实测过程中顺带修掉一个**与传输无关的既有 bug**：`kernel/mcp/mcp.c`
`send_initialize()` 的响应结尾多了一个 `}`（`...lines.\"}}}`），使
`initialize` 响应不是合法 JSON；`tools/list`/`ping` 等不受影响，但标准 MCP
客户端握手会失败。串口 `--stdio` 桥测试暴露后已修正为 `\"}}`，随后
stdio 全流程 PASS（该修复对 UDP 通道同样生效）。

## 8.1 K1/K2 修复复测（同一晚，K1/K2 修复固件）

hw-suite 全量回归报告了两个非阻塞 kernel 观察（K1 启动 tick 瞬态、K2 异步
输出无行首保护）。两者均已修复并重建重烧，本文件协议/CLI 契约不变：

- 固件：`build/potato_boot_hw.bin` **85952 字节**，
  `[POTATO] build Oct 19 2026 20:21:35`，烧录 Hash verified
  （`build/_serial_mcp_flash_k12.log`）。
- K1（`kernel/time/ccompare_tick.c`）：`tick_irq_arm()` 重新基准
  `g_last_fire_cc/g_next_cc` 并清零 `g_jit_seeded`；poll/中断两条路径的抖动
  统计跳过首个跨启动窗口样本。复测 4 次 `tickstat` 均
  `jit_min=973~999us jit_max=1001~1026us`（一次性 ~79ms 启动瞬态不再计入；
  TICK_GAP 观测保留）。
- K2（`kernel/kernel/uart.c` + `kernel/include/uart.h` + `kernel/trace/trace.c`
  + `kernel/trace/obs.c`）：新增 `uart_line_begin()`，`trace stream` / obs
  后台导出与健康打印前先收行。复测 `stream on` 并发下粘连行
  `potato> [POTATO][TRJ]` 计数 **0**，TRJ 行全部行首干净。
- 串口 MCP 复测：`py -3.14 tools/serial_mcp.py --port COM3 --reset selftest`
  → PASS，`transport=serial-json known=1 epoch=1`，`crc_bad=0 bad_prefix=0`
  （`build/_serial_mcp_selftest_k12.log`）。
- QEMU 全量回归（K1/K2 修复后）：67/67 通过（`build/_qemu_regress.log`）。
- **新固件真机全量复跑**（hw-suite，2026-10-19 20:2x）：
  `py -3.14 -m test.hw_runner --port COM3 --keep-going` →
  **70 PASS / 0 FAIL / 0 TIMEOUT / 2 SKIP / 未执行 0**，exit 0
  （`build/_hw_full3.log`）；smoke 9/9（`build/_hw_smoke3.log`）；
  串口 MCP selftest PASS（`build/_serial_mcp_selftest_k12_after_hw.log`）。
- K1 专项复测（READY 后立即连测 4 次）：`ticks=52/65/88/102`，
  `jit_min=999/974/974/974us`，`jit_max=1000/1025/1025/1025us`；
  3s idle 后 `ticks=3488 jit_min=974us jit_max=1025us`（启动窗口瞬态不入统计）。
- K2 专项复测：`trace stream on` 并发期间粘连行 `potato> [POTATO]` 计数 **0**；
  67 条 TRJ 行行首全部为 `[POTATO][TRJ]`；TRM JSON 可完整 `json.loads`。
  原始行见 `test/logs/hw/summary.md` §8 与 `build/_hw_k1k2_check3.log`。
- K3（trace 环容量/非原子读的设计观察）按 hw-suite 结论保留，不属本次修复；
  harness 已改用 trace meta 计数增量做事件断言。

## 9. 真机全量回归（M1–M9）

远程 MCP 是 M7 的一部分，但真机验收覆盖全部里程碑：
`test/hw_cases/`（72 条用例）在 COM3 上跑 M1–M9 全部命令面与串口 MCP 通道。
结果与证据见 [test/logs/hw/summary.md](../test/logs/hw/summary.md)：

```powershell
py -3.14 -m test.hw_runner --port COM3 --list        # 72 条用例
py -3.14 -m test.hw_runner --port COM3 --smoke       # 9 条冒烟
py -3.14 -m test.hw_runner --port COM3 --keep-going  # 全量回归
```

两轮固件实测均为 **70 PASS / 0 FAIL / 0 TIMEOUT / 2 显式 SKIP**
（旧固件 `19:43:33` → `build/_hw_full2.log`；K1/K2 修复固件
`20:21:35` → `build/_hw_full3.log`，smoke 9/9 `build/_hw_smoke3.log`）。
2 条 SKIP 是平台手段差异、均有替代覆盖证据：
(1) `m35/disconnect_heal` 需要 QMP 冻结 CPU，真机无 QMP；
(2) `m5/xip` 为 QEMU flash 原地执行布局，真机为全 RAM 布局。
`test/logs/hw/summary.md` 以新固件（20:21:35）为主体，旧固件基线保留为对照。

**Lead 最终验收复跑**（干净重建，固件 `20:29:27` / 85952 B / esptool Hash
verified）：`tools/serial_mcp.py --reset selftest` PASS
（`build/_final_hw_selftest2.log`）→ 全量回归 **70 PASS / 0 FAIL / 0 TIMEOUT /
2 SKIP**，exit 0（`build/_final_hw_runner4.log`）；同一源码 QEMU 全量
**67/67**、exit 0（`build/_final_qemu_runner3.log`）。
