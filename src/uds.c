/**
 * @file    uds.c
 * @brief   UDS 诊断服务源文件
 * @details 实现 ISO 14229 UDS 协议子集（会话控制 / ECU 复位 / 读 DID）
 *          仅支持单帧传输（SingleFrame），最大 7 字节数据
 */

#include "uds.h"
#include "can.h"
#include "flash_app.h"
#include "partition.h"
#include "uart.h"
#include "osif.h"
#include "interrupt_manager.h"
#include "device_registers.h"
#include "adc_mon.h"          /* 电压监控：欠压时拒绝/中止刷写 */
#include "app.h"

/* ==================== CMSIS 兼容函数（NXP SDK 不提供，自己实现） ==================== */

/**
 * @brief  设置主栈指针 MSP
 * @note   NXP S32 SDK 的 s32_core_cm4.h 不提供 __set_MSP()，
 *         这里用内联汇编实现（Cortex-M4: msr msp, r0）
 */
__attribute__((always_inline)) static inline void __set_MSP(uint32_t topOfMainStack)
{
    __asm volatile ("msr msp, %0" : : "r" (topOfMainStack) : );
}

/**
 * @brief  触发系统复位（NVIC System Reset）
 * @note   参考 S32 SDK 的 system_S32K144.c::SystemSoftwareReset() 实现
 *         通过写 SCB->AIRCR 的 SYSRESETREQ 位触发芯片复位
 */
static void NVIC_SystemReset(void)
{
    uint32_t regValue;

    /* 关中断，防止复位过程中被中断打断 */
    INT_SYS_DisableIRQGlobal();

    /* 读当前 AIRCR */
    regValue = S32_SCB->AIRCR;

    /* 清掉 VECTKEY 字段（高 16 位） */
    regValue &= ~(S32_SCB_AIRCR_VECTKEY_MASK);

    /* 写 VECTKEY = 0x05FA（必须，否则写不进去） */
    regValue |= S32_SCB_AIRCR_VECTKEY(FEATURE_SCB_VECTKEY);

    /* 置位 SYSRESETREQ（请求系统复位） */
    regValue |= S32_SCB_AIRCR_SYSRESETREQ(0x1U);

    /* 写回 AIRCR，复位立即生效 */
    S32_SCB->AIRCR = regValue;

    /* 死等：复位真正生效前不要往下走 */
    __asm volatile ("dsb 0xF" ::: "memory");
    while (1) {
        __asm volatile ("nop");
    }
}

/* ==================== 内部函数声明 ==================== */
static void UDS_HandleSessionControl(const can_frame_t *frame);
static void UDS_HandleECUReset(const can_frame_t *frame);
static void UDS_HandleReadDID(const can_frame_t *frame);
static void UDS_HandleSecurityAccess(const can_frame_t *frame);
static void UDS_HandleWriteDID(const can_frame_t *frame);
static void UDS_HandleRequestDownload(const can_frame_t *frame);
static void UDS_HandleTransferData(const can_frame_t *frame);
static void UDS_HandleRequestTransferExit(const can_frame_t *frame);
static void UDS_SendPositiveResponse(uint8_t sid, const uint8_t *payload, uint8_t payloadLen);
static void UDS_SendNegativeResponse(uint8_t sid, uint8_t nrc);

/* ==================== 下载状态管理 ==================== */
typedef enum {
    DOWNLOAD_IDLE,         /* 空闲，没有下载 */
    DOWNLOAD_IN_PROGRESS    /* 下载进行中 */
} download_state_t;

static download_state_t downloadState     = DOWNLOAD_IDLE;
static uint32_t downloadStartAddr          = 0U;   /* 下载起始地址（保存供 CRC 校验） */
static uint32_t downloadAddr              = 0U;   /* 当前写入地址 */
static uint32_t downloadTotalSize         = 0U;   /* 总大小 */
static uint8_t  blockCounter              = 1U;    /* 期望的块序号 */
static uint8_t  dataBuffer[8];                    /* 数据缓存（攒8字节写一次） */
static uint8_t  dataBufferIndex           = 0U;   /* 缓存当前字节数 */
static uint8_t  updateCommitted           = 0U;   /* 本会话 0x2E 是否已成功提交（幂等保护） */

/* ==================== 0x27 安全访问状态 ==================== */
static uint8_t  securityUnlocked          = 0U;   /* 0=锁定, 1=已解锁 */
static uint16_t securitySeed              = 0U;   /* 当前下发的种子 */
static uint8_t  securitySeedRequested     = 0U;   /* 0=未发种子, 1=已发种子待校验 */
static uint8_t  securityAttemptCounter    = 0U;   /* 失败计数器 */

/* ==================== S3Server 超时管理 ==================== */
static volatile uint32_t s3ServerTimer = 0U;  /* 剩余超时时间 (ms) */
static uint8_t bootloaderActive = 0U;          /* 0=可以跳App, 1=正在升级，禁止跳 */

/* ==================== 公共接口 ==================== */

/**
 * @brief  UDS 初始化
 */
void UDS_Init(void)
{
    s3ServerTimer = S3_SERVER_TIMEOUT_MS;  /* 启动 S3Server 倒计时 */
    bootloaderActive = 0U;

    /* 安全访问初始化为锁定状态 */
    securityUnlocked       = 0U;
    securitySeed           = 0U;
    securitySeedRequested  = 0U;
    securityAttemptCounter = 0U;

    /* 提交标志复位：允许新一轮下载后执行 0x2E 提交 */
    updateCommitted        = 0U;
}

/**
 * @brief  UDS 主处理函数
 * @note   在主循环中调用，从 CAN 队列取数据并解析
 */
void UDS_Process(void)
{
    /* 队列为空则直接返回 */
    if (!CAN_RxAvailable()) {
        return;
    }

    /* 取一帧数据 */
    can_frame_t frame;
    CAN_GetRxFrame(&frame);

    /* 校验：ID 必须是 0x7E0，DLC 至少 2 字节（PCI + SID） */
    if ((frame.id != CAN_RX_ID) || (frame.dlc < 2U)) {
        return;
    }

    /* 收到有效 UDS 请求，重置 S3Server 定时器 */
    s3ServerTimer = S3_SERVER_TIMEOUT_MS;

    /* 解析 SID（data[1]），分发到对应服务 */
    uint8_t sid = frame.data[1];

    switch (sid) {
        case UDS_SID_SESSION_CONTROL:
            UDS_HandleSessionControl(&frame);
            break;

        case UDS_SID_ECU_RESET:
            UDS_HandleECUReset(&frame);
            break;

        case UDS_SID_READ_DID:
            UDS_HandleReadDID(&frame);
            break;

        case UDS_SID_SECURITY_ACCESS:
            UDS_HandleSecurityAccess(&frame);
            break;

        case UDS_SID_WRITE_DID:
            UDS_HandleWriteDID(&frame);
            break;

        case UDS_SID_REQUEST_DOWNLOAD:
            UDS_HandleRequestDownload(&frame);
            break;

        case UDS_SID_TRANSFER_DATA:
            UDS_HandleTransferData(&frame);
            break;

        case UDS_SID_REQUEST_TRANSFER_EXIT:
            UDS_HandleRequestTransferExit(&frame);
            break;

        default:
            /* 不支持的服务 */
            UDS_SendNegativeResponse(sid, UDS_NRC_SERVICE_NOT_SUPPORTED);
            break;
    }
}

/* ==================== 服务处理实现 ==================== */

/**
 * @brief  处理 0x10 会话控制
 * @note   请求：02 10 03
 *         正响应：06 50 03 00 32 01 F4
 *         （P2=50ms, P2*=500ms）
 */
static void UDS_HandleSessionControl(const can_frame_t *frame)
{
    uint8_t pci     = frame->data[0];
    uint8_t subFunc = frame->data[2];

    /* 校验长度：PCI 低 4 位 = 2（SID + 子功能 = 2 字节） */
    if ((pci & 0x0FU) != 2U) {
        UDS_SendNegativeResponse(UDS_SID_SESSION_CONTROL, UDS_NRC_INVALID_MESSAGE_LENGTH);
        return;
    }

    /* 只支持默认会话和扩展会话 */
    if ((subFunc != UDS_SESSION_DEFAULT) && (subFunc != UDS_SESSION_EXTENDED)) {
        UDS_SendNegativeResponse(UDS_SID_SESSION_CONTROL, UDS_NRC_SUBFUNCTION_NOT_SUPPORTED);
        return;
    }

    /* 构造正响应负载：子功能 + P2(2字节) + P2*(2字节) */
    uint8_t payload[5];
    payload[0] = subFunc;   /* 子功能 */
    payload[1] = 0x00U;     /* P2 高字节 */
    payload[2] = 0x32U;     /* P2 低字节 = 50ms */
    payload[3] = 0x01U;     /* P2* 高字节 */
    payload[4] = 0xF4U;     /* P2* 低字节 = 500ms */

    UDS_SendPositiveResponse(UDS_SID_SESSION_CONTROL, payload, 5);
}

/**
 * @brief  处理 0x11 ECU 复位
 * @note   请求：02 11 01
 *         正响应：02 51 01
 *         （先回复，再复位。复位逻辑后续实现）
 */
static void UDS_HandleECUReset(const can_frame_t *frame)
{
    uint8_t pci     = frame->data[0];
    uint8_t subFunc = frame->data[2];

    /* 校验长度 */
    if ((pci & 0x0FU) != 2U) {
        UDS_SendNegativeResponse(UDS_SID_ECU_RESET, UDS_NRC_INVALID_MESSAGE_LENGTH);
        return;
    }

    /* 只支持硬复位（子功能 0x01） */
    if (subFunc != 0x01U) {
        UDS_SendNegativeResponse(UDS_SID_ECU_RESET, UDS_NRC_SUBFUNCTION_NOT_SUPPORTED);
        return;
    }

    /* 正响应：子功能 */
    uint8_t payload[1];
    payload[0] = subFunc;

    UDS_SendPositiveResponse(UDS_SID_ECU_RESET, payload, 1);

    /* 延时 100ms 后执行系统硬复位 */
    OSIF_TimeDelay(100);
    NVIC_SystemReset();  /* 不会返回 */
}

/**
 * @brief  处理 0x22 读 DID
 * @note   请求：03 22 F1 89
 *         正响应：07 62 F1 89 56 31 2E 30（"V1.0"）
 */
static void UDS_HandleReadDID(const can_frame_t *frame)
{
    uint8_t pci = frame->data[0];

    /* 校验长度：PCI 低 4 位 = 3（SID + 2字节DID = 3 字节） */
    if ((pci & 0x0FU) != 3U) {
        UDS_SendNegativeResponse(UDS_SID_READ_DID, UDS_NRC_INVALID_MESSAGE_LENGTH);
        return;
    }

    /* 提取 DID（data[2] << 8 | data[3]） */
    uint16_t did = ((uint16_t)frame->data[2] << 8) | frame->data[3];

    /* DID 0xF189：软件版本号（返回激活槽版本，ASCII "Vx.0"） */
    if (did == UDS_DID_SW_VERSION) {
        uint32_t ver = Partition_GetActiveVersion();
        uint8_t payload[6];
        payload[0] = (uint8_t)(did >> 8);       /* DID 高字节 */
        payload[1] = (uint8_t)(did & 0xFFU);    /* DID 低字节 */
        payload[2] = 'V';
        payload[3] = (uint8_t)('0' + (ver % 10U));  /* 个位（版本 0-9） */
        payload[4] = '.';
        payload[5] = '0';
        UDS_SendPositiveResponse(UDS_SID_READ_DID, payload, 6);
        return;
    }

    /* DID 0xF19A：当前激活槽号（0=SlotA, 1=SlotB） */
    if (did == UDS_DID_ACTIVE_SLOT) {
        uint8_t payload[3];
        payload[0] = (uint8_t)(did >> 8);
        payload[1] = (uint8_t)(did & 0xFFU);
        payload[2] = (Partition_GetActiveSlot() == SLOT_B) ? 0x01U : 0x00U;
        UDS_SendPositiveResponse(UDS_SID_READ_DID, payload, 3);
        return;
    }

    /* 不支持的 DID */
    UDS_SendNegativeResponse(UDS_SID_READ_DID, UDS_NRC_REQUEST_OUT_OF_RANGE);
}

/**
 * @brief  处理 0x27 安全访问（seed-key 解锁）
 * @note   子功能 0x01 请求种子：02 27 01
 *                 正响应：04 67 01 <seed_hi> <seed_lo>
 *         子功能 0x02 发送密钥：04 27 02 <key_hi>  <key_lo>
 *                 正响应：02 67 02
 *         密钥算法：key = (seed + 0x1111) & 0xFFFF
 *
 *         状态机：
 *           LOCKED --(0x27 01)--> SEED_SENT --(0x27 02 校验通过)--> UNLOCKED
 *                                     |
 *                                     +--(0x27 02 校验失败)--> LOCKED
 *
 *         未解锁时 0x34 请求下载会返回 NRC=0x33
 */
static void UDS_HandleSecurityAccess(const can_frame_t *frame)
{
    uint8_t pci     = frame->data[0];
    uint8_t dataLen = pci & 0x0FU;
    uint8_t subFunc = frame->data[2];

    /* 只支持子功能 0x01（请求种子）和 0x02（发送密钥） */
    if ((subFunc != UDS_SECURITY_REQUEST_SEED) &&
        (subFunc != UDS_SECURITY_SEND_KEY)) {
        UDS_SendNegativeResponse(UDS_SID_SECURITY_ACCESS, UDS_NRC_SUBFUNCTION_NOT_SUPPORTED);
        return;
    }

    /* 按子功能校验长度：
     *   0x01 请求种子：SID + subFunc = 2 字节
     *   0x02 发送密钥：SID + subFunc + 2字节key = 4 字节
     */
    if (subFunc == UDS_SECURITY_REQUEST_SEED) {
        if (dataLen != 2U) {
            UDS_SendNegativeResponse(UDS_SID_SECURITY_ACCESS, UDS_NRC_INVALID_MESSAGE_LENGTH);
            return;
        }
    } else {
        if (dataLen != 4U) {
            UDS_SendNegativeResponse(UDS_SID_SECURITY_ACCESS, UDS_NRC_INVALID_MESSAGE_LENGTH);
            return;
        }
    }

    /* 子功能 0x01：请求种子 */
    if (subFunc == UDS_SECURITY_REQUEST_SEED) {
        /* 超过失败重试上限，拒绝再次下发种子 */
        if (securityAttemptCounter >= UDS_SECURITY_MAX_ATTEMPTS) {
            UDS_SendNegativeResponse(UDS_SID_SECURITY_ACCESS, UDS_NRC_EXCEEDED_NUMBER_OF_ATTEMPTS);
            return;
        }

        /* 生成种子：用系统 tick 作为伪随机源（项目简化实现）。
           一轮只生成一次：重复请求种子返回同一个值——否则上位机
           超时重试会触发重新生成，旧种子算的 key 就和 ECU 存的对不上（NRC 0x35） */
        if (securitySeedRequested == 0U) {
            securitySeed = (uint16_t)(OSIF_GetMilliseconds() & 0xFFFFU);
            if (securitySeed == 0U) {
                securitySeed = 0x1234U;  /* 避免 0 种子 */
            }
        }

        /* 标记已下发种子，等待密钥 */
        securitySeedRequested = 1U;
        securityUnlocked      = 0U;

        /* 正响应：子功能 + 2字节种子（大端） */
        uint8_t payload[3];
        payload[0] = subFunc;
        payload[1] = (uint8_t)(securitySeed >> 8);
        payload[2] = (uint8_t)(securitySeed & 0xFFU);

        UDS_SendPositiveResponse(UDS_SID_SECURITY_ACCESS, payload, 3);
        return;
    }

    /* 子功能 0x02：发送密钥 */
    /* 已解锁时重复收到密钥（上轮正响应丢失、上位机重发）：直接回正响应，幂等 */
    if (securityUnlocked == 1U) {
        uint8_t okPayload[1];
        okPayload[0] = subFunc;
        UDS_SendPositiveResponse(UDS_SID_SECURITY_ACCESS, okPayload, 1);
        return;
    }

    /* 必须先请求过种子才能发密钥 */
    if (securitySeedRequested != 1U) {
        UDS_SendNegativeResponse(UDS_SID_SECURITY_ACCESS, UDS_NRC_REQUEST_SEQUENCE_ERROR);
        return;
    }

    /* 提取上位机发送的密钥（大端） */
    uint16_t recvKey = ((uint16_t)frame->data[3] << 8) | frame->data[4];

    /* 计算期望密钥：key = (seed + 0x1111) & 0xFFFF */
    uint16_t expectedKey = (uint16_t)((securitySeed + UDS_SECURITY_KEY_OFFSET) & 0xFFFFU);

    if (recvKey != expectedKey) {
        /* 密钥错误：累加失败计数，锁定状态，清除种子请求标志 */
        securityAttemptCounter++;
        securitySeedRequested = 0U;
        securityUnlocked      = 0U;

        /* 达到上限返回 0x36，否则返回 0x35 */
        if (securityAttemptCounter >= UDS_SECURITY_MAX_ATTEMPTS) {
            UDS_SendNegativeResponse(UDS_SID_SECURITY_ACCESS, UDS_NRC_EXCEEDED_NUMBER_OF_ATTEMPTS);
        } else {
            UDS_SendNegativeResponse(UDS_SID_SECURITY_ACCESS, UDS_NRC_INVALID_KEY);
        }
        return;
    }

    /* 密钥正确：解锁，清零失败计数 */
    securityUnlocked       = 1U;
    securitySeedRequested  = 0U;
    securityAttemptCounter = 0U;

    /* 正响应：子功能 */
    uint8_t payload[1];
    payload[0] = subFunc;
    UDS_SendPositiveResponse(UDS_SID_SECURITY_ACCESS, payload, 1);
}

/**
 * @brief  处理 0x2E 写 DID（用于 CRC32 校验）
 * @note   请求格式（DID 0xFF01）：
 *         data[0] = PCI = 0x06（6 字节数据）
 *         data[1] = 0x2E (SID)
 *         data[2] = 0xFF (DID 高字节)
 *         data[3] = 0x01 (DID 低字节)
 *         data[4] = CRC[31:24]
 *         data[5] = CRC[23:16]
 *         data[6] = CRC[15:8]
 *         data[7] = CRC[7:0]
 *
 *         正响应：04 6E FF 01 00  （末位 0=校验通过）
 *         否定响应：NRC=0x72 校验失败 / NRC=0x31 不支持的 DID
 */
static void UDS_HandleWriteDID(const can_frame_t *frame)
{
    uint8_t pci     = frame->data[0];
    uint8_t dataLen = pci & 0x0FU;

    /* 校验长度：SID + 2字节DID + 4字节CRC = 7 字节 */
    if (dataLen != 7U) {
        UDS_SendNegativeResponse(UDS_SID_WRITE_DID, UDS_NRC_INVALID_MESSAGE_LENGTH);
        return;
    }

    /* 提取 DID */
    uint16_t did = ((uint16_t)frame->data[2] << 8) | frame->data[3];

    /* 只支持 DID 0xFF01（CRC32 校验请求） */
    if (did != UDS_DID_CRC32) {
        UDS_SendNegativeResponse(UDS_SID_WRITE_DID, UDS_NRC_REQUEST_OUT_OF_RANGE);
        return;
    }

    /* 检查是否已下载完成（必须有 downloadTotalSize > 0 才能校验） */
    if (downloadState != DOWNLOAD_IDLE || downloadTotalSize == 0U) {
        UDS_SendNegativeResponse(UDS_SID_WRITE_DID, UDS_NRC_REQUEST_SEQUENCE_ERROR);
        return;
    }

    /* 幂等保护：本会话已成功提交过（0x2E 正响应丢失、CANoe 超时重发场景）。
       直接重发原正响应，不再重复提交——否则会把激活槽再翻回去。 */
    if (updateCommitted == 1U) {
        uint8_t retryPayload[3];
        retryPayload[0] = (uint8_t)(did >> 8);
        retryPayload[1] = (uint8_t)(did & 0xFFU);
        retryPayload[2] = 0x00U;  /* status = 0 表示成功 */
        UDS_SendPositiveResponse(UDS_SID_WRITE_DID, retryPayload, 3);
        return;
    }

    /* 提取上位机给的 CRC32（大端） */
    uint32_t expectedCrc = ((uint32_t)frame->data[4] << 24) |
                           ((uint32_t)frame->data[5] << 16) |
                           ((uint32_t)frame->data[6] << 8)  |
                           ((uint32_t)frame->data[7]);

    /* 计算 Flash 区域的 CRC32（downloadStartAddr 在 0x34 中保存） */
    uint32_t actualCrc = FlashApp_CalcCRC32(downloadStartAddr, downloadTotalSize);

    if (actualCrc != expectedCrc) {
        /* CRC 不一致，校验失败，不跳 App */
        UDS_SendNegativeResponse(UDS_SID_WRITE_DID, UDS_NRC_GENERAL_PROGRAMMING_FAILURE);
        UART_SendString("[OTA ] CRC FAIL\r\n");
        return;
    }

    /* CRC 一致，校验通过 */
    UART_SendString("[OTA ] CRC OK\r\n");

    /* 电压监控：提交前最后确认一次供电。欠压就不写描述表——
     * 内部正本和外部副本都不动，下次上电还是跑原来的固件 */
    if (AdcMon_IsCritical()) {
        UART_SendString("[OTA ] commit rejected: supply undervoltage\r\n");
        UDS_SendNegativeResponse(UDS_SID_WRITE_DID, UDS_NRC_CONDITIONS_NOT_CORRECT);
        return;
    }

    /* 提交升级：写描述表 + 翻转激活槽（原子切换） */
    UART_SendString("[OTA ] committing partition table ...\r\n");
    if (Partition_CommitUpdate(actualCrc, downloadTotalSize) != 0U) {
        UART_SendString("[OTA ] commit FAILED\r\n");
        UDS_SendNegativeResponse(UDS_SID_WRITE_DID, UDS_NRC_GENERAL_PROGRAMMING_FAILURE);
        return;
    }

    /* 清除升级标志 */
    FlashApp_EraseBootSector(UPGRADE_FLAG_ADDR);

    /* 标记本会话已提交：后续 0x2E 重发只回响应、不再提交（幂等） */
    updateCommitted = 1U;

    /* 正响应：DID + status(0=pass) */
    uint8_t payload[3];
    payload[0] = (uint8_t)(did >> 8);
    payload[1] = (uint8_t)(did & 0xFFU);
    payload[2] = 0x00U;  /* status = 0 表示成功 */

    UDS_SendPositiveResponse(UDS_SID_WRITE_DID, payload, 3);

    /* 等待 CANoe Step 6 发送 0x11 ECU Reset 触发完整复位，
       然后 Bootloader 重启后跳转到新激活槽的 App */
    UART_SendString("[OTA ] committed -> wait for ECU reset\r\n");

    /* 不直接跳转！防止 UDS 上下文跳转时状态未清理干净导致卡死 */
}

/* ==================== 下载服务实现 ==================== */

/**
 * @brief  处理 0x34 请求下载
 * @note   请求格式：
 *         data[0] = PCI
 *         data[1] = 0x34 (SID)
 *         data[2] = 地址高字节
 *         data[3] = 地址次高字节
 *         data[4] = 地址次低字节
 *         data[5] = 地址低字节
 *         data[6] = 大小高字节
 *         data[7] = 大小低字节
 *         正响应：03 74 20 04
 */
static void UDS_HandleRequestDownload(const can_frame_t *frame)
{
    uint8_t pci     = frame->data[0];
    uint8_t dataLen = pci & 0x0FU;

    /* 校验长度：至少 7 字节（SID + 4字节地址 + 2字节大小） */
    if (dataLen < 7U) {
        UDS_SendNegativeResponse(UDS_SID_REQUEST_DOWNLOAD, UDS_NRC_INVALID_MESSAGE_LENGTH);
        return;
    }

    /* 安全检查：必须先通过 0x27 安全访问解锁，否则拒绝下载 */
    if (securityUnlocked != 1U) {
        UDS_SendNegativeResponse(UDS_SID_REQUEST_DOWNLOAD, UDS_NRC_SECURITY_ACCESS_DENIED);
        return;
    }

    /* 解析大小（2字节，大端）；地址忽略上位机值，强制使用非激活槽 */
    uint16_t size = ((uint16_t)frame->data[6] << 8) | frame->data[7];

    /* 目标地址 = 非激活槽基地址：
       新固件永远写到非激活槽，掉电/失败不破坏正在运行的固件 */
    uint32_t addr = Partition_GetInactiveBase();

    /* 大小校验：不能为 0，且"起点 + 长度"不能越过 App 区末尾。
     * 注意 size 只有 2 字节（最大 65535），所以"是否超过单槽 224KB"永远为假
     * （原来的 (uint32_t)size > SLOT_SIZE 是条编译期就能判定的死检查）。
     * 真正该守的不变量是"整段下载区间落在 [APP_FLASH_BASE, APP_FLASH_END) 内"，
     * 用减法比较避免 32 位加法回绕；将来协议改成 4 字节长度时这条检查依然有效。 */
    if ((size == 0U) || (addr >= APP_FLASH_END) ||
        ((uint32_t)size > (APP_FLASH_END - addr))) {
        UDS_SendNegativeResponse(UDS_SID_REQUEST_DOWNLOAD, UDS_NRC_REQUEST_OUT_OF_RANGE);
        return;
    }

    /* 电压监控（策略 A：失去保护就不动 Flash）：
     * 1) ADC 监控没起来 → 电压判断不可信，同样拒绝，不能"默认认为安全"
     * 2) 供电欠压 → 拒绝开始下载
     * 两条都放在擦除之前，拒绝时不会碰任何 Flash */
    if (!AdcMon_IsReady()) {
        UART_SendString("[OTA ] 0x34 rejected: ADC monitor not ready\r\n");
        UDS_SendNegativeResponse(UDS_SID_REQUEST_DOWNLOAD, UDS_NRC_CONDITIONS_NOT_CORRECT);
        return;
    }

    if (AdcMon_IsCritical()) {
        UART_SendString("[OTA ] 0x34 rejected: supply undervoltage\r\n");
        UDS_SendNegativeResponse(UDS_SID_REQUEST_DOWNLOAD, UDS_NRC_CONDITIONS_NOT_CORRECT);
        return;
    }

    /* 擦除目标槽 */
    uint8_t result = FlashApp_Erase(addr, (uint32_t)size);
    if (result != 0U) {
        UDS_SendNegativeResponse(UDS_SID_REQUEST_DOWNLOAD, UDS_NRC_GENERAL_PROGRAMMING_FAILURE);
        return;
    }

    /* 初始化下载状态 */
    downloadState    = DOWNLOAD_IN_PROGRESS;
    downloadStartAddr = addr;   /* 保存下载起始地址供 CRC 校验 */
    downloadAddr     = addr;
    downloadTotalSize = (uint32_t)size;
    blockCounter     = 1U;
    dataBufferIndex  = 0U;
    updateCommitted  = 0U;  /* 新下载会话，允许 0x2E 提交 */
    bootloaderActive = 1U;  /* 升级中，禁止跳 App */

    /* 构造正响应：数据格式标识 + 地址长度 + 每块最大字节数 */
    uint8_t payload[3];
    payload[0] = 0x20U;  /* 数据格式：不压缩 */
    payload[1] = 0x04U; /* 地址长度=4字节 */
    payload[2] = 0x04U; /* 每块最大数据长度=4字节 */

    UDS_SendPositiveResponse(UDS_SID_REQUEST_DOWNLOAD, payload, 3);
}

/**
 * @brief  处理 0x36 传输数据
 * @note   请求格式：
 *         data[0] = PCI
 *         data[1] = 0x36 (SID)
 *         data[2] = BlockNumber (序号，从1开始)
 *         data[3..6] = 4字节数据
 *         正响应：02 76 <BlockNumber>
 */
static void UDS_HandleTransferData(const can_frame_t *frame)
{
    uint8_t pci          = frame->data[0];
    uint8_t dataLen      = pci & 0x0FU;
    uint8_t recvBlockNum = frame->data[2];

    /* 检查是否在下载状态 */
    if (downloadState != DOWNLOAD_IN_PROGRESS) {
        UDS_SendNegativeResponse(UDS_SID_TRANSFER_DATA, UDS_NRC_REQUEST_SEQUENCE_ERROR);
        return;
    }

    /* 检查块序号是否正确。
     * 放在电压检查之前：上位机重发同一帧时序号已经不匹配，回 0x73 让它重同步
     * 就够了，不能因为一次重发就把整个下载判死。 */
    if (recvBlockNum != blockCounter) {
        UDS_SendNegativeResponse(UDS_SID_TRANSFER_DATA, UDS_NRC_WRONG_BLOCK_SEQUENCE);
        return;
    }

    /* 电压监控：刷写中途检测到供电欠压 → 立即中止本次下载，不提交，
     * 保持当前运行的固件不变（这是"刷到一半掉电"的演示触发点）。
     * ADC 监控没起来时电压判断不可信，同样中止。 */
    if (!AdcMon_IsReady()) {
        UART_SendString("[OTA ] download aborted: ADC monitor not ready\r\n");
        UDS_SendNegativeResponse(UDS_SID_TRANSFER_DATA, UDS_NRC_CONDITIONS_NOT_CORRECT);
        downloadState   = DOWNLOAD_IDLE;
        dataBufferIndex = 0U;
        bootloaderActive = 0U;
        return;
    }

    if (AdcMon_IsCritical()) {
        UART_SendString("[OTA ] download aborted: supply undervoltage\r\n");
        UDS_SendNegativeResponse(UDS_SID_TRANSFER_DATA, UDS_NRC_CONDITIONS_NOT_CORRECT);
        downloadState   = DOWNLOAD_IDLE;
        dataBufferIndex = 0U;
        /* 下载已中止，允许 S3 超时后自动跳回原 App（否则会一直卡在 Boot 里） */
        bootloaderActive = 0U;
        return;
    }

    /* 提取数据（data[3]开始，最多4字节） */
    /* 校验最小长度：至少 SID + BlockNumber = 2 字节 */
    if (dataLen < 2U) {
        UDS_SendNegativeResponse(UDS_SID_TRANSFER_DATA, UDS_NRC_INVALID_MESSAGE_LENGTH);
        return;
    }
    uint8_t recvDataLen = dataLen - 2U;  /* 减去 SID 和 BlockNumber */
    if (recvDataLen > 4U) {
        /* 每块最多 4 字节（0x34 响应里已声明）。超了就明确拒绝，
         * 不能静默截断——截断会让上位机的字节流整体错位，
         * 现场只能等到 0x2E 报 CRC 失败，根本查不出原因。 */
        UDS_SendNegativeResponse(UDS_SID_TRANSFER_DATA, UDS_NRC_INVALID_MESSAGE_LENGTH);
        return;
    }

    /* 越界保护：已写字节 + 缓存中字节 + 本帧字节 不得超过 0x34 声明的总长度。
     * 否则上位机多发数据（或 block 序号一直对）会把 downloadAddr 一路推过
     * 槽末尾，写穿非激活槽之后的 Flash 区域，且不可恢复。
     * 上限取"总长度向上对齐到 4"：CANoe 脚本按 ceil(size/4) 发块，最后一块
     * 会用填充字节凑满 4 字节，必须容忍；这样最多多写 3 字节，仍在已擦除的
     * 槽内，且 CRC 只覆盖 size 字节，不受影响。 */
    {
        uint32_t receivedSoFar = (downloadAddr - downloadStartAddr) + (uint32_t)dataBufferIndex;
        uint32_t allowedTotal  = (downloadTotalSize + 3U) & ~3U;

        if ((receivedSoFar >= allowedTotal) ||
            ((uint32_t)recvDataLen > (allowedTotal - receivedSoFar))) {
            UART_SendString("[OTA ] download overflow -> abort, restart from 0x34\r\n");
            UDS_SendNegativeResponse(UDS_SID_TRANSFER_DATA, UDS_NRC_REQUEST_OUT_OF_RANGE);
            downloadState   = DOWNLOAD_IDLE;  /* 中止本次下载，需重新 0x34 */
            dataBufferIndex = 0U;
            bootloaderActive = 0U;            /* 允许 S3 超时后回到原 App */
            return;
        }
    }

    /* 把数据放入缓存；每攒满 8 字节立刻写一次 Flash。
     * 注意必须"边填边判满"：若一次性拷完再判，遇到上位机发 3/5 字节的不齐帧，
     * dataBufferIndex 会累积到 8 以上，dataBuffer[8] 就被越界写（数组只有 8 字节）。 */
    for (uint8_t i = 0U; i < recvDataLen; i++) {
        dataBuffer[dataBufferIndex++] = frame->data[3U + i];

        if (dataBufferIndex >= 8U) {
            uint8_t result = FlashApp_Write(downloadAddr, dataBuffer, 8U);
            if (result != 0U) {
                UDS_SendNegativeResponse(UDS_SID_TRANSFER_DATA, UDS_NRC_GENERAL_PROGRAMMING_FAILURE);
                downloadState   = DOWNLOAD_IDLE;
                dataBufferIndex = 0U;
                bootloaderActive = 0U;        /* 允许 S3 超时后回到原 App */
                return;
            }
            downloadAddr += 8U;
            dataBufferIndex = 0U;
        }
    }

    /* 块序号 +1（自然溢出: 255 → 0 → 1 → 2 → ...） */
    blockCounter++;

    /* 回复正响应 */
    uint8_t payload[1];
    payload[0] = recvBlockNum;

    UDS_SendPositiveResponse(UDS_SID_TRANSFER_DATA, payload, 1);
}

/**
 * @brief  处理 0x37 退出传输
 * @note   如果缓存里还有不足 8 字节的数据，补 0xFF 后写入
 *         正响应：01 77
 */
static void UDS_HandleRequestTransferExit(const can_frame_t *frame)
{
    (void)frame;  /* 0x37 请求不需要额外参数 */

    /* 检查是否在下载状态 */
    if (downloadState != DOWNLOAD_IN_PROGRESS) {
        UDS_SendNegativeResponse(UDS_SID_REQUEST_TRANSFER_EXIT, UDS_NRC_REQUEST_SEQUENCE_ERROR);
        return;
    }

    /* 处理剩余数据（不足 8 字节，补 0xFF 到 8 字节写入） */
    if (dataBufferIndex > 0U) {
        for (uint8_t i = dataBufferIndex; i < 8U; i++) {
            dataBuffer[i] = 0xFFU;
        }
        uint8_t result = FlashApp_Write(downloadAddr, dataBuffer, 8U);
        if (result != 0U) {
            UDS_SendNegativeResponse(UDS_SID_REQUEST_TRANSFER_EXIT, UDS_NRC_GENERAL_PROGRAMMING_FAILURE);
            downloadState = DOWNLOAD_IDLE;
            return;
        }
    }

    /* 下载完成，回到空闲状态 */
    downloadState = DOWNLOAD_IDLE;
    bootloaderActive = 0U;  /* 允许跳 App */

    /* 回复正响应（无额外数据） */
    UDS_SendPositiveResponse(UDS_SID_REQUEST_TRANSFER_EXIT, NULL, 0);

    /* 升级完成，等待上位机发送 0x2E 做 CRC32 校验后再跳 App */
    UART_SendString("[OTA ] transfer done -> wait for CRC32 check\r\n");
}

/* ==================== 内部辅助函数 ==================== */

/**
 * @brief  发送 UDS 肯定响应
 * @param  sid:        原始 SID（函数内部自动 +0x40）
 * @param  payload:    响应负载（SID 之后的全部数据，含子功能/DID等）
 * @param  payloadLen: 负载长度
 *
 * @note   CAN 单帧格式：
 *         data[0] = PCI（低 4 位 = 数据长度 = 1 + payloadLen）
 *         data[1] = SID + 0x40
 *         data[2..] = payload
 */
static void UDS_SendPositiveResponse(uint8_t sid, const uint8_t *payload, uint8_t payloadLen)
{
    uint8_t txData[8] = {0, 0, 0, 0, 0, 0, 0, 0};

    /* 数据总长度 = SID(1) + payload */
    uint8_t dataLen = payloadLen + 1U;

    /* 单帧最多 7 字节数据（PCI 1 字节 + 数据 7 字节 = 8） */
    if (dataLen > 7U) {
        dataLen = 7U;
    }

    /* PCI：单帧，低 4 位 = 数据长度 */
    txData[0] = dataLen;

    /* SID + 0x40 = 响应 SID */
    txData[1] = sid + 0x40U;

    /* 拷贝 payload */
    for (uint8_t i = 0; i < payloadLen && (i + 2U) < 8U; i++) {
        txData[i + 2U] = payload[i];
    }

    /* 发送（DLC 固定 8，不足部分补零） */
    CAN_SendMessage(CAN_TX_ID, txData, 8);
}

/**
 * @brief  发送 UDS 否定响应
 * @param  sid: 原始 SID
 * @param  nrc: 否定响应码
 *
 * @note   格式：03 7F <SID> <NRC>
 */
static void UDS_SendNegativeResponse(uint8_t sid, uint8_t nrc)
{
    uint8_t txData[8] = {0, 0, 0, 0, 0, 0, 0, 0};

    txData[0] = 0x03U;   /* PCI：3 字节数据 */
    txData[1] = 0x7FU;   /* 否定响应标记 */
    txData[2] = sid;      /* 原 SID */
    txData[3] = nrc;      /* 错误码 */

    CAN_SendMessage(CAN_TX_ID, txData, 8);
}

/* ==================== 超时与跳转 ==================== */

/**
 * @brief  UDS 定时器 tick（主循环每次调用递减 1ms）
 * @note   超时后自动跳转 App
 */
void UDS_Tick(void)
{
    if (s3ServerTimer > 0U) {
        s3ServerTimer--;
        if (s3ServerTimer == 0U) {
            /* 超时了，如果不在升级中就跳 App */
            if (!bootloaderActive) {
                UART_SendString("[UDS ] S3 timeout -> check App\r\n");
                Jump_To_App(Partition_GetActiveBase());
            } else {
                /* 升级中，延长超时 */
                UART_SendString("[UDS ] S3 timeout, download in progress\r\n");
                s3ServerTimer = S3_SERVER_TIMEOUT_MS;
            }
        }
    }
}

/**
 * @brief  跳转到 App
 * @note   1. 检查 App 栈顶是否合法（SRAM 范围内）
 *         2. 检查 App 复位向量是否合法（App Flash 范围内）
 *         3. 反初始化 CAN/UART 外设，防止残留中断在 App 侧触发 HardFault
 *         4. 重定向向量表到 App 地址
 *         5. 设置主栈指针为 App 栈顶
 *         6. 跳转到 App 的 Reset_Handler
 *         不会返回
 *
 *         两条调用路径统一在此处做 Deinit：
 *           - 上电首次跳（main.c，CAN 未初始化，Deinit 内部 if 判空安全）
 *           - S3Server 5 秒超时跳（UDS_Tick，CAN/UART 已初始化）
 *
 *         注：0x2E CRC 通过后不直接调用本函数，而是等上位机发 0x11 复位，
 *         让 Boot 走正常启动流程再跳，避免带着 UDS 上下文跳转。
 */
void Jump_To_App(uint32_t slotBase)
{
    /* 1. 读取 App 的栈顶地址和复位向量 */
    uint32_t appMsp = *(volatile uint32_t *)slotBase;
    uint32_t appPc  = *(volatile uint32_t *)(slotBase + 4U);

    /* 2. 合法性检查：
     *    - MSP 必须在 SRAM 范围内 (S32K144: 0x20000000 ~ 0x20006FFF，
     *      栈顶 __StackTop = 0x20007000，初始 MSP 就等于这个值)
     *    - PC  必须在当前槽范围内 (slotBase ~ slotBase+SLOT_SIZE)
     *    任一不通过就认为是非法 App（比如残留测试数据）
     */
    if ((appMsp == 0xFFFFFFFFU) || (appMsp == 0x00000000U) ||
        (appMsp < 0x20000000U) || (appMsp > 0x20007000U) ||
        (appPc  < slotBase) || (appPc >= (slotBase + SLOT_SIZE))) {
        UART_SendString("[ERR ] no valid App -> stay in bootloader\r\n");
        /* 没有 App，重新开始倒计时 */
        s3ServerTimer = S3_SERVER_TIMEOUT_MS;
        /* 确保中断重新打开（如果之前被关过） */
        INT_SYS_EnableIRQGlobal();
        return;
    }

    /* 3. 发送跳转提示（必须在 UART_Deinit 之前打印完） */
    UART_SendString("[BOOT] jump -> App\r\n");

    /* 4. 反初始化 CAN/UART 外设：
     *    - 先关 CAN：FlexCAN2 残留的接收中断会在 App 侧触发 HardFault
     *    - 再关 UART：LPUART1 残留的 RDRF/TC 中断同理
     *    Deinit 内部有 if(initialized) 保护，未初始化时是空操作
     */
    CAN_Deinit();
    UART_Deinit();

    /* 5. 关闭全局中断，防止跳转后旧中断触发 */
    INT_SYS_DisableIRQGlobal();

    /* 6. 重定向向量表到 App 槽地址 */
    S32_SCB->VTOR = slotBase;

    /* 7. 设置主栈指针为 App 栈顶 */
    __set_MSP(appMsp);

    /* 8. 跳转到 App 的 Reset_Handler（不会返回） */
    ((void (*)(void))appPc)();

    /* 不会执行到这里 */
}
