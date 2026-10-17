# 真机带板实验记录（2026-10-17，COM3 / CH340 / ESP32-S3）

首次真机烧录与诊断会话完整日志（第一轮：XIP 取指攻坚，已判死）。

**当前状态（2026-10-17 晚，第二轮）：真机已稳定启动并完成 M9 双核带板验证。**
0x42 flash XIP 取指在硅片上不可用（不再追），hw 平台改为**默认全 RAM 布局**
（代码进 IRAM、只读数据进 DRAM，ROM 直接加载两段 RAM 后执行，不依赖 MMU/icache）；
CPU1 需先经 `SYSTEM_CORE_1_CONTROL_0` 释放复位才认入口寄存器，释放后成功上线；
super watchdog 用 `RTC_CNTL_SWD_CONF` 关闭；M9 的 CPU1 tick / IPI / 绑核任务 /
负载均衡迁移全部在板上实测通过。详见文末「第二轮」一节。

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
- **真机外设**：hw 的 DW GMAC 后端未实现（`emac_init` 跳过、网络禁用），所以 M7
  网络栈在真机未跑。
- **真机 CPU1 tick 抢占**（`smp preempt on`）：代码路径含 +84 修复，默认关，未做真机
  稳定性验证。
- **调试开关**：`TUDOU_BOOT_TRACE=1` 会开启串口阶段标记；改该变量后必须
  `python tools/build.py --platform hw --clean`，否则增量编译会沿用旧目标文件。

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
