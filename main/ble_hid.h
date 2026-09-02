/*
 * BLE HID 复合设备（鼠标 + 键盘）
 *
 * 基于 ESP-IDF 通用 esp_hid 组件实现 HID over GATT(HOGP)。
 * 注意：ESP32-S3 不支持经典蓝牙，只能使用 BLE。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- HID Report ID ---------------- */
/* 与 ble_hid.c 中报告描述符里的 Report ID 必须一一对应 */
#define HID_RPT_ID_MOUSE     1   /* 鼠标：4 字节 */
#define HID_RPT_ID_KEYBOARD  2   /* 键盘：8 字节 */

#define HID_MOUSE_RPT_LEN    4   /* buttons + X + Y + wheel */
#define HID_KEYBOARD_RPT_LEN 8   /* modifier + reserved + keycode[6] */

/* ---------------- 方向键 HID Usage Code (Keyboard/Keypad Page 0x07) ---------------- */
#define HID_KEY_ARROW_RIGHT  0x4F
#define HID_KEY_ARROW_LEFT   0x50
#define HID_KEY_ARROW_DOWN   0x51
#define HID_KEY_ARROW_UP     0x52

/* ---------------- 常用按键 HID Usage Code (Keyboard/Keypad Page 0x07) ---------------- */
#define HID_KEY_TAB          0x2B   /* Tab，配合 Left Alt 实现切换程序(Alt+Tab) */

/* ---------------- 修饰键（键盘报文 modifier 字节的 bit 掩码） ---------------- */
#define HID_MODIFIER_LEFT_SHIFT   0x02
#define HID_MODIFIER_RIGHT_SHIFT  0x20
#define HID_MODIFIER_LEFT_CTRL    0x01
#define HID_MODIFIER_LEFT_ALT     0x04

/* ---------------- 鼠标按键位（buttons 字节的 bit 定义） ---------------- */
#define HID_MOUSE_BTN_LEFT   0x01
#define HID_MOUSE_BTN_RIGHT  0x02
#define HID_MOUSE_BTN_MIDDLE 0x04

/* ---------------- 发送队列消息 ---------------- */
typedef enum {
    HID_MSG_MOUSE,      /* 鼠标相对移动 */
    HID_MSG_KEYBOARD,   /* 键盘按下/松开 */
} hid_msg_type_t;

typedef struct {
    hid_msg_type_t type;
    union {
        struct {
            int8_t  dx;
            int8_t  dy;
            int8_t  wheel;     /* 滚轮相对量，正=上滚，负=下滚 */
            uint8_t buttons;   /* bit0=左键, bit1=右键, bit2=中键 */
        } mouse;
        struct {
            uint8_t keycode;
            uint8_t modifier;  /* 修饰键位掩码（HID_MODIFIER_*），0=无修饰键 */
            bool    pressed;
        } key;
    };
} hid_msg_t;

/**
 * @brief 初始化 BLE 协议栈 + HID 设备，并开始广播
 */
esp_err_t ble_hid_init(void);

/**
 * @brief 查询当前是否已与主机建立 HID 连接
 */
bool ble_hid_is_connected(void);

/**
 * @brief 启动 HID 报文串行发送任务（内部创建队列）
 *
 * 鼠标任务与按键任务会并发投递报文，统一由该任务串行调用协议栈，
 * 避免多任务同时调用 esp_hidd_dev_input_set() 产生竞态。
 * 必须在 ble_hid_init() 之前或之后调用均可，但要早于生产者任务启动。
 */
esp_err_t hid_sender_start(void);

/**
 * @brief 投递一次鼠标相对移动（生产者接口，非阻塞）
 */
esp_err_t ble_hid_send_mouse(int8_t dx, int8_t dy);

/**
 * @brief 投递一次完整的鼠标报文（位移 + 滚轮 + 按键，生产者接口，非阻塞）
 *
 * 用于动作引擎支持按住按键拖拽、滚轮滚动等完整能力。队列满时丢帧并告警，
 * 与原 ble_hid_send_mouse() 非阻塞语义一致。
 */
esp_err_t ble_hid_send_mouse_full(int8_t dx, int8_t dy, int8_t wheel, uint8_t buttons);

/**
 * @brief 投递一次完整的鼠标报文（阻塞版）
 *
 * 队列满时最多阻塞等待 100ms，避免大量分片位移（如鼠标复位）因丢帧而失真。
 * 超时仍未入队则丢弃并告警返回 ESP_ERR_NO_MEM。
 */
esp_err_t ble_hid_send_mouse_full_blocking(int8_t dx, int8_t dy, int8_t wheel, uint8_t buttons);

/**
 * @brief 投递一次键盘按下/松开（生产者接口，非阻塞）
 *
 * @param keycode HID Usage Code，如 HID_KEY_ARROW_LEFT
 * @param pressed true=按下，false=松开
 */
esp_err_t ble_hid_send_key(uint8_t keycode, bool pressed);

/**
 * @brief 投递一次带修饰键的键盘按下/松开（生产者接口，非阻塞）
 *
 * 修饰键位于键盘报文首字节的位掩码，与 keycode 同帧发出，主机才认为是组合键。
 * 典型用法（切换程序 Alt+Tab）：
 *   - ble_hid_send_key_mods(HID_KEY_TAB, HID_MODIFIER_LEFT_ALT, true);   // Alt+Tab 按下
 *   - ble_hid_send_key_mods(HID_KEY_TAB, HID_MODIFIER_LEFT_ALT, false);  // 松开 Tab，Alt 仍按住
 *   - ble_hid_send_key_mods(0, 0, false);                                // 全 0 报文，松开 Alt
 *
 * @param keycode  HID Usage Code（仅发修饰键时传 0）
 * @param modifier 修饰键掩码（HID_MODIFIER_* 的按位或，0=无）
 * @param pressed  true=按下，false=松开
 */
esp_err_t ble_hid_send_key_mods(uint8_t keycode, uint8_t modifier, bool pressed);

/**
 * @brief 发送一串 ASCII 文本（逐字符转 HID keycode，自动处理 Shift 大写/符号）
 *
 * 支持 a-z A-Z 0-9 及常见符号（空格 _ - . / : ; 等）。不支持的字符会被跳过。
 * 每个字符以“按下→松开”发送；结束后所有按键均松开。延迟由调用方控制。
 *
 * @param str 以 '\\0' 结尾的 C 字符串
 */
esp_err_t ble_hid_send_string(const char *str);

/**
 * @brief 重置蓝牙配对状态
 *
 * 清除已绑定的主机信息并重新开启广播，使设备可被主机重新搜索、重新配对。
 * 用于 KEY1+KEY3 组合键长按触发。
 */
esp_err_t ble_hid_reset(void);

#ifdef __cplusplus
}
#endif
