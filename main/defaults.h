/*
 * 默认配置（公开 / 开源版本）
 *
 * 本文件定义开源（给别人）构建的默认品牌与凭据。
 * 若工程目录中存在 main/local_defs.h（已被 .gitignore 忽略，不会上传），
 * 且未通过环境变量 CARINE_LOCAL=0 强制关闭，则改用其中的“个人/私有”默认值。
 *
 * 切换方式：
 *   - 本地（含个人信息）构建：保留 main/local_defs.h 即可，无需额外操作；
 *   - 开源 / 给别人构建：将 main/local_defs.h 改名或删除（或设 CARINE_LOCAL=0），
 *     自动回落到本文件的公开默认值。
 */
#ifndef DEFAULTS_H
#define DEFAULTS_H

#ifdef HAVE_LOCAL_DEFS
#include "local_defs.h"
#endif

/* Web 控制台登录账号/密码（出厂默认，可在页面修改） */
#ifndef WEB_LOGIN_USER
#define WEB_LOGIN_USER   "admin"
#endif
#ifndef WEB_LOGIN_PASS
#define WEB_LOGIN_PASS   "admin"
#endif

/* SoftAP 热点 */
#ifndef KM_AP_SSID_PREFIX
#define KM_AP_SSID_PREFIX  "BLE_KM_"     /* 运行时若 KM_AP_SSID_DYNAMIC=1 则拼接 MAC 后四位 */
#endif
#ifndef KM_AP_SSID_DYNAMIC
#define KM_AP_SSID_DYNAMIC 1            /* 1=SSID 末尾追加 MAC 后4位；0=直接使用前缀作为完整 SSID */
#endif
#ifndef KM_AP_PASS
#define KM_AP_PASS         "12345678"    /* 8 位，符合 WPA2 要求 */
#endif

/* STA 出厂默认凭据（页面可改；空串表示不预填，需用户自行填写） */
#ifndef DEF_STA_SSID
#define DEF_STA_SSID   ""
#endif
#ifndef DEF_STA_PASS
#define DEF_STA_PASS   ""
#endif

/* 页面标题 / 品牌（页面中所有 "Carine" 文案统一引用此宏） */
#ifndef APP_TITLE
#define APP_TITLE          "BLE_KM"
#endif

/* 蓝牙 HID 广播设备名（Windows 添加设备列表中显示的名称） */
#ifndef BLE_DEVICE_NAME
#define BLE_DEVICE_NAME    "BLE_KM"
#endif

/* 蓝牙 HID 设备厂商名（系统设备属性中显示） */
#ifndef BLE_MANUFACTURER_NAME
#define BLE_MANUFACTURER_NAME    "BLE_KM"
#endif

#endif /* DEFAULTS_H */
