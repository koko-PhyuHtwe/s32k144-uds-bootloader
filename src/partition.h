/**
 * @file    partition.h
 * @brief   分区描述表头文件
 * @details A/B 双区（Slot A / Slot B）架构的元数据管理：
 *          - 描述表存放在 Flash 元数据区 0x0000F000（1 个 4KB 扇区）
 *          - 记录每个槽的版本号 / CRC32 / 大小 / 状态，以及当前激活槽
 *          - 升级采用"写非激活槽 → CRC 校验 → 原子提交"策略
 */

#ifndef PARTITION_H
#define PARTITION_H

#include "sdk_project_config.h"

/* ==================== 槽状态 ==================== */
typedef enum {
    SLOT_STATE_EMPTY   = 0U,   /* 空槽（未烧录有效固件） */
    SLOT_STATE_VALID   = 1U,   /* 有效固件 */
    SLOT_STATE_INVALID = 2U    /* 预留：固件无效（回滚用） */
} slot_state_t;

/* ==================== 槽 ID ==================== */
typedef enum {
    SLOT_A = 0U,               /* Slot A */
    SLOT_B = 1U                /* Slot B */
} slot_id_t;

/* ==================== 分区描述表结构 ==================== */
/* 整体大小需为 8 的倍数（Flash 写入对齐） */
typedef struct {
    uint32_t magic;             /* 表头魔术字，判表是否已初始化 */
    uint32_t active_slot;       /* 当前激活槽：SLOT_A / SLOT_B */

    /* ---- Slot A 信息 ---- */
    uint32_t slot_a_version;    /* A 槽版本号 */
    uint32_t slot_a_crc32;      /* A 槽固件 CRC32 */
    uint32_t slot_a_size;       /* A 槽固件字节数 */
    uint8_t  slot_a_state;      /* A 槽状态 */
    uint8_t  slot_a_pad[3];

    /* ---- Slot B 信息 ---- */
    uint32_t slot_b_version;    /* B 槽版本号 */
    uint32_t slot_b_crc32;      /* B 槽固件 CRC32 */
    uint32_t slot_b_size;       /* B 槽固件字节数 */
    uint8_t  slot_b_state;      /* B 槽状态 */
    uint8_t  slot_b_pad[3];

    /* ---- 回滚预留字段（本次不启用逻辑） ---- */
    uint32_t boot_count;        /* 引导计数 */
    uint32_t rollback_count;    /* 回滚次数 */

    /* ---- 自校验与外部备份（旧版此处是纯 reserved[32]，结构体大小不变） ---- */
    uint32_t table_seq;         /* 提交序号，每次成功提交 +1 */
    uint32_t table_crc;         /* 整表 CRC32（本字段按 0 参与计算）；0 = 旧格式表 */
    uint8_t  reserved[24];      /* 预留对齐 */
} partition_table_t;

/* ==================== 魔术字 ==================== */
#define PARTITION_TABLE_MAGIC   0x50415231U  /* 'PAR1' */

/* ==================== 函数声明 ==================== */

/**
 * @brief  初始化分区模块：从元数据区读取描述表到 RAM
 * @note   若表无效（首次烧录），将初始化为默认值（激活 Slot A，两槽为空）
 */
void Partition_Init(void);

/**
 * @brief  描述表是否有效
 * @return 1=有效, 0=无效（magic 不匹配）
 */
uint8_t Partition_IsValid(void);

/**
 * @brief  获取当前激活槽 ID
 */
slot_id_t Partition_GetActiveSlot(void);

/**
 * @brief  获取当前激活槽的基地址
 */
uint32_t Partition_GetActiveBase(void);

/**
 * @brief  获取当前激活槽的版本号
 */
uint32_t Partition_GetActiveVersion(void);

/**
 * @brief  获取非激活槽的基地址，作为下载目标
 * @note   升级时永远写入非激活槽，完成后原子切换
 */
uint32_t Partition_GetInactiveBase(void);

/**
 * @brief  启动前校验激活槽固件完整性（重算 CRC32 与描述表比对）
 * @return 0 = 校验通过（VALID 且 CRC 一致）
 *         1 = 校验失败（VALID 但 CRC 不匹配，固件损坏）
 *         2 = 无法校验（槽非 VALID 或表无效，如 J-Link 直烧场景）
 * @note   仅对已登记的 VALID 槽做 CRC 校验；返回 2 时由调用方
 *         退化为 Jump_To_App 内部的向量表合法性检查
 */
uint8_t Partition_VerifyActiveSlot(void);

/**
 * @brief  提交升级：把新固件信息写入描述表并翻转激活槽
 * @param  crc32  新固件 CRC32（下载区域整块）
 * @param  size   新固件字节数
 * @return 0=成功, 1=失败
 * @note   操作顺序：
 *         1. 确定目标槽 = 非激活槽
 *         2. 新版本号 = 当前激活槽版本 + 1
 *         3. 更新 RAM 副本中目标槽的 version/crc/size/state
 *         4. 翻转 active_slot
 *         5. 擦写元数据区落盘（原子提交）
 */
uint8_t Partition_CommitUpdate(uint32_t crc32, uint32_t size);

#endif /* PARTITION_H */