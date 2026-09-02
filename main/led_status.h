/*
 * LED 状态指示
 *
 * 板载/外接 RGB 灯：WS2818（单总线归零码，时序同 WS2812）。
 *   - 数据脚默认 GPIO48（ESP32-S3 SuperMini 板载 RGB 公用脚）；
 *     若实际 DIN 接其它 GPIO，请修改下方的 LED_RGB_GPIO。
 *   - 空闲态保持高电平，避免 WS2818 持续收 0 码常亮白。
 *
 * 兼容指示：GPIO1 普通 LED（低电平亮 / 高电平灭），与 RGB 同步。
 *
 * 状态语义：
 *   - 运行   ：绿色（低亮度）
 *   - 停止   ：熄灭
 *   - 闪烁   ：绿色闪一下
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LED_STATUS_GPIO 1          /* 普通 LED（板载），低电平亮 / 高电平灭 */
#define LED_RGB_GPIO    48         /* GPIO48 同时接板载 LED 与 WS2818(DIN) */

/* 状态指示主模式：
 *   1 = 用 WS2818 RGB（运行=绿低亮，停止=灭）。注：GPIO48 在 Quad PSRAM 下为 VDD_SPI 脚，
 *       实测无法稳定驱动 WS2818 归零码，本板建议用 LED 模式。
 *   0 = 用板载 LED（运行=亮，停止=灭）；完全不驱动 WS2818，使其熄灭（默认）
 */
#define USE_RGB_STATUS  0

#define LED_RGB_R       0          /* 运行色：绿（低亮度），R=0 */
#define LED_RGB_G       40         /* 亮度调低（0~255，原满亮太刺眼） */
#define LED_RGB_B       0

/* GPIO1 普通 LED：低电平亮 / 高电平灭 */
#define LED1_ON_LEVEL   0
#define LED1_OFF_LEVEL  1
/* GPIO48 板载 LED：实测为高电平亮 / 低电平灭 */
#define BOARD_LED_ON_LEVEL  1
#define BOARD_LED_OFF_LEVEL 0

/**
 * @brief 初始化状态 LED（GPIO1 普通 LED + GPIO48 板载 LED），初始为灭/关闭
 */
esp_err_t led_status_init(void);

/**
 * @brief 设置 LED 状态
 *
 * @param on true=点亮/运行，false=熄灭/停止
 */
void led_status_set(bool on);

/**
 * @brief 闪烁一次（用于确认类反馈，如 +10 分钟）。
 */
void led_status_blink(void);

#ifdef __cplusplus
}
#endif
