#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "driver/gpio.h"

#include "hid_keys.h"
#include "ble_hid.h"
#include "xl9555.h"
#include "action_engine.h"
#include "web_server.h"

static const char *TAG = "KEYS";

/* ============== BOOT 键（GPIO0） ==============
 * 板载 BOOT 键接 GPIO0（strapping 引脚，仅复位瞬间采样启动模式；
 * 运行期当作普通输入读取，长按不会进下载模式）。低电平=按下。
 * 功能：短按 → 切换动作启停（单键切换，与 KEY2/KEY0 并存不冲突）；
 *       长按 ≥3s → 重置蓝牙；若“上一次长按松开后 5s 内”再次长按 ≥3s → 恢复出厂（账号+参数，
 *       与 KEY1+KEY3 组合键效果一致）。 */
#define BOOT_KEY_GPIO       GPIO_NUM_0
#define BOOT_LONGPRESS_US   (3000ULL * 1000)   /* 长按 3 秒触发蓝牙重置 */

#define STARTUP_GUARD_US    (1000ULL * 1000)   /* 上电 1 秒后允许按键触发（XL9555 上电 <300ms 即稳，留 3 倍余量） */

static bool     s_boot_last_pressed = false;
static int64_t  s_boot_press_start  = 0;
static bool     s_boot_long_fired   = false;
static int64_t  s_boot_scan_start   = 0;

/* ===== 长按复位的两段式逻辑 =====
 * 单次长按：仅重置蓝牙（断开主机/清配对/换地址/重新广播）。
 * 若“上一次长按松开后 5 秒内”再次按下并满足长按（>3s），则判定为整机出厂复位：
 *   恢复 Web 登录账号为 admin/admin，并将全部参数恢复默认并落盘。
 * 用 release 时间戳 + pending 标志实现，避免误触全盘清空。 */
#define FACTORY_WINDOW_US   (5ULL * 1000 * 1000)
static int64_t  s_arm_release_us  = 0;   /* 上一次长按松开的时刻，用于判定 5s 窗口 */
static bool     s_factory_pending = false;

/* 长按松开：若本次是长按（已触发蓝牙重置），记录松开时刻以开启 5s 窗口 */
static void on_longpress_release(int64_t now, bool fired)
{
    if (fired) {
        s_arm_release_us = now;
    }
}

/* 新一次按下：若处于 5s 窗口内，标记“下一次长按将恢复出厂” */
static void on_press_start(int64_t now)
{
    if (s_arm_release_us != 0) {
        if ((now - s_arm_release_us) <= (int64_t)FACTORY_WINDOW_US) {
            s_factory_pending = true;
        } else {
            s_arm_release_us = 0;   /* 窗口已过，撤销 */
        }
    }
}

/* 长按触发：先停动作并释放按键，再重置蓝牙；若为窗口内第二次长按则额外恢复出厂 */
static void on_longpress_fire(int64_t now)
{
    action_engine_stop_and_wait(2000);
    ble_hid_reset();
    if (s_factory_pending) {
        web_factory_reset();
        ESP_LOGW(TAG, "5s 内再次长按 → 恢复出厂（账号 admin/admin + 全部参数默认）");
        s_factory_pending = false;
        s_arm_release_us  = 0;
    } else {
        ESP_LOGI(TAG, "单次长按 → 仅重置蓝牙（松开后 5s 内再次长按可恢复出厂）");
    }
}

static void boot_key_monitor_task(void *arg)
{
    (void)arg;

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << BOOT_KEY_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,   /* GPIO0 内部上拉，按键接地 */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&io) != ESP_OK) {
        ESP_LOGE(TAG, "BOOT(GPIO0) 配置失败，禁用 BOOT 键功能");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "BOOT 键(GPIO0)监控已启动：短按切换启停 / 长按 3 秒重置蓝牙；松开后 5s 内再次长按恢复出厂(账号+参数)");

    while (1) {
        int64_t now = esp_timer_get_time();
        if (s_boot_scan_start == 0) {
            s_boot_scan_start = now;
        }

        /* 上电静默期：避免复位瞬间 BOOT 误触 */
        if ((now - s_boot_scan_start) < (int64_t)STARTUP_GUARD_US) {
            s_boot_last_pressed = false;
            s_boot_press_start  = 0;
            s_boot_long_fired   = false;
            vTaskDelay(pdMS_TO_TICKS(KEY_SCAN_PERIOD_MS));
            continue;
        }

        bool pressed = (gpio_get_level(BOOT_KEY_GPIO) == 0);  /* 低电平=按下 */

        if (pressed) {
            if (!s_boot_last_pressed) {
                s_boot_press_start = now;
                s_boot_long_fired  = false;
                on_press_start(now);
            } else if (!s_boot_long_fired &&
                       (now - s_boot_press_start) >= (int64_t)BOOT_LONGPRESS_US) {
                s_boot_long_fired = true;
                ESP_LOGW(TAG, "BOOT 键长按 3 秒 → 蓝牙重置（松开后 5s 内再次长按将恢复出厂）");
                on_longpress_fire(now);
            }
        } else {
            on_longpress_release(now, s_boot_long_fired);
            if (!s_boot_long_fired) s_factory_pending = false;
            /* 松开：若未按满长按时长，视为短按 → 切换启停 */
            if (s_boot_last_pressed && !s_boot_long_fired) {
                if (action_engine_is_running()) {
                    ESP_LOGI(TAG, "BOOT 键短按 → 切换为【停止】");
                    action_engine_stop();
                } else {
                    ESP_LOGI(TAG, "BOOT 键短按 → 切换为【启动】");
                    action_engine_start();
                }
            }
            s_boot_press_start = 0;
            s_boot_long_fired  = false;
        }

        s_boot_last_pressed = pressed;
        vTaskDelay(pdMS_TO_TICKS(KEY_SCAN_PERIOD_MS));
    }
}

/* 按键定义：Key0~Key3 对应 XL9555【第二组 Port1】的 P1_7~P1_4（低电平=按下）
 * 读回值 bit 位：Key0->bit3(P1_7), Key1->bit2(P1_6), Key2->bit1(P1_5), Key3->bit0(P1_4)
 */
static const char *s_key_name[KEY_NUM] = {
    "Key0(停止/右)", "Key1(重置下)", "Key2(启动/左)", "Key3(重置上)",
};

/* 每个按键在读回字节中的位（bit3=P1_7, bit2=P1_6, bit1=P1_5, bit0=P1_4） */
static const uint8_t s_key_bit[KEY_NUM] = { 3, 2, 1, 0 };

/* 边沿检测：记录上一周期各键的 pressed 状态 */
static bool s_last_pressed[KEY_NUM];

/* 组合键：Key1(下/P1_6) + Key3(上/P1_4) 同时按住 >3s → 重置蓝牙 */
static int64_t s_combo_start_us = 0;
static bool    s_combo_fired    = false;

/* 组合键成立时屏蔽 KEY0/KEY2 的边沿判定，防止重置过程中误启停 */
static bool s_combo_active = false;

/* 上电静默期：初始化完成后的 STARTUP_GUARD_US 内禁止触发任何按键动作，
 * 避开 XL9555 上电电平不稳/首帧误判。静默期内仍持续刷新 s_last_pressed，
 * 避免静默期结束后补触发一次“假下降沿”。 */
static int64_t s_scan_start_us = 0;

static void keys_scan_task(void *arg)
{
    (void)arg;

    while (1) {
        int64_t now = esp_timer_get_time();
        if (s_scan_start_us == 0) {
            s_scan_start_us = now;
        }

        uint8_t p4_p7 = 0x0F;
        if (xl9555_read_p4_p7(&p4_p7) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(KEY_SCAN_PERIOD_MS));
            continue;
        }

        /* 低电平=按下：p4_p7 的对应位为 0 表示按键按下 */
        bool pressed[KEY_NUM];
        for (int i = 0; i < KEY_NUM; i++) {
            pressed[i] = ((p4_p7 >> s_key_bit[i]) & 0x1) == 0;
        }

#if KEY_DEBUG_LEVEL
        static uint8_t s_last_raw = 0xFF;
        if (p4_p7 != s_last_raw) {
            s_last_raw = p4_p7;
            ESP_LOGI(TAG, "XL9555 P1_7~P1_4=0x%X -> STOP=%d RST_D=%d START=%d RST_U=%d",
                     p4_p7,
                     pressed[0] ? 1 : 0, pressed[1] ? 1 : 0,
                     pressed[2] ? 1 : 0, pressed[3] ? 1 : 0);
        }
#endif

        /* ---------------- 组合键：Key1 + Key3 长按 3 秒重置蓝牙 ---------------- */
        bool combo = pressed[1] && pressed[3];  /* 下 + 上 */
        if (combo) {
            if (s_combo_start_us == 0) {
                s_combo_start_us = now;
                s_combo_fired    = false;
                on_press_start(now);
                ESP_LOGI(TAG, "检测到【下+上】同时按下，保持 %d 秒可重置蓝牙",
                         KEY_COMBO_RESET_MS / 1000);
            } else if (!s_combo_fired &&
                       (now - s_combo_start_us) >= (int64_t)KEY_COMBO_RESET_MS * 1000) {
                s_combo_fired = true;
                ESP_LOGW(TAG, "【下+上】已保持 %d 秒，执行蓝牙重置（松开后 5s 内再次长按将恢复出厂）",
                         KEY_COMBO_RESET_MS / 1000);
                on_longpress_fire(now);
            }
        } else {
            on_longpress_release(now, s_combo_fired);
            if (!s_combo_fired) s_factory_pending = false;
            if (s_combo_start_us != 0 && !s_combo_fired) {
                ESP_LOGI(TAG, "【下+上】组合键取消（未按满 %d 秒）", KEY_COMBO_RESET_MS / 1000);
            }
            s_combo_start_us = 0;
            s_combo_fired    = false;
        }
        s_combo_active = combo;

        /* ---------------- 上电静默期（启动后 3 秒） ----------------
         * 初始化完成后的 STARTUP_GUARD_US 内禁止触发任何按键动作（启停/复位/加时/
         * 组合键重置），避开 XL9555 上电电平不稳、首帧误判。静默期内仍持续刷新
         * s_last_pressed，避免静默期结束后补触发一次“假下降沿”；同时清零组合键计时，
         * 防止静默期内按住组合键被误记时。 */
        if (s_scan_start_us != 0 && (now - s_scan_start_us) < (int64_t)STARTUP_GUARD_US) {
            for (int i = 0; i < KEY_NUM; i++) {
                s_last_pressed[i] = pressed[i];
            }
            s_combo_start_us = 0;
            s_combo_fired    = false;
            vTaskDelay(pdMS_TO_TICKS(KEY_SCAN_PERIOD_MS));
            continue;
        }

        /* ---------------- 启停边沿检测（组合键成立时屏蔽） ---------------- */
        if (!s_combo_active) {
            /* KEY2(bit1)：下降沿 → 启动 */
            if (pressed[2] && !s_last_pressed[2]) {
                ESP_LOGI(TAG, "%s 下降沿 → 启动动作引擎", s_key_name[2]);
                action_engine_start();
            }
            /* KEY0(bit3)：下降沿 → 停止 */
            if (pressed[0] && !s_last_pressed[0]) {
                ESP_LOGI(TAG, "%s 下降沿 → 停止动作引擎", s_key_name[0]);
                action_engine_stop();
            }
            /* KEY3(bit0)：下降沿 → 停止动作并复位鼠标（供确认落点，停止态同样执行） */
            if (pressed[3] && !s_last_pressed[3]) {
                ESP_LOGI(TAG, "%s 下降沿 → 停止并复位鼠标", s_key_name[3]);
                action_engine_stop_and_reset();
            }
        }

        /* ---------------- Key1 短按 → 增加运行时间 ----------------
         * 在 Key1 上升沿（松开）且本次按压未触发【下+上】组合键重置时生效，
         * 避免长按重置蓝牙时误累加运行额度。 */
        if (!pressed[1] && s_last_pressed[1] && !s_combo_fired) {
            ESP_LOGI(TAG, "%s 短按 → 增加运行时间（LED 闪烁确认）", s_key_name[1]);
            action_engine_add_runtime();
        }

        for (int i = 0; i < KEY_NUM; i++) {
            s_last_pressed[i] = pressed[i];
        }

        vTaskDelay(pdMS_TO_TICKS(KEY_SCAN_PERIOD_MS));
    }
}

esp_err_t keys_start(void)
{
    memset(s_last_pressed, 0, sizeof(s_last_pressed));
    s_combo_start_us = 0;
    s_combo_fired    = false;
    s_combo_active   = false;
    s_scan_start_us  = 0;   /* 重新进入上电静默期 */

    /* BOOT 键(GPIO0)监控任务：独立于 XL9555，即使板子未焊 XL9555 也能用。
     * 短按切换启停 / 长按 3 秒重置蓝牙。 */
    BaseType_t rb = xTaskCreate(boot_key_monitor_task, "boot_key", 3072, NULL,
                                KEY_TASK_PRIORITY, NULL);
    if (rb != pdPASS) {
        ESP_LOGE(TAG, "BOOT 键监控任务创建失败");
    }

    esp_err_t ret = xl9555_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "XL9555 初始化异常: %s", esp_err_to_name(ret));
        return ret;
    }

    if (!xl9555_is_available()) {
        /* 板子未焊接 XL9555：不创建扫描任务，仅提示硬件按键被禁用。
         * 动作引擎仍可通过 Web 配置 / 定时调度 / BOOT 键控制，不影响其余功能。 */
        ESP_LOGW(TAG, "XL9555 不可用，XL9555 硬件按键已禁用（BOOT 键 / Web 配置 / 定时任务仍可控）");
        return ESP_OK;
    }

    BaseType_t r = xTaskCreate(keys_scan_task, "keys_scan", 3072, NULL,
                               KEY_TASK_PRIORITY, NULL);
    if (r != pdPASS) {
        ESP_LOGE(TAG, "按键扫描任务创建失败");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "按键扫描任务已启动（控制面：KEY2 启动 / KEY0 停止 / KEY1+KEY3 重置；BOOT 键短按切换、长按 3s 重置）");
    return ESP_OK;
}
