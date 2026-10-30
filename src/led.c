/**
 * @file    led.c
 * @brief   LED 驱动源文件
 * @details 基于 S32K144 GPIO 的 LED 控制实现
 * @note    实物 LED 接法是 3V3 → LED → 限流电阻 → GPIO，
 *          所以是 **低电平点亮、高电平熄灭**（不是高电平有效！）。
 *          原来这里按高电平有效写，导致 LED_TurnOff() 反而把灯点亮，
 *          表现为"该灭的那颗一直亮着"。现在统一由 LED_ON_LEVEL 控制。
 */

#include "led.h"

/* ==================== 极性配置 ==================== */

/* 点亮 LED 时输出引脚的电平：0 = 低电平点亮（本板实物接法）
 * 如果换了板子或改了接线导致相反，把这里改成 1U 即可。 */
#define LED_ON_LEVEL    0U
#define LED_OFF_LEVEL   1U

/* ==================== 公共函数实现 ==================== */

/**
 * @brief  LED 初始化
 * @note   上电先把两颗灯都熄灭，具体亮哪颗由应用层决定
 */
void LED_Init(void)
{
    PINS_DRV_WritePin(LED0_PORT, LED0_PIN, LED_OFF_LEVEL);
    PINS_DRV_WritePin(LED1_PORT, LED1_PIN, LED_OFF_LEVEL);
}

/**
 * @brief  点亮指定 LED
 * @param  led_pin: LED 引脚号
 */
void LED_TurnOn(uint8_t led_pin)
{
    if (led_pin == LED0_PIN)
    {
        PINS_DRV_WritePin(LED0_PORT, LED0_PIN, LED_ON_LEVEL);
    }
    else if (led_pin == LED1_PIN)
    {
        PINS_DRV_WritePin(LED1_PORT, LED1_PIN, LED_ON_LEVEL);
    }
}

/**
 * @brief  熄灭指定 LED
 * @param  led_pin: LED 引脚号
 */
void LED_TurnOff(uint8_t led_pin)
{
    if (led_pin == LED0_PIN)
    {
        PINS_DRV_WritePin(LED0_PORT, LED0_PIN, LED_OFF_LEVEL);
    }
    else if (led_pin == LED1_PIN)
    {
        PINS_DRV_WritePin(LED1_PORT, LED1_PIN, LED_OFF_LEVEL);
    }
}

/**
 * @brief  翻转指定 LED 状态
 * @param  led_pin: LED 引脚号
 * @note   翻转与极性无关
 */
void LED_Toggle(uint8_t led_pin)
{
    if (led_pin == LED0_PIN)
    {
        PINS_DRV_TogglePins(LED0_PORT, 1u << LED0_PIN);
    }
    else if (led_pin == LED1_PIN)
    {
        PINS_DRV_TogglePins(LED1_PORT, 1u << LED1_PIN);
    }
}

/**
 * @brief  同时翻转两个 LED
 * @note   用于 Bootloader 主循环中的心跳指示
 */
void LED_ToggleBoth(void)
{
    PINS_DRV_TogglePins(LED0_PORT, 1u << LED0_PIN);
    PINS_DRV_TogglePins(LED1_PORT, 1u << LED1_PIN);
}