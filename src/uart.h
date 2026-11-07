/**
 * @file    uart.h
 * @brief   LPUART 驱动头文件
 * @details 基于 S32K144 的 LPUART1 串口通信模块，支持轮询模式发送
 */

#ifndef UART_H
#define UART_H

#include "sdk_project_config.h"

/* 串口实例 ID */
#define UART_INSTANCE       INST_LPUART_1    /* LPUART1 */

/* ==================== 函数声明 ==================== */

/* ---- 生命周期：初始化 / 反初始化 ---- */

/**
 * @brief  UART 初始化
 * @note   初始化 LPUART1，波特率 115200，8N1
 */
void UART_Init(void);

/**
 * @brief  UART 反初始化
 * @note   关闭 LPUART1，用于 Bootloader 跳 App 前清理
 */
void UART_Deinit(void);

/* ---- 发送接口：轮询（阻塞），返回时数据已发出 ---- */

/**
 * @brief  通过轮询模式发送数据
 * @param  data: 要发送的数据缓冲区指针
 * @param  len:  数据长度（字节）
 */
void UART_SendData(const uint8_t *data, uint32_t len);

/**
 * @brief  发送字符串（以 \0 结尾）
 * @param  str: 字符串指针
 */
void UART_SendString(const char *str);

/**
 * @brief  发送启动信息（ASCII 字符）
 */
void UART_SendBootMessage(void);

/* ---- 接收接口：中断搬移 + 此处非阻塞查询 ---- */

/**
 * @brief  非阻塞接收一个字节
 * @param  outByte: 输出接收到的字节
 * @return 1=收到数据, 0=无数据
 */
uint8_t UART_ReceiveByte(uint8_t *outByte);

#endif /* UART_H */
