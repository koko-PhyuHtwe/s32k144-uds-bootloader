/**
 * @file    main.c
 * @brief   主程序入口
 * @details S32K144 Bootloader/App 双构建入口
 *          - 不定义 BUILD_APP → 编译为 Bootloader（起始 0x00000000）
 *          - 定义 BUILD_APP → 编译为 App（起始 0x00010000）
 */

#ifdef BUILD_APP
/* ==================== App 构建 ==================== */
#include "sdk_project_config.h"
#include "app.h"

/**
 * @brief  App 主函数
 * @return 理论上不会返回
 */
int main(void)
{
    return App_Main();
}

#else
/* ==================== Bootloader 构建 ==================== */
#include "sdk_project_config.h"
#include "osif.h"
#include "led.h"
#include "uart.h"
#include "can.h"
#include "uds.h"
#include "flash_app.h"
#include "partition.h"
#include "spi_flash.h"        /* 描述表外部备份 */
#include "adc_mon.h"          /* 电压监控（演示：电位器模拟供电跌落） */
#include "app.h"  /* 共享升级标志定义 */

/**
 * @brief  主函数
 * @return 理论上不会返回
 */
int main(void)
{
    /* ===== 系统初始化（最小系统） ===== */

    /* 1. 初始化时钟 */
    CLOCK_DRV_Init(&clockMan1_InitConfig0);

    /* 2. 初始化引脚 */
    PINS_DRV_Init(NUM_OF_CONFIGURED_PINS0, g_pin_mux_InitConfigArr0);

    /* 3. 初始化 LED（用于状态指示） */
    LED_Init();

    /* 4. 初始化 UART 并发送启动信息 */
    UART_Init();
    UART_SendBootMessage();

    /* 5. 初始化 SPI Flash（描述表外部备份用；没焊芯片时会自动降级） */
    (void)SpiFlash_Init();

    /* 6. 初始化 Flash 驱动（恢复描述表 / 清升级标志 / 提交描述表都需要） */
    FlashApp_Init();

    /* 7. 初始化分区模块：内部表 → 外部备份 → 默认表 */
    Partition_Init();

    /* 8. 初始化 ADC 电压监控（ADC0 通道 2 = PTA6 / ADC0-2 接线柱） */
    if (AdcMon_Init()) {
        AdcMon_Print();
    }

    /* ===== 检查升级标志 ===== */
    uint32_t upgradeFlag = *(volatile uint32_t *)UPGRADE_FLAG_ADDR;
    if (upgradeFlag == UPGRADE_FLAG_MAGIC) {
        /* 有升级请求，留在 Bootloader */
        UART_SendString("[BOOT] upgrade flag set -> stay in bootloader | active slot ");
        UART_SendString((Partition_GetActiveSlot() == SLOT_B) ? "B\r\n" : "A\r\n");

        /* 清除升级标志（擦除升级标志扇区） */
        FlashApp_EraseBootSector(UPGRADE_FLAG_ADDR);
    } else {
        /* 没有升级请求：先校验激活槽固件 CRC（有元数据时），通过才跳。
           verify=2 表示无元数据（如 J-Link 直烧），退化为向量表检查 */
        uint8_t verify = Partition_VerifyActiveSlot();
        if (verify == 1U) {
            /* CRC 不匹配：固件损坏，留在 Bootloader 等待重新升级 */
            UART_SendString("[BOOT] active slot CRC mismatch -> stay in bootloader\r\n");
        } else {
            Jump_To_App(Partition_GetActiveBase());
        }
        /* 如果返回，说明没有合法 App，继续往下走 */
    }

    /* ===== 外设初始化（仅 Bootloader 模式需要） ===== */

    /* 9. 初始化 CAN（配置发送邮箱 M0 + 接收邮箱 M1，开启中断） */
    CAN_Init();

    /* 10. 初始化 UDS 诊断服务 */
    UDS_Init();

    /* ===== 主循环 ===== */
    for (;;)
    {
        /* UDS 处理（内部检查队列，有数据才解析） */
        UDS_Process();

        /* S3Server 超时计时（每毫秒减 1） */
        UDS_Tick();

        /* ADC 电压监控：每 10ms 采样一次（主循环约 1ms 一圈） */
        static uint8_t adcCounter = 0U;
        adcCounter++;
        if (adcCounter >= 10U) {
            adcCounter = 0U;
            AdcMon_Update();
        }

        /* 延时 1ms（作为超时计时基准） */
        OSIF_TimeDelay(1);

        /* 每 500ms 翻转 LED（心跳指示） */
        static uint16_t ledCounter = 0;
        ledCounter++;
        if (ledCounter >= 500U) {
            ledCounter = 0;
            LED_ToggleBoth();
        }
    }
}
#endif /* BUILD_APP */
