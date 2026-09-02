/*
 * WiFi 管理实现
 *
 * - 启动即建立 SoftAP（WIFI_MODE_APSTA，STA 部分按配置决定是否连路由器）。
 * - AP 永远不关闭；STA 连接失败仅记录，不影响 AP 可用。
 * - STA 拿到 IP 后启动 SNTP 校时（用于定时任务真实时间）。
 * - 未联网时用开机累计秒数作为软时钟，保证 AP-only 也能用相对时间定时。
 */
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_sntp.h"

#include "lwip/ip_addr.h"

#include "nvs_flash.h"
#include "nvs.h"

#include "wifi_manager.h"
#include "config_store.h"

static const char *TAG = "WIFI_MGR";

/* AP SSID 运行时构建：前缀 + （可选）MAC 后四位，见 build_ap_ssid() */
static char s_ap_ssid[33];

/* 构建 SoftAP SSID：前缀 + 可选 MAC 后 4 位十六进制（由 KM_AP_SSID_DYNAMIC 控制） */
static void build_ap_ssid(void)
{
    const char *prefix = KM_AP_SSID_PREFIX;
    if (KM_AP_SSID_DYNAMIC) {
        uint8_t mac[6];
        if (esp_wifi_get_mac(WIFI_IF_STA, mac) != ESP_OK) {
            mac[4] = 0; mac[5] = 0;
        }
        snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s%02X%02X", prefix, mac[4], mac[5]);
    } else {
        snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s", prefix);
    }
}

#define WIFI_CONNECTED_BIT  (1 << 0)
#define WIFI_FAIL_BIT       (1 << 1)
#define WIFI_APSTA_RETRY    5
/* STA 断开后重连退避：避免无限狂连持续占用射频、饿死 BLE（表现为 BLE 发送队列满刷屏）。
 * 断开后延迟 8 秒再重连，既保留自动恢复能力，又不会高频折腾射频。 */
#define WIFI_RECONNECT_DELAY_MS  8000

static EventGroupHandle_t s_wifi_evt = NULL;
static esp_netif_t *s_ap_netif = NULL;
static esp_netif_t *s_sta_netif = NULL;
static bool        s_sntp_started = false;
static esp_timer_handle_t s_reconnect_timer = NULL;
/* STA 连续重连失败计数：达到阈值后停止继续重连（STA 保持未连接但不扫描），
 * 避免连不上的路由器导致 WiFi 持续扫描、占用射频/heap 并饿死 BLE。 */
static uint32_t     s_sta_fail_count = 0;
#define WIFI_STA_FAIL_LIMIT   12   /* 约 12*8s≈96s 仍连不上则放弃，由用户重新保存触发 */

/* 系统 heap 监控：定期打印空闲/最小 heap，用于排查 wifi:mem fail 是否为泄漏 */
static void heap_monitor_timer_cb(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "HEAP free=%lu min=%lu",
             (unsigned long)xPortGetFreeHeapSize(),
             (unsigned long)esp_get_minimum_free_heap_size());
}

/* 软时钟基准：开机时刻（秒） */
static time_t s_boot_epoch = 0;

/* STA 重连退避定时器回调：延迟触发重连，避免断开后无限狂连占用射频、饿死 BLE */
static void wifi_reconnect_timer_cb(void *arg)
{
    (void)arg;
    config_t *cfg = config_store_get();
    if (cfg && cfg->wifi.sta_enabled && cfg->wifi.sta_ssid[0]) {
        esp_wifi_connect();
    }
}

/* 若配置为静态 IP，则在连接前停止 DHCP 客户端并写入静态地址（开机自动重连时复用） */
static void apply_static_ip_from_config(void)
{
    config_t *cfg = config_store_get();
    if (cfg == NULL || cfg->wifi.sta_dhcp || !cfg->wifi.sta_ip[0] || !cfg->wifi.sta_netmask[0]) {
        return;  /* DHCP 或地址不完整，保持默认 */
    }
    if (s_sta_netif == NULL) {
        return;
    }
    esp_netif_ip_info_t info;
    memset(&info, 0, sizeof(info));
    info.ip.addr      = ipaddr_addr(cfg->wifi.sta_ip);
    info.netmask.addr = ipaddr_addr(cfg->wifi.sta_netmask);
    info.gw.addr      = cfg->wifi.sta_gw[0] ? ipaddr_addr(cfg->wifi.sta_gw) : 0;
    if (info.ip.addr == 0 || info.netmask.addr == 0) {
        return;
    }
    esp_netif_dhcpc_stop(s_sta_netif);
    esp_netif_set_ip_info(s_sta_netif, &info);
    if (cfg->wifi.sta_dns[0]) {
        esp_netif_dns_info_t dnsinfo;
        memset(&dnsinfo, 0, sizeof(dnsinfo));
        dnsinfo.ip.u_addr.ip4.addr = ipaddr_addr(cfg->wifi.sta_dns);
        dnsinfo.ip.type = ESP_IPADDR_TYPE_V4;
        if (esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dnsinfo) == ESP_OK) {
            ESP_LOGI(TAG, "开机静态 DNS 已设置：%s", cfg->wifi.sta_dns);
        }
    }
    ESP_LOGI(TAG, "开机静态地址已应用：IP=%s GW=%s MASK=%s",
             cfg->wifi.sta_ip, cfg->wifi.sta_gw, cfg->wifi.sta_netmask);
}

/* ---------------- 事件回调 ---------------- */
static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            config_t *cfg = config_store_get();
            if (cfg && cfg->wifi.sta_enabled && cfg->wifi.sta_ssid[0]) {
                esp_wifi_connect();
                ESP_LOGI(TAG, "STA 启动，尝试连接 %s", cfg->wifi.sta_ssid);
            } else if (cfg && cfg->wifi.sta_enabled && !cfg->wifi.sta_ssid[0]) {
                ESP_LOGW(TAG, "STA 已启用但 SSID 为空，未发起连接");
            }
        } else if (id == WIFI_EVENT_STA_CONNECTED) {
            ESP_LOGI(TAG, "STA 已连接路由器");
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
            config_t *cfg = config_store_get();
            ESP_LOGW(TAG, "STA 断开(原因=%d)，AP 不受影响", d->reason);
            /* 自动重连（退避延迟）：覆盖上电射频未稳(reason=2)、认证失败(reason=203)等断线场景。
             * 不立即重连，改用定时器延迟 %d ms 再连，避免反复狂连持续占用射频、饿死 BLE
             * （此前表现为 BLE 发送队列满刷屏、位移分片丢弃）。
             * 若连续失败达阈值，则停止继续重连：STA 保持未连接但不扫描，消除 WiFi 持续扫描
             * 对射频/heap 的占用（表现为 wifi:mem fail / m f null 刷屏、BLE 拥塞）。
             * 用户重新保存 WiFi 配置会清零计数并再次尝试。 */
            s_sta_fail_count++;
            if (cfg && cfg->wifi.sta_enabled && cfg->wifi.sta_ssid[0] && s_sta_fail_count < WIFI_STA_FAIL_LIMIT) {
                if (s_reconnect_timer == NULL) {
                    esp_timer_create_args_t t = {
                        .callback = &wifi_reconnect_timer_cb,
                        .name = "wifi_reconn"
                    };
                    if (esp_timer_create(&t, &s_reconnect_timer) != ESP_OK) {
                        s_reconnect_timer = NULL;
                    }
                }
                if (s_reconnect_timer != NULL) {
                    esp_timer_stop(s_reconnect_timer);
                    esp_timer_start_once(s_reconnect_timer, WIFI_RECONNECT_DELAY_MS * 1000);
                    ESP_LOGI(TAG, "将在 %d ms 后尝试重连(%u/%u)", WIFI_RECONNECT_DELAY_MS, s_sta_fail_count, WIFI_STA_FAIL_LIMIT);
                } else {
                    esp_wifi_connect();
                }
            } else {
                esp_timer_stop(s_reconnect_timer);
                ESP_LOGW(TAG, "STA 连续重连失败 %u 次，停止自动重连（STA 保持未连接、不再扫描）。重新保存 WiFi 可再次尝试", s_sta_fail_count);
            }
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        /* 已成功拿到 IP，取消待触发的退避重连定时器并清零失败计数 */
        s_sta_fail_count = 0;
        if (s_reconnect_timer) {
            esp_timer_stop(s_reconnect_timer);
        }
        ESP_LOGI(TAG, "==================================================");
        ESP_LOGI(TAG, "STA 已连接并获取 IP：%s", "请记录以下地址");
        ESP_LOGI(TAG, "  IP 地址 : " IPSTR, IP2STR(&e->ip_info.ip));
        ESP_LOGI(TAG, "  网关     : " IPSTR, IP2STR(&e->ip_info.gw));
        ESP_LOGI(TAG, "  子网掩码 : " IPSTR, IP2STR(&e->ip_info.netmask));
        ESP_LOGI(TAG, "==================================================");

        /* 默认出口接口设定：AP(192.168.4.1)与 STA(110.x)都会注册默认路由，
         * LwIP 按 netif 顺序选默认出口。显式将 STA 设为系统默认出口，
         * 保证 ESP32【主动发起】的跨网段流量(如访问 112.x 上的服务)经 STA->网关 110.1 正确出去。
         * 注意：本调用不影响"被访问"场景——外部主机 ping/访问 ESP32 时，ESP32 的
         * 回包按"入接口原路返回"(弱主机模型)，与默认路由无关。因此跨网段能否访问 ESP32
         * 取决于上游路由器是否放行(110.x 与 112.x 是否三层互通)，非本固件可解决。 */
        if (s_sta_netif) {
            esp_netif_set_default_netif(s_sta_netif);
            ESP_LOGI(TAG, "已将 STA 设为系统默认出口接口（ESP32 主动出包走 STA 网关）");
        }

        /* 诊断：打印 AP 接口的网关，确认它不会抢走默认路由 */
        if (s_ap_netif) {
            esp_netif_ip_info_t ap_ip;
            if (esp_netif_get_ip_info(s_ap_netif, &ap_ip) == ESP_OK) {
                ESP_LOGI(TAG, "  [诊断] AP 接口 IP=" IPSTR " GW=" IPSTR,
                         IP2STR(&ap_ip.ip), IP2STR(&ap_ip.gw));
            }
        }

        xEventGroupSetBits(s_wifi_evt, WIFI_CONNECTED_BIT);
        if (!s_sntp_started) {
            esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
            /* 国内 NTP 服务器优先，避免 pool.ntp.org 在境内解析慢/不稳 */
            esp_sntp_setservername(0, "ntp.aliyun.com");
            esp_sntp_setservername(1, "cn.ntp.org.cn");
            esp_sntp_setservername(2, "time.windows.com");
            esp_sntp_init();
            s_sntp_started = true;
            ESP_LOGI(TAG, "SNTP 已启动（ntp.aliyun.com），等待校时（北京时间 UTC+8）");
        }
    }
}

/* ---------------- 本地时间（真实优先，软时钟兜底） ---------------- */
bool wifi_manager_get_local_time(struct tm *ti)
{
    if (ti == NULL) {
        return false;
    }
    time_t now;
    time(&now);
    /* 若 SNTP 已校时，now 为真实 epoch（远大于 2024 年起的秒数） */
    if (now > 1700000000) {
        localtime_r(&now, ti);
        return true;
    }
    /* 否则用软时钟：开机即 00:00:00，按自开机累计秒数推算 */
    time_t t = now % 86400;  /* 当天秒数 */
    ti->tm_hour = (int)(t / 3600);
    ti->tm_min  = (int)((t % 3600) / 60);
    ti->tm_sec  = (int)(t % 60);
    ti->tm_mday = 1; ti->tm_mon = 0; ti->tm_year = 70;
    return true;
}

/* ---------------- 对外接口 ---------------- */
esp_err_t wifi_manager_start(void)
{
    s_wifi_evt = xEventGroupCreate();

    /* 时区固定为北京时间（UTC+8），确保本地时间与定时任务都按北京时间，
     * 避免与用户所在地（中国）偏差 8 小时导致定时启停错位。 */
    setenv("TZ", "CST-8", 1);
    tzset();

    ESP_ERROR_CHECK(esp_netif_init());
    /* 若其它模块已创建 default event loop，重复创建会返回 INVALID_STATE，忽略即可 */
    esp_err_t e_loop = esp_event_loop_create_default();
    if (e_loop != ESP_OK && e_loop != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(e_loop);
    }

    /* AP 接口 */
    esp_netif_create_default_wifi_ap();
    /* STA 接口（即便暂不连，也创建以便共存） */
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &on_wifi_event, NULL, NULL));

    /* AP 配置：常开（SSID 为运行时拼接，先写入缓冲区再复制，不能用数组变量直接初始化） */
    build_ap_ssid();
    wifi_config_t ap_cfg = { 0 };
    strncpy((char *)ap_cfg.ap.ssid, s_ap_ssid, sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid[sizeof(ap_cfg.ap.ssid) - 1] = '\0';
    ap_cfg.ap.ssid_len = (uint8_t)strlen(s_ap_ssid);
    ap_cfg.ap.channel = KM_AP_CHANNEL;
    strncpy((char *)ap_cfg.ap.password, KM_AP_PASS, sizeof(ap_cfg.ap.password) - 1);
    ap_cfg.ap.password[sizeof(ap_cfg.ap.password) - 1] = '\0';
    ap_cfg.ap.max_connection = KM_AP_MAX_CONN;
    ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap_cfg.ap.pmf_cfg.required = false;
    if (strlen(KM_AP_PASS) == 0) {
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));

    /* 按 NVS 配置：若启用 STA，必须在 esp_wifi_start() 之前完成 set_config
     * （STA 一旦 start 便禁止再 set_config，否则返回 ESP_ERR_WIFI_STATE）。
     * 实际 connect 由 WIFI_EVENT_STA_START 事件回调发起。 */
    config_t *cfgp = config_store_get();
    if (cfgp && cfgp->wifi.sta_enabled && cfgp->wifi.sta_ssid[0]) {
        wifi_config_t sta_cfg = {0};
        strncpy((char *)sta_cfg.sta.ssid, cfgp->wifi.sta_ssid, sizeof(sta_cfg.sta.ssid) - 1);
        strncpy((char *)sta_cfg.sta.password, cfgp->wifi.sta_pass, sizeof(sta_cfg.sta.password) - 1);
        sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    } else if (cfgp && cfgp->wifi.sta_enabled && !cfgp->wifi.sta_ssid[0]) {
        ESP_LOGW(TAG, "STA 已启用但 SSID 为空，跳过自动连接（请通过页面重新填写 SSID 并保存）");
    }

    s_ap_netif  = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    s_sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");

    /* 若 NVS 配置为静态 IP，在 start 前应用（DHCP 关闭 + 写入静态地址），保证开机自动重连也走静态 */
    apply_static_ip_from_config();

    /* 固定 AP 的 IP（192.168.4.1） */
    if (s_ap_netif) {
        esp_netif_ip_info_t ip;
        memset(&ip, 0, sizeof(ip));
        ip.ip.addr = ipaddr_addr(KM_AP_IP);
        ip.gw.addr = ipaddr_addr(KM_AP_GW);
        ip.netmask.addr = ipaddr_addr(KM_AP_NETMASK);
        esp_netif_dhcps_stop(s_ap_netif);
        esp_netif_set_ip_info(s_ap_netif, &ip);
        esp_netif_dhcps_start(s_ap_netif);
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "SoftAP 已启动：%s / %s (IP %s)", s_ap_ssid, KM_AP_PASS, KM_AP_IP);

    /* 禁用 STA 省电模式(modem-sleep)：必须在 esp_wifi_start() 之后设置才生效。
       本设备为插电工作，保持射频常 awake 既能让 WiFi 页面稳定发送，
       更重要的是避免 STA 休眠导致 2.4G 射频在 WiFi/BLE 之间反复切换、
       破坏 BLE HID 链路（表现为 Indicate Not Enabled / 鼠标报文失败）。
       该设置已验证不影响 BLE 连接稳定性。 */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    /* 应用可配的 WiFi 发射功率（单位 0.25dBm），默认 10dBm；降低近距离发热 */
    config_t *rcfg = config_store_get();
    if (rcfg) {
        int pwr = rcfg->radio.wifi_power_025dbm;
        if (pwr < 0)   pwr = 0;
        if (pwr > 84)  pwr = 84;   /* esp_wifi_set_max_tx_power 上限约 21dBm */
        esp_err_t perr = esp_wifi_set_max_tx_power((int8_t)pwr);
        if (perr == ESP_OK) {
            ESP_LOGI(TAG, "WiFi 发射功率已设置为 %d (%.1fdBm)", pwr, pwr * 0.25);
        } else {
            ESP_LOGW(TAG, "WiFi 发射功率设置失败(%s)", esp_err_to_name(perr));
        }
    }

    /* 记录开机基准，并初始化软时钟 */
    time_t now = 0;
    time(&now);
    s_boot_epoch = now;

    /* STA 的 set_config 必须在 esp_wifi_start() 之前完成（STA 一旦 start 便禁止再 set_config）。
     * 这里仅打印状态；真正发起 connect 由 WIFI_EVENT_STA_START 事件回调负责（见 on_wifi_event）。 */
    if (rcfg && rcfg->wifi.sta_enabled && rcfg->wifi.sta_ssid[0]) {
        ESP_LOGI(TAG, "已按配置并行启动 AP+STA，STA 将自动连接：%s", rcfg->wifi.sta_ssid);
    } else {
        ESP_LOGI(TAG, "未配置 STA，仅 AP 模式（AP 常开）");
    }

    /* 系统 heap 监控：每 30s 打印空闲/最小 heap，用于排查 wifi:mem fail 是否为真泄漏 */
    {
        esp_timer_handle_t heap_t = NULL;
        esp_timer_create_args_t ht = { .callback = &heap_monitor_timer_cb, .name = "heap_mon" };
        if (esp_timer_create(&ht, &heap_t) == ESP_OK) {
            esp_timer_start_periodic(heap_t, 30 * 1000 * 1000);
        }
    }

    return ESP_OK;
}

esp_err_t wifi_manager_set_sta(const char *ssid, const char *pass)
{
    return wifi_manager_set_sta_ex(ssid, pass, true, NULL, NULL, NULL, NULL);
}

esp_err_t wifi_manager_set_sta_ex(const char *ssid, const char *pass,
                                  bool dhcp,
                                  const char *ip, const char *gw,
                                  const char *netmask, const char *dns)
{
    if (ssid == NULL || pass == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    wifi_config_t sta_cfg = {0};
    strncpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid) - 1);
    strncpy((char *)sta_cfg.sta.password, pass, sizeof(sta_cfg.sta.password) - 1);
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    bool static_ip = (!dhcp && ip && ip[0] != '\0' && netmask && netmask[0] != '\0');
    if (static_ip) {
        esp_netif_ip_info_t info;
        memset(&info, 0, sizeof(info));
        info.ip.addr      = ipaddr_addr(ip);
        info.netmask.addr = ipaddr_addr(netmask);
        info.gw.addr      = (gw && gw[0] != '\0') ? ipaddr_addr(gw) : 0;
        if (info.ip.addr == 0 || info.netmask.addr == 0) {
            ESP_LOGW(TAG, "静态 IP/掩码非法，退回 DHCP：ip=%s mask=%s", ip, netmask);
            static_ip = false;
        } else {
            if (s_sta_netif) {
                esp_netif_dhcpc_stop(s_sta_netif);
                esp_netif_set_ip_info(s_sta_netif, &info);
                if (dns && dns[0] != '\0') {
                    esp_netif_dns_info_t dnsinfo;
                    memset(&dnsinfo, 0, sizeof(dnsinfo));
                    dnsinfo.ip.u_addr.ip4.addr = ipaddr_addr(dns);
                    dnsinfo.ip.type = ESP_IPADDR_TYPE_V4;
                    if (esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dnsinfo) == ESP_OK) {
                        ESP_LOGI(TAG, "静态 DNS 已设置：%s", dns);
                    } else {
                        ESP_LOGW(TAG, "静态 DNS 设置失败：%s", dns);
                    }
                }
                ESP_LOGI(TAG, "STA 静态地址已写入：IP=%s GW=%s MASK=%s", ip, gw ? gw : "", netmask);
            }
        }
    }

    /* 持久化 STA 凭据到 NVS：重启后自动重连路由器（sta_enabled=true） */
    config_t *cfg_store = config_store_get();
    if (cfg_store != NULL) {
        cfg_store->wifi.sta_enabled = true;
        cfg_store->wifi.sta_dhcp   = dhcp;
        strncpy(cfg_store->wifi.sta_ssid, ssid, sizeof(cfg_store->wifi.sta_ssid) - 1);
        cfg_store->wifi.sta_ssid[sizeof(cfg_store->wifi.sta_ssid) - 1] = '\0';
        strncpy(cfg_store->wifi.sta_pass, pass, sizeof(cfg_store->wifi.sta_pass) - 1);
        cfg_store->wifi.sta_pass[sizeof(cfg_store->wifi.sta_pass) - 1] = '\0';
        if (static_ip) {
            strncpy(cfg_store->wifi.sta_ip, ip, sizeof(cfg_store->wifi.sta_ip) - 1);
            cfg_store->wifi.sta_ip[sizeof(cfg_store->wifi.sta_ip) - 1] = '\0';
            strncpy(cfg_store->wifi.sta_gw, gw ? gw : "", sizeof(cfg_store->wifi.sta_gw) - 1);
            cfg_store->wifi.sta_gw[sizeof(cfg_store->wifi.sta_gw) - 1] = '\0';
            strncpy(cfg_store->wifi.sta_netmask, netmask, sizeof(cfg_store->wifi.sta_netmask) - 1);
            cfg_store->wifi.sta_netmask[sizeof(cfg_store->wifi.sta_netmask) - 1] = '\0';
            strncpy(cfg_store->wifi.sta_dns, dns ? dns : "", sizeof(cfg_store->wifi.sta_dns) - 1);
            cfg_store->wifi.sta_dns[sizeof(cfg_store->wifi.sta_dns) - 1] = '\0';
        } else {
            cfg_store->wifi.sta_ip[0]      = '\0';
            cfg_store->wifi.sta_gw[0]      = '\0';
            cfg_store->wifi.sta_netmask[0] = '\0';
            cfg_store->wifi.sta_dns[0]     = '\0';
        }
        esp_err_t s_err = config_store_save(cfg_store);
        if (s_err == ESP_OK) {
            ESP_LOGI(TAG, "STA 凭据已保存至 NVS，下次开机自动重连: %s (%s)",
                     ssid, dhcp ? "DHCP" : "静态IP");
        } else {
            ESP_LOGW(TAG, "STA 凭据保存失败(%s)，本次连接仍有效但重启后不会自动重连",
                     esp_err_to_name(s_err));
        }
    }

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    if (err != ESP_OK) {
        return err;
    }
    /* 保持 APSTA 模式（AP 继续常开），并发连 STA */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    s_sta_fail_count = 0;   /* 用户重新保存 WiFi：清零失败计数，允许再次自动重连 */
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "STA 连接请求失败(%s)，AP 仍可用", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "STA 连接请求已提交：%s（AP 保持常开，%s）",
             ssid, dhcp ? "DHCP" : "静态IP");
    return ESP_OK;
}

esp_err_t wifi_manager_disable_sta(void)
{
    esp_wifi_disconnect();
    ESP_LOGI(TAG, "STA 已禁用，AP 继续常开");
    return ESP_OK;
}

void wifi_manager_get_ap_ip(char *buf, size_t len)
{
    if (!buf || !len) {
        return;
    }
    if (s_ap_netif) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(s_ap_netif, &ip) == ESP_OK) {
            esp_ip4addr_ntoa(&ip.ip, buf, len);
            return;
        }
    }
    snprintf(buf, len, "0.0.0.0");
}

void wifi_manager_get_sta_ip(char *buf, size_t len)
{
    if (!buf || !len) {
        return;
    }
    if (s_sta_netif) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(s_sta_netif, &ip) == ESP_OK &&
            ip.ip.addr != 0) {
            esp_ip4addr_ntoa(&ip.ip, buf, len);
            return;
        }
    }
    snprintf(buf, len, "0.0.0.0");
}

void wifi_manager_get_mode_str(char *buf, size_t len)
{
    if (!buf || !len) {
        return;
    }
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) != ESP_OK) {
        snprintf(buf, len, "OFF");
        return;
    }
    if (mode == WIFI_MODE_APSTA) {
        snprintf(buf, len, "AP+STA");
    } else if (mode == WIFI_MODE_AP) {
        snprintf(buf, len, "AP");
    } else if (mode == WIFI_MODE_STA) {
        snprintf(buf, len, "STA");
    } else {
        snprintf(buf, len, "OFF");
    }
}

/* 获取运行时拼接后的 AP SSID（供日志/提示使用） */
void wifi_manager_get_ap_ssid(char *buf, size_t len)
{
    if (!buf || !len) {
        return;
    }
    snprintf(buf, len, "%s", s_ap_ssid);
}

/* ---------------- 手动校时 ----------------
 * 重新触发 SNTP 校时：若 STA 未联网则失败（无网络无法校时）；
 * 若已初始化则先停止再重新初始化，强制立即向 NTP 服务器同步一次。
 * 时区已固定为北京时间（CST-8），校时后 localtime 即为北京时间。
 */
esp_err_t wifi_manager_sync_time(void)
{
    int8_t mode = 0;
    if (esp_wifi_get_mode((wifi_mode_t *)&mode) != ESP_OK) {
        return ESP_FAIL;
    }
    if (mode != WIFI_MODE_STA && mode != WIFI_MODE_APSTA) {
        ESP_LOGW(TAG, "手动校时失败：STA 未启用/未连接");
        return ESP_FAIL;
    }
    /* 已初始化则重启 SNTP，强制重新同步 */
    if (s_sntp_started) {
        esp_sntp_stop();
        s_sntp_started = false;
    }
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "cn.ntp.org.cn");
    esp_sntp_setservername(2, "time.windows.com");
    esp_sntp_init();
    s_sntp_started = true;
    ESP_LOGI(TAG, "手动校时已触发（ntp.aliyun.com），稍候查看时间");
    return ESP_OK;
}

/* ---------------- 等待 STA 连上（供启动顺序协调用） ----------------
 * 阻塞最多 timeout_ms 等待 STA 拿到 IP。用于"先让 Wi-Fi 稳连、再起 BLE"，
 * 避免蓝牙先占用射频把 STA 踢掉（reason=203/205 交替必现问题）。
 * 若未启用 STA / 超时未连上，立即返回 false（不阻塞主流程，BLE 照常起）。 */
bool wifi_manager_wait_sta_connected(uint32_t timeout_ms)
{
    config_t *cfg = config_store_get();
    if (!cfg || !cfg->wifi.sta_enabled || !cfg->wifi.sta_ssid[0]) {
        return false;  /* 未配 STA，无需等 */
    }
    if (s_wifi_evt == NULL) {
        return false;
    }
    EventBits_t bits = xEventGroupWaitBits(s_wifi_evt, WIFI_CONNECTED_BIT,
                                          pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms));
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

/* ---------------- 浏览器本地时间校时（离线可用） ----------------
 * 由 Web 页面“同步浏览器时间”按钮调用：前端用 Date.now() 取得本机 epoch
 * 秒数 POST 上来，这里直接 settimeofday 写系统时钟。无需联网，AP-only
 * 场景下即可把设备时间校正为用户的真实本地时间（北京时间 UTC+8），
 * 供定时任务按真实时间启停。成功后 now > 1700000000，time_synced() 即返回 true。
 */
esp_err_t wifi_manager_set_time(time_t epoch)
{
    if (epoch < 1700000000) {
        ESP_LOGW(TAG, "设时拒绝：epoch 过早(%lld)，疑似无效", (long long)epoch);
        return ESP_ERR_INVALID_ARG;
    }
    struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
    if (settimeofday(&tv, NULL) != 0) {
        ESP_LOGE(TAG, "settimeofday 失败");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "已用浏览器本地时间校时：epoch=%lld", (long long)epoch);
    return ESP_OK;
}

/* ---------------- 当前本地时间字符串（北京时间 UTC+8） ----------------
 * 返回形如 "2026-08-12 15:04:05" 的字符串。未联网/未校时时返回开机软时钟。
 * 同时由 synced 指出当前是否为真实校时时间（now > 2024 年起的秒数）。
 */
void wifi_manager_get_time_str(char *buf, size_t len)
{
    if (!buf || !len) {
        return;
    }
    struct tm ti;
    bool synced = wifi_manager_get_local_time(&ti);
    (void)synced;
    snprintf(buf, len, "%04d-%02d-%02d %02d:%02d:%02d",
             ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
             ti.tm_hour, ti.tm_min, ti.tm_sec);
}

/* 返回当前是否为真实校时时间（true=已联网校时；false=软时钟） */
bool wifi_manager_time_synced(void)
{
    time_t now;
    time(&now);
    return (now > 1700000000);
}
