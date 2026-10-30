/**
 * @file    spi_flash_test.h
 * @brief   SPI Flash 自检接口
 */

#ifndef SPI_FLASH_TEST_H
#define SPI_FLASH_TEST_H

/**
 * @brief  运行 SPI Flash 硬件自检（结果通过串口 115200 打印）
 * @details 流程：读 JEDEC ID -> 擦最后一个扇区 -> 写 256 字节图案
 *          -> 读回比对。测试区固定在芯片最后一个扇区（0x7FF000），
 *          不会碰仓库头/golden image 等正式数据。
 * @note   需先完成 CLOCK/UART 初始化
 */
void SpiFlash_RunSelfTest(void);

#endif /* SPI_FLASH_TEST_H */