# QEMU 测试台

tudouOS 的 QEMU 回归台用乐鑫 QEMU 分支（`-machine esp32s3`）启动内核，
通过 QMP 控制 CPU/复位，通过串口读写控制台与 MCP。每个用例独立 VM、
动态端口、日志全量落盘，避免用例间状态污染。

## 运行

```powershell
py -3.14 tools/build.py --clean
py -3.14 -m test.runner                 # 全量
py -3.14 -m test.runner --filter m9     # 按名字过滤
py -3.14 -m test.runner --list          # 列出用例
```

日志在 `test/logs/`（gitignored）：每个用例的串口 transcript、QEMU 输出与
QMP 事件；失败时还会落 `.error.log`。

## 测试台结构

- `test/runner.py`：发现 `test/cases/test_*.py`，逐用例启动 VM。
- `test/qmp_client.py`：QMP 命令（stop/cont/reset/query-status）。
- `test/serial_tap.py`：串口读取、按行匹配、超时与全量记录。
- `test/cases/`：内核/协议/驱动用例；模块级 `QEMU_ARGS` 可追加 QEMU 参数
  （例如 `-nic user,model=open_eth`）。
- `test/hw_runner.py` + `test/hw_cases/`：同一套断言的串口真机回归台。

## 写用例的约定

- 用例函数接收 `ctx`，用 `ctx.serial.expect(regex, timeout)` 等待锚点。
- 锚点尽量以 `\r` 或后续字面量结尾，避免半行早匹配（`ticks=2` 抢在
  `2023` 前）。
- 不要依赖"流量结束后立刻 dump"这类时序；事件计数用 `trace meta` 的
  累计计数做前后差值。
- 每个用例独立 VM；不要假设上一个用例的状态。
- 超时与日志会落盘，失败信息要包含实际匹配到的文本。

## QEMU 模型的已知限制

以下限制来自乐鑫 QEMU 分支的 esp32s3 模型，内核已做适配；真机行为以
TRM 为准。

1. **TIMG/SYSTIMER 计数器配置会崩仿真**：QEMU 构建的 tick 使用核内
   CCOMPARE0（int6），不用 TIMG/SYSTIMER 作为 tick 源。
2. **INTLEVEL=15 期间挂起设备源可能活锁**：启动时先放行全局中断，再逐级
   使能设备源；临界区只屏蔽必要级别。
3. **ccount 按翻译块跳步**：CCOMPARE 可能一次跨过多个周期，tick 按实际
   跨过的周期补记；`pot_tick_poll` 作为看门狗补账。
4. **MMU/flash 取指是模型特有**：QEMU 走 flash XIP 布局；真机默认全 RAM，
   不依赖 XIP/icache。MMU 表项宽度与数据读 fault 语义与真机不同，不要用
   QEMU 行为推断真机。
5. **genex 切走 + CCOMPARE 活动可能丢 tick**：QEMU 默认关闭 tick 抢占
   （`preempt off`），真机默认开启；该差异不影响检查点调度与睡眠唤醒。
6. **level-1 异常 PS**：Xtensa level-1 没有可写 EPS1；真机 `rfe` 以 EPC1
   为准。QEMU 模型的 `rfe` 从 SR194 取 PS，内核在 QEMU 构建保留了对应写
   入（见 `switch.S` 的平台分叉）。
7. **UART 是仿真设备**：波特率不真实限速，时序断言以宿主时钟/事件计数为准。

## 常用命令

```powershell
py -3.14 qemu/run_qemu.py            # 交互运行
py -3.14 qemu/run_qemu.py --gdb      # gdbstub
py -3.14 -m test.runner --filter mcp # 只跑 MCP 用例
```

QEMU 二进制路径由 `tools/envlib.py` 查找：`TUDOU_QEMU` → PATH →
`third_party/`。版本不匹配时 `tools/check_env.py` 会提示。
