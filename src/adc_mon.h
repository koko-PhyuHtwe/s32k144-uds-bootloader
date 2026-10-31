/**
 * @file    adc_mon.h
 * @brief   ADC 电压监控（演示用）
 * @details 用 ADC0 通道 2（PTA6 / ADC0-2 接线柱）采集模拟电压，
 *          经滑动平均后判断"正常 / 警告 / 危险"三档。
 *
 *          演示时把板载电位器拧到 3.3V 位置当作"正常供电"，
 *          刷写过程中往下拧模拟"供电跌落"，触发保护。
 *
 *          注意：这是模拟电压，不是真的在测 12V 供电。
 *          真做产品时把电位器换成 12V 分压电阻，软件只改阈值口径。
 */

#ifndef ADC_MON_H
#define ADC_MON_H

#include <stdint.h>
#include <stdbool.h>

/* ==================== 配置参数 ==================== */

#define ADC_MON_INSTANCE        0U          /* ADC0 */
#define ADC_MON_CHAN_INDEX      0U          /* 硬件控制通道索引（用 0 号即可） */

#define ADC_MON_VREF_MV         3300U       /* 参考电压 VrefH = 3.3V */
#define ADC_MON_ADC_MAX         4095U       /* 12 位满量程 */

#define ADC_MON_AVG_SIZE        8U          /* 滑动平均点数 */
#define ADC_MON_TIMEOUT_LOOPS   200000U     /* 转换等待上限（纯计数，防死等） */

#define ADC_MON_WARN_MV         3100U       /* 低于此值 = 警告 */
#define ADC_MON_CRITICAL_MV     2900U       /* 低于此值 = 危险，拒绝擦写 */
#define ADC_MON_DEBOUNCE_COUNT  3U          /* 连续几次判定才切换状态 */

/* ==================== 电压状态 ==================== */

typedef enum
{
    ADC_MON_STATE_UNKNOWN = 0,  /* 还没采过样 */
    ADC_MON_STATE_OK,           /* 电压正常 */
    ADC_MON_STATE_WARN,         /* 偏低，允许但告警 */
    ADC_MON_STATE_CRITICAL      /* 过低，禁止擦写 Flash */
} adc_mon_state_t;

/* ==================== 接口 ==================== */

/**
 * @brief  初始化 ADC（复位、配置转换器、自校准、首次转换）
 * @return true=初始化成功
 */
bool AdcMon_Init(void);

/**
 * @brief  读一次原始值（0~4095）
 * @note   转换超时返回满量程（当作"电压正常"），避免误触发保护
 */
uint16_t AdcMon_ReadRaw(void);

/**
 * @brief  原始值换算成毫伏
 */
uint32_t AdcMon_RawToMillivolt(uint16_t raw);

/**
 * @brief  周期调用（建议 10ms 一次）：采样 + 滑动平均 + 状态判断
 * @note   状态发生切换时才打印
 */
void AdcMon_Update(void);

/**
 * @brief  取最近一次的平均电压（毫伏）
 */
uint32_t AdcMon_GetMillivolt(void);

/**
 * @brief  取最近一次的原始值
 */
uint16_t AdcMon_GetRaw(void);

/**
 * @brief  取当前电压状态
 */
adc_mon_state_t AdcMon_GetState(void);

/**
 * @brief  监控是否可用（ADC 初始化是否成功）
 * @note   返回 false 时"电压是否危险"的判断不可信（s_state 会一直停在
 *         UNKNOWN），上层必须拒绝擦写 Flash——宁可刷不进去，也不能在
 *         失去保护的情况下动 Flash。
 */
bool AdcMon_IsReady(void);

/**
 * @brief  电压是否危险（危险时上层应拒绝擦写 Flash）
 */
bool AdcMon_IsCritical(void);

/**
 * @brief  状态名（"OK" / "WARN" / "CRITICAL" / "UNKNOWN"）
 */
const char *AdcMon_StateName(adc_mon_state_t state);

/**
 * @brief  打印当前电压状态（串口）
 */
void AdcMon_Print(void);

#endif /* ADC_MON_H */