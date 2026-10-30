/**
 * @file    partition.c
 * @brief   分区描述表源文件
 * @details A/B 双区元数据管理：读写 Flash 元数据区里的分区描述表，
 *          支持"写非激活槽 → 原子提交"的升级提交流程。
 */

#include "partition.h"
#include "flash_app.h"
#include "spi_flash.h"
#include "uart.h"
#include <string.h>

/* ==================== 外部备份位置（W25Q64） ==================== */
/* 描述表约 80 字节，放在 SPI Flash 第 0 个扇区的首页；
 * 扇区擦除时只影响这一块，给将来的固件仓库留出 0x1000 之后的全部空间。 */
#define PARTITION_BACKUP_BASE   0x00000000U

/* ==================== RAM 描述表副本 ==================== */
static partition_table_t g_partition;

/* ==================== 内部函数 ==================== */

/**
 * @brief  计算描述表 CRC32（table_crc 字段本身按 0 参与）
 */
static uint32_t Partition_CalcCrc(const partition_table_t *tbl)
{
    partition_table_t tmp = *tbl;
    tmp.table_crc = 0U;
    return FlashApp_CalcCRC32((uint32_t)&tmp, (uint32_t)sizeof(tmp));
}

/**
 * @brief  把当前 RAM 表写入外部 Flash 备份并读回校验
 * @return 0=成功, 1=失败（失败不阻断升级流程）
 */
static uint8_t Partition_SaveToExternal(void)
{
    partition_table_t chk;

    if (!SpiFlash_IsPresent()) {
        return 1U;
    }

    if (!SpiFlash_EraseSector(PARTITION_BACKUP_BASE)) {
        return 1U;
    }

    if (!SpiFlash_WritePage(PARTITION_BACKUP_BASE,
                            (const uint8_t *)&g_partition,
                            (uint32_t)sizeof(g_partition))) {
        return 1U;
    }

    /* 读回比对：外部表也必须是对的，否则恢复时会把坏表当成好表 */
    if (!SpiFlash_Read(PARTITION_BACKUP_BASE, (uint8_t *)&chk,
                       (uint32_t)sizeof(chk))) {
        return 1U;
    }

    return (memcmp(&chk, &g_partition, sizeof(chk)) == 0U) ? 0U : 1U;
}

/**
 * @brief  尝试从外部 Flash 恢复描述表
 * @return 1=恢复成功（并已写回内部元数据区）, 0=外部也没有可用备份
 */
static uint8_t Partition_RestoreFromExternal(void)
{
    partition_table_t ext;

    if (!SpiFlash_IsPresent()) {
        return 0U;
    }

    if (!SpiFlash_Read(PARTITION_BACKUP_BASE, (uint8_t *)&ext,
                       (uint32_t)sizeof(ext))) {
        return 0U;
    }

    /* 外部表本身也要过 magic + CRC 两道关 */
    if (ext.magic != PARTITION_TABLE_MAGIC) {
        return 0U;
    }
    if ((ext.table_crc != 0U) && (Partition_CalcCrc(&ext) != ext.table_crc)) {
        return 0U;
    }

    g_partition = ext;

    /* 写回内部元数据区：下次上电直接读到好表，不用再走恢复 */
    (void)FlashApp_WriteBootSector(PARTITION_TABLE_ADDR,
                                   (const uint8_t *)&g_partition,
                                   (uint32_t)sizeof(g_partition));

    return 1U;
}

/**
 * @brief  打印无符号十进制数（模块内部用，避免依赖 printf）
 */
static void Partition_PrintU32(uint32_t value)
{
    char    buf[11];
    char    tmp[10];
    uint8_t len = 0U;
    uint8_t i;

    if (value == 0U) {
        UART_SendString("0");
        return;
    }

    while ((value > 0U) && (len < 10U)) {
        tmp[len] = (char)('0' + (value % 10U));
        value /= 10U;
        len++;
    }

    for (i = 0U; i < len; i++) {
        buf[i] = tmp[len - 1U - i];
    }
    buf[len] = '\0';

    UART_SendString(buf);
}

/**
 * @brief  外部备份里是否存着与当前内部表完全相同的一份
 * @return 1=一致（备份可用）, 0=没有或内容不同
 */
static uint8_t Partition_ExternalMatches(void)
{
    partition_table_t ext;

    if (!SpiFlash_IsPresent()) {
        return 0U;
    }

    if (!SpiFlash_Read(PARTITION_BACKUP_BASE, (uint8_t *)&ext,
                       (uint32_t)sizeof(ext))) {
        return 0U;
    }

    return (memcmp(&ext, &g_partition, sizeof(ext)) == 0U) ? 1U : 0U;
}

/**
 * @brief  开机打印一行分区表状态（给使用者看，不需要调试器）
 */
static void Partition_PrintStatus(uint8_t backupOk)
{
    UART_SendString("[Partition] seq=");
    Partition_PrintU32((uint32_t)g_partition.table_seq);

    UART_SendString(" active=");
    UART_SendString((g_partition.active_slot == (uint32_t)SLOT_B) ? "B" : "A");

    UART_SendString(" crc=");
    UART_SendString((g_partition.table_crc != 0U) ? "ok" : "legacy");

    UART_SendString(" backup=");
    UART_SendString((backupOk != 0U) ? "ok" : "none");

    UART_SendString("\r\n");
}

/* ==================== 公共函数实现 ==================== */

/**
 * @brief  初始化：从元数据区读取描述表
 * @note   顺序：内部表（magic + CRC）→ 外部备份 → 默认表
 */
void Partition_Init(void)
{
    uint8_t needRestore = 0U;

    /* 外部备份可能要用到 SPI Flash */
    (void)SpiFlash_Init();

    /* 从 Flash 元数据区读取（Flash 可 memory-mapped 直接读） */
    memcpy(&g_partition, (const void *)PARTITION_TABLE_ADDR, sizeof(g_partition));

    if (g_partition.magic == PARTITION_TABLE_MAGIC) {
        /* 新格式表带自校验；table_crc==0 的手工表视为旧格式，只认 magic */
        if ((g_partition.table_crc != 0U) &&
            (Partition_CalcCrc(&g_partition) != g_partition.table_crc)) {
            UART_SendString("Partition table CRC error, try SPI backup...\r\n");
            needRestore = 1U;
        }
    } else {
        UART_SendString("Partition table missing, try SPI backup...\r\n");
        needRestore = 1U;
    }

    if (needRestore) {
        if (Partition_RestoreFromExternal()) {
            UART_SendString("Partition table restored from SPI flash.\r\n");
        } else {
            /* 外部也没有 → 首次烧录或彻底损坏：重建默认表（激活 Slot A，两槽为空） */
            memset(&g_partition, 0, sizeof(g_partition));
            g_partition.magic        = PARTITION_TABLE_MAGIC;
            g_partition.active_slot  = (uint32_t)SLOT_A;
            g_partition.slot_a_state = SLOT_STATE_EMPTY;
            g_partition.slot_b_state = SLOT_STATE_EMPTY;
        }
    }

    /* 一行状态：不需要调试器，看串口就能判断"外部备份有没有生效" */
    Partition_PrintStatus(Partition_ExternalMatches());
}

/**
 * @brief  描述表是否有效
 */
uint8_t Partition_IsValid(void)
{
    return (g_partition.magic == PARTITION_TABLE_MAGIC) ? 1U : 0U;
}

/**
 * @brief  获取当前激活槽 ID
 */
slot_id_t Partition_GetActiveSlot(void)
{
    if (g_partition.active_slot == (uint32_t)SLOT_B) {
        return SLOT_B;
    }
    return SLOT_A;
}

/**
 * @brief  获取当前激活槽基地址
 */
uint32_t Partition_GetActiveBase(void)
{
    return (Partition_GetActiveSlot() == SLOT_A) ? SLOT_A_BASE : SLOT_B_BASE;
}

/**
 * @brief  获取当前激活槽版本号
 */
uint32_t Partition_GetActiveVersion(void)
{
    if (Partition_GetActiveSlot() == SLOT_A) {
        return g_partition.slot_a_version;
    }
    return g_partition.slot_b_version;
}

/**
 * @brief  获取非激活槽基地址（下载目标）
 */
uint32_t Partition_GetInactiveBase(void)
{
    return (Partition_GetActiveSlot() == SLOT_A) ? SLOT_B_BASE : SLOT_A_BASE;
}

/**
 * @brief  提交升级：把新固件信息写入描述表并翻转激活槽
 * @param  crc32  新固件 CRC32
 * @param  size   新固件字节数
 * @return 0=成功, 1=失败
 * @note   A/B 双区流程：
 *         1. 目标槽 = 非激活槽（新固件写入非激活槽）
 *         2. 新版本号 = 当前激活槽版本 + 1
 *         3. 更新 RAM 副本中目标槽的 version/crc/size/state
 *         4. 翻转 active_slot
 *         5. 擦写元数据区落盘（原子提交）
 */
uint8_t Partition_CommitUpdate(uint32_t crc32, uint32_t size)
{
    /* 目标槽 = 非激活槽：升级永远写非激活槽，提交时原子切换 */
    slot_id_t target = (Partition_GetActiveSlot() == SLOT_A) ? SLOT_B : SLOT_A;

    /* 新版本号 = 当前激活槽版本 + 1 */
    uint32_t newVersion = Partition_GetActiveVersion() + 1U;

    /* 更新 RAM 副本中目标槽的字段 */
    if (target == SLOT_A) {
        g_partition.slot_a_version = newVersion;
        g_partition.slot_a_crc32   = crc32;
        g_partition.slot_a_size    = size;
        g_partition.slot_a_state   = SLOT_STATE_VALID;
    } else {
        g_partition.slot_b_version = newVersion;
        g_partition.slot_b_crc32   = crc32;
        g_partition.slot_b_size    = size;
        g_partition.slot_b_state   = SLOT_STATE_VALID;
    }

    /* 翻转激活槽 */
    g_partition.active_slot = (uint32_t)target;

    /* 更新序号并重算自校验 */
    g_partition.table_seq++;
    g_partition.table_crc = Partition_CalcCrc(&g_partition);

    /* 1) 先写外部备份：这一步失败不影响升级，内部表照写 */
    if (Partition_SaveToExternal() != 0U) {
        UART_SendString("WARN: partition backup to SPI flash failed\r\n");
    }

    /* 2) 再原子落盘：擦写内部描述表扇区 */
    return FlashApp_WriteBootSector(PARTITION_TABLE_ADDR,
                                    (const uint8_t *)&g_partition,
                                    sizeof(g_partition));
}

/**
 * @brief  启动前校验激活槽固件完整性
 */
uint8_t Partition_VerifyActiveSlot(void)
{
    slot_id_t active;
    uint32_t  base, size, crc;
    uint8_t   state;

    if (!Partition_IsValid()) {
        return 2U;  /* 表无效，无法校验 */
    }

    active = Partition_GetActiveSlot();
    if (active == SLOT_A) {
        base  = SLOT_A_BASE;
        size  = g_partition.slot_a_size;
        crc   = g_partition.slot_a_crc32;
        state = g_partition.slot_a_state;
    } else {
        base  = SLOT_B_BASE;
        size  = g_partition.slot_b_size;
        crc   = g_partition.slot_b_crc32;
        state = g_partition.slot_b_state;
    }

    /* 非 VALID 状态（EMPTY / 未登记）→ 没有可对 CRC 的元数据，
       交回给 Jump_To_App 的向量表检查 */
    if (state != SLOT_STATE_VALID) {
        return 2U;
    }

    /* 大小非法视为固件损坏 */
    if ((size == 0U) || (size > SLOT_SIZE)) {
        return 1U;
    }

    /* 重算整槽 CRC32，与描述表存储值比对 */
    return (FlashApp_CalcCRC32(base, size) == crc) ? 0U : 1U;
}