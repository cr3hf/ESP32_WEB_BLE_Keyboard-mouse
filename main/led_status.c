/*
 * LED 状态指示实现
 *
 * ESP32-S3 SuperMini：GPIO48 同时接“板载 LED（电平驱动，高电平亮）”与
 * “WS2818 RGB（单总线归零码）”。两者共用一个脚，无法同时正确驱动，
 * 因此用 USE_RGB_STATUS 选择主模式（见 led_status.h）：
 *
 *   USE_RGB_STATUS = 0（默认，用板载 LED）：
 *     - GPIO48 普通电平控制板载 LED（运行=亮，停止=灭）
 *     - 完全不驱动 WS2818 → RGB 熄灭
 *     - GPIO1 普通 LED 同步（低电平亮）
 *
 *   USE_RGB_STATUS = 1（用 WS2818 RGB）：
 *     - GPIO48 归零码驱动 WS2818（运行=绿低亮，停止=灭）
 *     - 板载 LED 状态下空闲电平拉低使其熄灭
 *     - GPIO1 普通 LED 同步
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"

#include "led_status.h"

static const char *TAG = "LED_STATUS";

static bool s_led_initialized = false;
static bool s_led_on         = false;  /* 当前稳态：运行=true / 停止=false */

#if USE_RGB_STATUS
/* ---- WS2818 比特级时序（基于 160MHz 估算，含 GPIO 操作开销的保守值） ---- */
#define T1H_CYC 90   /* T1H≈0.80us */
#define T0H_CYC 45   /* T0H≈0.40us */
#define BIT_TOTAL_CYC 200

static inline void ws_delay_cyc(unsigned cyc)
{
    if (cyc < 12) {
        cyc = 0;
    } else {
        cyc -= 12;
    }
    for (volatile unsigned i = 0; i < cyc; i++) {
        __asm__ __volatile__("nop");
    }
}

static inline void ws_send_bit(int bit)
{
    gpio_set_level(LED_RGB_GPIO, 1);
    ws_delay_cyc(bit ? T1H_CYC : T0H_CYC);
    gpio_set_level(LED_RGB_GPIO, 0);
    ws_delay_cyc(BIT_TOTAL_CYC - (bit ? T1H_CYC : T0H_CYC));
}

static void ws_send_grb(uint8_t g, uint8_t r, uint8_t b)
{
    for (int i = 7; i >= 0; i--) ws_send_bit((g >> i) & 0x1);
    for (int i = 7; i >= 0; i--) ws_send_bit((r >> i) & 0x1);
    for (int i = 7; i >= 0; i--) ws_send_bit((b >> i) & 0x1);
}

/* 帧末拉低并保持 >50us，使 WS2818 锁存且板载 LED（高电平亮）熄灭 */
static void ws_latch_low(void)
{
    gpio_set_level(LED_RGB_GPIO, 0);
    ws_delay_cyc(8000);
}

static void ws_refresh(void)
{
    if (!s_led_initialized) {
        return;
    }
    portDISABLE_INTERRUPTS();
    if (s_led_on) {
        ws_send_grb(LED_RGB_G, LED_RGB_R, LED_RGB_B);
    } else {
        ws_send_grb(0, 0, 0);
    }
    ws_latch_low();   /* 空闲低电平：WS2818 锁存，板载 LED 灭 */
    portENABLE_INTERRUPTS();
}
#endif /* USE_RGB_STATUS */

esp_err_t led_status_init(void)
{
    /* 普通 LED(GPIO1)：推挽输出，初始灭（高电平） */
    gpio_reset_pin(LED_STATUS_GPIO);
    ESP_RETURN_ON_ERROR(gpio_set_direction(LED_STATUS_GPIO, GPIO_MODE_OUTPUT), TAG,
                        "LED GPIO(%d) 方向配置失败", LED_STATUS_GPIO);
    gpio_set_level(LED_STATUS_GPIO, LED1_OFF_LEVEL);

#if USE_RGB_STATUS
    /* 用 WS2818 RGB：GPIO48 推挽输出，初始空闲低电平（板载 LED 灭） */
    gpio_reset_pin(LED_RGB_GPIO);
    ESP_RETURN_ON_ERROR(gpio_set_direction(LED_RGB_GPIO, GPIO_MODE_OUTPUT), TAG,
                        "WS2818 GPIO(%d) 方向配置失败", LED_RGB_GPIO);
    gpio_set_level(LED_RGB_GPIO, 0);
    s_led_on = false;
    ws_refresh();
#else
    /* 用板载 LED：GPIO48 普通电平驱动，运行=亮，停止=灭；不动 WS2818 → RGB 熄灭 */
    gpio_reset_pin(LED_RGB_GPIO);
    ESP_RETURN_ON_ERROR(gpio_set_direction(LED_RGB_GPIO, GPIO_MODE_OUTPUT), TAG,
                        "板载 LED GPIO(%d) 方向配置失败", LED_RGB_GPIO);
    gpio_set_level(LED_RGB_GPIO, BOARD_LED_OFF_LEVEL);  /* 灭 */
    s_led_on = false;
#endif

    s_led_initialized = true;
    ESP_LOGI(TAG, "状态 LED 已初始化 (主模式=%s, GPIO%d 普通LED, GPIO%d 共用脚)",
             USE_RGB_STATUS ? "RGB" : "LED", LED_STATUS_GPIO, LED_RGB_GPIO);
    return ESP_OK;
}

void led_status_set(bool on)
{
    if (!s_led_initialized) {
        return;
    }
    if (on == s_led_on) {
        return;  /* 同状态跳过，避免动作循环中高频刷灯 */
    }
    s_led_on = on;

#if USE_RGB_STATUS
    ws_refresh();
    gpio_set_level(LED_STATUS_GPIO, on ? LED1_ON_LEVEL : LED1_OFF_LEVEL);
#else
    gpio_set_level(LED_RGB_GPIO, on ? BOARD_LED_ON_LEVEL : BOARD_LED_OFF_LEVEL);
    gpio_set_level(LED_STATUS_GPIO, on ? LED1_ON_LEVEL : LED1_OFF_LEVEL);
#endif
}

void led_status_blink(void)
{
    if (!s_led_initialized) {
        return;
    }
#if USE_RGB_STATUS
    portDISABLE_INTERRUPTS();
    ws_send_grb(LED_RGB_G, LED_RGB_R, LED_RGB_B);
    ws_latch_low();
    portENABLE_INTERRUPTS();
    gpio_set_level(LED_STATUS_GPIO, LED1_ON_LEVEL);
    vTaskDelay(pdMS_TO_TICKS(150));
    ws_refresh();
    gpio_set_level(LED_STATUS_GPIO, s_led_on ? LED1_ON_LEVEL : LED1_OFF_LEVEL);
#else
    gpio_set_level(LED_RGB_GPIO, BOARD_LED_ON_LEVEL);
    gpio_set_level(LED_STATUS_GPIO, LED1_ON_LEVEL);
    vTaskDelay(pdMS_TO_TICKS(150));
    gpio_set_level(LED_RGB_GPIO, s_led_on ? BOARD_LED_ON_LEVEL : BOARD_LED_OFF_LEVEL);
    gpio_set_level(LED_STATUS_GPIO, s_led_on ? LED1_ON_LEVEL : LED1_OFF_LEVEL);
#endif
}
