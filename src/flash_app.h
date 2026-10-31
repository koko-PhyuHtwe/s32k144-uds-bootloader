#ifndef FLASH_APP_H
#define FLASH_APP_H

#include "sdk_project_config.h"

/* ==================== Flash 地址规划 ==================== */
/* Bootloader 代码  0x00000000 ~ 0x0000DFFF (56KB)                      */
/* 升级标志区       0x0000E000 ~ 0x0000EFFF (4KB, 1 个扇区)             */
/* 元数据区(描述表) 0x0000F000 ~ 0x0000FFFF (4KB, 1 个扇区)             */
/* Slot A (App)     0x00010000 ~ 0x00047FFF (224KB)                    */
/* Slot B (App)     0x00048000 ~ 0x0007FFFF (224KB)                    */
#define BOOT_FLASH_SIZE      0x0000E000U  /* Boot 代码区 56KB */
#define BOOT_DATA_BASE       0x0000E000U  /* Boot 保护数据区起始（升级标志+描述表） */
#define UPGRADE_FLAG_ADDR    0x0000E000U  /* 升级标志区地址 */
#define UPGRADE_FLAG_SIZE    0x00001000U  /* 升级标志区大小 4KB */
#define PARTITION_TABLE_ADDR 0x0000F000U  /* 元数据区(描述表)地址 */
#define PARTITION_TABLE_SIZE 0x00001000U  /* 元数据区大小 4KB */
#define BOOT_FLASH_END       0x00010000U  /* Boot 区结束 (= App 区起始) */

#define APP_FLASH_BASE       0x00010000U  /* App 区起始 */
#define APP_FLASH_END        0x00080000U  /* App 区结束 (含 A/B 两槽) */

/* ==================== A/B 双区槽位 ==================== */
#define SLOT_A_BASE          0x00010000U  /* Slot A 起始 */
#define SLOT_B_BASE          0x00048000U  /* Slot B 起始 */
#define SLOT_SIZE            0x00038000U  /* 单槽大小 224KB */

/* ==================== Flash 对齐常量 ==================== */
#define FLASH_SECTOR_SIZE   0x1000U    /* 扇区大小 4KB (S32K144 P-Flash) */
#define FLASH_WRITE_ALIGN   8U          /* 写入对齐 8 字节 */

/* ==================== 函数声明 ==================== */
void    FlashApp_Init(void);
uint8_t FlashApp_Erase(uint32_t addr, uint32_t size);
uint8_t FlashApp_Write(uint32_t addr, const uint8_t *data, uint32_t size);
uint8_t FlashApp_Verify(uint32_t addr, const uint8_t *expected, uint32_t size);
uint32_t FlashApp_CalcCRC32(uint32_t addr, uint32_t size);

/* 擦除 Boot 保护数据区（升级标志扇区/描述表扇区）内一个 4KB 扇区 */
uint8_t FlashApp_EraseBootSector(uint32_t addr);

/* 擦除并写入 Boot 保护数据区一个 4KB 扇区（供升级标志/描述表用） */
uint8_t FlashApp_WriteBootSector(uint32_t addr, const uint8_t *data, uint32_t size);

#endif /* FLASH_APP_H */
