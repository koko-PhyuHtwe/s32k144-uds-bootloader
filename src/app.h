/**
 * @file    app.h
 * @brief   应用程序头文件
 * @details App 程序：LED 闪烁 + 串口打印 + 等待升级命令
 *          编译时定义 BUILD_APP 宏启用
 */

#ifndef APP_H
#define APP_H

#include "sdk_project_config.h"

/* ==================== App 版本 ==================== */
#define APP_VERSION      "V1.0"
#define APP_VERSION_NUM  1U          /* 数字版本号（写入描述表，Bootloader 提交时自动 +1） */

/* ==================== 升级标志 ==================== */
#define UPGRADE_FLAG_MAGIC    0x5AA55AA5U  /* 升级请求标志值 */
/* 注：升级标志地址 UPGRADE_FLAG_ADDR 定义在 flash_app.h（Boot 保护区常量区） */

/* ==================== 函数声明 ==================== */
int App_Main(void);

#endif /* APP_H */
