# S32K144 CAN Bootloader + App 完整工程（A/B 双分区）

基于 S32 Design Studio (S32DS) + S32K144 (Cortex-M4F) 的 **CAN UDS Bootloader + App** 一体化工程。
单个工程、两个构建配置，分别生成 Bootloader 和 App 固件。

当前版本：**v2.1（A/B 双分区 + SPI Flash 分区表备份 + ADC 电压监控 + 槽位 LED 指示，全部验证通过）**

---

## 开发阶段记录

| 阶段 | 内容 | 状态 |
|------|------|------|
| v0.1 | 最小系统模板（时钟 + LED + UART + CAN 基础收发） | ✅ |
| v0.2 | CAN 环形队列改造，解耦中断与业务 | ✅ |
| v0.3 | UDS 基础服务（0x10 / 0x11 / 0x22） | ✅ |
| v0.4 | Flash 驱动集成 + UDS 0x34/0x36/0x37 下载链路 | ✅ |
| v0.5 | S3Server 超时保护 + 自动跳 App | ✅ |
| v0.6 | Boot 模式选择（上电先尝试跳 App） | ✅ |
| v0.7 | 固件 CRC32 校验（0x2E WriteDID） | ✅ |
| v0.8 | 双构建配置 + Boot↔App 双向跳转 | ✅ |
| v0.9 | UART 中断接收 + App 触发升级（发 'U'） | ✅ |
| v1.0 | CANoe 自动刷写脚本 + 完整链路验证 | ✅ |
| v1.1 | 0x27 安全访问（seed-key 解锁） | ✅ |
| v1.2 | Jump_To_App 三条路径统一 Deinit（防 HardFault） | ✅ |
| v1.3 | CAN ID 合规化（0x7E0/0x7E8）+ 升级标志迁移到 Boot 保护区 0xE000 | ✅ |
| v1.4 | CANoe 传输容错（NRC 0x73 重同步 + 200ms 超时 + 5 次重试） | ✅ |
| v1.5 | CAN 非阻塞发送（消除阻塞式发送 ~1s 卡顿） | ✅ |
| **v2.0** | **A/B 双分区完整实现（双镜像 + 0x22 选槽 + 原子提交）** | ✅ 已验证 |
| **v2.1** | **分区表 SPI Flash 备份 + ADC 供电电压监控（NRC 0x22 拦截）+ 槽位 LED 指示** | ✅ 已验证 |

---

## 芯片规格

| 项目 | 内容 |
|---|---|
| MCU | **S32K144** (Cortex-M4F, LQFP100) |
| SDK | S32SDK_S32K1XX_RTM_4.0.2 |
| 外部晶振 | 8 MHz（SOSC） |
| SPLL 倍频 | ×40（VCO 320 MHz，输出 160 MHz） |
| 核心频率 | 80 MHz（RUN/HSRUN 模式） |
| Flash | 512 KB |
| SRAM | 60 KB |

---

## 时钟架构

```
        ┌─────────────────────────────────────────────┐
        │              外部晶振 (SOSC)                  │
        │              8 MHz                           │
        └─────────────────┬───────────────────────────┘
                          │
        ┌─────────────────▼───────────────────────────┐
        │    SPLL: VCO = 8MHz × 40 = 320 MHz          │
        │    SPLL_CLK_OUT = VCO ÷ 2 = 160 MHz         │
        └─────────────────┬───────────────────────────┘
                          │
        ┌─────────────────▼───────────────────────────┐
        │    DIVCORE (÷2)                              │
        │    CORE_CLK = 160 ÷ 2 = 80 MHz               │
        └─────────────────┬───────────────────────────┘
                          │
              ┌───────────┴───────────┐
              │                       │
    ┌─────────▼─────────┐   ┌─────────▼─────────┐
    │   DIVBUS (÷2)     │   │  DIVSLOW (÷4)     │
    │ BUS_CLK = 40 MHz  │   │ FLASH_CLK = 20 MHz│
    └────────────────────┘   └───────────────────┘
```

| 时钟域 | 频率 | 用途 |
|--------|------|------|
| **CORE_CLK** | 80 MHz | CPU 核心 |
| **BUS_CLK** | 40 MHz | 总线外设（GPIO、UART 等） |
| **FLASH_CLK** | 20 MHz | Flash 存储器 |
| **SOSCDIV2_CLK** | 8 MHz | LPUART1 时钟源 |
| **FIRCDIV1_CLK** | 48 MHz | CLKOUT、RTC 时钟源 |

---

## Flash 分区规划（A/B 双分区）

```
0x00000000 ┌────────────────────────────┐
           │  Boot 代码区（56KB）       │  0x0000 ~ 0xDFFF
0x0000E000 ├────────────────────────────┤  ← 升级标志区 UPGRADE_FLAG_ADDR（1 扇区）
0x0000F000 ├────────────────────────────┤  ← 分区描述表 PARTITION_TABLE_ADDR（1 扇区）
           │                            │
0x00010000 ├────────────────────────────┤  ← Slot A（App，224KB）
           │  Slot A  (0x10000~0x47FFF) │
0x00048000 ├────────────────────────────┤  ← Slot B（App，224KB）
           │  Slot B  (0x48000~0x7FFFF) │
0x00080000 └────────────────────────────┘
```

**关键地址定义（[flash_app.h](file:///c:/Users/10608/Documents/NXP_S32_3.4/demo_s32k144/src/flash_app.h)）：**
```c
#define UPGRADE_FLAG_ADDR    0x0000E000U   /* 升级标志（Boot 保护区） */
#define PARTITION_TABLE_ADDR 0x0000F000U   /* 分区描述表（A/B 状态） */
#define SLOT_A_BASE          0x00010000U   /* Slot A 基址 */
#define SLOT_B_BASE          0x00048000U   /* Slot B 基址 */
#define SLOT_SIZE            0x00038000U   /* 单槽 224KB */
```

**升级标志：** `0x5AA55AA5` 写入 0xE000（[app.h](file:///c:/Users/10608/Documents/NXP_S32_3.4/demo_s32k144/src/app.h)），App 发 'U' 触发，Bootloader 检测到则留在 Bootloader 等待 UDS 下载。

---

## A/B 双分区设计（v2.0 核心）

### 为什么需要两份镜像

Cortex-M 的程序地址在**编译链接时定死**（向量表、函数、常量都是绝对地址），因此：

| 镜像 | 链接脚本 | 链接基址 | 复位向量 PC |
|---|---|---|---|
| **app_a.bin** | `S32K144_64_app_a.ld` | 0x10000（Slot A） | 0x00010401 |
| **app_b.bin** | `S32K144_64_app_b.ld` | 0x48000（Slot B） | 0x00048401 |

同一份源码链接两次得到两份成品。**镜像的链接地址必须 = 目标槽的地址**，否则跳转时 Bootloader 的 PC 合法性校验（`PC ∈ [slotBase, slotBase+224KB)`）会拒绝，App 起不来。

### 升级流程（写入非激活槽 + 原子切换）

```
上位机(CANoe)                              Bootloader
   │ 0x10 03 扩展会话                           │
   │──────────────────────────────────────────→│ 回 0x50
   │ 0x22 F19A 读激活槽（新增）                 │
   │──────────────────────────────────────────→│ 回 0x62 F1 9A <slot>
   │  ← 根据激活槽选镜像：                      │
   │    激活=A → 目标B → app_b.bin             │
   │    激活=B → 目标A → app_a.bin             │
   │ 0x27 01/02 安全解锁                        │
   │ 0x34 请求下载（地址字段被忽略）            │
   │──────────────────────────────────────────→│ 写"非激活槽"并擦除
   │ 0x36 ×N 传数据                             │
   │ 0x37 退出传输                              │
   │ 0x2E FF01 <CRC32>                          │
   │──────────────────────────────────────────→│ CRC 校验通过
   │                                            │ 提交分区表（写新槽信息+翻转激活槽）
   │                                            │ 擦除升级标志
   │ 0x11 ECU 复位                              │
   │──────────────────────────────────────────→│ 复位 → 跳"新激活槽" App
```

**关键点：**
- **刷哪个区 = Bootloader 决定**（永远写非激活槽），上位机只通过 0x22 F19A **读取**激活槽来**选对镜像**。
- **提交 = 原子操作**：分区表（0xF000）一次擦写完成"写新槽信息 + 翻转激活槽"。
- **掉电安全**：中途断电只毁非激活槽，激活槽（运行中的固件）完好，上电照常启动。

### 断电测试结果（已验证 ✅）

在 A 区下载传输中途断电，重新上电后：
1. **升级标志在下载开始时已被擦除**（防止重启后重复进入 Boot），上电 Boot 读标志 = 无 → 直接读描述表 → 激活槽还是 B → **跳 B 区旧固件，正常启动**
2. 再次发 'U' → Bootloader 读激活槽仍为 B → 继续选 A 区下载（分区表未被"半提交"，无脏状态）

---

## 双构建配置

| 构建配置 | 链接脚本 | 预处理宏 | 起始地址 | 用途 |
|---------|---------|---------|---------|------|
| **Boot_FLASH** | `S32K144_64_flash.ld` | 无 | 0x00000000 | Bootloader 固件 |
| **Boot_RAM** | `S32K144_64_ram.ld` | 无 | RAM | 调试用（不烧 Flash） |
| **APP_A** | `S32K144_64_app_a.ld` | `BUILD_APP` | 0x00010000 | App（Slot A 镜像） |
| **APP_B** | `S32K144_64_app_b.ld` | `BUILD_APP` | 0x00048000 | App（Slot B 镜像） |

App 切 IDE 构建配置分别编译：选中 **APP_A** / **APP_B**，各按一次 `Ctrl+B`，自动生成 `APP_A/app_a.bin` 和 `APP_B/app_b.bin`（已配置 Create Flash Image 自动出 bin）。

同一套源文件，[main.c](file:///c:/Users/10608/Documents/NXP_S32_3.4/demo_s32k144/src/main.c) 中用 `#ifdef BUILD_APP` 切换：
```c
#ifdef BUILD_APP
/* App 构建：只调用 app_main() */
int main(void) { return app_main(); }
#else
/* Bootloader 构建：完整初始化 + UDS 循环 */
int main(void) { ... }
#endif
```

> A/B 双镜像（app_a/app_b）由 IDE 双构建配置产出，编译完成即自动转为 bin，CANoe 脚本可直接从编译目录读取（见"快速开始"）。

---

## 工程结构

```
demo_s32k144/
├── src/                               ← 公共源代码
│   ├── main.c                         ← 主程序（双构建入口）
│   ├── app.h / app.c                  ← App 主逻辑 + 升级标志定义
│   ├── led.h / led.c                  ← LED 驱动
│   ├── uart.h / uart.c                ← LPUART1 驱动（中断接收）
│   ├── can.h / can.c                  ← FlexCAN2 驱动（环形队列 + 非阻塞发送）
│   ├── uds.h / uds.c                  ← UDS 服务 + S3Server 超时 + 跳转
│   ├── partition.h / partition.c      ← A/B 分区描述表管理（含 SPI Flash 备份/恢复）
│   ├── spi_flash.h / spi_flash.c      ← W25Q64 SPI NOR 驱动（GPIO 片选 + 100 kHz）
│   ├── adc_mon.h / adc_mon.c          ← 供电电压监控（ADC0_CH2，欠压拦截 UDS）
│   └── flash_app.h / flash_app.c      ← Flash 封装（擦写校验/CRC32）
├── board/                             ← 板级配置（Config Tools 生成）
├── Project_Settings/Linker_Files/
│   ├── S32K144_64_flash.ld            ← Bootloader 链接脚本
│   ├── S32K144_64_app_a.ld            ← Slot A 链接脚本（224KB）
│   └── S32K144_64_app_b.ld            ← Slot B 链接脚本（224KB）
├── .project / .cproject               ← S32DS Eclipse 工程文件
│
├── canoe/
│   └── uds_download.can               ← CANoe CAPL 自动升级脚本（v2，含选槽）
├── tools/                             ← 上位机辅助工具
│   ├── uds_download.py                ← Python 离线 UDS 报文生成工具
│   ├── check_crc32.py                 ← CRC32 一致性校验工具
│   └── uds_frames.csv                 ← 参考报文清单（Python 工具生成）
│
├── Boot_FLASH/                        ← Bootloader 编译输出（IDE 生成）
├── Boot_RAM/                          ← RAM 调试编译输出（IDE 生成）
├── APP_A/                             ← app_a.elf + app_a.bin（IDE 生成）
├── APP_B/                             ← app_b.elf + app_b.bin（IDE 生成）
│
├── .gitignore
└── README.md
```

---

## 引脚分配

| 引脚 | 功能 | 说明 |
|------|------|------|
| **PTD15** | GPIO 输出 | LED0（**蓝灯**），**低电平点亮** |
| **PTD16** | GPIO 输出 | LED1（**绿灯**），**低电平点亮** |
| **PTC6** | LPUART1_RX | 串口接收 |
| **PTC7** | LPUART1_TX | 串口发送 |
| **PTC16** | CAN2_RX | FlexCAN2 接收 |
| **PTC17** | CAN2_TX | FlexCAN2 发送 |
| **PTB14** | LPSPI1_SCK | W25Q64 时钟（ALT3） |
| **PTB15** | LPSPI1_SIN | W25Q64 MISO / DO（ALT3） |
| **PTB16** | LPSPI1_SOUT | W25Q64 MOSI / DI（ALT3） |
| **PTB17** | GPIO 输出 | W25Q64 片选 CS#（**低有效**，软件拉片选） |
| **PTA6** | ADC0_CH2 | 供电电压监控输入（0–3.3V，板上接线柱 ADC0-2） |
| **PTA4** | SWD_DIO | SWD 调试数据线 |
| **PTC4** | SWD_CLK | SWD 调试时钟线 |

---

## Boot 模式流程图

```
上电复位
    │
    ▼
┌──────────────────────────────────┐
│  Bootloader 启动                  │
│  - 初始化时钟/引脚/LED/UART       │
└──────────────┬───────────────────┘
               │
               ▼
┌──────────────────────────────────┐
│  检查 0x0000E000 处的升级标志？    │
└──────┬─────────────────┬──────────┘
       │ == MAGIC         │ != MAGIC
       ▼                  ▼
┌───────────────┐    ┌─────────────────────────┐
│ 停在 Bootloader│    │ 尝试跳转到激活槽 App？    │
│ (等 UDS 下载)   │    │ - 读 App MSP/Reset向量  │
└───────┬───────┘    │ - 检查地址合法性          │
        │            └──────┬──────────────┬────┘
        ▼                   │ 合法         │ 不合法
┌────────────────────┐      ▼              ▼
│ UDS 升级流程        │   跳转成功          ┌──────────────┐
│ 0x10→0x22→0x27     │   进入 App          │ 5 秒 S3Server │
│ →0x34→0x36×N       │                     │   倒计时     │
│ →0x37→0x2E→0x11    │                     └──────┬───────┘
└────────┬───────────┘                            │
         │ CRC 通过 → 提交分区表(翻转激活槽)        │ 倒计时结束
         │ 擦除升级标志                            ▼
         ▼                                 回到"尝试跳激活槽 App"
    0x11 复位 → 跳新激活槽
```

---

## LED 状态指示

硬件对应：`PTD15 = LED0 = 蓝灯`，`PTD16 = LED1 = 绿灯`，**低电平点亮**（`led.c` 的 `LED_ON_LEVEL = 0`）。

| 状态 | 蓝灯 (LED0/PTD15) | 绿灯 (LED1/PTD16) |
|------|------------------|------------------|
| **Bootloader 运行中** | 500ms 同步闪烁 | 500ms 同步闪烁 |
| **App 运行，激活槽 = A** | 熄灭 | **200ms 闪烁** |
| **App 运行，激活槽 = B** | **200ms 闪烁** | 熄灭 |
| **卡死 / 未启动** | — | — |

激活槽取自分区描述表（`0xF000` 的 `active_slot`），串口横幅同一时间打印 `Active slot: A|B`，两处可互相印证。

---

## UDS 诊断服务完整列表

| SID | 服务 | 子功能/DID | 说明 |
|-----|------|-----------|------|
| **0x10** | 会话控制 | 0x01 默认 / 0x03 扩展 | 切换会话，重置 S3Server 计时器 |
| **0x11** | ECU 复位 | 0x01 硬复位 | 100ms 后 NVIC 系统复位 |
| **0x22** | 读 DID | 0xF189 版本号 / **0xF19A 激活槽** | 0xF19A 用于上位机选镜像 |
| **0x27** | 安全访问 | 0x01 请求种子 / 0x02 发送密钥 | seed-key 解锁，未解锁时 0x34 被拒绝 (NRC=0x33) |
| **0x2E** | 写 DID | 0xFF01 (4 字节 CRC32) | 校验 App Flash 完整性，通过则提交升级+清除标志 |
| **0x34** | 请求下载 | 4 字节地址 + 2 字节大小 | **需先解锁**，擦除目标槽，进入升级模式 |
| **0x36** | 传输数据 | BlockNumber + 4 字节数据 | 攒满 8 字节写一次 Flash |
| **0x37** | 退出传输 | 无 | 补 0xFF 写入剩余数据，退出升级模式 |

> **欠压保护（v2.1）**：`adc_mon` 判定供电 < 2.9 V 时，`0x34` / `0x36` / `0x2E` 直接返回 **NRC 0x22**（conditionsNotCorrect），串口对应打印 `0x34 rejected / Download aborted / Commit rejected: supply undervoltage`。下载中途被中止时会回到空闲、S3 超时后自动跳回原激活槽（不会卡在 Boot）。

### 0x27 安全访问机制

采用 seed-key 双步解锁机制，防止未授权刷写：

```
请求种子 (0x27 01)
    │
    ▼
Bootloader 生成 2 字节随机种子 seed
回复正响应：67 01 <seed_hi> <seed_lo>
    │
    ▼
上位机计算密钥：key = (seed + 0x1111) & 0xFFFF
发送密钥 (0x27 02 <key_hi> <key_lo>)
    │
    ▼
Bootloader 校验密钥
  ├─ 正确 → 回复 67 02，进入解锁状态，允许 0x34 下载
  └─ 错误 → 回复 NRC=0x35 (invalidKey)，失败 3 次后锁死返回 NRC=0x36
```

**密钥算法**（项目简化实现，面试可讲）：
```c
key = (seed + 0x1111) & 0xFFFF
```

**状态变量**（[uds.c](file:///c:/Users/10608/Documents/NXP_S32_3.4/demo_s32k144/src/uds.c)）：
- `securityUnlocked` — 解锁标志 (0=锁定, 1=已解锁)
- `securitySeed` — 当前下发的种子
- `securitySeedRequested` — 是否已发种子待校验
- `securityAttemptCounter` — 失败重试计数 (上限 3 次)

### 完整 UDS 升级流程（9 步）

```
Step 1: 0x10 03                  → 扩展会话 (正响应 0x50 03)
Step 2: 0x22 F1 9A               → 读激活槽 (正响应 0x62 F1 9A <slot>)
                                 → 激活=A → 加载 app_b.bin；激活=B → 加载 app_a.bin
Step 3: 0x27 01                  → 请求种子 (正响应 0x67 01 <seed>)
Step 4: 0x27 02 <key>            → 发送密钥 (正响应 0x67 02) → 解锁
Step 5: 0x34 00 01 00 00 XX YY   → 请求下载 (地址忽略，写非激活槽)
                                 ← 正响应 0x74 20 04 04
Step 6: 0x36 <BlockNum> <4字节> × N → 传输数据块 (正响应 0x76)
Step 7: 0x37 00                  → 退出传输 (正响应 0x77)
Step 8: 0x2E FF 01 <CRC[3:0]>    → CRC 校验 (正响应 0x6E FF 01 00)
                                 → CRC OK → 提交分区表(翻转激活槽) → 擦标志
Final : 0x11 01                  → ECU 复位 (正响应 0x51 01)
                                 → Bootloader: 无标志 → 跳新激活槽 App
```

---

## S3Server 超时机制

| 条件 | 行为 |
|------|------|
| 上电后 5 秒内无有效 UDS 请求 | 尝试跳转到激活槽 App |
| 收到任何有效 UDS 请求 | 重置计时器为 5 秒 |
| 升级中（0x34 之后、0x37 之前） | 超时不跳，继续等待（防止中断写入） |
| App 合法性校验 | MSP 必须在 SRAM 范围 (0x20000000~0x20010000)，PC 必须在当前槽范围 (slotBase~slotBase+224KB)，否则留在 Bootloader |

---

## UART 命令接口（App 运行时）

| 命令 | 说明 |
|------|------|
| **U** 或 **u** | 触发升级：写升级标志 `0x5AA55AA5` 到 `0x0000E000` → 系统复位 → 进入 Bootloader |

---

## CANoe 自动刷写说明

提供 [uds_download.can](file:///c:/Users/10608/Documents/NXP_S32_3.4/demo_s32k144/canoe/uds_download.can)（CAPL v2 脚本），自动完成上述 9 步 UDS 流程。

**CAPL v2 特性：**
- 0x22 F19A 读激活槽 → **自动选择 app_a.bin / app_b.bin**
- 数据块超时重发 + **NRC 0x73 自动重同步**（响应偶发丢失时自愈，不中断刷写）
- 单块超时 200ms，重试 5 次
- 镜像文件自动查找（CANoe 工程目录 → 工程源码目录 → `APP_A/B` 编译目录，三级回退）

### 使用方法

1. 在 S32DS 里编译 APP_A / APP_B，bin 自动产出到对应编译目录
2. 打开 CANoe，配置 CAN 通道波特率 500kbps，ID 过滤关闭
3. 载入 `canoe/uds_download.can` 脚本（修改后需重新 Build，F8）
4. App 运行时串口发 'U' 进入 Bootloader（或上电无 App 时自动等待）
5. 点击 CANoe 开始测量 → 自动完成刷写 → 复位 → 跳转新激活槽 App

### CAN ID 约定

| 方向 | CAN ID | 说明 |
|------|--------|------|
| 上位机 → 板卡 | **0x7E0** | UDS 请求（CAPL 发送） |
| 板卡 → 上位机 | **0x7E8** | UDS 响应（Bootloader 回复） |

> 遵循 ISO 14229-2 物理寻址约定（0x7E0 请求 / 0x7E8 响应）。

---

## 模块说明

### LED 模块 (`led.h / led.c`)

| 函数 | 功能 |
|------|------|
| `LED_Init()` | 初始化 LED，LED0 亮、LED1 灭 |
| `LED_TurnOn(pin)` | 点亮指定 LED |
| `LED_TurnOff(pin)` | 熄灭指定 LED |
| `LED_Toggle(pin)` | 翻转指定 LED |
| `LED_ToggleBoth()` | 同时翻转两个 LED |

### UART 模块 (`uart.h / uart.c`)

| 参数 | 值 |
|------|-----|
| 实例 | LPUART1 |
| 波特率 | 115200 |
| 格式 | 8N1 |
| 发送 | 轮询 `LPUART_DRV_SendDataPolling` |
| 接收 | SDK 异步中断 + 回调（`uart_rx_callback` → `g_rxReady` 标志） |

上电串口输出：
```
========================================
  S32K144 System Boot
  CORE_CLK  = 80 MHz
  BUS_CLK   = 40 MHz
  FLASH_CLK = 20 MHz
  LPUART1   = 115200 8N1
========================================
```

### CAN 模块 (`can.h / can.c`)

| 参数 | 值 |
|------|-----|
| 实例 | FlexCAN2 |
| PE 时钟源 | OSC = 8 MHz |
| 工作模式 | Normal |
| 波特率 | 500 kbps |
| 采样点 | 87.5% |

**邮箱分配：**

| 邮箱 | 方向 | ID | 说明 |
|------|------|----|------|
| M0 | 发送 | 0x7E8 | UDS 正/负响应 |
| M1 | 接收 | 0x7E0 | UDS 请求（ID 过滤） |

**波特率计算：**
```
TQ 总数 = 1(SYNC) + (PROPSEG+1) + (PSEG1+1) + (PSEG2+1)
         = 1 + 6 + 7 + 2 = 16
Bit Rate = 8MHz / (0+1) / 16 = 500,000 bps
采样点   = (1+6+7) / 16 = 87.5%
```

**环形队列：** 中断（ISR）写队列，主循环读队列处理。

**发送方式（v1.5 修复）：** `FLEXCAN_DRV_Send` **非阻塞发送**——帧由硬件立即发出，主循环不等待 TX 完成信号。解决了阻塞式发送（`SendBlocking`）偶发卡死 ~1s、导致 CANoe 超时风暴的问题。

### Flash 封装模块 (`flash_app.h / flash_app.c`)

基于 NXP C40ASF Flash 驱动。

| 操作 | 地址对齐 | 大小对齐 |
|------|----------|----------|
| 擦除 | 16 字节 | 4KB（扇区大小） |
| 写入 | 8 字节 | 8 字节 |

| 函数 | 功能 |
|------|------|
| `FlashApp_Init()` | 初始化 Flash 驱动 |
| `FlashApp_Erase(addr, size)` | 擦除指定区域，拒绝擦除 Bootloader 区 |
| `FlashApp_Write(addr, data, size)` | 写入数据（必须 8 字节对齐） |
| `FlashApp_Verify(addr, expected, size)` | 读回校验 Flash |
| `FlashApp_CalcCRC32(addr, size)` | 查表法 CRC-32/ISO-HDLC |
| `FlashApp_WriteBootSector(addr, data, size)` | 擦写 Boot 保护区（升级标志/分区表） |

### 分区描述表模块 (`partition.h / partition.c`)

| 字段 | 说明 |
|------|------|
| `magic` | 表头魔术字 `0x50415231`（'PAR1'） |
| `active_slot` | 当前激活槽（A/B） |
| `slot_a_version/crc32/size/state` | Slot A 固件元信息 |
| `slot_b_version/crc32/size/state` | Slot B 固件元信息 |
| `boot_count / rollback_count` | 回滚预留字段（本次未启用逻辑） |
| `table_seq / table_crc` | 表序号 + 表自身 CRC32（写外部备份时更新） |

**外部备份**：每次提交升级前，先把这 80 字节写一份到 W25Q64 的 `0x000000`（`Partition_SaveToExternal()`，擦 → 写 → 读回比对）；内部表损坏时 `Partition_Init()` 会从备份读回并重新写回 `0xF000`（`Partition_RestoreFromExternal()`）。上电打印 `[Partition] seq=N active=A|B crc=ok|legacy backup=ok|none` 便于确认状态。

### SPI Flash 模块 (`spi_flash.h / spi_flash.c`)

板上 W25Q64（8 MB SPI NOR），挂在 LPSPI1 上，**片选由 GPIO 软件控制**（PTB17，低有效）。

| 项 | 值 |
|------|------|
| 总线 | LPSPI1，PTB14 / PTB15 / PTB16（SCK / MISO / MOSI，均 ALT3） |
| 片选 | PTB17，GPIO 输出，低有效，一条命令一次传输、全程不松开 |
| 速率 | 100 kHz（源时钟 SOSCDIV2 = 8 MHz） |
| 容量 / 结构 | 8 MB，页 256 B，扇区 4 KB |
| 当前用途 | 分区描述表备份（`0x000000`，只占 80 字节） |

| 函数 | 功能 |
|------|------|
| `SpiFlash_Init()` | 初始化 LPSPI1 + 读 JEDEC ID（期望 `0xEF4017`）判断芯片是否在位 |
| `SpiFlash_IsPresent()` | 芯片在线标志（不在位时所有上层调用安全跳过） |
| `SpiFlash_Read(addr, buf, len)` | 读数据（≤256 字节，一次传输完成） |
| `SpiFlash_WritePage(addr, buf, len)` | 写一页（≤256 字节，拒绝跨页） |
| `SpiFlash_EraseSector(addr)` | 擦 4 KB 扇区（地址自动向下对齐） |
| `SpiFlash_WaitReady(timeoutMs)` | 轮询 WIP 位等待擦写完成 |

> ⚠️ 用 LPSPI 硬件自动片选（PCS3）时，多字节命令帧中途会松开片选，W25Q64 会把后续字节当成新命令，读回全 `0xFF`。所以这里坚持 **GPIO 软件片选 + 100 kHz**（与板厂例程一致）。

### ADC 电压监控模块 (`adc_mon.h / adc_mon.c`)

ADC0_CH2（PTA6）采样供电电压，用于升级前与升级中的欠压保护。

| 项 | 值 |
|------|------|
| 通道 | ADC0 外部通道 2（PTA6，板上接线柱 ADC0-2） |
| 量程 | 0–3.3 V，12 位（`mv = raw * 3300 / 4095`） |
| 采样 | 主循环约 10 ms 一次，8 点滑动平均 |
| 阈值 | ≥3100 mV 正常 / 2900–3100 mV 警告 / <2900 mV 危险（连续 3 次确认） |

危险状态下 `0x34` / `0x36` / `0x2E` 一律返回 **NRC 0x22**（conditionsNotCorrect）并在串口打印原因。Bootloader 运行期持续采样；App 运行期目前不采样。

> 演示用法：把 ADC0-2 接电位器（3.3 V — 抽头 — GND），刷写过程中拧低即可复现"掉压中止"。

---

## 快速开始

### 1. S32DS 图形界面编译（纯 IDE，无脚本）

1. 选中 **Boot_FLASH** 构建配置 → `Ctrl+B` 编译 Bootloader
2. 选中 **APP_A** → `Ctrl+B`，产物 `APP_A/app_a.elf` + `app_a.bin`（Create Flash Image 自动转换）
3. 选中 **APP_B** → `Ctrl+B`，产物 `APP_B/app_b.elf` + `app_b.bin`

切换构建配置：右键工程 → Build Configurations → Set Active → 选配置。

### 2. 烧录 Bootloader

用 J-Link 下载 `Boot_FLASH/demo_s32k144.hex` 或 elf 到板子。

### 3. 刷写 App（两种方式）

**方式 A：J-Link 直接烧**（开发调试最快）
- Slot A：烧 `APP_A/app_a.bin` 到 0x10000
- Slot B：烧 `APP_B/app_b.bin` 到 0x48000

**方式 B：CANoe UDS 刷写（真实 A/B 升级流程）**
- 上电 Bootloader → CANoe 开始测量 → 自动读槽选镜像 → 下载 → 复位
- 或 App 正常运行 → 串口发 **U** → 复位 → Bootloader 等待 → CANoe 刷写

---

## 快速测试清单

| # | 场景 | 操作 | 预期结果 |
|---|------|------|----------|
| 1 | 空板（无 App）上电 | 烧 Bootloader，不烧 App | 蓝绿灯 500ms 交替闪，5 秒打印 `No valid App` |
| 2 | 有 App 上电 | Bootloader + App 都烧好 | 打印 Boot 信息 → `Jumping to App...` → 进入 App |
| 3 | App LED 指示 | 观察 LED | 蓝灯常亮，绿灯 200ms 快闪 |
| 4 | App 串口升级 | 串口发 `U` | `Upgrade triggered...` → 复位 → Bootloader 打印 `Upgrade flag detected` |
| 5 | CANoe 自动刷写 | CANoe 开始测量 | 自动 9 步流程（0x22 选槽 + 0x27 解锁）→ 复位 → 新激活槽 App 启动 |
| 6 | A/B 槽位翻转 | 连续两次升级 | 每次升级后 App 在 A、B 槽交替运行 |
| 7 | 掉电安全 | 升级传输中断电再上电 | 5 秒后旧固件照常启动（见下节） |

### 断电测试（A/B 核心验证）

```
① App 发 'U' → Bootloader
② CANoe 下载传输到一半 → 拔板子电源
③ 重新上电 → "Upgrade flag detected" → S3 等待
④ 等 5 秒（不做任何操作）→ Bootloader 跳激活槽 → 旧固件照常运行 ✅
⑤ 再发 'U' → 重新下载 → 完整跑完 → 新固件从另一槽启动
```

---

## 可靠性设计要点

1. **跳转前 Deinit 外设（v1.2）**：`Jump_To_App()` 统一调用 `CAN_Deinit()` + `UART_Deinit()`，清理残留中断防止 App 侧 HardFault。顺序：打印 → CAN_Deinit → UART_Deinit → 关全局中断 → 设 VTOR → 设 MSP → 跳转。
2. **非阻塞 CAN 发送（v1.5）**：主循环不被 TX 完成信号阻塞，消除 ~1s 卡顿。
3. **传输容错（v1.4）**：CANoe 收到 NRC 0x73（重复块）时自动重同步续发，单帧响应丢失可自愈。
4. **原子提交（v2.0）**：分区表一次擦写完成"写新槽 + 翻转激活槽"，断电不产生半提交状态。
5. **ECU Reset 优先**：走 0x11 完整复位再跳 App，避免 UDS 上下文直接跳转残留状态卡死。
6. **看门狗（Demo 阶段未启用）**：S32 SDK 的 `SystemInit()` 默认 `DISABLE_WDOG=1`，通过 `0xD928C520` 解锁序列关闭 WDOG。Demo 阶段关闭是因为调试期不需要、且 Flash 操作期间喂狗会干扰时序。**量产方案**：改为不禁用，Boot 刷写超时喂狗、App 运行周期喂狗，超时复位回安全状态。

---

## Git 忽略规则（已在 .gitignore 中配置）

**✅ 提交：**
- `src/` 所有代码
- `board/` 配置
- `Project_Settings/Linker_Files/` 链接脚本
- `.project` / `.cproject`
- `canoe/uds_download.can`（CANoe 脚本）
- `tools/`（Python 工具脚本）
- `.gitignore` / `README.md`

**🚫 忽略：**
- `Boot_*/`、`APP_*/` 编译输出目录（`app_a.elf` / `app_a.bin` / `.map` 全在里面）
- `*.o`、`*.obj`、`*.elf`、`*.bin`、`*.hex`、`*.map`、`*.siz`、`*.d`、`*.su`、`*.lst`
- `*.cbf`（CANoe 编译产物，改 `.can` 后需在 CANoe 里 F8 重新生成）
- `Debug_Configurations/`、`.settings/`
- `.cproject.bak`、`.project.bak`、`*.bak`、`*.orig`（改 IDE 工程文件前的备份）
- `SDK/` NXP SDK 本地副本
- `*.mex` S32DS 配置工程（当前文件已被 git 跟踪，改动仍会正常出现在 `git status` 里）

---

## 注意事项

1. **外部晶振 8 MHz**：改频率需修改 `clock_config.c` 并重新计算 SPLL 倍频。
2. **CANoe 数据数组大小**：CAPL 里 `gAppData[65536]`，App 超过 64KB 需增大（0x34 大小字段也只有 2 字节）。
3. **镜像链接地址必须匹配目标槽**：app_a→0x10000、app_b→0x48000，混用会导致跳转校验拒绝。
4. **Flash 写入对齐**：地址/长度 8 字节对齐；0x36 每帧 4 字节、攒满 8 字节写一次，最后一块在 0x37 补 0xFF。
5. **CRC 算法一致性**：固件查表 CRC-32/ISO-HDLC，Python `zlib.crc32`，CAPL 同多项式 0xEDB88320，三者必须一致。
6. **升级标志地址**：0xE000（Boot 保护区），App 与 Bootloader 必须使用同一地址（历史版本曾用 0x3FFF0，已废弃）。
7. **0x27 请求长度**：子功能 0x01 为 2 字节（`02 27 01`），0x02 为 4 字节（`04 27 02 key_hi key_lo`），分别校验。
8. **CAPL 脚本修改后需重新 Build**（F8），否则加载的是旧编译产物 .cbf。
9. **LED 是低电平点亮**：PTD15 = LED0 = 蓝灯、PTD16 = LED1 = 绿灯，`led.c` 里 `LED_ON_LEVEL = 0`。换板子若为高电平点亮，把这个宏改成 `1`。
10. **SPI 片选必须 GPIO 软件控制**：不要改回 LPSPI 的 PCS3 硬件片选——多字节命令帧中途会松开片选，芯片把后续字节当新命令，读回全 `0xFF`。
11. **App 两个构建配置要分别重编**：改过 App 代码后必须 APP_A、APP_B 各 Build 一次，否则两个槽里装的是同一份旧镜像（槽位灯会永远显示同一个槽，容易误判成代码 bug）。
12. **分区表备份与正本内容一致**：提交时先写 W25Q64（`0x000000`）再写内部 `0xF000`；若内部表损坏，上电会从外部读回并重写正本，串口打印 `Partition table restored from SPI flash.`。
13. **ADC 欠压只看 Boot**：App 运行期不采样电压，所以"运行中掉压"不会触发保护；演示时应在刷写（Boot）阶段拧电位器。

---

## 许可证

SDK 源码版权归 NXP 所有，遵循 SDK 自带许可条款。`src/` 目录下用户编写的代码遵循自由使用原则。
