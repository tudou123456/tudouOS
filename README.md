# tudouOS 土豆OS

面向 **ESP32-S3** 的自研嵌入式操作系统：自研土豆内核（C + Xtensa 汇编），
研发检验全程跑在 QEMU（乐鑫官方分支，`-machine esp32s3`），由 Python
QMP 测试台自动验收；同一份镜像可用 esptool 直接烧入真机开发板。

> 当前阶段：**M4 已完成**——RTOS 核心 + 消息基座 + 轻量内存管理
> （TLSF 类分配器/canary 护栏/碎片度量）落地，测试台 **26 用例全绿**
> （M1×4 + M2×5 + M3×5 + M3.5×6 + M4×6）。产品定位与四大核心支柱（内核原生 MCP
> 消息总线 / 仿真硬件双端同源 / 内核可观测 / AI 交互低抖动调度）见
> [docs/architecture.md](docs/architecture.md)，MCP 消息基座设计见
> [docs/mcp-bus-design.md](docs/mcp-bus-design.md)。

## 目录结构

```
kernel/          土豆内核（boot.S 启动、UART、mini printf、命令循环、看门狗）
  linker/tudou.ld  链接脚本（全 RAM 运行，IRAM/DRAM 布局见注释）
tools/           check_env.py 环境体检 / build.py 一键构建
qemu/            run_qemu.py 交互/测试台/gdbstub 三种模式拉起 QEMU
test/            QMP 测试台（runner + qmp_client + serial_tap + cases/）
docs/            架构蓝图、路线图、QMP 测试台手册
build/           构建产物（kernel.elf / potato_boot.bin / flash_image.bin）
third_party/     本地工具链与 QEMU（不入库）
```

## 环境准备（一次性）

```bash
python tools/check_env.py      # 体检：交叉编译器 / esptool / QEMU
python -m pip install esptool  # 缺 esptool 时
```

三件套的获取（check_env 缺什么会打印对应下载地址）：

1. **交叉编译器** `xtensa-esp-elf-gcc`：乐鑫 crosstool-NG 发布包
   （Windows x64 zip），解压到 `third_party/toolchain/` 任意子目录。
2. **QEMU** `qemu-system-xtensa`：乐鑫 QEMU fork 的 Windows 包
   （qemu-xtensa-softmmu-*-x86_64-w64-mingw32.tar.xz），解压到
   `third_party/qemu/` 任意子目录。
3. **esptool**：`pip install esptool`。

也可用环境变量 `TUDOU_CC` / `TUDOU_QEMU` 直接指定可执行文件路径。
若 QEMU 包不可用，WSL2 里跑 QEMU、Windows 跑测试台亦可（全走 TCP）。

## 快速开始

```bash
python tools/build.py      # 编译内核 → ESP 镜像 → 4MB flash 镜像
python qemu/run_qemu.py    # 交互运行（串口直连终端，可敲 ping/info/echo）
python -m test.runner      # QMP 自动验收（4 个用例 + 日志落盘）
```

## 烧录真机（ESP32-S3 开发板）

```bash
esptool --chip esp32s3 --port COMx --baud 921600 \
    write-flash 0x0 build/potato_boot.bin
```

（0x0 为 S3 二级引导偏移，构建结束也会打印。串口 115200-8N1。）

## 测试用例（M1~M4 验收，26 个全绿）

| 用例 | 验证 |
| --- | --- |
| test_boot_banner | ROM→内核完整启动链路、版本号、内存布局、READY 标记 |
| test_ping_pong | 串口命令交互（ping→pong、echo、info） |
| test_qmp_reset | QMP 复位后完整重启、RESET 事件、复位后功能正常 |
| test_qmp_stop_cont | QMP 冻结 CPU 期间串口静默，恢复后补处理命令 |
| test_tick_alive | 1kHz tick 与真实时间对齐（±20%） |
| test_uart_rx_irq | UART RX 中断 + 环形缓冲连收 20 行不丢 |
| test_exception_dump | 非法指令异常现场 dump，QMP 复位后完整恢复 |
| test_trace_dump | 追踪环事件序号递增、时间戳单调 |
| test_irq_latency_smoke | tick 触发间隔有界冒烟（硬预算以真机为准） |
| test_sched_checkpoint_preempt | 后台忙烧被 UI 睡醒任务在检查点抢占（心跳交织证据） |
| test_three_class_stress | 三分类严格优先级 + 老化防饿死（AI 先完成、UI/后台保底推进） |
| test_bg_budget | 后台预算限速（1000tick 窗口 ≤700tick） |
| test_sleep_cadence | 任务睡眠节拍（宿主时钟容差断言） |
| test_sched_trace | 追踪环含 SCHED_SWITCH 事件（调度可回放） |
| test_msg_flood | 消息洪泛 12.5×容量：有界不 OOM、丢新计数准确、系统存活 |
| test_msg_qos1_ping | QoS1 收发回执：20 条全部送达且回执，零丢失零重发 |
| test_msg_retry_dedup | 延迟回执触发重发：去重窗口保证恰一次投递 |
| test_msg_timeout | 无接收方 QoS1：重发耗尽后失败上报（不无限重发） |
| test_msg_disconnect_heal | QMP 冻结模拟断线：恢复后无丢失无重复 |
| test_msg_trace | 追踪环含 MSG_ENQ/DEQ/ACK 事件（消息流水可回放） |
| test_mem_basic | 64 块渐增尺寸分配校验释放回归，回收后堆完整 |
| test_mem_coalesce | 挖洞碎裂后大块可得，全释放 largest 恢复=total |
| test_mem_canary | 越界写捕获计数，系统存活不崩溃 |
| test_mem_exhaust | 耗尽失败计数，全释放后大块复得 |
| test_mem_churn_soak | 随机尺寸持续压测零损坏零泄漏（alloc/free 平衡） |
| test_mem_trace | 追踪环含 MEM_ALLOC/FREE 事件（分配流水可回放） |

**调度观测命令**：`tasks`（各任务状态/CPU 占用/切换数/诊断计数）、
`taskdemo preempt|stress|bg|off`（调度演示与压测）、`sleep <ms>`、
`flood <n>`（串口压测）。

**消息基座命令**（M3.5）：`msgdemo flood|ping|lazy|timeout|heal [n]`
（洪泛/回执/延迟回执/超时/断线自愈压测）、`msgstat`（通道健康快照：
水位/丢弃/重发/回执计数）。

**内存管理命令**（M4）：`memstat`（堆健康快照：总量/占用/最大块/
水位/分配释放失败捕获计数/碎片率）、`memtest selftest|holes|fault|
exhaust|churn|off`（分配回归/碎片合并/越界注入/耗尽恢复/soak 压测）。

## 常见问题

- **项目路径含中文导致构建报 `can't create xxx.o`**：工具链的 binutils
  不支持非 ASCII 路径（本项目位于 `D:\公司软件\tudouOS`）。已建目录联接
  `D:\tudouOS` → 本项目，构建时走联接路径即可：
  `cmd /c "cd /d D:\tudouOS && python tools\build.py"`。
  （QEMU 运行与测试台不受影响，两条路径都可用。）
- **测试台/交互模式无任何输出**：多半是构建产物不存在或工具链缺失，
  先 `python tools/check_env.py`。
- **想看内核反汇编/调试**：`build/kernel.elf` 带符号，
  `python qemu/run_qemu.py --gdb` + `xtensa-esp32s3-elf-gdb`。
- **QMP 怎么单独玩**：交互模式已开 QMP 4444 端口，可自行发送 JSON，
  例如 `{"execute":"system_reset"}`（需先 `qmp_capabilities`）。
