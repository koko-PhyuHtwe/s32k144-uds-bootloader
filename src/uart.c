/**
 * @file    uart.c
 * @brief   LPUART 驱动源文件
 * @details 基于 S32K144 LPUART1 的串口通信实现（轮询模式）
 */

#include "uart.h"
#include "device_registers.h"

/* ==================== 静态变量定义 ==================== */

/* 接收缓冲区 */
static volatile uint8_t g_rxByte = 0U;
static volatile uint8_t g_rxReady = 0U;  /* 收到新数据标志 */
static uint8_t uartInitialized = 0;     /* UART 初始化标志 */

/* 启动信息字符串（纯 ASCII） */
static const uint8_t bootMsg[] =
    "\r\n[SYS ] S32K144 Bootloader\r\n"
    "[SYS ] CORE 80MHz | BUS 40MHz | FLASH 20MHz | UART 115200\r\n";

/* ==================== 静态函数声明 ==================== */

/* LPUART 接收回调函数 */
void uart_rx_callback(void *driverState, uart_event_t event, void *userData);

/* ==================== 公共函数实现 ==================== */

/**
 * @brief  UART 初始化
 * @note   调用 LPUART_DRV_Init() 初始化 LPUART1，并启动接收
 */
void UART_Init(void)
{
    LPUART_DRV_Init(UART_INSTANCE, &lpUartState1, &lpuart_1_InitConfig0);

    /* 先注册回调，再启动接收 */
    LPUART_DRV_InstallRxCallback(UART_INSTANCE, uart_rx_callback, NULL);
    LPUART_DRV_ReceiveData(UART_INSTANCE, (uint8_t *)&g_rxByte, 1U);

    /* 标记已初始化 */
    uartInitialized = 1;
}

/**
 * @brief  UART 反初始化
 * @note   关闭 LPUART1，用于 Bootloader 跳 App 前清理
 */
void UART_Deinit(void)
{
    if (uartInitialized) {
        LPUART_DRV_Deinit(UART_INSTANCE);
        uartInitialized = 0;
    }
}

/**
 * @brief  LPUART 接收回调函数
 */
void uart_rx_callback(void *driverState, uart_event_t event, void *userData)
{
    (void)driverState;
    (void)userData;

    if (event == UART_EVENT_RX_FULL)
    {
        /* 数据已接收到，设置标志 */
        g_rxReady = 1U;
        /* 重新启动接收 */
        LPUART_DRV_ReceiveData(UART_INSTANCE, (uint8_t *)&g_rxByte, 1U);
    }
}

/**
 * @brief  通过轮询模式发送数据
 * @param  data: 数据指针
 * @param  len:  数据长度
 */
void UART_SendData(const uint8_t *data, uint32_t len)
{
    LPUART_DRV_SendDataPolling(UART_INSTANCE, data, len);
}

/**
 * @brief  发送字符串
 * @param  str: 字符串指针
 */
void UART_SendString(const char *str)
{
    uint32_t len = 0U;
    while (str[len] != '\0') {
        len++;
    }
    LPUART_DRV_SendDataPolling(UART_INSTANCE, (const uint8_t *)str, len);
}

/**
 * @brief  发送启动信息
 */
void UART_SendBootMessage(void)
{
    UART_SendData(bootMsg, sizeof(bootMsg) - 1U);
}

/**
 * @brief  非阻塞接收一个字节
 * @param  outByte: 输出接收到的字节
 * @return 1=收到数据, 0=无数据
 */
uint8_t UART_ReceiveByte(uint8_t *outByte)
{
    if (g_rxReady != 0U)
    {
        g_rxReady = 0U;
        *outByte = g_rxByte;
        return 1U;
    }
    return 0U;
}
