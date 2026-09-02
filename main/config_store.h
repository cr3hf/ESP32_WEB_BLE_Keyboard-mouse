/*
 * 配置存储（config_store）
 *
 * 集中管理所有可经 Web 配置的参数：运行模式、随机权重、动作序列、
 * 定时规则、WiFi（STA）凭据。所有配置打包为一个固定版本号的结构体，
 * 以 NVS blob 形式持久化到命名空间 "km_cfg"，断电不丢失。
 *
 * 该模块是动作引擎与 Web 服务之间共享的"配置契约"：
 *   - action_engine 读取此处加载到 RAM 的变量来决定调度行为；
 *   - web_server 通过 config_store_get() 读写并 config_store_save() 落盘。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- 动作 ID（与 action_engine.c 中 action_id_t 一致） ---------------- */
typedef enum {
    ACT_DRAG = 0,   /* 拖拽 */
    ACT_CLICK,      /* 点击 */
    ACT_WHEEL,      /* 滚轮 */
    ACT_ARROW,      /* 方向键 */
    ACT_REST,       /* 休息 */
    ACT_MOVE,       /* 滑动鼠标 */
    ACT_WORD,       /* 打字：从单词表随机抽词，逐字符发送 */
    ACT_ALT_TAB,    /* 切换程序：Alt+Tab 组合键（次数可配 0~1） */
    ACT_COUNT,
} act_id_t;

/* ---------------- 运行模式 ---------------- */
typedef enum {
    RUN_MODE_RANDOM = 0,   /* 随机模式：按权重概率抽取动作 */
    RUN_MODE_SEQUENCE,     /* 序列模式：按用户编排的顺序循环执行 */
} run_mode_t;

/* ---------------- 序列循环策略 ---------------- */
typedef enum {
    CYCLE_ORDERED = 0,     /* 顺序循环：A→B→C→A... */
    CYCLE_SHUFFLE,         /* 乱序洗牌：每轮开始前打乱 */
    CYCLE_ONCE,            /* 单次：按编排顺序执行一轮后自动停止 */
} cycle_mode_t;

/* ---------------- 定时规则类型 ---------------- */
typedef enum {
    TIMER_START_STOP = 0,  /* 定时启停：到 start 时刻启动、到 stop 时刻停止 */
    TIMER_SINGLE_ACTION,   /* 定时单动作：到点执行一次指定动作 */
    TIMER_PERIODIC,        /* 周期循环：每 period_min 分钟执行一轮序列 */
} timer_type_t;

/* ---------------- 随机权重（合计 100，0=禁用该类动作） ---------------- */
typedef struct {
    int drag;
    int click;
    int wheel;
    int arrow;
    int rest;
    int move;
    int word;       /* 打字（随机单词） */
    int alt_tab;    /* 切换程序（Alt+Tab） */
} action_weights_t;

/* ---------------- 动作序列（上限 64 项） ---------------- */
#define ACT_SEQ_MAX  64
typedef struct {
    uint8_t     count;                       /* 实际项数，0~ACT_SEQ_MAX */
    uint8_t     actions[ACT_SEQ_MAX];        /* act_id_t 列表 */
    cycle_mode_t cycle;                       /* 顺序 / 洗牌 */
} action_seq_t;

/* ---------------- 单条定时规则（上限 64 条） ---------------- */
#define TIMER_RULE_MAX  64
typedef struct {
    timer_type_t type;
    bool         enabled;
    uint8_t      hour;        /* 启停/单动作的触发小时 [0,23] */
    uint8_t      minute;      /* 启停/单动作的触发分钟 [0,59] */
    uint8_t      action_id;   /* 单动作类型（act_id_t），仅 TIMER_SINGLE_ACTION 用 */
    uint8_t      ss_action;   /* 启停动作：0=启动, 1=停止，仅 TIMER_START_STOP 用 */
    uint16_t     period_min;  /* 周期循环分钟数，仅 TIMER_PERIODIC 用（>=1） */
} timer_rule_t;

/* ---------------- WiFi（STA）凭据与地址 ---------------- */
#define WIFI_SSID_MAX  32
#define WIFI_PASS_MAX  64
#define IP_STR_MAX     16   /* "255.255.255.255\0" 恰好 16 字节 */
typedef struct {
    bool  sta_enabled;                 /* 是否启用 STA 连接路由器 */
    char  sta_ssid[WIFI_SSID_MAX];     /* 路由器 SSID（含结束符） */
    char  sta_pass[WIFI_PASS_MAX];     /* 路由器密码（含结束符） */
    bool  sta_dhcp;                    /* true=DHCP 自动获取（默认）；false=静态 IP */
    char  sta_ip[IP_STR_MAX];          /* 静态 IP（sta_dhcp=false 时生效） */
    char  sta_gw[IP_STR_MAX];          /* 静态网关 */
    char  sta_netmask[IP_STR_MAX];     /* 静态子网掩码 */
    char  sta_dns[IP_STR_MAX];         /* 静态 DNS（可选，留空则用系统默认） */
} wifi_cfg_t;

/* ---------------- 射频发射功率（Web 可配；可保存） ----------------
 * WiFi：esp_wifi_set_max_tx_power 单位是 0.25dBm，范围 0~84（0~21dBm）。
 * BLE ：esp_ble_tx_power_set 的档位枚举 esp_power_level_t。
 *       注意：枚举值随芯片不同而异！ESP32-S3 上：
 *       ESP_PWR_LVL_N3 = 3 对应 -3dBm，ESP_PWR_LVL_P3 = 5 对应 +3dBm 等。
 *       （esp32h4 上 N3=8，请勿混淆）存储为整型，蓝牙应用时转换为枚举。
 * 默认：WiFi 10dBm、BLE -3dBm（已降档以降发热）。
 */
#define RADIO_WIFI_POWER_DFT  40   /* 40*0.25=10dBm（esp_wifi_set_max_tx_power 单位 0.25dBm） */
#define RADIO_BLE_POWER_DFT   5    /* ESP32-S3: ESP_PWR_LVL_P3 = 5 对应 +3dBm */
typedef struct {
    int wifi_power_025dbm;   /* WiFi 最大发射功率，单位 0.25dBm */
    int ble_power_level;     /* BLE 发射功率档位（esp_power_level_t 枚举值） */
} radio_cfg_t;

/* ---------------- 动作时间参数（Web 可配；宏为默认值） ----------------
 * 每组 *_min 与 *_max 对应“在 [min,max] 闭区间内随机取值”。
 * repeat/tick 为次数/格数，同样可配。宏默认值见 action_engine.h。
 */
typedef struct {
    /* 拖拽 */
    int drag_repeat_min, drag_repeat_max;
    int drag_distance_min, drag_distance_max;   /* 单次拖拽位移（逻辑像素） */
    int drag_step_min, drag_step_max;           /* 每次分片步进（每帧位移量，≤127） */
    int drag_interval_min, drag_interval_max;
    int drag_end_delay_min, drag_end_delay_max;
    /* 点击 */
    int click_repeat_min, click_repeat_max;
    int click_distance_min, click_distance_max; /* 单次移动到位移（逻辑像素） */
    int click_step_min, click_step_max;         /* 每次分片步进（每帧位移量，≤127） */
    int click_hold_min, click_hold_max;
    int click_interval_min, click_interval_max;
    int click_end_delay_min, click_end_delay_max;
    /* 滚轮 */
    int wheel_repeat_min, wheel_repeat_max;
    int wheel_distance_min, wheel_distance_max; /* 滚轮前/后/横向预移动位移（逻辑像素） */
    int wheel_step_min, wheel_step_max;         /* 每次分片步进（每帧位移量，≤127） */
    int wheel_tick_min, wheel_tick_max;
    int wheel_interval_min, wheel_interval_max;
    int wheel_end_delay_min, wheel_end_delay_max;
    /* 方向键 */
    int arrow_repeat_min, arrow_repeat_max;
    int arrow_interval_min, arrow_interval_max;
    int arrow_end_delay_min, arrow_end_delay_max;
    /* 休息 */
    int rest_delay_min, rest_delay_max;
    /* 滑动鼠标 */
    int move_repeat_min, move_repeat_max;
    int move_distance_min, move_distance_max;   /* 单次滑动总位移（逻辑像素） */
    int move_step_min, move_step_max;           /* 每次分片步进（每帧位移量，≤127） */
    int move_interval_min, move_interval_max;
    int move_end_delay_min, move_end_delay_max;
    /* 状态 LED 闪烁时间 */
    int led_blink_on_ms;
    int led_freq_per_1min_ms;
    int led_freq_max_ms;
    int led_blink_once_ms;
    int led_blink_once_gap_ms;
    /* 打字（单词） */
    int word_repeat_min, word_repeat_max;     /* 单次动作打字词的个数（随机区间） */
    int word_char_delay_min, word_char_delay_max;   /* 字符间延迟（ms） */
    int word_space_delay_min, word_space_delay_max; /* 词尾空格延迟（ms） */
    int word_interval_min, word_interval_max; /* 一次动作结束后间隔（ms） */
    int word_end_delay_min, word_end_delay_max;
    /* 切换程序（Alt+Tab）：切换次数 + 每次按键间隔 + 动作后休息时间 */
    int alt_tab_repeat_min, alt_tab_repeat_max;         /* 切换次数（0~1 次） */
    int alt_tab_interval_min, alt_tab_interval_max;     /* 每次按键之间的间隔（ms） */
    int alt_tab_end_delay_min, alt_tab_end_delay_max;   /* 动作结束后的休息时间（ms） */
} action_timing_t;

/* ---------------- 运动模式参数（Web 可配；随 3 套参数各自独立） ----------------
 * 对应 action_engine.h 中原硬编码宏，提升为可配置字段，便于不同屏幕/多屏布局各存一套。
 *   screen_scale_pct : 显示缩放(%)，折算有效活动范围（缩放越大，允许逻辑位移越小）
 *   pos_limit_x/y    : 活动范围半宽/半高（相对屏幕中心，像素），按缩放折算为有效范围
 *   home_corner      : 复位撞角（0=右上 1=左上 2=右下 3=左下），见 action_engine.c mouse_corner_t
 *   home_push_px     : 复位撞角推程（发送位移计数，越大越能越过边界被系统截停）
 *   home_back_x/y    : 回中心经验值（X/Y 独立，决定最终落点）
 */
typedef struct {
    int screen_scale_pct;
    int pos_limit_x;
    int pos_limit_y;
    int home_corner;
    int home_push_px;
    int home_back_x;
    int home_back_y;
} motion_cfg_t;

/* ---------------- 打字（单词）配置 ----------------
 * 单词表以“单个 C 字符串”存储，词与词之间用空格分隔，结尾也允许有空格。
 * 运行时按空格切分为单词，每次动作从中随机抽词逐字符发送，词后追加空格。
 */
#define WORD_LIST_MAX  768   /* 单词表最大字符数（含分隔空格与结束符） */
typedef struct {
    char list[WORD_LIST_MAX];  /* 空格分隔的单词表原始文本 */
} word_cfg_t;

/* ---------------- 单套参数（随 3 套参数各自独立） ----------------
 * 仅“动作时间、运动模式、随机权重”三类随套数变化；其余（序列/定时/WiFi/单词/功率）共用。
 */
#define PROFILE_COUNT  3
typedef struct {
    action_weights_t  weights;   /* 随机模式权重 */
    action_timing_t   timing;    /* 动作时间参数 */
    motion_cfg_t      motion;    /* 运动模式参数 */
} profile_t;

/* ---------------- 顶层配置结构 ----------------
 * 版本历史：
 *   9  —— 打字（单词）动作、射频功率等
 *   10 —— 新增“切换程序(Alt+Tab)”动作：weights.alt_tab 与 timing.alt_tab_* 三组参数
 *   11 —— 修复 v10 在 action_timing_t 子结构尾部追加字段导致其后 word/radio 偏移错位、
 *        旧配置字节级迁移时参数被单词表数据污染（实测 alt_tab 次数变成 1696625763 等乱值）。
 *        改为“版本或长度不一致即整体重置为默认值”，不再做字节级迁移，杜绝错位损坏。
 *   12 —— 引入“3 套参数(profiles)”：仅动作时间/运动模式/随机权重随套数独立，
 *        其余参数共用；新增 active_profile（选择即生效，其余套只保存不生效）、motion_cfg_t。
 * 约定：旧配置与新布局不兼容时统一重置为默认（会清空 WiFi 凭据等，需重新在页面配置）。
 */
#define CFG_VERSION  12
typedef struct {
    uint32_t        version;            /* 结构版本，便于后续扩展兼容 */
    uint8_t         active_profile;     /* 当前生效的参数套序号 0~(PROFILE_COUNT-1)（选择即生效，其余套只保存不生效） */
    run_mode_t      run_mode;           /* 随机 / 序列（共用，不随套数变化） */
    profile_t       profiles[PROFILE_COUNT]; /* 3 套独立参数：时间 / 运动 / 权重 */
    action_seq_t    sequence;           /* 序列模式编排（共用） */
    timer_rule_t    timers[TIMER_RULE_MAX];
    uint8_t         timer_count;        /* 实际规则数 0~TIMER_RULE_MAX */
    wifi_cfg_t      wifi;               /* STA 凭据（共用） */
    word_cfg_t      word;               /* 打字单词表（共用） */
    radio_cfg_t     radio;              /* 射频发射功率（共用） */
} config_t;

/**
 * @brief 加载配置：从 NVS 读 blob；不存在或损坏则用默认值初始化并落盘。
 *        必须在 web_server / action_engine 应用配置之前调用。
 */
esp_err_t config_store_load(void);

/**
 * @brief 保存配置：写 NVS blob 并提交。成功返回 ESP_OK。
 */
esp_err_t config_store_save(const config_t *cfg);

/**
 * @brief 将全部参数恢复为默认值并立即写入 NVS（无需再点“保存配置”）。
 *        用于“恢复出厂设置”。注意：会清空 WiFi 凭据、单词表、功率等用户配置。
 */
esp_err_t config_store_reset_to_defaults(void);

/**
 * @brief 获取配置指针（指向内部单例，可安全读写后调用 save）。
 */
config_t *config_store_get(void);

/**
 * @brief 返回当前生效套数（active_profile）的参数指针（指向内部单例）。
 *        自动把 active_profile 钳制到 [0, PROFILE_COUNT-1]，避免越界。
 */
profile_t *config_store_active_profile(void);

/**
 * @brief 用当前配置填充默认权重副本（供动作引擎读取）。
 *        返回指向内部 weights 的指针。
 */
const action_weights_t *config_store_weights(void);

/**
 * @brief 返回内置默认单词表（C/C++/Nordic 编程库 API）。
 *        当用户单词表为空时，动作引擎与页面回退使用该表。
 */
const char *config_store_default_word_list(void);

#ifdef __cplusplus
}
#endif
