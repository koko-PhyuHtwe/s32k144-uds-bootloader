/**
 * @file    uart.c
 * @brief   LPUART 驱动源文件
 * @details 基于 S32K144 LPUART1 的串口通信实现
 *          - 发送：轮询（阻塞），函数返回时数据已发出
 *          - 接收：中断回调搬移 + 主循环非阻塞查询取走
 *
 *          按阅读顺序组织（同一功能相关的代码挨在一起）：
 *            1. 初始化 / 反初始化
 *            2. 接收链路：中断回调 → 主循环取走
 *            3. 发送：轮询（阻塞）
 */

#include "uart.h"
#include "device_registers.h"

/* ==================== 静态变量定义 ==================== */

/* 接收缓冲区 */
static volatile uint8_t s_rxByte      = 0U;   /* 最近收到的字节 */
static volatile uint8_t s_rxReady     = 0U;   /* 1=有新数据待取 */
static uint8_t          s_initialized = 0U;   /* UART 初始化标志 */

/* 启动信息字符串（纯 ASCII） */
static const uint8_t s_bootMsg[] =
    "\r\n[SYS ] S32K144 Bootloader\r\n"
    "[SYS ] CORE 80MHz | BUS 40MHz | FLASH 20MHz | UART 115200\r\n";

/* ==================== 静态函数声明 ==================== */

/* LPUART 接收回调函数（由驱动在中断中调用，仅本文件内部使用） */
static void UART_RxCallback(void *driverState, uart_event_t event, void *userData);

/* ==================== 初始化 / 反初始化 ==================== */

/**
 * @brief  UART 初始化
 * @note   调用 LPUART_DRV_Init() 初始化 LPUART1，并启动接收
 */
void UART_Init(void)
{
    LPUART_DRV_Init(UART_INSTANCE, &lpUartState1, &lpuart_1_InitConfig0);

    /* 先注册回调，再启动接收 */
    LPUART_DRV_InstallRxCallback(UART_INSTANCE, UART_RxCallback, NULL);
    LPUART_DRV_ReceiveData(UART_INSTANCE, (uint8_t *)&s_rxByte, 1U);

    /* 标记已初始化 */
    s_initialized = 1;
}

/**
 * @brief  UART 反初始化
 * @note   关闭 LPUART1，用于 Bootloader 跳 App 前清理
 */
void UART_Deinit(void)
{
    if (s_initialized) {
        LPUART_DRV_Deinit(UART_INSTANCE);
        s_initialized = 0;
    }
}

/* ==================== 接收链路：中断回调 → 主循环取走 ==================== */

/**
 * @brief  LPUART 接收回调函数
 * @note   由驱动在接收中断中调用：数据已被写入 s_rxByte，
 *         这里只需置标志并重新启动下一次接收
 */
static void UART_RxCallback(void *driverState, uart_event_t event, void *userData)
{
    (void)driverState;
    (void)userData;

    if (event == UART_EVENT_RX_FULL)
    {
        /* 数据已接收到，设置标志 */
        s_rxReady = 1U;
        /* 重新启动接收 */
        LPUART_DRV_ReceiveData(UART_INSTANCE, (uint8_t *)&s_rxByte, 1U);
    }
}

/**
 * @brief  非阻塞接收一个字节
 * @param  outByte: 输出接收到的字节
 * @return 1=收到数据, 0=无数据
 */
uint8_t UART_ReceiveByte(uint8_t *outByte)
{
    if (s_rxReady != 0U)
    {
        s_rxReady = 0U;
        *outByte = s_rxByte;
        return 1U;
    }
    return 0U;
}

/* ==================== 发送：轮询（阻塞） ==================== */

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
    UART_SendData(s_bootMsg, sizeof(s_bootMsg) - 1U);
}
