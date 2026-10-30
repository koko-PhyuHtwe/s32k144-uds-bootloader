/**
 * @file    spi_flash_test.c
 * @brief   W25Q64 硬件自检
 * @details 依次验证：SPI 通信（读 ID）、擦除、页写入、读回比对。
 *          任何一步失败都会打印失败原因并停止，方便定位是接线、
 *          片选极性、时钟还是芯片本身的问题。
 *
 *          测试地址固定在最后一个扇区（8MB - 4KB = 0x7FF000），
 *          不占用将来仓库要用的低地址区域。
 */

#include "spi_flash_test.h"
#include "spi_flash.h"
#include "uart.h"
#include "device_registers.h"

#define TEST_SECTOR_ADDR    (SPI_FLASH_SIZE_BYTES - SPI_FLASH_SECTOR_SIZE)  /* 0x7FF000 */
#define TEST_PATTERN_LEN    SPI_FLASH_PAGE_SIZE                             /* 256 字节 */

static uint8_t s_wrBuf[TEST_PATTERN_LEN];
static uint8_t s_rdBuf[TEST_PATTERN_LEN];

/* ==================== 串口打印辅助 ==================== */

/**
 * @brief  以 "0x1234ABCD" 形式打印 32 位十六进制数
 */
static void Uart_PrintHex32(uint32_t value)
{
    static const char hexChar[] = "0123456789ABCDEF";
    char   text[11];
    int8_t i;

    text[0] = '0';
    text[1] = 'x';
    for (i = 9; i >= 2; i--)
    {
        text[i] = hexChar[value & 0x0FU];
        value >>= 4U;
    }
    text[10] = '\0';

    UART_SendString(text);
}

/**
 * @brief  打印无符号十进制数
 */
static void Uart_PrintDec(uint32_t value)
{
    char     text[12];
    int8_t   index = 11;

    text[index] = '\0';
    if (value == 0U)
    {
        UART_SendString("0");
        return;
    }

    while ((value > 0U) && (index > 0))
    {
        index--;
        text[index] = (char)('0' + (value % 10U));
        value /= 10U;
    }

    UART_SendString(&text[index]);
}

/**
 * @brief  打印结果前缀
 */
static void Uart_PrintResult(bool ok)
{
    UART_SendString(ok ? "[ OK ] " : "[FAIL] ");
}

/* ==================== 自检主流程 ==================== */

void SpiFlash_RunSelfTest(void)
{
    uint32_t i;
    uint32_t mismatchIndex = 0U;
    bool     ok;
    bool     dataOk = true;
    uint8_t  status;

    UART_SendString("\r\n---------- SPI Flash Self Test ----------\r\n");

    /* ---- 步骤 1：初始化 + 读 JEDEC ID ---- */
    UART_SendString("[1/4] Probe chip (CMD 0x9F) ... ");
    ok = SpiFlash_Init();

    if (!ok)
    {
        uint32_t rawId;
        uint8_t  status1;

        UART_SendString("FAILED\r\n");
        UART_SendString("\r\n      ---- 诊断信息 ----\r\n");

        /* 引脚复用和 LPSPI 寄存器（确认引脚真的切到 ALT3、分频/极性对） */
        UART_SendString("      PORTB.PCR14..17 = ");
        Uart_PrintHex32(PORTB->PCR[14]);
        UART_SendString(" ");
        Uart_PrintHex32(PORTB->PCR[15]);
        UART_SendString(" ");
        Uart_PrintHex32(PORTB->PCR[16]);
        UART_SendString(" ");
        Uart_PrintHex32(PORTB->PCR[17]);
        UART_SendString("\r\n      LPSPI1 TCR/CCR   = ");
        Uart_PrintHex32(LPSPI1->TCR);
        UART_SendString(" / ");
        Uart_PrintHex32(LPSPI1->CCR);
        UART_SendString("\r\n");

        rawId = SpiFlash_ReadJedecId();
        UART_SendString("      Raw JEDEC ID = ");
        Uart_PrintHex32(rawId);
        UART_SendString("\r\n");

        status1 = SpiFlash_ReadStatus1();
        UART_SendString("      Status1      = ");
        Uart_PrintHex32((uint32_t)status1);
        UART_SendString("\r\n");

        rawId &= 0x00FFFFFFUL;
        if (rawId == 0x00FFFFFFUL)
        {
            UART_SendString("      -> MISO 恒高：芯片根本没在驱动这条线\r\n");
            UART_SendString("         (片选没送到 / 芯片没供电 / DIP 开关断开)\r\n");
        }
        else if (rawId == 0UL)
        {
            UART_SendString("      -> MISO 恒低：芯片根本没在驱动这条线\r\n");
            UART_SendString("         (片选没送到 / 芯片没供电 / MISO 被下拉)\r\n");
        }
        else
        {
            UART_SendString("      -> 总线上有数据，但制造商 ID 不合法\r\n");
        }

        UART_SendString("      Retry @1MHz  = ");
        Uart_PrintHex32(SpiFlash_ProbeAtBaud(1000000U));
        UART_SendString("\r\n");

        /* 连读 8 次：数值固定=线路被钳位/芯片在乱答；数值乱跳=悬空 */
        UART_SendString("      Trace ID  x8 : ");
        for (i = 0U; i < 8U; i++)
        {
            Uart_PrintHex32(SpiFlash_ReadJedecId());
            UART_SendString(" ");
        }
        UART_SendString("\r\n      Trace SR1 x8 : ");
        for (i = 0U; i < 8U; i++)
        {
            Uart_PrintHex32((uint32_t)SpiFlash_ReadStatus1());
            UART_SendString(" ");
        }
        UART_SendString("\r\n");

        UART_SendString("      Check:\r\n");
        UART_SendString("        * SW2 DIP: LPSPI1_CLK/MISO/MOSI/CSN3 all ON\r\n");
        UART_SendString("        * R36 10k pull-up on CS#\r\n");
        UART_SendString("        * Flash VCC = 3.3V (measure C42)\r\n");
        UART_SendString("----------------------------------------\r\n");
        return;
    }

    UART_SendString("OK  ID = ");
    Uart_PrintHex32(SpiFlash_ReadJedecId());
    if (SpiFlash_ReadJedecId() != SPI_FLASH_JEDEC_W25Q64)
    {
        UART_SendString("  (expect 0x00EF4017, different chip?)");
    }
    UART_SendString("\r\n");

    /* ---- 步骤 2：擦除扇区 ---- */
    UART_SendString("[2/4] Erase sector at ");
    Uart_PrintHex32(TEST_SECTOR_ADDR);
    UART_SendString(" ... ");

    ok = SpiFlash_EraseSector(TEST_SECTOR_ADDR);
    Uart_PrintResult(ok);
    UART_SendString("\r\n");
    if (!ok)
    {
        UART_SendString("      Erase failed (WREN ignored or WIP timeout)\r\n");
        UART_SendString("----------------------------------------\r\n");
        return;
    }

    /* ---- 步骤 3：写入 256 字节图案 ---- */
    for (i = 0U; i < TEST_PATTERN_LEN; i++)
    {
        s_wrBuf[i] = (uint8_t)((i * 7U) + 0x5AU);
    }

    UART_SendString("[3/4] Write 256 bytes ... ");
    ok = SpiFlash_WritePage(TEST_SECTOR_ADDR, s_wrBuf, TEST_PATTERN_LEN);
    Uart_PrintResult(ok);
    UART_SendString("\r\n");
    if (!ok)
    {
        UART_SendString("      Page program failed\r\n");
        UART_SendString("----------------------------------------\r\n");
        return;
    }

    /* ---- 步骤 4：读回比对 ---- */
    UART_SendString("[4/4] Read back and compare ... ");
    ok = SpiFlash_Read(TEST_SECTOR_ADDR, s_rdBuf, TEST_PATTERN_LEN);

    status = SpiFlash_ReadStatus1();
    UART_SendString("\r\n      Status1 = ");
    Uart_PrintHex32((uint32_t)status);
    UART_SendString("\r\n");

    if (!ok)
    {
        UART_SendString("      Read failed\r\n");
        UART_SendString("----------------------------------------\r\n");
        return;
    }

    for (i = 0U; i < TEST_PATTERN_LEN; i++)
    {
        if (s_rdBuf[i] != s_wrBuf[i])
        {
            dataOk = false;
            mismatchIndex = i;
            break;
        }
    }

    if (!dataOk)
    {
        UART_SendString("      Content mismatch at offset ");
        Uart_PrintDec(mismatchIndex);
        UART_SendString(": wrote ");
        Uart_PrintHex32((uint32_t)s_wrBuf[mismatchIndex]);
        UART_SendString(", read ");
        Uart_PrintHex32((uint32_t)s_rdBuf[mismatchIndex]);
        UART_SendString("\r\n");
    }

    Uart_PrintResult(dataOk);
    UART_SendString(dataOk ? "256/256 bytes matched\r\n" : "data corrupted\r\n");
    UART_SendString(dataOk ? "RESULT: SPI Flash WORKS\r\n" : "RESULT: SPI Flash BROKEN\r\n");
    UART_SendString("----------------------------------------\r\n");
}