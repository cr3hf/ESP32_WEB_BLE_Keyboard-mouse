/*
 * Web 服务（web_server）
 *
 * 基于 ESP-IDF 原生 esp_http_server，监听 0.0.0.0:80。
 * 因 SoftAP 与 STA 共存（见 wifi_manager），AP 子网(192.168.4.1) 与
 * STA 局域网 IP 都能访问同一个页面，无需为每个网段起独立 server。
 *
 * 页面为单页 HTML/CSS/JS，内嵌为 Flash 常量（static const char[]），
 * 不引入文件系统；REST 接口使用 cJSON 临时解析（用完即释放）。
 *
 * 提供接口：
 *   GET  /                -> 页面
 *   GET  /api/status      -> 运行状态/蓝牙/WiFi 信息
 *   GET  /api/config      -> 当前配置(JSON)
 *   POST /api/config      -> 保存配置(JSON) 并应用到引擎
 *   POST /api/control     -> 启停引擎 {"action":"start"|"stop"}
 *   POST /api/wifi        -> 保存 STA 凭据并连接 {"ssid","pass"}
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动 Web 服务（需在 wifi_manager_start 之后）。监听 0.0.0.0:80。
 */
esp_err_t web_server_start(void);

/**
 * @brief 停止 Web 服务。
 */
esp_err_t web_server_stop(void);

/**
 * @brief 恢复出厂设置（供硬件长按等物理触发调用）：重置 Web 登录账号为 admin/admin 并重置全部参数到默认，立即落盘。
 *        无鉴权，调用方需保证触发来源可信（如物理按键长按）。
 */
void web_factory_reset(void);

#ifdef __cplusplus
}
#endif
