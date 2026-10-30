/**
 * @file    app.c
 * @brief   应用程序源文件
 * @details App 程序：LED 闪烁 + 串口打印 + 等待升级命令
 *          通过 BUILD_APP 宏启用，编译为独立 App 镜像
 */

#include "app.h"
#include "led.h"
#include "uart.h"
#include "flash_app.h"
#include "partition.h"
#include "osif.h"
#include "device_registers.h"
#include "interrupt_manager.h"
#include "sdk_project_config.h"

/* ==================== 槽位指示灯配置 ==================== */
/* 实物：PTD15 = LED0 = 蓝色，PTD16 = LED1 = 绿色。
 * 规则：跑 A 槽 → 绿灯闪（LED1，蓝灯灭）；跑 B 槽 → 蓝灯闪（LED0，绿灯灭）。
 * 想让颜色对调，把下面两个宏互换即可，不用改别处。 */
#define LED_SLOT_A_PIN   LED1_PIN   /* A 槽 = 绿色 */
#define LED_SLOT_B_PIN   LED0_PIN   /* B 槽 = 蓝色 */

/* ==================== 内部函数声明 ==================== */

static void SystemReset(void);
static void TriggerUpgrade(void);
static uint8_t App_GetActiveSlot(void);

/* ==================== 内部函数实现 ==================== */

/**
 * @brief  读取当前激活槽
 * @note   直接读 Flash 0x0000F000 处的描述表，不调用 Partition_Init()，
 *         避免 App 里再去初始化 SPI Flash / 走恢复流程。
 * @return SLOT_A / SLOT_B；描述表无效时返回 0xFF
 */
static uint8_t App_GetActiveSlot(void)
{
    const partition_table_t *tbl = (const partition_table_t *)PARTITION_TABLE_ADDR;

    if (tbl->magic != PARTITION_TABLE_MAGIC)
    {
        return 0xFFU;   /* 表无效（没刷过分区表），按 A 槽处理 */
    }

    return (tbl->active_slot == (uint32_t)SLOT_B) ? (uint8_t)SLOT_B : (uint8_t)SLOT_A;
}

/**
 * @brief  触发系统复位（NVIC System Reset）
 */
static void SystemReset(void)
{
    uint32_t regValue;

    INT_SYS_DisableIRQGlobal();
    regValue = S32_SCB->AIRCR;
    regValue &= ~(S32_SCB_AIRCR_VECTKEY_MASK);
    regValue |= S32_SCB_AIRCR_VECTKEY(FEATURE_SCB_VECTKEY);
    regValue |= S32_SCB_AIRCR_SYSRESETREQ(0x1U);
    S32_SCB->AIRCR = regValue;
    __asm volatile ("dsb 0xF" ::: "memory");
    while (1) {
        __asm volatile ("nop");
    }
}

/**
 * @brief  触发升级：写升级标志到 Flash，然后复位
 *         Bootloader 启动时检测到标志会留在 Bootloader 等待 UDS 下载
 */
static void TriggerUpgrade(void)
{
    /* 升级标志数据：magic + padding（补齐 8 字节对齐） */
    uint32_t flagData[2] = { UPGRADE_FLAG_MAGIC, 0xFFFFFFFFU };

    UART_SendString("\r\n>>> Upgrade triggered!\r\n");
    UART_SendString(">>> Writing upgrade flag...\r\n");

    /* 初始化 Flash 驱动 */
    FlashApp_Init();

    /* 擦除并写入升级标志扇区（升级标志区在 Boot 保护区，需专用接口） */
    if (FlashApp_WriteBootSector(UPGRADE_FLAG_ADDR, (const uint8_t *)flagData, 8U) != 0U)
    {
        UART_SendString(">>> ERROR: Flash write upgrade flag failed!\r\n");
        return;
    }

    UART_SendString(">>> Upgrade flag written, resetting...\r\n");

    /* 延时确保串口发送完成 */
    OSIF_TimeDelay(100);

    /* 系统复位，重启进入 Bootloader */
    SystemReset();
}

/* ==================== 公共函数实现 ==================== */

/**
 * @brief  App 主函数
 * @return 理论上不会返回
 */
int app_main(void)
{
    /* ===== 系统初始化 ===== */
    CLOCK_DRV_Init(&clockMan1_InitConfig0);
    PINS_DRV_Init(NUM_OF_CONFIGURED_PINS0, g_pin_mux_InitConfigArr0);

    /* ===== 外设初始化 ===== */
    LED_Init();
    UART_Init();

    /* ===== 判定当前运行的是哪个槽（直接读描述表） ===== */
    uint8_t activeSlot = App_GetActiveSlot();
    uint8_t slotLedPin = (activeSlot == SLOT_B) ? LED_SLOT_B_PIN : LED_SLOT_A_PIN;

    /* ===== App 启动信息 ===== */
    UART_SendString("\r\n");
    UART_SendString("========================================\r\n");
    UART_SendString("  S32K144 App Running\r\n");
    UART_SendString("  Version: ");
    UART_SendString(APP_VERSION);
    UART_SendString("\r\n");
    UART_SendString("  Active slot: ");
    if (activeSlot == SLOT_B) {
        UART_SendString("B");
    } else if (activeSlot == SLOT_A) {
        UART_SendString("A");
    } else {
        UART_SendString("? (partition table invalid)");
    }
    UART_SendString("\r\n");
    UART_SendString("  Send 'U' to enter upgrade mode\r\n");
    UART_SendString("========================================\r\n");

    /* ===== 槽位灯：A 槽亮绿灯，B 槽亮蓝灯，另一个熄灭 ===== */
    if (activeSlot == SLOT_B) {
        LED_TurnOff(LED_SLOT_A_PIN);
        LED_TurnOn(LED_SLOT_B_PIN);
    } else {
        LED_TurnOff(LED_SLOT_B_PIN);
        LED_TurnOn(LED_SLOT_A_PIN);
    }

    /* ===== 主循环：LED 心跳 + 检查升级命令 ===== */
    uint16_t ledCounter = 0;
    uint8_t rxByte;
    for (;;)
    {
        /* 检查串口是否收到 'U' 命令 */
        if (UART_ReceiveByte(&rxByte))
        {
            /* 调试：打印收到的字符 */
            UART_SendString("\r\n[RX] ");
            UART_SendData(&rxByte, 1);
            UART_SendString("\r\n");

            if (rxByte == 'U')
            {
                TriggerUpgrade();
            }
        }

        /* 200ms 翻转"当前槽"对应的灯（A 槽=绿灯，B 槽=蓝灯） */
        ledCounter++;
        if (ledCounter >= 200U) {
            ledCounter = 0;
            LED_Toggle(slotLedPin);
        }

        OSIF_TimeDelay(1);
    }
}
