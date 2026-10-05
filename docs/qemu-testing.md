# QMP 研发检验测试台

测试台的目标：把"研发检验"变成一条命令——任何内核改动都能在 QEMU 里
自动验证，不依赖人工看串口。

## 架构

```
python -m test.runner
   │  每个用例拉起一台专属 QEMU（-S 冻结启动）
   ├── QMP  tcp:127.0.0.1:<动态端口>   控制面（复位/暂停/恢复/查状态）
   └── UART tcp:127.0.0.1:<动态端口>   数据面（采集内核输出、注入命令）
        │ 连接就绪后才 cont 放行 CPU，启动日志零丢失
        ▼
   qemu-system-xtensa -machine esp32s3 -drive file=build/flash_image.bin,...
```

- `test/qmp_client.py`：QMP 客户端（greeting → `qmp_capabilities` →
  `command()`；异步事件缓存于 `events` 列表）。
- `test/serial_tap.py`：串口采集器（后台线程收字节；`expect(正则, 超时)`
  顺序匹配并推进位置，避免误匹配旧输出）。
- `test/runner.py`：运行器（用例发现、VM 生命周期、日志落盘、汇总）。
- `test/cases/test_boot.py`：M1 验收用例（boot_banner / ping_pong /
  qmp_reset / qmp_stop_cont）。

## 常用命令

```bash
python -m test.runner                 # 全部用例
python -m test.runner --filter reset  # 按名过滤
python -m test.runner --list          # 列出用例
```

每个用例产生四份日志（test/logs/）：`*.serial.log`（串口全记录）、
`*.qemu.log`（QEMU 自身输出）、`*.qmp-events.json`（RESET/SHUTDOWN 等事件）、
失败时附 `*.error.log`（断言栈）。

## 写新用例

在 `test/cases/` 新建 `test_*.py`，模块级 `test_` 开头函数即用例：

```python
def test_my_feature(ctx):
    ctx.serial.expect(r"\[POTATO\] READY", timeout=20)
    ctx.serial.send("mycommand")
    ctx.serial.expect(r"expected output", timeout=5)
    st = ctx.qmp.command("query-status")   # 任意 QMP 命令
    assert st["status"] == "running"
```

## QMP 能力速查（本测试台可用）

| 命令 | 用途 |
| --- | --- |
| `query-status` | running/paused 状态断言 |
| `system_reset` | 整机复位（重启可靠性、掉电模拟） |
| `stop` / `cont` | 冻结/恢复 CPU（时序注入、暂停期静默断言） |
| `quit` | 干净退出 QEMU |
| `human-monitor-command` | 透传 HMP：`info registers`、`x/8i $pc`、`gdbserver tcp::1234` 等 |

## 内核跑挂了怎么查

1. 看该用例的 `*.serial.log` 尾部（最后输出到哪条日志，定位挂点）。
2. 交互复现：`python qemu/run_qemu.py`（串口直连终端）。
3. 上 GDB：`python qemu/run_qemu.py --gdb`，另开
   `xtensa-esp32s3-elf-gdb build/kernel.elf` → `target remote :1234` → `c`。
4. 无 GDB 时可用 QMP 透传 HMP 拿寄存器现场：
   `ctx.qmp.command("human-monitor-command",
                   {"command-line": "info registers"})`。

## 约定

- 内核输出统一带 `[POTATO]` 前缀，作为断言锚点；新增模块请保持该约定。
- 用例必须幂等：runner 对每个用例新建一台 QEMU，禁止跨用例共享状态。
- 端口由 runner 动态分配，可与交互模式的 QEMU（4444/5555）并行使用。
