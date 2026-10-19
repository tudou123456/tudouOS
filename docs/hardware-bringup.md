# 真机带板实验记录（2026-10-17，COM3 / CH340 / ESP32-S3）

首次真机烧录与诊断会话完整日志（第一轮：XIP 取指攻坚，已判死）。

**当前状态（2026-10-17 晚，第二轮）：真机已稳定启动并完成 M9 双核带板验证。**
0x42 flash XIP 取指在硅片上不可用（不再追），hw 平台改为**默认全 RAM 布局**
（代码进 IRAM、只读数据进 DRAM，ROM 直接加载两段 RAM 后执行，不依赖 MMU/icache）；
CPU1 需先经 `SYSTEM_CORE_1_CONTROL_0` 释放复位才认入口寄存器，释放后成功上线；
super watchdog 用 `RTC_CNTL_SWD_CONF` 关闭；M9 的 CPU1 tick / IPI / 绑核任务 /
负载均衡迁移全部在板上实测通过。详见文末「第二轮」一节。

**网络补充（2026-10-19 事实核查）：** ESP32-S3 硅片没有内部以太网 MAC，
「真机 EMAC / DesignWare GMAC / RTL8211 PHY」的旧记录不成立（见
[ethernet.md](ethernet.md) §8）；QEMU 的 open_eth 只是机器模型测试外设。真机远程
MCP 当前走串口传输（[serial-mcp.md](serial-mcp.md)），ARP/ping/UDP 仅在 QEMU
验证；真机以太网需外挂 SPI 模块（未实现/未到货，见 §6）。

## 已实证事实（按可信度排序）

1. **ROM→内核启动链路 ✅**：potato_boot.bin（flash 0x0）双 RAM 段加载、
   入口跳转正确；串口 ROM 日志/内核探针输出正常。
2. **MMU 表语义（推翻 E-03 结论）**：表 @0x600C5000 为 **32 位槽/槽内
   16 位表项值**（[13:0]=64KB 物理页号，[14]=invalid，复位值 0x4000）；
   **16 位读写对表完全无效**（读恒 0、写丢弃）。QEMU 模型与硅片一致，
   E-03 的"TRM 16 位"指表项值宽度而非槽位步进。
3. **0x3C DBUS 窗口读完全可用**：槽 0=物理页 1 时 `l32i @0x3C0045b8`
   返回与 flash 逐字节一致的正确数据——映射/表/dcache→flash 通路健康。
4. **0x3C 窗口禁止取指**：EXCCAUSE=2（InstructionFetchError）。
5. **0x42 IBUS 窗口数据读恒返 0**（SHUT 清除后）或 0xBAD00BAD（SHUT=3
   时）——该读不构成映射有效性判据（探针实验方法论教训）。
6. **ROM 跳转前 ICACHE_CTRL1(0x600C4064) SHUT_CORE0/1_BUS=0b11**：
   ROM 把两条核总线都关了。在 cache 使能之后清 SHUT 会陷入"读全 0"
   错乱态，必须先清再开。
7. **ROM 引导镜像的 flash 映射段路径存在但不可靠**：手工构造页对齐
   （段数据落 0x10020）单镜像后 ROM 打印 `load:0x42000020,len:...`
   （接受并处理映射段！），但随后经其自身映射读校验和得到 0xFF 而拒
   绝（IDF 二级引导为纯 IRAM，此路径在野外几乎无人踩）。
8. **ROM cache 驱动的 MMIO 纪律**：每次 l32i/s32i 前都有 `memw`。
   驱动位于 0x600C4060/0x64/0x88/0xA0 字面量池周围（0x4004ba30/
   0x4004c550/0x4004e4bc，ROM 转储反汇编已留档 build 分支目录）。
9. **看门狗**：wdt_early_disable（TIMG0/TIMG1/RTC_CNTL）真机有效；
   但**超级看门狗（rst:0x12 SUPER_WDT）不受其影响**，长耗时诊断需
   另找 SWD 寄存器（键值疑为 0x8F1D312A，未验证）。
10. **eps2(E-02 后续)**：本地汇编器实证 esp32s3 配置 SR 编号——
    EPC1=177、EPC2..6=178..182、EPS2..4=194..196、176=configid0、
    161=dbreakc1、193 无效、**eps1 名字不存在**。真机 EPS1 编号仍悬而
    （候选 176/194 均有疑），hw 构建已在 switch.S 双写、vectors.S 双读。

## 'X' 单次成功现场（唯一一次 0x42 取指成功）

配置组合（boot.S v2+探针修复链，历史文件见 D:\tudouhw 快照）：
disable → SHUT 清 → prelock/preload/autoload 清零 → **tag 阵列断电重启**
（0x600C4068 FORCE_PD→延时→FORCE_PU）→ 写表（s32i+memw）→ SYNC 全量
无效化（0x600C4088 序列）→ IBUS/DBUS 边界写全窗口 → CACHE_ILG 清 →
**DCACHE_CTRL|=EN** → **ICACHE_CTRL|=0xF（32KB/8路/32B块几何）** →
UART 打印级延时 → call0 IROM 函数 → **执行成功打印 'X'** → 返回路径
第二次取指崩溃。

该组合的确定性重建（清掉打印间延时/换目标函数/预烫数据读/降 40MHz）
均失败——**存在未识别的时序或状态依赖**。

## 剩余嫌疑清单（下一步）

1. **调用 ROM cache 驱动函数**（最正统）：ROM 符号地址需 esp32s3.rom.ld
   （github 不可达时用 ROM 转储+反汇编定位 `Cache_Enable_ICache` 等，
   转储工具链已在 D:\tudouhw 验证可用）。ROM 函数为窗口 ABI，call0 调用
   需注意 a8-a15 会进被调窗口（调用前不保活即可）。
2. **SPI/flash 信号完整性**：换 QIO 模式、降频（注意：改镜像头 40m 后
   ROM 日志仍 clock div:1，频控疑未生效，需查 esptool v5 头编码）。
3. **ICACHE_PRELOAD 手动预载一行**做受控取指实验（隔离 miss 路径）。
4. **SWD（超级看门狗）寄存器**：长诊断前置关闭。
5. 换一块正品模组对照（排除板级硬件）。

## 工具与资产

- `tools/hw_monitor.py`：串口监控/复位/命令注入（COM3, 115200）。
- `tools/build.py --platform hw`：hw 构建（obj-hw 独立目录、*_hw 产物）。
- `test/cases/test_build_hw.py`：hw 编译门禁（D-26）。
- D:\tudouhw：隔离快照工作台（绕开并行会话的 WIP），含 ROM 转储日志
  （romdump2.log/wins.log，字节序=每字 MSB 先）、ROM 窗口反汇编。

---

## 第二轮（2026-10-17 晚）：全 RAM 布局、CPU1 上线、M9 带板验证

第一轮结论：0x42 flash XIP 取指不可用。第二阶段目标改为“不依赖 XIP 也能跑”：
hw 平台**默认全 RAM 布局**，代码进 IRAM、只读数据进 DRAM，ROM 直接把两个 RAM 段
加载好再跳入口；QEMU 仍走 XIP 不变（67 例回归不受影响）。

### 1. 全 RAM 布局（`tools/build.py` + `kernel/linker/tudou.ld`）

- `tudou.ld` 改为 cpp 双布局：`#if TUDOU_PLATFORM == 0 || TUDOU_HW_XIP != 0` 走原 XIP
  布局；`#else`（hw 默认）走 RAM-only：`.iram_text` 收全部代码，`.rodata`/`.pot_drivers_seg`
  进 DRAM，`_irom_*` 用 0 占位。
- 链接脚本由 `cc -E -P` 预处理生成（`build/tudou<SUFFIX>.ld`），`TUDOU_HW_XIP` 由
  `--xip` 控制（保留 XIP 实验能力）。
- RAM-only 时构建只产出 `kernel_hw.elf` 与 `potato_boot_hw.bin`（含两个 RAM 段镜像，
  约 82KB），直接 `esptool write-flash 0x0`；不再有 `irom_hw.bin`/`flash_image_hw.bin`。
- 真机 ROM 日志：`load:0x3fca8000,len:0x3b74` + `load:0x40380000,len:0x1036c` +
  `entry 0x40380474`——两段 RAM 由 ROM 直接搬运，全程不碰 icache/MMU。

### 2. 两个致命坑（均已修）

1. **IRAM 只能取指、不能普通数据读**：第一版把 `.rodata` 与 `pot_drivers` 表放进
   `.iram_text`，`drivers_init` 排序时第一次 `drv->priority` 读就异常（NMI 上下文里
   再次出错→双异常→挂死→WDT）。修法：只读数据/驱动表全部放 DRAM（`> dram`）。
2. **`.bss` 清零会覆盖自身 IRAM 代码**：S3 的 SRAM 在指令/数据总线双窗口，
   旧布局 `dram 0x3FC88000` 与 `iram 0x40380000` 存在别名重叠，清 bss 到 0x3FC904F0
   时把 0x403804F0 附近代码清掉 → `IllegalInstruction`。修法：RAM-only 布局改为
   `iram 0x40380000 LENGTH 0x18000`、`dram 0x3FCA8000 LENGTH 0x34700`（到
   ESP-IDF `dram0_0_seg` 末端 0x3FCDC700），两窗口不重叠。

修完后真机稳定启动：`STUWCDV0123djkllm...` 阶段标记 + `[POTATO] hw timebase calibrated:
cpu=19000000Hz tick_period=19000cc` + `banner v0.7.0` + `READY`，
`mem iram=0x40380474-0x40390300 data=0x3fcabb0c-0x3fcabb74 bss=0x3fcabb80-0x3fcc5d90
stack=0x3fcc5d90-0x3fcc9d90 heap=0x3fcc9d90-0x3fcdc700`。

### 3. CPU1 无法上线 → `SYSTEM_CORE_1_CONTROL_0`

- 现象：`smp start` 打印 entry 地址后轮询 `online` 200ms 超时（rc=-1）。
- 根因：真机 CPU1 复位后由 `SYSTEM_CORE_1_CONTROL_0`（0x600C0000）保持复位/停核：
  bit0 RUNSTALL、bit1 CLKGATE_EN、bit2 RESETING；复位值 `0x04`（RESETING=1、RUNSTALL=1），
  此时写入口寄存器无效。QEMU 在启动时自动拉起 CPU1，故只在真机暴露。
- 修法：`pot_smp_start()` 真机分支先释放：`ctl0 &= ~(BIT0|BIT2); ctl0 |= BIT1;` 再写
  `0x600C0004`。实测 `ctl0 0x7 -> 0x2 (read 0x2)`，随后 CPU1 标记 `a→b→c→e→f` 全过，
  `cpu1 online id=1`、`confirm online=2`、rc=0。

### 4. super watchdog（SWD）周期复位

- 现象：启动约 8s 后 `rst:0x12 (SUPER_WDT_RST)`，与内核是否跑任务无关。
- 根因：`RTC_CNTL_SWD_CONF`（0x600080B4）复位值 `0x04B00000`：SWD 使能、未开自动喂、
  无软件喂→超时复位。ROM 的 RTC WDT 配置只对 rtc_wdt 有效，不关 SWD。
- 修法：`wdt_early_disable()` 追加 `wdt_swd_disable()`（写 `SWD_WPROTECT`=0xB8
  key 0x8F1D312A，`SWD_CONF` 置 bit30 `SWD_DISABLE`，再回写 WPROTECT）；并在
  tick ISR 里 `wdt_swd_feed()` 兜底（真机分支）。启动打印
  `[POTATO] hw rtc-cntl: swd-conf=0x... (want bit30)` 便于确认。修复后连续 40s 无复位。

### 5. M9 带板实测数据

- `smp stat`：`cpus=2 online=2 started=1 ipi_pending=0/0`；CPU1
  `ticks=4180 idle=... cur=cpu1idle boot=...ms`。
- `smp ipi <n>`：CPU1 `ipi_rx`/`resched` 随发送增长，`unk=0`。
- `smp spawn 1 2`：绑核任务在 CPU1 运行（`cur=smpw0`，计数持续增长）。
- `smp spawn any 4`：`cpu0 mig_out=1`、CPU1 `mig_in=1`，负载均衡迁移在真机发生；
  两个核 `sw` 均大幅增长（双核真并行）。
- 真机中断帧槽与 QEMU 不同：hw 把 SR176 存帧 +80、SR194 存 +84（QEMU 只有
  +80=SR194 有效）；`pot_do_resume_irq` 已按平台选择读 +84（hw）/ +80（QEMU）。

### 6. 仍然遗留

- **XIP 路径未修**：第一轮全部嫌疑（ROM Cache_Enable_ICache、flash 时序、PRELOAD、
  SWD 干扰等）未逐一验证；hw 默认全 RAM 后不再阻塞，`--xip` 仅作实验开关。
- **真机以太网**：ESP32-S3 硅片没有内部以太网 MAC（DesignWare GMAC + 外部 PHY
  方案不成立，见 [ethernet.md](ethernet.md) §8）；真机以太网需外挂 SPI 模块
  （W5500/DM9051 类，**未实现/未到货**），在此之前 ARP/ping/UDP 只在 QEMU 验证；
  真机远程 MCP 走串口传输（[serial-mcp.md](serial-mcp.md)）。
- **真机 CPU1 tick 抢占**（`smp preempt on`）：代码路径含 +84 修复，默认关，未做真机
  稳定性验证。
- **调试开关**：`TUDOU_BOOT_TRACE=1` 会开启串口阶段标记；改该变量后必须
  `python tools/build.py --platform hw --clean`，否则增量编译会沿用旧目标文件。
- **真机待办基线（K1/K2 已修复，K3 保留观察）**：
  - **K1 启动 tick 瞬态——已修复**（`kernel/time/ccompare_tick.c`：`tick_irq_arm()`
    重基准 `g_last_fire_cc/g_next_cc` 并清 `g_jit_seeded`，poll/handler 抖动统计跳过
    首个跨启动窗口样本）。真机新固件 READY 后连测 `jit_min≈974–999us`、
    `jit_max≈1000–1025us`，3s 空闲不变；早前 ≈45ms/79ms 记录即该一次性启动窗口
    样本，稳态抖动约 0.9–1.0ms。`TICK_GAP` 观测仍保留。
  - **K2 输出行首保护——已修复**（新增 `uart_line_begin()`：输出前若行未结束先补
    CRLF；`trace stream on` 后台 TRJ 导出与 `obs health` 打印前调用）。真机
    `stream on` 并发下 `potato> [POTATO]` 粘连计数 0，67 条 TRJ 行行首干净、JSON
    可完整解析；行尾契约未变，既有测试锚点不受影响。
  - **K3 trace 环覆盖（设计观察，不修代码）**：1kHz tick 洪流使 256 项 trace 环
    覆盖约 128ms，`obs_health` 读活环非原子；回归台已改用 `trace meta`/`trace imp`
    计数增量做事件断言。若需长窗口事件回放，应降低 tick 事件入环频率或扩容。
  - **极端并发限制**：UART 无硬行锁；`trace stream on` + 高频串口 MCP 长响应时
    建议先 `trace stream off`（详见 [serial-mcp.md](serial-mcp.md) §7）。
  - **真机串口自动回归台已完成**（`test/hw_runner.py` + `test/hw_cases/`，72 用例，
    M1–M9 全量 70 PASS / 0 FAIL / 2 显式 SKIP，见
    [test/logs/hw/summary.md](../test/logs/hw/summary.md)）；48h 长稳 soak 未跑。

### 7. 最终带板验收（正式包，2026-10-17 21:2x）

- 构建：`python tools/build.py --platform hw --clean` → `potato_boot_hw.bin` 81920B
  （RAM 段 ~46KB 压缩）；`esptool write-flash 0x0` 哈希校验通过。
- 冷启动 22s：ROM `rst:0x1 (POWERON),boot:0x8 (SPI_FAST_FLASH_BOOT)` → 内核
  `banner v0.7.0` + `[POTATO] READY`，无 Guru/异常/复位。
- `smp selftest` → `rc=0`；`smp start` → `rc=0 online=2`；CPU1 tick 计数增长
  （`ticks≈2459`）、`cur=cpu1idle`。
- `smp ipi 16`（累计 18 次发送）：CPU1 `ipi_rx=18 resched=17 unk=0`。
- `smp spawn 1 2` → `made=2`，CPU1 `sw` 增长、work 计数 705/705；再
  `smp spawn any 4` 受上限 4 约束 `made=2`，两核 work 计数持续增长。
- 结论：真机生产镜像启动稳定、控制台完整、SMP 双核通路可用；hw 全 RAM 布局
  与 `SYSTEM_CORE_1_CONTROL_0`/SWD 两处真机差异均已固化在代码里。

### 8. 真机 M1–M9 全量回归 + 串口 MCP 验收（2026-10-19 晚，第三轮）

- 平台：普通 ESP32-S3 开发板（无网口，CH340/COM3，115200）。ESP32-S3 硅片无
  内部以太网 MAC（[ethernet.md](ethernet.md) §8），远程 MCP 改走串口帧传输
  serial-json（[serial-mcp.md](serial-mcp.md)）。
- 回归台：`test/hw_runner.py` + `test/hw_cases/`（72 条用例，M1–M9 分组；逐用例
  复位 + 等 READY；与 QEMU 台架并存、`test/cases/` 互不污染）：
  `py -3.14 -m test.hw_runner --port COM3 --keep-going`。
- 结果：**PASS=70 / FAIL=0 / TIMEOUT=0 / SKIP=2（显式真机限制）**，退出码 0；
  `--smoke` 9/9 PASS。报告与逐条日志：
  [test/logs/hw/summary.md](../test/logs/hw/summary.md)。旧固件（`19:43:33`）与
  K1/K2 修复固件（`20:21:35`）两轮均为此结果（`build/_hw_full2.log`、
  `build/_hw_full3.log`）。
- 串口 MCP 验收：`py -3.14 tools/serial_mcp.py --port COM3 selftest` PASS
  （hello→tools/list 6 工具→ping→sysinfo→dump；rx_frames=4 bad_frames=0）；
  跨进程 fs 往返、同 seq 重放不重复执行、`--reset` 复位恢复、`--stdio` MCP
  客户端桥全通（证据 `build/_serial_mcp_*.log`）。
- K1/K2 修复真机验证：K1 启动瞬态不再计入 `jit_max`（稳态 ≈1.0ms，见 §6）；
  K2 `trace stream on` 并发下提示符粘连 0、TRJ 行首干净（`build/_hw_k1k2_check3.log`）。
- **最终验收（Lead 独立复跑，2026-10-19 20:3x）**：`--platform hw --clean`
  干净重建（`potato_boot_hw.bin` 85952 B，启动身份 `[POTATO] build Oct 19 2026
  20:29:27`）→ `esptool write-flash 0x0` → `Hash of data verified`
  （`build/_final_hw_flash2.log`）→ `tools/serial_mcp.py --reset selftest` PASS
  （`build/_final_hw_selftest2.log`）→ 全量回归 **70 PASS / 0 FAIL / 0 TIMEOUT /
  2 SKIP / 未执行 0，exit 0**（`build/_final_hw_runner4.log`）；同一源码 QEMU
  全量 **67/67**、exit 0（`build/_final_qemu_runner3.log`）。
- 顺带修复：`kernel/mcp/mcp.c::send_initialize()` 响应多一个 `}` 的既有 bug
  （initialize 响应原为非法 JSON；对 UDP/串口通道均生效）；`test_obs.py`
  `test_obs_panic_dump` 的半行快照 flake（分析器改喂最后一个完整行）。
- 结论：**M1–M9 控制面全部在真机跑通**。剩余真机缺口仅两项已知限制：以太网需
  外挂 SPI 模块（W5500/DM9051，未实现/未到货）、CPU1 tick 抢占（默认关、未验证）；
  另 K3（trace 环覆盖）为设计观察、K1/K2 已修复并真机验证，不阻塞功能验收。
