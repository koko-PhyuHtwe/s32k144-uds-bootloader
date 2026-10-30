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
#include "osif.h"
#include "device_registers.h"
#include "interrupt_manager.h"
#include "sdk_project_config.h"

/* ==================== 内部函数声明 ==================== */

static void SystemReset(void);
static void TriggerUpgrade(void);

/* ==================== 内部函数实现 ==================== */

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

    /* ===== App 启动信息 ===== */
    UART_SendString("\r\n");
    UART_SendString("========================================\r\n");
    UART_SendString("  S32K144 App Running\r\n");
    UART_SendString("  Version: ");
    UART_SendString(APP_VERSION);
    UART_SendString("\r\n");
    UART_SendString("  Send 'U' to enter upgrade mode\r\n");
    UART_SendString("========================================\r\n");

    /* 点亮 LED1 表示 App 运行 */
    LED_TurnOn(LED1_PIN);
    LED_TurnOff(LED0_PIN);

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

        /* 200ms 翻转 LED0（App 心跳，比 Bootloader 快） */
        ledCounter++;
        if (ledCounter >= 200U) {
            ledCounter = 0;
            LED_Toggle(LED0_PIN);
        }

        OSIF_TimeDelay(1);
    }
}
