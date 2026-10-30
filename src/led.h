/**
 * @file    led.h
 * @brief   LED 驱动头文件
 * @details 基于 S32K144 的 LED 控制模块，支持初始化、点亮、熄灭和翻转操作
 */

#ifndef LED_H
#define LED_H

#include "sdk_project_config.h"

/* LED 引脚定义 */
#define LED0_PORT   PTD
#define LED0_PIN    15
#define LED1_PORT   PTD
#define LED1_PIN    16

/**
 * @brief  LED 初始化
 * @note   上电把两颗 LED 都熄灭（LED 为低电平点亮，极性在 led.c 里配置）
 */
void LED_Init(void);

/**
 * @brief  点亮指定 LED
 * @param  led_pin: LED 引脚 (LED0_PIN 或 LED1_PIN)
 */
void LED_TurnOn(uint8_t led_pin);

/**
 * @brief  熄灭指定 LED
 * @param  led_pin: LED 引脚 (LED0_PIN 或 LED1_PIN)
 */
void LED_TurnOff(uint8_t led_pin);

/**
 * @brief  翻转指定 LED 状态
 * @param  led_pin: LED 引脚 (LED0_PIN 或 LED1_PIN)
 */
void LED_Toggle(uint8_t led_pin);

/**
 * @brief  同时翻转两个 LED（用于状态指示）
 */
void LED_ToggleBoth(void);

#endif /* LED_H */
