/*
 * WiFi 管理（wifi_manager）
 *
 * 设备启动即常开 SoftAP 热点（默认 KM-Config / 192.168.4.1）；
 * 同时可并发连接路由器 STA（凭据来自 NVS）。AP 永远不关闭——
 * 无论 STA 是否连上、用户是否知道 STA 的局域网 IP，都能通过 AP 的
 * 192.168.4.1 访问 Web 页面；STA 连上后其局域网 IP 同样可访问
 * （esp_http_server 监听 0.0.0.0，两网段自然共享）。
 *
 * 时间：STA 连上路由器后启动 SNTP 校时，供定时任务读取本地时间；
 * 未联网时使用"自开机累计"的软时钟（从 00:00:00 起），保证 AP-only
 * 场景下定时功能也能按设备相对时间工作。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* AP 热点名/密码默认值见 defaults.h（公开默认）或 main/local_defs.h（本地私有，不提交）。
 * AP SSID 不再是编译期常量：由 wifi_manager.c 在运行时按前缀 + MAC 后四位拼接，
 * 通过 wifi_manager_get_ap_ssid() 获取。 */
#include "defaults.h"

#define KM_AP_IP        "192.168.4.1"
#define KM_AP_GW        "192.168.4.1"
#define KM_AP_NETMASK   "255.255.255.0"
#define KM_AP_MAX_CONN  4
#define KM_AP_CHANNEL   1

/**
 * @brief 启动 WiFi：初始化 esp_netif，先起 SoftAP（常开），再按 NVS 配置并发起 STA。
 *        必须在 web_server_start 之前调用。AP 始终存活，STA 失败不影响 AP。
 */
esp_err_t wifi_manager_start(void);

/**
 * @brief 获取当前 SoftAP 的完整 SSID（运行时拼接 MAC 后四位后的结果）。
 *        用于启动日志与提示，避免直接依赖编译期常量。buf 长度建议 ≥ 33。
 */
void wifi_manager_get_ap_ssid(char *buf, size_t len);

/**
 * @brief 阻塞等待 STA 拿到 IP（最多 timeout_ms）。
 *        用于"先让 Wi-Fi 稳连、再起 BLE"的启动顺序协调：蓝牙先起会抢占射频把
 *        已发起的 STA 连接踢掉（reason=203/205，复位交替必现）。
 *        未启用 STA 或超时未连上则立即返回 false（不阻塞，BLE 照常起）。
 */
bool wifi_manager_wait_sta_connected(uint32_t timeout_ms);

/**
 * @brief 保存并应用 STA 凭据（SSID/密码）。立即并发连接路由器；不关闭 AP。
 *        成功返回 ESP_OK（仅表示已提交连接请求）。
 */
esp_err_t wifi_manager_set_sta(const char *ssid, const char *pass);

/**
 * @brief 扩展版：在 set_sta 基础上支持静态 IP 配置。
 *        dhcp=true 时使用路由器 DHCP（默认）；dhcp=false 时使用静态地址。
 *        静态模式下若 ip/netmask/gw 合法，会在连接前停止 DHCP 客户端并写入静态 IP，
 *        dns 非空则同时设置静态 DNS。任一参数可为 NULL（按 DHCP 处理或忽略）。
 */
esp_err_t wifi_manager_set_sta_ex(const char *ssid, const char *pass,
                                  bool dhcp,
                                  const char *ip, const char *gw,
                                  const char *netmask, const char *dns);

/**
 * @brief 关闭 STA（保留 AP）。仅当配置中 sta_enabled=false 时由配置加载调用。
 */
esp_err_t wifi_manager_disable_sta(void);

/**
 * @brief 获取 SoftAP 的 IP（字符串，如 "192.168.4.1"）。未就绪返回 "0.0.0.0"。
 */
void wifi_manager_get_ap_ip(char *buf, size_t len);

/**
 * @brief 获取 STA 的 IP（字符串，连上路由器才有；未连返回 "0.0.0.0"）。
 */
void wifi_manager_get_sta_ip(char *buf, size_t len);

/**
 * @brief 获取当前 WiFi 模式描述串："AP" / "AP+STA" / "OFF"。
 */
void wifi_manager_get_mode_str(char *buf, size_t len);

/**
 * @brief 获取本地时间（已 SNTP 校时则真实时间；否则开机累计软时钟）。
 *        成功填充 tm 并返回 true；失败返回 false。
 */
bool wifi_manager_get_local_time(struct tm *ti);

/**
 * @brief 当前时间是否已联网校时（true=真实北京时间；false=开机软时钟）。
 */
bool wifi_manager_time_synced(void);

/**
 * @brief 获取本地时间字符串（北京时间 UTC+8，形如 "2026-08-12 15:04:05"）。
 *        未联网/未校时返回开机软时钟。buf 长度建议 ≥ 20。
 */
void wifi_manager_get_time_str(char *buf, size_t len);

/**
 * @brief 手动触发一次 SNTP 校时（强制重新同步）。
 *        需 STA 已启用/连接；失败返回 ESP_FAIL（无网络无法校时）。
 */
esp_err_t wifi_manager_sync_time(void);

/**
 * @brief 用外部传入的 epoch 秒数直接校时（settimeofday）。
 *        典型场景：Web 页面“同步浏览器时间”按钮把用户本机时间 POST 上来，
 *        无需联网即可把设备时钟校正为真实本地时间（北京时间 UTC+8）。
 *        epoch 过旧（< 2023-11）会被拒绝，返回 ESP_ERR_INVALID_ARG。
 */
esp_err_t wifi_manager_set_time(time_t epoch);

#ifdef __cplusplus
}
#endif
