/**
 * @file    adc_mon.c
 * @brief   ADC 电压监控实现
 * @details 轮询方式采集 ADC0 通道 2，8 点滑动平均，3 次去抖后切换状态。
 *          不使用中断（通道配置里 interruptEnable = false）。
 */

#include "adc_mon.h"
#include "peripherals_adc_config_1.h"
#include "adc_driver.h"
#include "uart.h"

/* ==================== 内部状态 ==================== */

static uint16_t s_raw;                                  /* 最近一次原始值 */
static uint32_t s_mv;                                   /* 最近一次平均电压 */
static uint16_t s_buf[ADC_MON_AVG_SIZE];                /* 滑动平均环形缓冲 */
static uint8_t  s_idx;                                  /* 环形缓冲写指针 */
static uint8_t  s_filled;                               /* 已填充个数 */
static bool     s_inited;                               /* 初始化完成标志 */

static adc_mon_state_t s_state         = ADC_MON_STATE_UNKNOWN;
static adc_mon_state_t s_candidate     = ADC_MON_STATE_UNKNOWN;
static uint8_t         s_candidateCnt  = 0U;

/* ==================== 内部工具 ==================== */

/**
 * @brief  十进制打印（无 printf，避免引入库）
 */
static void AdcMon_PrintU32(uint32_t value)
{
    char tmp[12];
    uint8_t i = 0U;

    if (value == 0U) {
        UART_SendString("0");
        return;
    }

    while ((value > 0U) && (i < (uint8_t)(sizeof(tmp) - 1U))) {
        tmp[i] = (char)('0' + (value % 10U));
        value /= 10U;
        i++;
    }

    while (i > 0U) {
        i--;
        UART_SendData((const uint8_t *)&tmp[i], 1U);
    }
}

/**
 * @brief  等待转换完成（带计数上限，防止死等）
 */
static bool AdcMon_WaitConvDone(uint32_t limit)
{
    uint32_t n = 0U;

    while (ADC_DRV_GetConvCompleteFlag(ADC_MON_INSTANCE, ADC_MON_CHAN_INDEX) == false) {
        n++;
        if (n > limit) {
            return false;
        }
    }

    return true;
}

/* ==================== 对外接口 ==================== */

bool AdcMon_Init(void)
{
    uint8_t i;

    /* 复位 → 写转换器配置 → 自校准 */
    ADC_DRV_Reset(ADC_MON_INSTANCE);
    ADC_DRV_ConfigConverter(ADC_MON_INSTANCE, &adc_config_1_ConvConfig0);
    ADC_DRV_AutoCalibration(ADC_MON_INSTANCE);

    /* 软件触发模式下，写控制通道即启动一次转换 */
    ADC_DRV_ConfigChan(ADC_MON_INSTANCE, ADC_MON_CHAN_INDEX, &adc_config_1_ChnConfig0);

    if (!AdcMon_WaitConvDone(ADC_MON_TIMEOUT_LOOPS)) {
        UART_SendString("[ADC] conversion timeout (check ADC0 clock / PTA6)\r\n");
        return false;
    }

    ADC_DRV_GetChanResult(ADC_MON_INSTANCE, ADC_MON_CHAN_INDEX, &s_raw);
    s_mv     = AdcMon_RawToMillivolt(s_raw);

    /* 用首次采样预填滑动窗口：这样第一次 AdcMon_Print() 就能给出真实状态，
       不会出现 state=UNKNOWN */
    for (i = 0U; i < ADC_MON_AVG_SIZE; i++) {
        s_buf[i] = s_raw;
    }
    s_filled = ADC_MON_AVG_SIZE;
    s_idx    = 0U;

    if (s_mv < ADC_MON_CRITICAL_MV) {
        s_state = ADC_MON_STATE_CRITICAL;
    } else if (s_mv < ADC_MON_WARN_MV) {
        s_state = ADC_MON_STATE_WARN;
    } else {
        s_state = ADC_MON_STATE_OK;
    }

    s_inited = true;

    return true;
}

uint16_t AdcMon_ReadRaw(void)
{
    uint16_t raw = 0U;

    /* 触发下一次转换 */
    ADC_DRV_ConfigChan(ADC_MON_INSTANCE, ADC_MON_CHAN_INDEX, &adc_config_1_ChnConfig0);

    if (!AdcMon_WaitConvDone(ADC_MON_TIMEOUT_LOOPS)) {
        /* 超时按满量程处理：宁可认为电压正常，也不要误触发保护 */
        return (uint16_t)ADC_MON_ADC_MAX;
    }

    ADC_DRV_GetChanResult(ADC_MON_INSTANCE, ADC_MON_CHAN_INDEX, &raw);

    return raw;
}

uint32_t AdcMon_RawToMillivolt(uint16_t raw)
{
    return ((uint32_t)raw * ADC_MON_VREF_MV) / ADC_MON_ADC_MAX;
}

void AdcMon_Update(void)
{
    uint32_t sum = 0U;
    uint32_t avgRaw;
    uint8_t  i;
    adc_mon_state_t next;

    if (!s_inited) {
        return;
    }

    /* 1. 采样并放入环形缓冲 */
    s_raw = AdcMon_ReadRaw();
    s_buf[s_idx] = s_raw;
    s_idx = (uint8_t)((s_idx + 1U) % ADC_MON_AVG_SIZE);
    if (s_filled < ADC_MON_AVG_SIZE) {
        s_filled++;
    }

    /* 2. 求平均 */
    for (i = 0U; i < s_filled; i++) {
        sum += s_buf[i];
    }
    avgRaw = sum / s_filled;
    s_mv   = AdcMon_RawToMillivolt((uint16_t)avgRaw);

    /* 3. 判定档位 */
    if (s_mv < ADC_MON_CRITICAL_MV) {
        next = ADC_MON_STATE_CRITICAL;
    } else if (s_mv < ADC_MON_WARN_MV) {
        next = ADC_MON_STATE_WARN;
    } else {
        next = ADC_MON_STATE_OK;
    }

    /* 4. 去抖：连续 N 次同结果才真正切换 */
    if (next == s_state) {
        s_candidateCnt = 0U;
        return;
    }

    if (next == s_candidate) {
        if (s_candidateCnt < 255U) {
            s_candidateCnt++;
        }
    } else {
        s_candidate     = next;
        s_candidateCnt  = 1U;
    }

    /* 首次采样直接生效，之后需要连续 N 次 */
    if ((s_state == ADC_MON_STATE_UNKNOWN) || (s_candidateCnt >= ADC_MON_DEBOUNCE_COUNT)) {
        s_state        = next;
        s_candidateCnt = 0U;
        AdcMon_Print();
    }
}

uint32_t AdcMon_GetMillivolt(void)
{
    return s_mv;
}

uint16_t AdcMon_GetRaw(void)
{
    return s_raw;
}

adc_mon_state_t AdcMon_GetState(void)
{
    return s_state;
}

bool AdcMon_IsCritical(void)
{
    return (s_state == ADC_MON_STATE_CRITICAL);
}

const char *AdcMon_StateName(adc_mon_state_t state)
{
    switch (state) {
        case ADC_MON_STATE_OK:       return "OK";
        case ADC_MON_STATE_WARN:     return "WARN";
        case ADC_MON_STATE_CRITICAL: return "CRITICAL";
        default:                     return "UNKNOWN";
    }
}

void AdcMon_Print(void)
{
    UART_SendString("[ADC] CH2 raw=");
    AdcMon_PrintU32((uint32_t)s_raw);
    UART_SendString(" mv=");
    AdcMon_PrintU32(s_mv);
    UART_SendString(" state=");
    UART_SendString(AdcMon_StateName(s_state));
    UART_SendString("\r\n");
}