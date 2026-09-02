/*
 * BLE HID 复合设备（鼠标 + 键盘）实现
 *
 * 使用 ESP-IDF 通用 esp_hid 组件，传输层为 ESP_HID_TRANSPORT_BLE。
 * 报告描述符中用 Report ID 区分鼠标(1)和键盘(2)。
 */
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_bt.h"
#include "esp_hidd.h"
#include "esp_hid_common.h"
#include "esp_hidd_gatts.h"
#include "esp_gatts_api.h"
#include "esp_gap_ble_api.h"

#include "esp_hid_gap.h"
#include "ble_hid.h"
#include "config_store.h"
#include "defaults.h"

static const char *TAG = "BLE_HID";

/* 蓝牙广播设备名由 defaults.h 的 BLE_DEVICE_NAME 提供（公开默认 BLE_KM；本地 private 见 local_defs.h） */

/* 发送队列深度。鼠标每 20ms 一帧，留出足够缓冲 */
#define HID_SEND_QUEUE_LEN 64

static esp_hidd_dev_t  *s_hid_dev      = NULL;
static QueueHandle_t    s_send_queue   = NULL;
static volatile bool    s_connected    = false;

/* 当前已连接主机的蓝牙地址，重置时用于主动断开（可能尚未写入 bond 列表） */
static esp_bd_addr_t    s_peer_addr;
static volatile bool    s_peer_valid   = false;

/* ==========================================================================
 * HID 报告描述符（鼠标 + 键盘复合设备）
 * ========================================================================== */
static const unsigned char s_hid_report_map[] = {
    /* ---------------- 鼠标 (Report ID = 1) ---------------- */
    0x05, 0x01,        /* Usage Page (Generic Desktop)      */
    0x09, 0x02,        /* Usage (Mouse)                     */
    0xA1, 0x01,        /* Collection (Application)          */
    0x85, HID_RPT_ID_MOUSE, /*   Report ID (1)              */
    0x09, 0x01,        /*   Usage (Pointer)                 */
    0xA1, 0x00,        /*   Collection (Physical)           */
    /* 3 个按键位 + 5 位填充 */
    0x05, 0x09,        /*     Usage Page (Buttons)          */
    0x19, 0x01,        /*     Usage Minimum (1)             */
    0x29, 0x03,        /*     Usage Maximum (3)             */
    0x15, 0x00,        /*     Logical Minimum (0)           */
    0x25, 0x01,        /*     Logical Maximum (1)           */
    0x95, 0x03,        /*     Report Count (3)              */
    0x75, 0x01,        /*     Report Size (1)               */
    0x81, 0x02,        /*     Input (Data, Variable, Abs)   */
    0x95, 0x01,        /*     Report Count (1)              */
    0x75, 0x05,        /*     Report Size (5)               */
    0x81, 0x03,        /*     Input (Const, Var, Abs) 填充  */
    /* X / Y 相对位移 + 滚轮 */
    0x05, 0x01,        /*     Usage Page (Generic Desktop)  */
    0x09, 0x30,        /*     Usage (X)                     */
    0x09, 0x31,        /*     Usage (Y)                     */
    0x09, 0x38,        /*     Usage (Wheel)                 */
    0x15, 0x81,        /*     Logical Minimum (-127)        */
    0x25, 0x7F,        /*     Logical Maximum (127)         */
    0x75, 0x08,        /*     Report Size (8)               */
    0x95, 0x03,        /*     Report Count (3)              */
    0x81, 0x06,        /*     Input (Data, Var, Relative)   */
    0xC0,              /*   End Collection                  */
    0xC0,              /* End Collection                    */

    /* ---------------- 键盘 (Report ID = 2) ---------------- */
    0x05, 0x01,        /* Usage Page (Generic Desktop)      */
    0x09, 0x06,        /* Usage (Keyboard)                  */
    0xA1, 0x01,        /* Collection (Application)          */
    0x85, HID_RPT_ID_KEYBOARD, /* Report ID (2)             */
    /* 8 个修饰键位 (Ctrl/Shift/Alt/GUI 左右) */
    0x05, 0x07,        /*   Usage Page (Key Codes)          */
    0x19, 0xE0,        /*   Usage Minimum (224)             */
    0x29, 0xE7,        /*   Usage Maximum (231)             */
    0x15, 0x00,        /*   Logical Minimum (0)             */
    0x25, 0x01,        /*   Logical Maximum (1)             */
    0x75, 0x01,        /*   Report Size (1)                 */
    0x95, 0x08,        /*   Report Count (8)                */
    0x81, 0x02,        /*   Input (Data, Variable, Abs)     */
    /* 保留字节 */
    0x95, 0x01,        /*   Report Count (1)                */
    0x75, 0x08,        /*   Report Size (8)                 */
    0x81, 0x01,        /*   Input (Constant) 保留           */
    /* LED 输出报告 (NumLock/CapsLock 等)，主机会下发 */
    0x95, 0x05,        /*   Report Count (5)                */
    0x75, 0x01,        /*   Report Size (1)                 */
    0x05, 0x08,        /*   Usage Page (LEDs)               */
    0x19, 0x01,        /*   Usage Minimum (1)               */
    0x29, 0x05,        /*   Usage Maximum (5)               */
    0x91, 0x02,        /*   Output (Data, Variable, Abs)    */
    0x95, 0x01,        /*   Report Count (1)                */
    0x75, 0x03,        /*   Report Size (3)                 */
    0x91, 0x01,        /*   Output (Constant) 填充          */
    /* 6 个同时按下的按键码 */
    0x95, 0x06,        /*   Report Count (6)                */
    0x75, 0x08,        /*   Report Size (8)                 */
    0x15, 0x00,        /*   Logical Minimum (0)             */
    0x25, 0x65,        /*   Logical Maximum (101)           */
    0x05, 0x07,        /*   Usage Page (Key Codes)          */
    0x19, 0x00,        /*   Usage Minimum (0)               */
    0x29, 0x65,        /*   Usage Maximum (101)             */
    0x81, 0x00,        /*   Input (Data, Array)             */
    0xC0,              /* End Collection                    */
};

static esp_hid_raw_report_map_t s_report_maps[] = {
    {
        .data = s_hid_report_map,
        .len  = sizeof(s_hid_report_map),
    },
};

static esp_hid_device_config_t s_hid_config = {
    .vendor_id       = 0x16C0,
    .product_id      = 0x05DF,
    .version         = 0x0100,
    .device_name     = BLE_DEVICE_NAME,
    .manufacturer_name = BLE_MANUFACTURER_NAME,
    .serial_number   = "0001",
    .report_maps     = s_report_maps,
    .report_maps_len = 1,
};

/* ==========================================================================
 * HIDD 事件回调
 * ========================================================================== */
static void hidd_event_callback(void *handler_args, esp_event_base_t base,
                                int32_t id, void *event_data)
{
    esp_hidd_event_t        event = (esp_hidd_event_t)id;
    esp_hidd_event_data_t  *param = (esp_hidd_event_data_t *)event_data;

    switch (event) {
    case ESP_HIDD_START_EVENT:
        ESP_LOGI(TAG, "HID 协议栈已启动，开始 BLE 广播，设备名: %s", BLE_DEVICE_NAME);
        esp_hid_ble_gap_adv_start();
        break;

    case ESP_HIDD_CONNECT_EVENT:
        s_connected = true;
        ESP_LOGI(TAG, "===== 蓝牙已连接！电脑应已识别为鼠标+键盘 =====");
        break;

    case ESP_HIDD_PROTOCOL_MODE_EVENT:
        ESP_LOGI(TAG, "协议模式切换: %s",
                 param->protocol_mode.protocol_mode ? "REPORT" : "BOOT");
        break;

    case ESP_HIDD_OUTPUT_EVENT:
        /* 主机下发的输出报告，例如键盘 CapsLock/NumLock 指示灯状态 */
        ESP_LOGI(TAG, "收到主机输出报告 report_id=%d, 长度=%d, 数据=0x%02x",
                 param->output.report_id, param->output.length,
                 param->output.length ? param->output.data[0] : 0);
        break;

    case ESP_HIDD_FEATURE_EVENT:
        ESP_LOGI(TAG, "收到 FEATURE 报告 report_id=%d", param->feature.report_id);
        break;

    case ESP_HIDD_DISCONNECT_EVENT:
        s_connected = false;
        s_peer_valid = false;
        ESP_LOGW(TAG, "===== 蓝牙已断开，原因: 0x%02x，重新开始广播 =====",
                 param->disconnect.reason);
        esp_hid_ble_gap_adv_start();
        break;

    case ESP_HIDD_STOP_EVENT:
        s_connected = false;
        ESP_LOGW(TAG, "HID 协议栈已停止");
        break;

    default:
        break;
    }
}

/*
 * esp_hid_gap.c 中在配对完成(ESP_GAP_BLE_AUTH_CMPL_EVT)时会调用此函数，
 * 这是官方示例约定的外部符号，必须提供实现，否则链接失败。
 * 本项目的鼠标/按键任务是常驻的、靠连接标志位自行判断，这里只打日志。
 */
void ble_hid_task_start_up(void)
{
    ESP_LOGI(TAG, "配对认证完成，HID 通道就绪");
}

/*
 * 由 esp_hid_gap.c 在 ESP_GAP_BLE_AUTH_CMPL_EVT 中调用，
 * 记录当前主机地址。重置时即使 bond 列表尚未落盘也能主动断开。
 */
void ble_hid_record_peer(esp_bd_addr_t bda)
{
    memcpy(s_peer_addr, bda, sizeof(esp_bd_addr_t));
    s_peer_valid = true;
    ESP_LOGI(TAG, "已记录主机地址 %02x:%02x:%02x:%02x:%02x:%02x",
             bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
}

/* ==========================================================================
 * 报文串行发送任务
 * ========================================================================== */
/* 带重试的 HID 输入报文发送。
 * esp_hidd_dev_input_set 底层走 BLE Indicate（需对端 confirm 后才算完成）。
 * 若上一条 indicate 的 confirm 尚未返回就连续发送，ESP 栈会拒绝（返回 -1 / ESP_FAIL），
 * 表现为“鼠标报文发送失败：ESP_FAIL”。这里在失败后做有限次退避重试，给底层留出
 * 确认窗口；多数瞬时拥塞（如复位时密集位移、对端短暂繁忙）可自愈，同时避免失败帧刷屏。 */
#define HID_SEND_RETRIES        8
#define HID_SEND_RETRY_DELAY_MS 10

static esp_err_t hid_send_report_retry(uint8_t rpt_id, const uint8_t *buf, size_t len)
{
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < HID_SEND_RETRIES; i++) {
        err = esp_hidd_dev_input_set(s_hid_dev, 0, rpt_id, (uint8_t *)buf, len);
        if (err == ESP_OK) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(HID_SEND_RETRY_DELAY_MS));
    }
    return err;
}

static void hid_send_mouse_report(int8_t dx, int8_t dy, int8_t wheel, uint8_t buttons)
{
    uint8_t buf[HID_MOUSE_RPT_LEN] = {
        buttons,        /* 按键位 */
        (uint8_t)dx,    /* X 相对位移 */
        (uint8_t)dy,    /* Y 相对位移 */
        (uint8_t)wheel, /* 滚轮 */
    };

    esp_err_t err = hid_send_report_retry(HID_RPT_ID_MOUSE, buf, HID_MOUSE_RPT_LEN);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "鼠标报文发送失败: %s", esp_err_to_name(err));
    } else {
        ESP_LOGD(TAG, "鼠标报文已发送 dx=%d dy=%d wheel=%d btn=0x%02x",
                 dx, dy, wheel, buttons);
    }
}

/* 键盘报文：修饰键写在首字节，与 keycode 同帧发出才能构成组合键。
 * 注意：modifier 在“松开”帧同样保留（例如松开 Tab 时 Alt 仍按住），
 * 只有发送 modifier=0 且 keycode=0 的全 0 报文才表示“所有按键均已释放”。 */
static void hid_send_keyboard_report(uint8_t keycode, uint8_t modifier, bool pressed)
{
    uint8_t buf[HID_KEYBOARD_RPT_LEN] = {0};

    /* buf[0]=修饰键, buf[1]=保留, buf[2..7]=按键码 */
    buf[0] = modifier;
    if (pressed) {
        buf[2] = keycode;
    }
    /* 松开时 keycode 为 0（修饰键保留），全 0 报文表示没有任何键被按下 */

    esp_err_t err = hid_send_report_retry(HID_RPT_ID_KEYBOARD, buf, HID_KEYBOARD_RPT_LEN);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "键盘报文发送失败: %s", esp_err_to_name(err));
    } else {
        ESP_LOGD(TAG, "键盘报文已发送 keycode=0x%02x mods=0x%02x %s",
                 keycode, modifier, pressed ? "按下" : "松开");
    }
}

static void hid_sender_task(void *arg)
{
    (void)arg;
    hid_msg_t msg;

    while (1) {
        if (xQueueReceive(s_send_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        /* 出队后再确认一次连接状态，避免断开瞬间残留报文 */
        if (!s_connected || s_hid_dev == NULL) {
            continue;
        }

        switch (msg.type) {
        case HID_MSG_MOUSE:
            hid_send_mouse_report(msg.mouse.dx, msg.mouse.dy,
                                  msg.mouse.wheel, msg.mouse.buttons);
            break;
        case HID_MSG_KEYBOARD:
            hid_send_keyboard_report(msg.key.keycode, msg.key.modifier, msg.key.pressed);
            break;
        default:
            break;
        }

        /* 帧间微小让出：降低 indicate 发送速率，给对端 confirm 留窗口，
         * 缓解“运行久了鼠标报文 Indicate Failed”的拥塞（约 1ms/帧，不影响流畅度）。 */
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

/* ==========================================================================
 * 对外接口
 * ========================================================================== */
esp_err_t hid_sender_start(void)
{
    s_send_queue = xQueueCreate(HID_SEND_QUEUE_LEN, sizeof(hid_msg_t));
    if (s_send_queue == NULL) {
        ESP_LOGE(TAG, "发送队列创建失败");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreate(hid_sender_task, "hid_sender", 4096, NULL, 6, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "发送任务创建失败");
        vQueueDelete(s_send_queue);
        s_send_queue = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "HID 发送任务已启动");
    return ESP_OK;
}

bool ble_hid_is_connected(void)
{
    return s_connected;
}

static esp_err_t hid_msg_post(const hid_msg_t *msg)
{
    if (s_send_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_connected) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 非阻塞投递：队列满说明蓝牙拥塞，丢弃。日志降级为 Debug，避免拥塞时刷屏淹没真正错误 */
    if (xQueueSend(s_send_queue, msg, 0) != pdTRUE) {
        ESP_LOGD(TAG, "发送队列已满，丢弃一帧报文");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* 阻塞投递：队列满时最多等待 100ms，保证分片位移等连续报文的完整性 */
static esp_err_t hid_msg_post_blocking(const hid_msg_t *msg)
{
    if (s_send_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_connected) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xQueueSend(s_send_queue, msg, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "发送队列拥塞，阻塞 100ms 后仍满，丢弃一帧报文");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t ble_hid_send_mouse(int8_t dx, int8_t dy)
{
    hid_msg_t msg = {
        .type = HID_MSG_MOUSE,
        .mouse = { .dx = dx, .dy = dy, .wheel = 0, .buttons = 0 },
    };
    return hid_msg_post(&msg);
}

esp_err_t ble_hid_send_mouse_full(int8_t dx, int8_t dy, int8_t wheel, uint8_t buttons)
{
    hid_msg_t msg = {
        .type = HID_MSG_MOUSE,
        .mouse = { .dx = dx, .dy = dy, .wheel = wheel, .buttons = buttons },
    };
    return hid_msg_post(&msg);
}

esp_err_t ble_hid_send_mouse_full_blocking(int8_t dx, int8_t dy, int8_t wheel, uint8_t buttons)
{
    hid_msg_t msg = {
        .type = HID_MSG_MOUSE,
        .mouse = { .dx = dx, .dy = dy, .wheel = wheel, .buttons = buttons },
    };
    return hid_msg_post_blocking(&msg);
}

esp_err_t ble_hid_send_key_mods(uint8_t keycode, uint8_t modifier, bool pressed)
{
    hid_msg_t msg = {
        .type = HID_MSG_KEYBOARD,
        .key = { .keycode = keycode, .modifier = modifier, .pressed = pressed },
    };
    return hid_msg_post(&msg);
}

esp_err_t ble_hid_send_key(uint8_t keycode, bool pressed)
{
    return ble_hid_send_key_mods(keycode, 0, pressed);
}

/* 字符转 HID keycode 与是否需要 Shift。
 * 支持：a-z A-Z 0-9 空格 _ - . / : ; + = ( ) [ ] { } < > 等常见符号。
 * 返回 false 表示不支持该字符（调用方可选择跳过）。 */
static bool char_to_hid(char c, uint8_t *keycode, bool *need_shift)
{
    *need_shift = false;
    if (c >= 'a' && c <= 'z') { *keycode = 0x04 + (c - 'a'); return true; }
    if (c >= 'A' && c <= 'Z') { *keycode = 0x04 + (c - 'A'); *need_shift = true; return true; }
    if (c >= '0' && c <= '9') { *keycode = 0x1E + (c - '0'); return true; }
    switch (c) {
        case ' ': *keycode = 0x2C; return true;
        case '-': *keycode = 0x2D; return true;
        case '_': *keycode = 0x2D; *need_shift = true; return true;
        case '=': *keycode = 0x2E; return true;
        case '+': *keycode = 0x2E; *need_shift = true; return true;
        case '[': *keycode = 0x2F; return true;
        case '{': *keycode = 0x2F; *need_shift = true; return true;
        case ']': *keycode = 0x30; return true;
        case '}': *keycode = 0x30; *need_shift = true; return true;
        case '\\': *keycode = 0x31; return true;
        case '|': *keycode = 0x31; *need_shift = true; return true;
        case ';': *keycode = 0x33; return true;
        case ':': *keycode = 0x33; *need_shift = true; return true;
        case '\'': *keycode = 0x34; return true;
        case '"': *keycode = 0x34; *need_shift = true; return true;
        case '`': *keycode = 0x35; return true;
        case '~': *keycode = 0x35; *need_shift = true; return true;
        case ',': *keycode = 0x36; return true;
        case '<': *keycode = 0x36; *need_shift = true; return true;
        case '.': *keycode = 0x37; return true;
        case '>': *keycode = 0x37; *need_shift = true; return true;
        case '/': *keycode = 0x38; return true;
        case '?': *keycode = 0x38; *need_shift = true; return true;
        default: return false;
    }
}

/* 逐字符发送字符串（含 Shift 处理），字符间由调用方控制延迟。
 * 遇到不支持的字符跳过。字符串结束后所有按键已松开。 */
esp_err_t ble_hid_send_string(const char *str)
{
    if (!str) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t kc; bool shift;
    for (const char *p = str; *p; ++p) {
        if (!char_to_hid(*p, &kc, &shift)) {
            continue;  /* 跳过不支持字符 */
        }
        /* Shift 必须作为修饰键写在报文首字节，与 keycode 同帧发出，
         * 否则主机只收到一个普通按键码，大写字母与 '_' ':' 等符号不会生效。 */
        uint8_t mods = shift ? (uint8_t)HID_MODIFIER_LEFT_SHIFT : 0u;
        ble_hid_send_key_mods(kc, mods, true);
        ble_hid_send_key_mods(kc, mods, false);
    }
    return ESP_OK;
}

esp_err_t ble_hid_reset(void)
{
    ESP_LOGW(TAG, "===== 开始重置蓝牙：断开当前主机并清除全部配对记录 =====");

    /* 1. 先停止广播，避免重置过程中被新主机连入 */
    esp_ble_gap_stop_advertising();

    /* 2. 主动断开当前连接。
     *    分两路：a) 已记录的当前主机地址；b) bond 列表中的全部地址。
     *    断开是异步的，随后统一等待链路真正释放。 */
    bool any_disconnect = false;

    /* 快照对端地址：回调线程可能在本函数执行期间清除 s_peer_valid */
    esp_bd_addr_t peer;
    bool has_peer = s_peer_valid;
    if (has_peer) {
        memcpy(peer, s_peer_addr, sizeof(esp_bd_addr_t));
        esp_err_t d = esp_ble_gap_disconnect(peer);
        ESP_LOGI(TAG, "断开当前主机 %02x:%02x:%02x:%02x:%02x:%02x -> %s",
                 peer[0], peer[1], peer[2], peer[3], peer[4], peer[5],
                 esp_err_to_name(d));
        any_disconnect = true;
    }

    int bonded = esp_ble_get_bond_device_num();
    esp_ble_bond_dev_t *list = NULL;
    if (bonded > 0) {
        list = calloc(bonded, sizeof(esp_ble_bond_dev_t));
        if (list == NULL) {
            ESP_LOGE(TAG, "分配绑定列表内存失败(%d 条)", bonded);
            return ESP_ERR_NO_MEM;
        }
        if (esp_ble_get_bond_device_list(&bonded, list) == ESP_OK) {
            for (int i = 0; i < bonded; i++) {
                /* 跳过已断开过的当前主机，避免重复调用 */
                if (has_peer &&
                    memcmp(list[i].bd_addr, peer, sizeof(esp_bd_addr_t)) == 0) {
                    continue;
                }
                esp_ble_gap_disconnect(list[i].bd_addr);
                any_disconnect = true;
            }
        }
    }

    /* 3. 等待断开完成（DISCONNECT 事件会把 s_connected 置 false），最长 1 秒 */
    if (any_disconnect) {
        for (int i = 0; i < 20 && s_connected; i++) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        ESP_LOGI(TAG, "链路断开%s", s_connected ? "超时(继续执行清除)" : "完成");
    }
    s_connected = false;

    /* 4. 链路释放后再删除配对记录，确保对方被彻底移除、可被其他用户连接 */
    if (bonded > 0 && list != NULL) {
        int removed = 0;
        for (int i = 0; i < bonded; i++) {
            esp_err_t r = esp_ble_remove_bond_device(list[i].bd_addr);
            if (r == ESP_OK) {
                removed++;
                ESP_LOGI(TAG, "已删除配对记录 %02x:%02x:%02x:%02x:%02x:%02x",
                         list[i].bd_addr[0], list[i].bd_addr[1], list[i].bd_addr[2],
                         list[i].bd_addr[3], list[i].bd_addr[4], list[i].bd_addr[5]);
            } else {
                ESP_LOGW(TAG, "删除配对记录 #%d 失败: %s", i, esp_err_to_name(r));
            }
        }
        ESP_LOGI(TAG, "共清除 %d/%d 条配对记录", removed, bonded);
    } else {
        ESP_LOGI(TAG, "没有已绑定记录需要清除");
    }
    free(list);

    /* 5. 确认清空结果 */
    int left = esp_ble_get_bond_device_num();
    if (left > 0) {
        ESP_LOGW(TAG, "仍残留 %d 条配对记录", left);
    } else {
        ESP_LOGI(TAG, "配对记录已全部清空");
    }

    s_peer_valid = false;
    memset(s_peer_addr, 0, sizeof(s_peer_addr));

    /* 6. 清空发送队列中的残留报文，避免重连后误发 */
    if (s_send_queue) {
        xQueueReset(s_send_queue);
    }

    /* 7. 关键修复：轮换一个全新随机地址再广播。
     *    不换地址的话，Windows 仍保存着基于老地址的旧配对，会不断自动重连；
     *    重连后本端 LTK 已删除 → 加密失败(ltk_req_neg_reply)→ 断开 → 再重连，
     *    形成重连风暴(日志里的 bta_dm_set_encryption ... not find peer_bdaddr)。
     *    换地址后 Windows 旧配对匹配不上，必须重新配对，风暴消失。
     *    随机地址必须在"停止广播、连接已断开"状态下才能设置，故先停广播。 */
    esp_ble_gap_stop_advertising();
    vTaskDelay(pdMS_TO_TICKS(50));  /* 等待停止广播完成 */
    esp_hid_ble_use_new_random_addr();

    /* 8. 重新开启广播，恢复可被任意主机搜索的状态 */
    esp_err_t ret = esp_hid_ble_gap_adv_start();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "===== 蓝牙已重置，正在广播 \"%s\"（新地址），等待新设备配对 =====",
                 BLE_DEVICE_NAME);
    } else {
        ESP_LOGE(TAG, "重置后重新广播失败: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t ble_hid_init(void)
{
    esp_err_t ret;

    /* 1. 初始化蓝牙控制器 + Bluedroid + GAP（仅 BLE 模式） */
    ret = esp_hid_gap_init(ESP_BT_MODE_BLE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_hid_gap_init 失败: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 2. 配置 BLE 广播数据：带 HID Service UUID(0x1812) 与 Appearance，
     *    这是 Windows 能自动识别为鼠标键盘的关键。 */
    ret = esp_hid_ble_gap_adv_init(ESP_HID_APPEARANCE_GENERIC, BLE_DEVICE_NAME);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_hid_ble_gap_adv_init 失败: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 3. 注册 GATT 服务表 */
    ret = esp_ble_gatts_register_callback(esp_hidd_gatts_event_handler);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "注册 GATTS 回调失败: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 4. 初始化 HID 设备，成功后会收到 ESP_HIDD_START_EVENT 并开始广播 */
    ret = esp_hidd_dev_init(&s_hid_config, ESP_HID_TRANSPORT_BLE,
                            hidd_event_callback, &s_hid_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_hidd_dev_init 失败: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 应用可配的 BLE 发射功率（档位枚举值，默认 N3=-3dBm），降低近距离发热。
     * 注意：调用前 config_store_load() 必须已执行，以确保读到保存值而非全 0。 */
    config_t *rc = config_store_get();
    if (rc) {
        int lvl = rc->radio.ble_power_level;
        if (lvl < 0)    lvl = 0;
        if (lvl > 7)    lvl = 7;    /* ESP_PWR_LVL 枚举 0~7（v5.3.5：-12~+9dBm） */
        esp_err_t perr = esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT,
                                              (esp_power_level_t)lvl);
        if (perr == ESP_OK) {
            ESP_LOGI(TAG, "BLE 发射功率档位已设置为 %d", lvl);
        } else {
            ESP_LOGW(TAG, "BLE 发射功率设置失败(%s)", esp_err_to_name(perr));
        }
    }

    ESP_LOGI(TAG, "BLE HID 初始化完成，等待电脑配对连接...");
    return ESP_OK;
}
