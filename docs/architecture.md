# tudouOS 架构蓝图

tudouOS 是面向 ESP32-S3 的自研嵌入式操作系统：自研**土豆 RTOS 内核
（potato kernel）**——抢占式实时内核（任务、调度、中断、IPC、确定性时序），
配自研系统服务组成完整 OS。研发检验全程跑在 QEMU 里（乐鑫官方 QEMU 分支
的 `esp32s3` 机器），通过 QMP 由程序化测试台控制，产物不加修改即可烧入
真机开发板。

## 总体形态

```
┌─────────────────────────────────────────────────┐
│  应用 / 串口 Shell（M6+）                        │
├─────────────────────────────────────────────────┤
│  系统服务：文件系统、网络栈、驱动框架（M5+）      │
├─────────────────────────────────────────────────┤
│  土豆 RTOS 内核                                  │
│   任务与抢占调度(M3) 内存管理(M4)                 │
│   中断/异常/定时器(M2) IPC/Mutex/Sem(M3)         │
│   启动、UART 控制台、mini libc        ← M1 已交付 │
├─────────────────────────────────────────────────┤
│  ESP32-S3 硬件（Xtensa LX7 双核 / 真机或 QEMU）  │
└─────────────────────────────────────────────────┘
```

## M1 已交付（最小闭环）

- **引导链路**：片上 ROM → flash 0x0 处的 ESP 镜像（`kernel.elf` 经
  esptool `elf2image` 生成，不依赖 ESP-IDF；0x0 是 S3 的二级引导偏移）→ `_start`。
- **内核全 RAM 运行**：IRAM 0x40380000 起（代码），DRAM 0x3FC88000 起
  （数据/栈/堆）。LMA==VMA，`.data` 由 ROM 搬运就位，`.bss` 由启动代码清零。
- **call0 ABI**：M1 全内核用 `-mabi=call0` 编译，函数调用不旋转寄存器窗口，
  无需窗口异常向量；自带 `memcpy/memset/strcmp` 等极小 libc 与软除法，
  不链接任何预编译库。
- **单核运行**：仅在 CPU0 上跑（S3 是双核，CPU1 保持不动，SMP 列入 M8）。
- **基础外设**：UART0 轮询驱动 + mini printf + 串口命令循环
  （`ping/echo/info/help`）；启动即关闭 TIMG0/TIMG1/RTC 三处看门狗。
- **研发检验闭环**：`python tools/build.py` 构建 → `python -m test.runner`
  在 QEMU 里自动验收（详见 [qemu-testing.md](qemu-testing.md)）。

## 关键设计决策记录

| 决策 | 理由 |
| --- | --- |
| 不用 ESP-IDF/FreeRTOS 任何代码 | 100% 自研；ROM 是芯片内置，无法也无需绕开 |
| 内核作为"二级引导镜像"放在 0x1000 | ROM 直接加载 RAM 段并跳入口，免去自写 flash XIP/缓存配置；M5 内核变大后再引入自研 bootloader + XIP |
| QEMU 与真机同一份 flash 镜像 | 测试通过的镜像 `esptool write-flash` 直接上板，保证仿真/真机行为一致 |
| QMP + UART 全走 TCP | 测试台纯 Python 跨平台；QEMU 可跑在本机或 WSL2/远程 |
| 启动横幅作为测试锚点 | `[POTATO]` 前缀行既是人看的日志，也是测试断言点 |

## 路线图

| 里程碑 | 内容 | 验收方式（QMP 测试台用例） |
| --- | --- | --- |
| M1 ✅ | 启动 + UART 控制台 + 测试台闭环 | 横幅/ping-pong/QMP 复位/停止恢复 |
| M2 | Xtensa 中断与异常：VECBASE、窗口异常处理、UART RX 中断、systimer（RTOS 实时时基） | 中断延迟统计、异常注入后打印现场（PC/EPC/EXCCAUSE）并恢复 |
| M3 | RTOS 核心：任务/线程模型、上下文切换、抢占式优先级调度、互斥/信号量/队列 | 任务切换波形断言、优先级抢占时序、饱和压力测试 |
| M4 | 堆内存管理 + 内存故障检测；评估切换窗口 ABI | 反复分配/释放回归、越界捕获 |
| M5 | 自研二级 bootloader、flash XIP（MMU 映射）、分区表、驱动框架、GPIO/SPI/I2C | 大内核 XIP 启动、驱动单元测试框架 |
| M6 | 自研文件系统（flash 上）+ 串口 Shell | 掉电一致性用例（QMP 复位模拟掉电） |
| M7 | 以太网：QEMU open_eth ↔ 真机 EMAC（QEMU 不仿真 WiFi，WiFi 真机再调） | ARP/ping 回环用例（user-mode 网络） |
| M8 | SMP：CPU1 启动与核间中断、负载均衡 | 双核并行压力与一致性用例 |

每个里程碑都以"能用测试台自动验收"为完成标准，回归用例只增不减。
