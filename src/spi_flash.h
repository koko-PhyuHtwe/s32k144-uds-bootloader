/**
 * @file    spi_flash.h
 * @brief   W25Q64 SPI NOR Flash 驱动头文件
 * @details 硬件连接（板载 U5 W25Qxx，8MB）：
 *            PTB14 -> LPSPI1_SCK   （串阻 R39 1k）
 *            PTB15 <- LPSPI1_SIN   （串阻 R38 1k）
 *            PTB16 -> LPSPI1_SOUT  （串阻 R37 1k）
 *            PTB17 -> CS#          （串阻 R40 1k，低有效，**软件 GPIO 控制**）
 *          片选由软件用 PINS_DRV_WritePin() 拉低/拉高（PTB17 配成 GPIO 输出），
 *          不用 LPSPI 的 PCS3 硬件自动片选——与厂家例程
 *          （23_s32k144_Demo_SPI_W25Qxx，`#define SPI_CSN(x) PINS_DRV_WritePin(PTB,17,x)`）
 *          保持一致，实测这条路径可靠。
 *          时钟：LPSPI1 源自 SOSCDIV2 = 8MHz，实际 SCK = 8M/(分频)，当前取 100 kHz。
 *
 * @note    使用前必须先完成 CLOCK_DRV_Init() 与 PINS_DRV_Init()。
 */

#ifndef SPI_FLASH_H
#define SPI_FLASH_H

#include "sdk_project_config.h"
#include <stdbool.h>

/* ==================== 硬件参数 ==================== */

/** LPSPI 实例号（LPSPI1） */
#define SPI_FLASH_INSTANCE          INST_LPSPI_1

/** 页大小：写入的最小单位，不可跨页 */
#define SPI_FLASH_PAGE_SIZE         256U

/** 扇区大小：擦除的最小单位 */
#define SPI_FLASH_SECTOR_SIZE       4096U

/** 芯片总容量：W25Q64 = 8MB */
#define SPI_FLASH_SIZE_BYTES        (8UL * 1024UL * 1024UL)

/** LPSPI1 的源时钟（SOSCDIV2_CLK = 8MHz），改时钟时要同步改这里 */
#define SPI_FLASH_CLK_HZ            8000000U

/** 单次传输最大字节数（4 字节命令+地址 + 一页数据） */
#define SPI_FLASH_MAX_XFER          260U

/** 单次 SPI 传输超时（毫秒） */
#define SPI_FLASH_XFER_TIMEOUT_MS   100U

/** 页编程超时（毫秒），W25Q64 典型 0.7ms / 最大 3ms */
#define SPI_FLASH_PROGRAM_TIMEOUT_MS 50U

/** 扇区擦除超时（毫秒），W25Q64 典型 45ms / 最大 400ms */
#define SPI_FLASH_ERASE_TIMEOUT_MS  1000U

/** W25Q64 的 JEDEC ID（0x9F 命令返回值） */
#define SPI_FLASH_JEDEC_W25Q64      0x00EF4017UL

/** 状态寄存器 1 的忙标志位（Write In Progress） */
#define SPI_FLASH_SR1_WIP           0x01U

/* ==================== 接口函数 ==================== */

/**
 * @brief  初始化 LPSPI1 并探测 Flash 是否存在
 * @return true=芯片在线（读到了合法 JEDEC ID），false=未检测到
 * @note   内部会调用 LPSPI_DRV_MasterInit()，重复调用不会重复初始化
 */
bool SpiFlash_Init(void);

/**
 * @brief  查询芯片是否在线（须先调用 SpiFlash_Init）
 */
bool SpiFlash_IsPresent(void);

/**
 * @brief  读取 JEDEC ID（0x9F）
 * @return 24 位 ID，例如 W25Q64 为 0x00EF4017；失败返回 0
 */
uint32_t SpiFlash_ReadJedecId(void);

/**
 * @brief  读取状态寄存器 1
 * @return 状态字节，bit0 = WIP（1 表示正在擦/写）
 */
uint8_t SpiFlash_ReadStatus1(void);

/**
 * @brief  等待芯片空闲
 * @param  timeoutMs: 超时时间（毫秒）
 * @return true=已空闲，false=超时
 */
bool SpiFlash_WaitReady(uint32_t timeoutMs);

/**
 * @brief  读取数据（任意地址，不跨 8MB 边界）
 * @param  addr: 起始地址
 * @param  buf:  接收缓冲区
 * @param  len:  长度，最大 SPI_FLASH_PAGE_SIZE(256)
 * @return true=成功
 */
bool SpiFlash_Read(uint32_t addr, uint8_t *buf, uint32_t len);

/**
 * @brief  写一页数据（内部自动发 WREN 并等待编程完成）
 * @param  addr: 起始地址（不能跨页）
 * @param  buf:  数据缓冲区
 * @param  len:  长度，最大 256，且 addr%256 + len 不得超 256
 * @return true=成功
 * @note   写前该区域必须是已擦除状态（0xFF）
 */
bool SpiFlash_WritePage(uint32_t addr, const uint8_t *buf, uint32_t len);

/**
 * @brief  擦除一个 4KB 扇区（内部自动发 WREN 并等待擦除完成）
 * @param  addr: 扇区内任意地址，内部自动向下对齐到扇区首
 * @return true=成功
 */
bool SpiFlash_EraseSector(uint32_t addr);

#endif /* SPI_FLASH_H */