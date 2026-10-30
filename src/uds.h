/**
 * @file    uds.h
 * @brief   UDS 诊断服务头文件
 * @details 实现 ISO 14229 UDS 协议子集（8 个服务：会话控制 / ECU 复位 / 读 DID / 安全访问 / 写 DID / 请求下载 / 传输数据 / 退出传输）
 */

#ifndef UDS_H
#define UDS_H

#include "sdk_project_config.h"

/* ==================== UDS 服务 ID (SID) ==================== */
#define UDS_SID_SESSION_CONTROL       0x10U  /* 会话控制 */
#define UDS_SID_ECU_RESET             0x11U  /* ECU 复位 */
#define UDS_SID_READ_DID              0x22U  /* 读数据标识符 */
#define UDS_SID_SECURITY_ACCESS       0x27U  /* 安全访问（seed-key 解锁） */
#define UDS_SID_WRITE_DID             0x2EU  /* 写数据标识符（用于 CRC32 校验） */
#define UDS_SID_REQUEST_DOWNLOAD      0x34U  /* 请求下载 */
#define UDS_SID_TRANSFER_DATA         0x36U  /* 传输数据 */
#define UDS_SID_REQUEST_TRANSFER_EXIT 0x37U  /* 退出传输 */

/* ==================== UDS 否定响应码 (NRC) ==================== */
#define UDS_NRC_SERVICE_NOT_SUPPORTED       0x11U  /* 服务不支持 */
#define UDS_NRC_SUBFUNCTION_NOT_SUPPORTED   0x12U  /* 子功能不支持 */
#define UDS_NRC_INVALID_MESSAGE_LENGTH      0x13U  /* 消息长度错误 */
#define UDS_NRC_REQUEST_OUT_OF_RANGE        0x31U  /* 请求超出范围 */
#define UDS_NRC_REQUEST_SEQUENCE_ERROR      0x24U  /* 请求序列错误 */
#define UDS_NRC_CONDITIONS_NOT_CORRECT      0x22U  /* 条件不满足（供电欠压） */
#define UDS_NRC_SECURITY_ACCESS_DENIED      0x33U  /* 安全访问拒绝（未解锁） */
#define UDS_NRC_INVALID_KEY                 0x35U  /* 密钥错误 */
#define UDS_NRC_EXCEEDED_NUMBER_OF_ATTEMPTS 0x36U  /* 超过尝试次数 */
#define UDS_NRC_WRONG_BLOCK_SEQUENCE        0x73U  /* 块序号错误 */
#define UDS_NRC_GENERAL_PROGRAMMING_FAILURE 0x72U  /* 编程失败 */

/* ==================== 会话类型 ==================== */
#define UDS_SESSION_DEFAULT      0x01U  /* 默认会话 */
#define UDS_SESSION_EXTENDED     0x03U  /* 扩展会话 */

/* ==================== 0x27 安全访问子功能 ==================== */
#define UDS_SECURITY_REQUEST_SEED  0x01U  /* 请求种子 */
#define UDS_SECURITY_SEND_KEY      0x02U  /* 发送密钥 */

/* 密钥算法：key = (seed + 0x1111) & 0xFFFF */
#define UDS_SECURITY_KEY_OFFSET    0x1111U

/* 安全访问失败重试上限 */
#define UDS_SECURITY_MAX_ATTEMPTS  3U

/* ==================== DID 定义 ==================== */
#define UDS_DID_SW_VERSION       0xF189U  /* 软件版本号（返回激活槽版本） */
#define UDS_DID_ACTIVE_SLOT      0xF19AU  /* 当前激活槽号（0=A, 1=B） */
#define UDS_DID_CRC32            0xFF01U  /* CRC32 校验请求 */

/* ==================== S3Server 超时 ==================== */
#define S3_SERVER_TIMEOUT_MS     5000U  /* 5 秒超时 */

/* ==================== 函数声明 ==================== */

/**
 * @brief  UDS 初始化
 */
void UDS_Init(void);

/**
 * @brief  UDS 主处理函数
 * @note   在主循环中调用，从 CAN 队列取数据并解析
 */
void UDS_Process(void);

/**
 * @brief  UDS 定时器 tick（每毫秒调用一次）
 * @note   超时后自动跳转 App
 */
void UDS_Tick(void);

/**
 * @brief  跳转到指定槽的 App
 * @param  slotBase: 槽基地址（SLOT_A_BASE 或 SLOT_B_BASE）
 * @note   不会返回，直接跳到 App 的 Reset_Handler
 */
void Jump_To_App(uint32_t slotBase);

#endif /* UDS_H */
