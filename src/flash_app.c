/**
 * @file    flash_app.c
 * @brief   内部 P-Flash 驱动实现
 * @details 提供擦除、写入、校验、CRC32 计算，并负责 Boot 保护区
 *          （升级标志 / 分区描述表）的专用擦写接口。
 *
 *          地址权限边界：
 *            App 区    [0x00010000, 0x00080000)  → FlashApp_Erase / FlashApp_Write
 *            Boot 保护区 [0x0000E000, 0x00010000)  → FlashApp_EraseBootSector
 *                                                    / FlashApp_WriteBootSector
 */

#include "flash_app.h"
#include "peripherals_flash_1.h"
#include "interrupt_manager.h"

/* ==================== 静态变量定义 ==================== */

/* Flash 驱动配置结构体（Init 后填充，后续所有操作都用它） */
static flash_ssd_config_t s_flashConfig;

/* ==================== 公共函数实现 ==================== */

/**
 * @brief  初始化 Flash 驱动
 */
void FlashApp_Init(void)
{
    FLASH_DRV_Init(&Flash_InitConfig0, &s_flashConfig);
}

/**
 * @brief  擦除 Flash 指定区域
 * @param  addr  起始地址（会自动对齐到扇区边界）
 * @param  size  擦除大小（会向上取整到扇区边界）
 * @return 0=成功, 1=失败
 * @note    S32K144 P-Flash: 地址 16 字节对齐, 大小 4KB 对齐
 */
uint8_t FlashApp_Erase(uint32_t addr, uint32_t size)
{
    /* 1. 地址安全检查：不能擦 Bootloader 区域，且整个擦除区间落在 App 区内 */
    if (addr < APP_FLASH_BASE) {
        return 1U;
    }
    if (size == 0U) {
        return 1U;
    }
    /* 用减法比较而非 addr+size，避免 size 极大时 32 位加法回绕绕过检查 */
    if ((addr >= APP_FLASH_END) || (size > (APP_FLASH_END - addr))) {
        return 1U;
    }

    /* 2. 对齐：地址 16 字节对齐，大小 4KB 对齐 */
    uint32_t alignedAddr = addr & ~0x0FU;
    uint32_t alignedSize = size;
    if ((alignedSize % 0x1000U) != 0U) {
        alignedSize = ((alignedSize / 0x1000U) + 1U) * 0x1000U;
    }

    /* 2.1 对齐后必须复查上界：size 向上取整可能越出 App 区末尾
     *     （例：addr=0x7F800,size=0x800 → 取整成 0x1000，会擦到 0x70000~0x80800） */
    if (alignedSize > (APP_FLASH_END - alignedAddr)) {
        return 1U;
    }

    /* 3. 关中断（擦除期间不能被打断） */
    INT_SYS_DisableIRQGlobal();

    /* 4. 执行擦除 */
    status_t status = FLASH_DRV_EraseSector(&s_flashConfig, alignedAddr, alignedSize);

    /* 5. 开中断 */
    INT_SYS_EnableIRQGlobal();

    return (status == STATUS_SUCCESS) ? 0U : 1U;
}

/**
 * @brief  写入数据到 Flash
 * @param  addr  起始地址（必须 8 字节对齐）
 * @param  data  要写入的数据
 * @param  size  数据大小（必须 8 字节对齐）
 * @return 0=成功, 1=失败
 * @note   addr/size 必须完整落在 App 区 [APP_FLASH_BASE, APP_FLASH_END) 内，
 *         否则拒绝写入（防止升级数据越界写穿激活槽/槽末尾）。
 */
uint8_t FlashApp_Write(uint32_t addr, const uint8_t *data, uint32_t size)
{
    /* 1. 地址安全检查：下界 + 上界，整个写入范围必须落在 App 区内 */
    if (addr < APP_FLASH_BASE) {
        return 1U;
    }
    if (addr >= APP_FLASH_END) {
        return 1U;
    }
    if (size == 0U) {
        return 1U;
    }
    /* 用减法比较而非 addr+size，避免 size 极大时 32 位加法回绕绕过检查 */
    if (size > (APP_FLASH_END - addr)) {
        return 1U;
    }

    /* 2. 对齐检查：地址和大小都必须 8 字节对齐 */
    if ((addr % FLASH_WRITE_ALIGN != 0U) || (size % FLASH_WRITE_ALIGN != 0U)) {
        return 1U;
    }

    /* 3. 关中断 */
    INT_SYS_DisableIRQGlobal();

    /* 4. 执行写入 */
    status_t status = FLASH_DRV_Program(&s_flashConfig, addr, size, data);

    /* 5. 开中断 */
    INT_SYS_EnableIRQGlobal();

    return (status == STATUS_SUCCESS) ? 0U : 1U;
}

/**
 * @brief  擦除 Boot 保护数据区内一个 4KB 扇区
 * @param  addr  扇区起始地址（须 4KB 对齐，落在 0xE000 ~ 0xFFFF）
 * @return 0=成功, 1=失败
 * @note   用于擦除升级标志扇区 / 描述表扇区，不受 App 区限制。
 */
uint8_t FlashApp_EraseBootSector(uint32_t addr)
{
    /* 1. 地址必须落在 Boot 保护数据区 [0xE000, 0x10000) 且 4KB 对齐 */
    if ((addr < BOOT_DATA_BASE) || (addr >= BOOT_FLASH_END)) {
        return 1U;
    }
    if ((addr % FLASH_SECTOR_SIZE) != 0U) {
        return 1U;
    }

    /* 2. 关中断 */
    INT_SYS_DisableIRQGlobal();

    /* 3. 擦除该扇区 */
    status_t status = FLASH_DRV_EraseSector(&s_flashConfig, addr, FLASH_SECTOR_SIZE);

    /* 4. 开中断 */
    INT_SYS_EnableIRQGlobal();

    return (status == STATUS_SUCCESS) ? 0U : 1U;
}

/**
 * @brief  擦除并写入 Boot 保护数据区一个 4KB 扇区
 * @param  addr  扇区起始地址（须 4KB 对齐，落在 0xE000 ~ 0xFFFF）
 * @param  data  要写入的数据
 * @param  size  数据大小（必须 8 字节对齐）
 * @return 0=成功, 1=失败
 * @note   先擦除 addr 所在 4KB 扇区，再写入。供升级标志 / 描述表落盘使用。
 */
uint8_t FlashApp_WriteBootSector(uint32_t addr, const uint8_t *data, uint32_t size)
{
    /* 1. 地址必须落在 Boot 保护数据区 [0xE000, 0x10000) 且 4KB 对齐 */
    if ((addr < BOOT_DATA_BASE) || (addr >= BOOT_FLASH_END)) {
        return 1U;
    }
    if ((addr % FLASH_SECTOR_SIZE) != 0U) {
        return 1U;
    }

    /* 2. 对齐检查：大小必须 8 字节对齐 */
    if ((size == 0U) || (size % FLASH_WRITE_ALIGN != 0U)) {
        return 1U;
    }

    /* 3. 关中断 */
    INT_SYS_DisableIRQGlobal();

    /* 4. 擦除该扇区 */
    status_t status = FLASH_DRV_EraseSector(&s_flashConfig, addr, FLASH_SECTOR_SIZE);

    /* 5. 擦除成功才写 */
    if (status == STATUS_SUCCESS) {
        status = FLASH_DRV_Program(&s_flashConfig, addr, size, data);
    }

    /* 6. 开中断 */
    INT_SYS_EnableIRQGlobal();

    return (status == STATUS_SUCCESS) ? 0U : 1U;
}

/**
 * @brief  校验 Flash 数据
 * @param  addr     Flash 地址
 * @param  expected 期望的数据
 * @param  size     校验长度
 * @return 0=一致, 1=不一致
 */
uint8_t FlashApp_Verify(uint32_t addr, const uint8_t *expected, uint32_t size)
{
    const uint8_t *flashPtr = (const uint8_t *)addr;

    for (uint32_t i = 0; i < size; i++) {
        if (flashPtr[i] != expected[i]) {
            return 1U;
        }
    }
    return 0U;
}

/* ==================== CRC32 校验 ==================== */

/**
 * @brief  CRC32 查找表（IEEE 802.3, 多项式 0xEDB88320 反射形式）
 * @note   与 zlib.crc32() / Python binascii.crc32() / PNG CRC 算法一致
 */
static const uint32_t crc32Table[256] = {
    0x00000000U, 0x77073096U, 0xEE0E612CU, 0x990951BAU, 0x076DC419U, 0x706AF48FU, 0xE963A535U, 0x9E6495A3U,
    0x0EDB8832U, 0x79DCB8A4U, 0xE0D5E91EU, 0x97D2D988U, 0x09B64C2BU, 0x7EB17CBDU, 0xE7B82D07U, 0x90BF1D91U,
    0x1DB71064U, 0x6AB020F2U, 0xF3B97148U, 0x84BE41DEU, 0x1ADAD47DU, 0x6DDDE4EBU, 0xF4D4B551U, 0x83D385C7U,
    0x136C9856U, 0x646BA8C0U, 0xFD62F97AU, 0x8A65C9ECU, 0x14015C4FU, 0x63066CD9U, 0xFA0F3D63U, 0x8D080DF5U,
    0x3B6E20C8U, 0x4C69105EU, 0xD56041E4U, 0xA2677172U, 0x3C03E4D1U, 0x4B04D447U, 0xD20D85FDU, 0xA50AB56BU,
    0x35B5A8FAU, 0x42B2986CU, 0xDBBBC9D6U, 0xACBCF940U, 0x32D86CE3U, 0x45DF5C75U, 0xDCD60DCFU, 0xABD13D59U,
    0x26D930ACU, 0x51DE003AU, 0xC8D75180U, 0xBFD06116U, 0x21B4F4B5U, 0x56B3C423U, 0xCFBA9599U, 0xB8BDA50FU,
    0x2802B89EU, 0x5F058808U, 0xC60CD9B2U, 0xB10BE924U, 0x2F6F7C87U, 0x58684C11U, 0xC1611DABU, 0xB6662D3DU,
    0x76DC4190U, 0x01DB7106U, 0x98D220BCU, 0xEFD5102AU, 0x71B18589U, 0x06B6B51FU, 0x9FBFE4A5U, 0xE8B8D433U,
    0x7807C9A2U, 0x0F00F934U, 0x9609A88EU, 0xE10E9818U, 0x7F6A0DBBU, 0x086D3D2DU, 0x91646C97U, 0xE6635C01U,
    0x6B6B51F4U, 0x1C6C6162U, 0x856530D8U, 0xF262004EU, 0x6C0695EDU, 0x1B01A57BU, 0x8208F4C1U, 0xF50FC457U,
    0x65B0D9C6U, 0x12B7E950U, 0x8BBEB8EAU, 0xFCB9887CU, 0x62DD1DDFU, 0x15DA2D49U, 0x8CD37CF3U, 0xFBD44C65U,
    0x4DB26158U, 0x3AB551CEU, 0xA3BC0074U, 0xD4BB30E2U, 0x4ADFA541U, 0x3DD895D7U, 0xA4D1C46DU, 0xD3D6F4FBU,
    0x4369E96AU, 0x346ED9FCU, 0xAD678846U, 0xDA60B8D0U, 0x44042D73U, 0x33031DE5U, 0xAA0A4C5FU, 0xDD0D7CC9U,
    0x5005713CU, 0x270241AAU, 0xBE0B1010U, 0xC90C2086U, 0x5768B525U, 0x206F85B3U, 0xB966D409U, 0xCE61E49FU,
    0x5EDEF90EU, 0x29D9C998U, 0xB0D09822U, 0xC7D7A8B4U, 0x59B33D17U, 0x2EB40D81U, 0xB7BD5C3BU, 0xC0BA6CADU,
    0xEDB88320U, 0x9ABFB3B6U, 0x03B6E20CU, 0x74B1D29AU, 0xEAD54739U, 0x9DD277AFU, 0x04DB2615U, 0x73DC1683U,
    0xE3630B12U, 0x94643B84U, 0x0D6D6A3EU, 0x7A6A5AA8U, 0xE40ECF0BU, 0x9309FF9DU, 0x0A00AE27U, 0x7D079EB1U,
    0xF00F9344U, 0x8708A3D2U, 0x1E01F268U, 0x6906C2FEU, 0xF762575DU, 0x806567CBU, 0x196C3671U, 0x6E6B06E7U,
    0xFED41B7AU, 0x89D32BE0U, 0x10DA7A5AU, 0x67DD4ACCU, 0xF9B9DF6FU, 0x8EBEEFF9U, 0x17B7BE43U, 0x60B08ED5U,
    0xD6D6A3E8U, 0xA1D1937EU, 0x38D8C2C4U, 0x4FDFF252U, 0xD1BB67F1U, 0xA6BC5767U, 0x3FB506DDU, 0x48B2364BU,
    0xD80D2BDAU, 0xAF0A1B4CU, 0x36034AF6U, 0x41047A60U, 0xDF60EFC3U, 0xA867DF55U, 0x316E58EEU, 0x4669BE02U,
    0xCB61B38CU, 0xBC66831AU, 0x256FD2A0U, 0x5268E236U, 0xCC0C7795U, 0xBB0B4703U, 0x220216B9U, 0x5505262FU,
    0xC5BA3BBEU, 0xB2BD0B28U, 0x2BB45A92U, 0x5CB36A04U, 0xC2D7FFA7U, 0xB5D0CF31U, 0x2CD99E8BU, 0x5BDEAE1DU,
    0x9B64C2B0U, 0xEC63F226U, 0x756AA39CU, 0x026D930AU, 0x9C0906A9U, 0xEB0E363FU, 0x72076785U, 0x05005713U,
    0x95BF4A82U, 0xE2B87A14U, 0x7BB12BAEU, 0x0CB61B38U, 0x92D28E9BU, 0xE5D5BE0DU, 0x7CDCEFB7U, 0x0BDBDF21U,
    0x86D3D2D4U, 0xF1D4E242U, 0x68DDB3F8U, 0x1FDA836EU, 0x81BE16CDU, 0xF6B9265BU, 0x6FB077E1U, 0x18B74777U,
    0x88085AE6U, 0xFF0F6A70U, 0x66063BCAU, 0x11010B5CU, 0x8F659EFFU, 0xF862AE69U, 0x616BFFD3U, 0x166CCF45U,
    0xA00AE278U, 0xD70DD2EEU, 0x4E048354U, 0x3903B3C2U, 0xA7672661U, 0xD06016F7U, 0x4969294EU, 0x3E6E77DBU,
    0xAED16A4AU, 0xD9D65ADCU, 0x40DF0B66U, 0x37D83BF0U, 0xA9BCAE53U, 0xDEBB9EC5U, 0x47B2CF7FU, 0x30B5FFE9U,
    0xBDBDF21CU, 0xCABAC28AU, 0x53B39330U, 0x24B4A3A6U, 0xBAD03605U, 0xCDD70693U, 0x54DE5729U, 0x23D967BFU,
    0xB3667A2EU, 0xC4614AB8U, 0x5D681B02U, 0x2A6F2B94U, 0xB40BBE37U, 0xC30C8EA1U, 0x5A05DF1BU, 0x2D02EF8DU
};

/**
 * @brief  计算 Flash 指定区域的 CRC32
 * @param  addr  起始地址
 * @param  size  字节数
 * @return CRC32 值（与 zlib.crc32() 一致）
 * @note   算法：CRC-32/ISO-HDLC
 *         - 多项式：0xEDB88320（反射）
 *         - 初始值：0xFFFFFFFF
 *         - 输出异或：0xFFFFFFFF
 *         上位机可用 Python: zlib.crc32(data) & 0xFFFFFFFF
 */
uint32_t FlashApp_CalcCRC32(uint32_t addr, uint32_t size)
{
    const uint8_t *data = (const uint8_t *)addr;
    uint32_t crc = 0xFFFFFFFFU;

    for (uint32_t i = 0U; i < size; i++) {
        crc = crc32Table[(crc ^ data[i]) & 0xFFU] ^ (crc >> 8U);
    }

    return (crc ^ 0xFFFFFFFFU);
}
