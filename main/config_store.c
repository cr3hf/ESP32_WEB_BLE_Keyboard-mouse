/*
 * 配置存储实现
 *
 * 所有配置打包为 config_t，以 NVS blob 形式存于命名空间 "km_cfg"（key="cfg"）。
 * 默认值在 load 时若 NVS 无有效数据则初始化并落盘，保证首次上电开箱即用。
 */
#include <string.h>

#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_err.h"

#include "config_store.h"
#include "defaults.h"

static const char *TAG = "CFG_STORE";

#define NVS_NS      "km_cfg"
#define NVS_KEY     "cfg"

/* 默认单词表：C/C++/Nordic 编程库 API 与常见变量名，空格分隔（末尾留一空格） */
static const char DEFAULT_WORD_LIST[] =
    "struct enum uint8_t int32_t bool const static void "
    "nrf_gpio_pin_set nrf_drv_timer_trigger nrf_saadc_sample "
    "sd_ble_gap_connect sdk_config.h app_timer_start "
    "vector string mutex handle context buffer pointer "
    "sensor_value device_handle callback flag retry "
    "CONFIG_NRFX_TIMER_ENABLED NRFX_UARTE_ENABLED "
    "nrfx_uarte_tx nrf_ble_scan_start "
    "std_map std_shared_ptr spi_transfer i2c_read "
    "power_state ble_conn_handle tx_queue rx_buffer ";


/* 内部单例：所有模块共享同一份配置（RAM 中） */
static config_t s_config;
/* 默认值快照：放置在 BSS（而非栈上），供旧版本配置迁移时补齐尾部新增字段。
 * config_t 体积约 2KB，两份放入栈会挤占 main 任务栈，故用静态存储。 */
static config_t s_defaults;

/* 默认权重（取自出厂配置文件） */
static void cfg_default_weights(action_weights_t *w)
{
    w->drag    = 0;
    w->click   = 50;
    w->wheel   = 50;
    w->arrow   = 30;
    w->rest    = 35;
    w->move    = 50;
    w->word    = 0;
    w->alt_tab = 0;   /* 切换程序：默认禁用，需用户在页面开启 */
}

/* 默认动作时间（取自出厂配置文件） */
static void cfg_default_timing(action_timing_t *t)
{
    /* 拖拽: 位移距离 + 分片步进 + 次数 + 间隔 + 结束延迟 */
    t->drag_repeat_min = 1;   t->drag_repeat_max = 5;
    t->drag_distance_min = 20; t->drag_distance_max = 100;      /* 单次拖拽总位移（逻辑像素） */
    t->drag_step_min = 10;    t->drag_step_max = 30;            /* 每次分片步进（每帧位移量） */
    t->drag_interval_min = 600; t->drag_interval_max = 1500;
    t->drag_end_delay_min = 500; t->drag_end_delay_max = 5000;
    /* 点击: 移动距离 + 分片步进 + 次数 + 按住 + 间隔 + 结束延迟 */
    t->click_repeat_min = 1;  t->click_repeat_max = 10;
    t->click_distance_min = 10; t->click_distance_max = 100;   /* 点击前移动到位移（逻辑像素） */
    t->click_step_min = 1;    t->click_step_max = 30;           /* 每次分片步进（每帧位移量） */
    t->click_hold_min = 20;   t->click_hold_max = 250;
    t->click_interval_min = 100; t->click_interval_max = 1000;
    t->click_end_delay_min = 1000; t->click_end_delay_max = 5000;
    /* 滚轮: 预移动距离 + 分片步进 + 次数 + 格数 + 间隔 + 结束延迟 */
    t->wheel_repeat_min = 1;  t->wheel_repeat_max = 5;
    t->wheel_distance_min = 10; t->wheel_distance_max = 100;    /* 滚轮前预移动位移（逻辑像素） */
    t->wheel_step_min = 1;    t->wheel_step_max = 30;           /* 每次分片步进（每帧位移量） */
    t->wheel_tick_min = 1;    t->wheel_tick_max = 8;
    t->wheel_interval_min = 100; t->wheel_interval_max = 500;
    t->wheel_end_delay_min = 1000; t->wheel_end_delay_max = 5000;
    /* 方向键: ARROW_REPEAT_* / ARROW_INTERVAL_* / ARROW_END_DELAY_* */
    t->arrow_repeat_min = 1;  t->arrow_repeat_max = 20;
    t->arrow_interval_min = 50; t->arrow_interval_max = 800;
    t->arrow_end_delay_min = 1000; t->arrow_end_delay_max = 5000;
    /* 休息: REST_DELAY_* */
    t->rest_delay_min = 1000; t->rest_delay_max = 20000;
    /* 滑动鼠标: 位移距离 + 分片步进 + 次数 + 间隔 + 结束延迟 */
    t->move_repeat_min = 1;   t->move_repeat_max = 20;
    t->move_distance_min = 10; t->move_distance_max = 30;       /* 单次滑动总位移（逻辑像素） */
    t->move_step_min = 1;     t->move_step_max = 15;            /* 每次分片步进（每帧位移量） */
    t->move_interval_min = 100; t->move_interval_max = 500;
    t->move_end_delay_min = 500; t->move_end_delay_max = 1000;
    /* LED: LED_BLINK_ON_MS / LED_FREQ_PER_1MIN_MS / LED_FREQ_MAX_MS / LED_BLINK_ONCE_MS / LED_BLINK_ONCE_GAP_MS */
    t->led_blink_on_ms = 80;
    t->led_freq_per_1min_ms = 50;
    t->led_freq_max_ms = 2000;
    t->led_blink_once_ms = 200;
    t->led_blink_once_gap_ms = 200;
    /* 打字: 词数 + 字符延迟 + 空格延迟 + 间隔 + 结束延迟 */
    t->word_repeat_min = 1;   t->word_repeat_max = 5;
    t->word_char_delay_min = 40; t->word_char_delay_max = 700;
    t->word_space_delay_min = 40; t->word_space_delay_max = 1000;
    t->word_interval_min = 500; t->word_interval_max = 2000;
    t->word_end_delay_min = 500; t->word_end_delay_max = 1200;
    /* 切换程序(Alt+Tab): 切换次数(0~1) + 按键间隔(500~1000) + 动作后休息(700~1500) */
    t->alt_tab_repeat_min = 0;  t->alt_tab_repeat_max = 1;
    t->alt_tab_interval_min = 500; t->alt_tab_interval_max = 1000;
    t->alt_tab_end_delay_min = 700; t->alt_tab_end_delay_max = 1500;
}

/* 默认运动模式（取自 action_engine.h 原宏默认值） */
static void cfg_default_motion(motion_cfg_t *m)
{
    m->screen_scale_pct = 125;   /* SCREEN_SCALE_PCT 默认 125% */
    m->pos_limit_x = 400;        /* MOUSE_POS_LIMIT_X 默认 400 */
    m->pos_limit_y = 200;        /* MOUSE_POS_LIMIT_Y 默认 200 */
    m->home_corner = 0;          /* MOUSE_HOME_CORNER 默认 CORNER_TOP_RIGHT */
    m->home_push_px = 2000;      /* MOUSE_HOME_PUSH_PX 默认 2000 */
    m->home_back_x = 400;        /* MOUSE_HOME_BACK_X 默认 400 */
    m->home_back_y = 200;        /* MOUSE_HOME_BACK_Y 默认 200 */
}

static void cfg_set_defaults(config_t *c)
{
    memset(c, 0, sizeof(*c));
    c->version = CFG_VERSION;
    c->active_profile = 0;
    c->run_mode = RUN_MODE_RANDOM;

    /* 3 套参数：每套默认一致（动作时间 / 运动模式 / 随机权重），后续可在页面分别调整 */
    for (int i = 0; i < PROFILE_COUNT; i++) {
        cfg_default_weights(&c->profiles[i].weights);
        cfg_default_timing(&c->profiles[i].timing);
        cfg_default_motion(&c->profiles[i].motion);
    }

    /* 默认序列：取自出厂配置文件（15 项，顺序循环） */
    {
        static const uint8_t dft_seq[] = {
            5, 1, 2, 5, 1, 2, 5, 1, 3, 2, 1, 5, 1, 2, 4
        };
        const int n = (int)(sizeof(dft_seq) / sizeof(dft_seq[0]));
        c->sequence.count = (n <= ACT_SEQ_MAX) ? (uint8_t)n : ACT_SEQ_MAX;
        for (int i = 0; i < c->sequence.count; i++) {
            c->sequence.actions[i] = dft_seq[i];
        }
    }
    c->sequence.cycle = CYCLE_ORDERED;

    c->timer_count = 0;
    memset(c->timers, 0, sizeof(c->timers));
    /* 默认单条定时规则（取自出厂配置文件，disabled） */
    c->timer_count = 1;
    c->timers[0].type = TIMER_START_STOP;
    c->timers[0].enabled = false;
    c->timers[0].hour = 18;
    c->timers[0].minute = 32;
    c->timers[0].action_id = 0;
    c->timers[0].ss_action = 1;
    c->timers[0].period_min = 1;

    /* STA 出厂默认凭据：来自 defaults.h（公开为空）或 main/local_defs.h（本地私有）。
     * 为空则默认不启用 STA，由用户通过页面填写。 */
    c->wifi.sta_enabled = (DEF_STA_SSID[0] != '\0');
    strncpy(c->wifi.sta_ssid, DEF_STA_SSID, sizeof(c->wifi.sta_ssid) - 1);
    c->wifi.sta_ssid[sizeof(c->wifi.sta_ssid) - 1] = '\0';
    strncpy(c->wifi.sta_pass, DEF_STA_PASS, sizeof(c->wifi.sta_pass) - 1);
    c->wifi.sta_pass[sizeof(c->wifi.sta_pass) - 1] = '\0';
    c->wifi.sta_dhcp = true;   /* 默认 DHCP 自动获取 IP */
    c->wifi.sta_ip[0] = '\0';
    c->wifi.sta_gw[0] = '\0';
    c->wifi.sta_netmask[0] = '\0';
    c->wifi.sta_dns[0] = '\0';

    /* 射频功率默认（取自出厂配置文件）：WiFi=10dBm、BLE=-3dBm */
    c->radio.wifi_power_025dbm = 40;   /* 40*0.25=10dBm */
    c->radio.ble_power_level   = 5;    /* ESP32-S3: ESP_PWR_LVL_P3=+3dBm */


    /* 默认单词表 */
    strncpy(c->word.list, DEFAULT_WORD_LIST, sizeof(c->word.list) - 1);
    c->word.list[sizeof(c->word.list) - 1] = '\0';
}

esp_err_t config_store_load(void)
{
    cfg_set_defaults(&s_defaults);
    s_config = s_defaults;

    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS 打开失败(%s)，使用默认配置", esp_err_to_name(err));
        return err;
    }

    size_t len = sizeof(s_config);
    err = nvs_get_blob(h, NVS_KEY, &s_config, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "NVS 无配置，写入默认配置");
        err = config_store_save(&s_config);
        nvs_close(h);
        return err;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS 读取失败(%s)，使用默认配置", esp_err_to_name(err));
        nvs_close(h);
        return err;
    }

    if (len > sizeof(s_config)) {
        /* 存储的配置比当前结构体还大（降级固件等异常）：无法安全解析，重置为默认 */
        ESP_LOGW(TAG, "配置长度异常(len=%d > %d)，重置为默认", (int)len, (int)sizeof(s_config));
        s_config = s_defaults;
        err = config_store_save(&s_config);
    } else if (s_config.version != CFG_VERSION || len != sizeof(s_config)) {
        /* 结构版本或长度不一致：旧配置无法按当前布局安全解析。
         * 曾在 action_timing_t 子结构尾部追加字段，导致其后的 word/radio 偏移错位，
         * 字节级迁移会把单词表数据污染进参数区（实测 alt_tab 次数变成 1696625763）。
         * 故不再做字节级迁移，直接整体重置为默认值并落盘，保证参数干净、立即可用。
         * 代价：用户原有配置（含 WiFi 凭据）会被清空，需重新在页面配置。 */
        ESP_LOGW(TAG, "配置版本 %lu→%d 不兼容，已重置为默认值（含 WiFi，请重新配网）",
                 (unsigned long)s_config.version, CFG_VERSION);
        s_config = s_defaults;
        err = config_store_save(&s_config);
    } else {
        ESP_LOGI(TAG, "配置加载成功：mode=%d timer=%d", s_config.run_mode, s_config.timer_count);
    }

    /* 兜底：确保 word.list 以 '\0' 结尾（旧 NVS 数据可能未终止，避免 config_to_json 越界读取） */
    s_config.word.list[sizeof(s_config.word.list) - 1] = '\0';
    nvs_close(h);
    return ESP_OK;
}

esp_err_t config_store_save(const config_t *cfg)
{
    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 打开失败(%s)，配置未保存", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_blob(h, NVS_KEY, cfg, sizeof(*cfg));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 写入失败(%s)", esp_err_to_name(err));
        nvs_close(h);
        return err;
    }
    err = nvs_commit(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 提交失败(%s)", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "配置已保存");
    }
    nvs_close(h);
    return err;
}

config_t *config_store_get(void)
{
    return &s_config;
}

esp_err_t config_store_reset_to_defaults(void)
{
    cfg_set_defaults(&s_config);
    return config_store_save(&s_config);
}

const char *config_store_default_word_list(void)
{
    return DEFAULT_WORD_LIST;
}

const action_weights_t *config_store_weights(void)
{
    return &config_store_active_profile()->weights;
}

profile_t *config_store_active_profile(void)
{
    int i = (int)s_config.active_profile;
    if (i < 0 || i >= PROFILE_COUNT) {
        i = 0;
    }
    return &s_config.profiles[i];
}
