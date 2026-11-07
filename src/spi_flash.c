/**
 * @file    spi_flash.c
 * @brief   W25Q64 SPI NOR Flash 驱动实现
 * @details 基于 LPSPI1 阻塞式传输。所有命令都以"一次 LPSPI 传输"完成
 *          （命令 + 地址 + 数据拼在同一个发送缓冲区里），片选由软件
 *          在传输前后手动拉低/拉高（PTB17 = GPIO），整条命令期间不会松开。
 *
 *          注意：LPSPI 单次传输长度受 uint16_t 限制，这里每次最多搬运
 *          一页（256 字节）+ 4 字节命令/地址 = 260 字节。
 */

#include "spi_flash.h"
#include "lpspi_master_driver.h"
#include "peripherals_lpspi_1.h"
#include "pins_driver.h"
#include "osif.h"
#include "uart.h"
#include <string.h>

/* ==================== W25Q64 命令码 ==================== */

#define CMD_WRITE_ENABLE    0x06U   /* 写使能 */
#define CMD_READ_STATUS1    0x05U   /* 读状态寄存器 1 */
#define CMD_READ_DATA       0x03U   /* 读数据 */
#define CMD_PAGE_PROGRAM    0x02U   /* 页编程 */
#define CMD_SECTOR_ERASE    0x20U   /* 扇区擦除（4KB） */
#define CMD_JEDEC_ID        0x9FU   /* 读 JEDEC ID */

/* ==================== 片选与速率 ==================== */

/**
 * @brief 片选由软件控制：PTB17 配成普通 GPIO（见 board/pin_mux.c）
 *
 * 厂家例程（23_s32k144_Demo_SPI_W25Qxx）就是这么做的：
 *   #define SPI_CSN(x)  PINS_DRV_WritePin(PTB,17,x);
 * 好处是片选不依赖 LPSPI 的 PCS 时序，整条命令期间可以自己决定拉低多久。
 */
#define SPI_FLASH_CS_LOW()    PINS_DRV_WritePin(PTB, 17U, 0U)
#define SPI_FLASH_CS_HIGH()   PINS_DRV_WritePin(PTB, 17U, 1U)

/** 先用厂家验证过的 100 kHz，跑通后再往上提 */
/* 实测：把上面这个值改成 4 MHz 后读 ID 依然正常（0x00EF4017），
 * 说明决定能否通信的是片选方式，与速率无关；此处仍保持 100 kHz，给 1kΩ 串阻留余量。 */
#define SPI_FLASH_BAUD_HZ     100000UL

/* ==================== 内部变量 ==================== */

/** 发送/接收共用缓冲区（静态分配，避免占用栈空间） */
static uint8_t  s_txBuf[SPI_FLASH_MAX_XFER];
static uint8_t  s_rxBuf[SPI_FLASH_MAX_XFER];

static bool     s_initialized = false;   /* LPSPI 是否已初始化 */
static bool     s_present     = false;   /* 芯片是否被探测到 */
static uint32_t s_jedecId     = 0U;      /* 缓存的 JEDEC ID */

/* ==================== 内部函数 ==================== */

/**
 * @brief  执行一次 LPSPI 全双工传输
 * @param  tx:  发送缓冲区
 * @param  rx:  接收缓冲区
 * @param  len: 字节数
 * @return true=成功
 */
static bool SpiFlash_Transfer(const uint8_t *tx, uint8_t *rx, uint16_t len)
{
    status_t status;

    if ((tx == NULL) || (rx == NULL) || (len == 0U) || (len > SPI_FLASH_MAX_XFER))
    {
        return false;
    }

    SPI_FLASH_CS_LOW();
    status = LPSPI_DRV_MasterTransferBlocking(SPI_FLASH_INSTANCE, tx, rx, len,
                                              SPI_FLASH_XFER_TIMEOUT_MS);
    SPI_FLASH_CS_HIGH();

    return (status == STATUS_SUCCESS);
}

/**
 * @brief  发送写使能命令
 * @return true=成功。后续必须紧跟一条写/擦命令，否则 WEL 会被清除
 */
static bool SpiFlash_WriteEnable(void)
{
    s_txBuf[0] = CMD_WRITE_ENABLE;
    return SpiFlash_Transfer(s_txBuf, s_rxBuf, 1U);
}

/**
 * @brief  把 24 位地址填入发送缓冲区
 */
static void SpiFlash_PutAddr(uint8_t *dst, uint32_t addr)
{
    dst[0] = (uint8_t)((addr >> 16U) & 0xFFU);
    dst[1] = (uint8_t)((addr >> 8U) & 0xFFU);
    dst[2] = (uint8_t)(addr & 0xFFU);
}

/* ==================== 串口打印辅助（不引入 printf） ==================== */

/**
 * @brief 打印 8 位十六进制，带 0x 前缀
 * @note  Boot 区只有 56KB，且已经用到 UART 驱动，所以手写转换，不拉 printf
 */
static void SpiFlash_UartHex32(uint32_t value)
{
    uint8_t i;
    uint8_t ch;

    UART_SendString("0x");
    for (i = 0U; i < 8U; i++)
    {
        uint8_t nib = (uint8_t)((value >> ((7U - i) * 4U)) & 0x0FU);

        ch = (nib < 10U) ? (uint8_t)('0' + nib) : (uint8_t)('A' + nib - 10U);
        UART_SendData(&ch, 1U);
    }
}

/**
 * @brief 打印十进制（无前导零）
 */
static void SpiFlash_UartDecimal(uint32_t value)
{
    uint8_t buf[10];
    uint8_t len = 0U;
    uint8_t ch;

    if (value == 0U)
    {
        ch = (uint8_t)'0';
        UART_SendData(&ch, 1U);
        return;
    }

    while ((value > 0U) && (len < (uint8_t)sizeof(buf)))
    {
        buf[len] = (uint8_t)('0' + (uint8_t)(value % 10U));
        value /= 10U;
        len++;
    }
    while (len > 0U)
    {
        len--;
        UART_SendData(&buf[len], 1U);
    }
}

/* ==================== 公共函数 ==================== */

bool SpiFlash_Init(void)
{
    uint8_t manufacturer;

    if (s_initialized)
    {
        return s_present;
    }
    s_initialized = true;
    s_present     = false;

    /* 先把片选拉高（不选中），再初始化外设 */
    SPI_FLASH_CS_HIGH();

    /* 初始化 LPSPI1（内部会创建信号量并使能 LPSPI1 中断）。
     * 用配置副本，只改波特率，不动 .mex 生成的常量。 */
    {
        lpspi_master_config_t cfg = lpspi_1_MasterConfig0;
        cfg.bitsPerSec  = SPI_FLASH_BAUD_HZ;
        cfg.lpspiSrcClk = SPI_FLASH_CLK_HZ;

        if (LPSPI_DRV_MasterInit(SPI_FLASH_INSTANCE, &lpspi_1State,
                                 &cfg) != STATUS_SUCCESS)
        {
            UART_SendString("[SPI ] LPSPI1 init failed -> backup disabled\r\n");
            return false;
        }
    }

    /* W25Q64 上电到可接受命令需要 tPUW（最大 5ms） */
    OSIF_TimeDelay(5);

    /* 读 JEDEC ID 探测芯片；读不到会返回 0x000000 或 0xFFFFFF */
    s_jedecId = SpiFlash_ReadJedecId();
    manufacturer = (uint8_t)((s_jedecId >> 16U) & 0xFFU);

    s_present = ((manufacturer != 0x00U) && (manufacturer != 0xFFU));

    /* 只在初始化（上电）时打印一次状态：App 不调用本函数，不会重复刷屏 */
    if (s_present)
    {
        UART_SendString("[SPI ] W25Q64 present | jedec=");
        SpiFlash_UartHex32(s_jedecId);
        UART_SendString(" | ");
        SpiFlash_UartDecimal(SPI_FLASH_BAUD_HZ / 1000UL);
        UART_SendString(" kHz\r\n");
    }
    else
    {
        UART_SendString("[SPI ] W25Q64 absent -> backup disabled\r\n");
    }

    return s_present;
}

bool SpiFlash_IsPresent(void)
{
    return s_present;
}

uint32_t SpiFlash_ReadJedecId(void)
{
    s_txBuf[0] = CMD_JEDEC_ID;
    s_txBuf[1] = 0xFFU;    /* 空拍，只为把 ID 移出来 */
    s_txBuf[2] = 0xFFU;
    s_txBuf[3] = 0xFFU;

    if (!SpiFlash_Transfer(s_txBuf, s_rxBuf, 4U))
    {
        return 0U;
    }

    return (((uint32_t)s_rxBuf[1] << 16U) |
            ((uint32_t)s_rxBuf[2] << 8U)  |
             (uint32_t)s_rxBuf[3]);
}

uint8_t SpiFlash_ReadStatus1(void)
{
    s_txBuf[0] = CMD_READ_STATUS1;
    s_txBuf[1] = 0xFFU;

    if (!SpiFlash_Transfer(s_txBuf, s_rxBuf, 2U))
    {
        return 0xFFU;   /* 传输失败按"忙"处理 */
    }

    return s_rxBuf[1];
}

bool SpiFlash_WaitReady(uint32_t timeoutMs)
{
    uint32_t elapsedMs = 0U;

    while (elapsedMs < timeoutMs)
    {
        if ((SpiFlash_ReadStatus1() & SPI_FLASH_SR1_WIP) == 0U)
        {
            return true;
        }
        OSIF_TimeDelay(1);
        elapsedMs++;
    }

    return false;
}

bool SpiFlash_Read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    if ((buf == NULL) || (len == 0U) || (len > SPI_FLASH_PAGE_SIZE))
    {
        return false;
    }
    if ((addr + len) > SPI_FLASH_SIZE_BYTES)
    {
        return false;
    }

    s_txBuf[0] = CMD_READ_DATA;
    SpiFlash_PutAddr(&s_txBuf[1], addr);
    (void)memset(&s_txBuf[4], 0xFF, len);      /* 空拍，时钟要一直打 */

    if (!SpiFlash_Transfer(s_txBuf, s_rxBuf, (uint16_t)(4U + len)))
    {
        return false;
    }

    /* 前 4 字节是命令/地址阶段的回读，丢弃 */
    (void)memcpy(buf, &s_rxBuf[4], len);
    return true;
}

bool SpiFlash_WritePage(uint32_t addr, const uint8_t *buf, uint32_t len)
{
    uint32_t pageOffset;

    if ((buf == NULL) || (len == 0U) || (len > SPI_FLASH_PAGE_SIZE))
    {
        return false;
    }

    pageOffset = addr % SPI_FLASH_PAGE_SIZE;
    if ((pageOffset + len) > SPI_FLASH_PAGE_SIZE)
    {
        return false;       /* 跨页，调用者需自行拆分 */
    }
    if ((addr + len) > SPI_FLASH_SIZE_BYTES)
    {
        return false;
    }

    if (!SpiFlash_WriteEnable())
    {
        return false;
    }

    s_txBuf[0] = CMD_PAGE_PROGRAM;
    SpiFlash_PutAddr(&s_txBuf[1], addr);
    (void)memcpy(&s_txBuf[4], buf, len);

    if (!SpiFlash_Transfer(s_txBuf, s_rxBuf, (uint16_t)(4U + len)))
    {
        return false;
    }

    return SpiFlash_WaitReady(SPI_FLASH_PROGRAM_TIMEOUT_MS);
}

bool SpiFlash_EraseSector(uint32_t addr)
{
    addr &= ~(SPI_FLASH_SECTOR_SIZE - 1U);      /* 向下对齐到扇区首 */

    if (addr >= SPI_FLASH_SIZE_BYTES)
    {
        return false;
    }

    if (!SpiFlash_WriteEnable())
    {
        return false;
    }

    s_txBuf[0] = CMD_SECTOR_ERASE;
    SpiFlash_PutAddr(&s_txBuf[1], addr);

    if (!SpiFlash_Transfer(s_txBuf, s_rxBuf, 4U))
    {
        return false;
    }

    return SpiFlash_WaitReady(SPI_FLASH_ERASE_TIMEOUT_MS);
}