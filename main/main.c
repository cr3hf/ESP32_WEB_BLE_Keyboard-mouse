/*
 * ESP32-S3 蓝牙鼠标键盘（随机动作模拟器）
 *
 * 功能：
 *   1. 作为 BLE HID 复合设备（鼠标 + 键盘）连接电脑
 *   2. 默认处于停止状态，按 KEY2 启动，按 KEY0 停止
 *   3. KEY1 短按：运行额度 +10 分钟（LED 闪烁两次确认，间隔随累计额度增长）
 *   4. KEY1 + KEY3 同时长按 3 秒重置蓝牙配对
 *   5. 启动后按概率随机执行：拖拽 / 点击 / 滚轮 / 方向键 / 休息 五类动作
 *   6. GPIO1 状态 LED：动作执行中亮（低电平），停止时灭（高电平）
 *
 * 注意：ESP32-S3 不支持经典蓝牙，本程序使用 BLE HID over GATT。
 */
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"

#include "led_status.h"
#include "ble_hid.h"
#include "hid_keys.h"
#include "action_engine.h"
#include "config_store.h"
#include "wifi_manager.h"
#include "web_server.h"

static const char *TAG = "MAIN";

/* 初始化 NVS，BLE 的配对绑定信息需要存储在这里 */
static esp_err_t init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要擦除后重建");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  ESP32-S3 蓝牙鼠标键盘 (随机动作)");
    ESP_LOGI(TAG, "  芯片: ESP32-S3   协议: HID over GATT");
    ESP_LOGI(TAG, "========================================");

    /* 1. NVS（BLE 绑定信息存储） */
    ESP_ERROR_CHECK(init_nvs());

    /* 2. 状态 LED（GPIO1，初始灭） */
    ESP_ERROR_CHECK(led_status_init());

    /* 3. HID 报文发送任务（必须早于生产者任务启动） */
    ESP_ERROR_CHECK(hid_sender_start());

    /* 4. 配置存储：从 NVS 加载用户配置（运行模式/权重/序列/定时/射频功率/STA 凭据）。
     *    必须在 ble_hid_init / wifi_manager_start 之前，以便它们读取保存的功率与 STA 配置。 */
    ESP_ERROR_CHECK(config_store_load());

    /* 5. WiFi：先启动并让 STA 抢先连路由器（见下方说明），SoftAP 常开（热点名见启动日志 / 192.168.4.1）。
     *    顺序很关键：必须早于 BLE 初始化！电脑蓝牙一直连着本设备，若 BLE 先起会立刻抢占射频，
     *    导致随后启动的 STA 被 coexistence 踢掉(reason=203/205)，表现为"复位交替必现连不上"。
     *    先起 Wi-Fi 让 STA 先拿到射频并发起连接，再等其稳连，最后才起 BLE，
     *    已建立的 STA 不会被后续蓝牙抢占踢掉。 */
    ESP_ERROR_CHECK(wifi_manager_start());

    /* 5.1 阻塞等待 STA 连上路由器（最多 8s）。未配 STA / 超时未连上则立即返回，
     *     不阻塞后续 BLE 启动（AP 模式下蓝牙照常用）。这一步物理上保证 STA 先稳连，
     *     是治愈"复位交替必现 STA 失败"的核心。 */
    bool sta_ok = wifi_manager_wait_sta_connected(8000);
    ESP_LOGI(TAG, "STA 连接等待结果：%s", sta_ok ? "已连上路由器" : "未连(或未配STA，继续)");

    /* 6. BLE HID 初始化并开始广播（内部会按已加载配置应用 BLE 发射功率）。
     *    放在 Wi-Fi 之后（且等 STA 稳连后），避免蓝牙抢占射频导致 STA 连不上。 */
    ESP_ERROR_CHECK(ble_hid_init());

    /* 7. 动作引擎任务（必须在按键任务之前创建，按键回调会操作其 EventGroup） */
    ESP_ERROR_CHECK(action_engine_start_task());

    /* 8. 按键扫描任务（控制面：KEY2 启动 / KEY0 停止 / KEY1+KEY3 重置） */
    ESP_ERROR_CHECK(keys_start());

    /* 9. 应用配置到动作引擎（模式/权重/序列） */
    action_engine_apply_config(config_store_get());

    /* 10. Web 服务：内置页面 + REST 接口，监听 0.0.0.0（AP/STA 双网段均可访问） */
    ESP_ERROR_CHECK(web_server_start());

    /* 11. 定时调度扫描任务（每秒检查一次规则，触发启停/单动作/周期循环） */
    ESP_ERROR_CHECK(action_engine_start_scheduler());

    ESP_LOGI(TAG, "全部模块启动完成");

    /* 若本固件是通过 OTA 从另一分区启动的，确认其有效，取消回滚定时器。
     * 未在 OTA 分区运行时此调用无副作用。 */
    if (esp_ota_check_rollback_is_possible()) {
        ESP_LOGI(TAG, "OTA 启动成功，确认当前分区有效（取消回滚）");
        esp_ota_mark_app_valid_cancel_rollback();
    }
    ESP_LOGI(TAG, "请在电脑上打开 蓝牙设置 → 添加设备 → 蓝牙，选择 \"%s\" 进行配对",
             "ESP32-S3 KM");
    ESP_LOGI(TAG, "配对后：KEY2 启动 / KEY0 停止 / KEY1 短按+10分钟(LED闪2次确认) / KEY1+KEY3 长按3秒重置蓝牙");
    ESP_LOGI(TAG, "BOOT 键(GPIO0)：短按切换启动/停止 / 长按3秒重置蓝牙（与 KEY1+KEY3 组合等效）");
    char ap_ssid_buf[33];
    wifi_manager_get_ap_ssid(ap_ssid_buf, sizeof(ap_ssid_buf));
    ESP_LOGI(TAG, "Web 配置：连接热点 \"%s\"(密码 %s) 访问 http://%s/ （连上路由器后 STA 局域网 IP 亦可访问）",
             ap_ssid_buf, KM_AP_PASS, "192.168.4.1");
}
