/*
 * 随机动作引擎实现
 *
 * 单任务：按概率随机抽取 5 类动作并执行；KEY2 启动 / KEY0 停止；
 * 任意延迟与循环均可被 STOP / 蓝牙断开中断（最长延迟 = 一次分片延迟）。
 * 每个动作退出时统一调用 action_release_all() 释放全部按键，杜绝残留。
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_random.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"

#include "ble_hid.h"
#include "led_status.h"
#include "action_engine.h"

static const char *TAG = "ACTION_ENGINE";

#include "config_store.h"
#include "wifi_manager.h"

/* EventGroup 位定义 */
#define RUN_BIT        (1 << 0)   /* 用户已按 KEY2 启动 */
#define STOP_BIT       (1 << 1)   /* 用户按 KEY0 停止（仅用于唤醒延迟，逻辑主看 RUN_BIT） */
#define DISCONNECT_BIT (1 << 2)   /* 蓝牙断开，需要中止当前动作 */

/* 动作 ID 复用 config_store.h 的 act_id_t（避免重复定义枚举常量） */
#include <time.h>
typedef act_id_t action_id_t;

/* ---------------- 全局状态 ---------------- */
static EventGroupHandle_t s_action_evt = NULL;
static bool              s_task_created = false;

/* 当前/最近一次执行的动作（供 Web 状态页展示） */
static action_id_t s_current_action = ACT_REST;
/* 当前“休息”动作结束的绝对时刻(us)；0 表示当前不在休息中。供 /api/status 展示休息剩余时间。 */
static int64_t s_rest_end_us = 0;
static const char *s_action_names[] = {
    "拖拽",   /* ACT_DRAG  = 0 */
    "点击",   /* ACT_CLICK = 1 */
    "滚轮",   /* ACT_WHEEL = 2 */
    "方向键", /* ACT_ARROW = 3 */
    "休息",   /* ACT_REST  = 4 */
    "滑动鼠标", /* ACT_MOVE = 5 */
    "打字",   /* ACT_WORD  = 6 */
    "切换程序", /* ACT_ALT_TAB = 7 */
};

/* 当前动作的重复进度（供 Web 状态页展示，如“点击 2/10”）。
 * s_action_total=本次动作总次数；s_action_index=当前正在执行第几次（1..total）。
 * 休息等无重复次数的动作 total=0，进度显示为空。 */
static int s_action_total = 0;
static int s_action_index = 0;

void action_engine_set_current_action(act_id_t id)
{
    s_current_action = id;
    s_action_total = 0;
    s_action_index = 0;
}

/* 动作开始：设置本次总次数，进度归零 */
void action_engine_set_progress_total(int total)
{
    s_action_total = total;
    s_action_index = 0;
}

/* 每执行一次重复时调用，推进当前进度 */
void action_engine_tick_progress(void)
{
    if (s_action_total > 0) {
        if (s_action_index < s_action_total) {
            s_action_index++;
        }
    }
}

const char *action_engine_current_action_name(void)
{
    if (s_current_action >= ACT_DRAG && s_current_action <= ACT_ALT_TAB) {
        return s_action_names[s_current_action - ACT_DRAG];
    }
    return "休息";
}

/* 返回当前动作总次数（纯数字字符串，如 "20"）；无次数信息时返回空串。
 * 中文“（次）”由前端拼接，避免 UTF-8 多字节触发 snprintf 截断告警。 */
const char *action_engine_current_action_progress(void)
{
    if (s_action_total > 0) {
        static char buf[16];
        snprintf(buf, sizeof(buf), "%d", s_action_total);
        return buf;
    }
    return "";
}

/* 返回当前“休息”剩余时间（微秒）；若当前不在休息或已结束则返回 0。
 * 供 /api/status 在状态为“休息”时展示倒计时。 */
int64_t action_engine_current_rest_remaining_us(void)
{
    if (s_rest_end_us == 0) {
        return 0;
    }
    int64_t remain = s_rest_end_us - esp_timer_get_time();
    return remain > 0 ? remain : 0;
}

/* 运行时间额度：到期时间戳(us)。为 0 表示无额度（未运行/已到期）。
 * Key1 短按累加额度；任务循环检测到当前时间超过该值则自动停止。 */
static int64_t s_run_deadline_us = 0;
/* 最近一次“定时停止”规则解析出的绝对停止时刻(us)。
 * 0 表示当前没有生效的定时停止任务。与 s_run_deadline_us 取更早者作为实际到期点，
 * 从而让 LED 按“距离停止还有多久”来闪烁倒计时。 */
static int64_t s_sched_stop_deadline_us = 0;
/* 累计运行额度（分钟），用于 Key1 闪烁间隔换算与日志。 */
static int32_t s_runtime_minutes = 0;
/* 上次按分钟闪烁时对应的剩余整分钟数；用于每分钟一次的剩余时间提示，
 * 仅在计时器（额度）激活时有效。 */
static int32_t s_last_blink_minute = -1;

/* 前向声明：运行额度计时器清零 */
static void reset_runtime(void);
/* 前向声明：打印运行剩余时间日志 */
static void log_remaining_time(void);
/* 前向声明：独立 LED 控制任务 */
static void led_control_task(void *arg);
/* 前向声明：通知 LED 任务“闪一下”确认（Key1 短按触发） */
static void led_notify_blink_once(void);
/* 前向声明：按 action_id 执行一个动作原语（供序列/单动作复用） */
static void exec_action_by_id(uint8_t action_id);
/* 前向声明：定时调度扫描任务 */
static void scheduler_task(void *arg);

/* ---------------- Web/NVS 可配状态（运行时覆盖宏默认） ---------------- */
static run_mode_t       s_run_mode   = RUN_MODE_RANDOM;
static action_weights_t s_weights = {                 /* 默认 = 头文件宏值 */
    ACT_W_DRAG, ACT_W_CLICK, ACT_W_WHEEL, ACT_W_ARROW, ACT_W_REST, ACT_W_MOVE, ACT_W_WORD,
    ACT_W_ALT_TAB,
};
static action_seq_t     s_sequence;                   /* 序列模式编排 */
static uint8_t          s_seq_cursor = 0;             /* 序列游标 */
static uint8_t          s_seq_shuffle[ACT_SEQ_MAX];   /* 洗牌用临时游标顺序 */
static uint8_t          s_seq_shuffle_len = 0;        /* 当前轮洗牌长度 */
static bool             s_seq_once_stop = false;      /* 单次循环：一轮执行完自动停止标记 */

/* 清零运行额度计时器：停止/重置时调用，避免残留额度导致下次启动即停。 */
static void reset_runtime(void)
{
    s_run_deadline_us = 0;
    s_sched_stop_deadline_us = 0;
    s_runtime_minutes = 0;
    s_last_blink_minute = -1;
}

/* ---------------- LED 控制（独立任务） ----------------
 * LED 不再由动作引擎零散控制，改由专门的 led_control_task 统一驱动：
 *   - 非定时运行（deadline==0 且运行中）→ 常亮
 *   - 定时运行 → 按剩余时间决定闪烁频率：每 2 分钟对应 100ms 间隔
 *     （如 10 分钟 → 500ms）；剩余越短闪得越快
 *   - 停止 → 灭
 *   - 收到“闪一下”通知（Key1 短按）→ 插入一次确认闪烁，不跟时间挂钩
 */
#define LED_BLINK_ON_MS         80      /* 单次点亮时长(ms) */
#define LED_FREQ_PER_1MIN_MS    50      /* 每 1 分钟剩余对应的闪烁间隔(ms)，默认 50 */
#define LED_FREQ_MAX_MS         2000    /* 闪烁间隔上限(ms)，最长 2s 闪一次 */
#define LED_BLINK_ONCE_MS       200     /* “闪一下”确认时长(ms) */
#define LED_BLINK_ONCE_GAP_MS   200     /* “闪一下”确认灭间隔(ms) */
static EventGroupHandle_t s_led_evt = NULL;
#define LED_BLINK_ONCE_BIT  (1 << 0)

/* 校时保护：刚成功校时（浏览器/网络）后的一段时间窗口内，
 * 禁止定时任务触发，避免时间跳变导致“误以为过期而补执行”或恰好命中触发分钟。
 * 用 wifi_manager_time_synced() 的 false->true 边沿检测启动保护窗口。 */
static bool     s_prev_synced = false;
static int64_t  s_timeset_guard_until = 0;   /* esp_timer 绝对微秒，0 表示无保护 */
#define TIMESET_GUARD_US  (10ULL * 1000 * 1000)   /* 校时后保护 10 秒 */

/* 取当前运行剩余时间（微秒）；无额度返回 0。
 * 实际到期点 = Key1 累加额度 与 最近定时停止时刻 中更早的一个（取 min），
 * 这样：即使只配置了“定时停止”而没有 Key1 额度，运行中也会按距离停止还有
 * 多久来闪烁倒计时；若同时有两者，则 whichever 先到期先停。 */
static int64_t get_remaining_us(void)
{
    int64_t now = esp_timer_get_time();
    int64_t eff = 0;
    if (s_run_deadline_us != 0 && (eff == 0 || s_run_deadline_us < eff)) {
        eff = s_run_deadline_us;
    }
    if (s_sched_stop_deadline_us != 0 && (eff == 0 || s_sched_stop_deadline_us < eff)) {
        eff = s_sched_stop_deadline_us;
    }
    if (eff == 0) {
        return 0;
    }
    int64_t remain = eff - now;
    return remain > 0 ? remain : 0;
}

/* 打印运行剩余时间（仅在运行且存在到期点：Key1 额度 或 定时停止 任一有效） */
static void log_remaining_time(void)
{
    if ((xEventGroupGetBits(s_action_evt) & RUN_BIT) == 0) {
        return;
    }
    if (s_run_deadline_us == 0 && s_sched_stop_deadline_us == 0) {
        return;   /* 非定时运行，无剩余时间概念 */
    }
    int64_t remain_us = get_remaining_us();
    int32_t remain_min = (int32_t)(remain_us / 1000000LL / 60);
    int32_t remain_sec = (int32_t)((remain_us / 1000000LL) % 60);
    ESP_LOGI(TAG, "[动作结束·运行剩余时间] %ld 分 %ld 秒",
             (long)remain_min, (long)remain_sec);
}

/* 通知 LED 任务“闪一下”确认（Key1 短按触发），不跟时间挂钩 */
static void led_notify_blink_once(void)
{
    if (s_led_evt != NULL) {
        xEventGroupSetBits(s_led_evt, LED_BLINK_ONCE_BIT);
    }
}

/* 逻辑光标位置（相对屏幕中心，单位像素）。
 * 仅用于把动作内移动钳制在 MOUSE_POS_LIMIT_X/Y 范围内，不读写真实光标。 */
static int32_t s_cur_x = 0;
static int32_t s_cur_y = 0;

/* 将坐标钳制到允许活动范围（相对中心）。
 * 有效范围 = 基准范围按屏幕缩放折算：缩放越大，允许的逻辑位移越小，
 * 这样真实光标（受系统缩放影响）才不会移出用户期望的物理边界。
 * 全程（所有动作、跨多次移动）都受此钳制，s_cur 永远落在 [ -EFF, +EFF ] 内。 */
/* 有效活动半范围（按当前生效套的缩放折算），供 clamp / 随机取点使用 */
static void active_eff_limits(int32_t *ex, int32_t *ey)
{
    const motion_cfg_t *m = &config_store_active_profile()->motion;
    int scale = (m->screen_scale_pct > 0) ? m->screen_scale_pct : 100;
    *ex = (int32_t)m->pos_limit_x * 100 / scale;
    *ey = (int32_t)m->pos_limit_y * 100 / scale;
}
static int32_t clamp_x(int32_t x)
{
    int32_t ex, ey;
    active_eff_limits(&ex, &ey);
    if (x > ex) return ex;
    if (x < -ex) return -ex;
    return x;
}
static int32_t clamp_y(int32_t y)
{
    int32_t ex, ey;
    active_eff_limits(&ex, &ey);
    if (y > ey) return ey;
    if (y < -ey) return -ey;
    return y;
}

/* ---------------- 工具：随机数 ---------------- */
/* [min, max] 闭区间整数随机数 */
static int rand_range(int min, int max)
{
    if (max <= min) {
        return min;
    }
    uint32_t r = esp_random();
    return (int)(r % (uint32_t)(max - min + 1)) + min;
}

/* 动作时间参数（Web 可配，默认来自 action_engine.h 宏值） */
static const action_timing_t *ae_timing(void)
{
    return &config_store_active_profile()->timing;
}

/* ---------------- 工具：可中断延迟 ----------------
 * 返回 false 表示被 STOP_BIT / DISCONNECT_BIT 中断，调用方应逐层退出并收尾。
 */
static bool action_delay_ms(uint32_t ms)
{
    if (ms == 0) {
        return true;
    }
    EventBits_t bits = xEventGroupWaitBits(
        s_action_evt,
        STOP_BIT | DISCONNECT_BIT,
        pdFALSE,            /* 不清除位 */
        pdFALSE,            /* 任一位置位即返回 */
        pdMS_TO_TICKS(ms));
    return (bits & (STOP_BIT | DISCONNECT_BIT)) == 0;
}

/* ---------------- 发送层 ---------------- */

/* 按自定义 chunk（每帧步进上限，单位逻辑像素）分片发送相对位移，帧间插入可中断延迟。
 * 返回 false 表示被中断。
 */
static bool mouse_move_relative_chunk(int32_t total_dx, int32_t total_dy, uint8_t chunk, uint32_t step_ms, uint8_t buttons)
{
    if (chunk < 1) chunk = 1;
    if (chunk > 127) chunk = 127;   /* HID 相对位移单帧上限 */

    /* 蓝牙未连接时整段位移跳过：避免向满队列无谓投递触发刷屏，也没设备接收 */
    if (!ble_hid_is_connected()) {
        return true;
    }

    bool neg_x = total_dx < 0;
    bool neg_y = total_dy < 0;
    int32_t rem_x = total_dx < 0 ? -total_dx : total_dx;
    int32_t rem_y = total_dy < 0 ? -total_dy : total_dy;

    while (rem_x > 0 || rem_y > 0) {
        int32_t cx = (rem_x > chunk) ? chunk : rem_x;
        int32_t cy = (rem_y > chunk) ? chunk : rem_y;
        int8_t dx = (int8_t)(neg_x ? -cx : cx);
        int8_t dy = (int8_t)(neg_y ? -cy : cy);

        /* 位移分片用非阻塞投递：相对位移连续补差，丢单帧不影响最终位置，
         * 且避免阻塞 100ms 拖垮动作引擎、加剧队列积压（此前 blocking 在 BLE 拥塞时会恶化拥塞） */
        esp_err_t err = ble_hid_send_mouse_full(dx, dy, 0, buttons);
        if (err != ESP_OK) {
            ESP_LOGD(TAG, "位移分片投递跳过: %s", esp_err_to_name(err));
        }

        rem_x -= cx;
        rem_y -= cy;

        if (rem_x > 0 || rem_y > 0) {
            if (!action_delay_ms(step_ms)) {
                return false;
            }
        }
    }
    return true;
}

/* 按 ≤ACTION_MOVE_CHUNK_MAX 分片发送相对位移（默认步进），帧间插入可中断延迟。
 * 返回 false 表示被中断。
 */
static bool mouse_move_relative(int32_t total_dx, int32_t total_dy, uint32_t step_ms, uint8_t buttons)
{
    return mouse_move_relative_chunk(total_dx, total_dy, ACTION_MOVE_CHUNK_MAX, step_ms, buttons);
}

/* 从当前逻辑位置出发，沿随机方向滑动一段随机距离（受 [dist_min,dist_max] 控制），
 * 每帧步进在 [step_min,step_max] 间随机（钳制 ≤127），帧间 step_ms 延迟且可中断。
 * 逻辑坐标 s_cur 同步更新。返回 false 表示被中断。
 * 用于拖拽/点击/滚轮的"位移段"，使三者与滑动鼠标一样可配距离+步进。
 */
static bool mouse_move_random(int32_t dist_min, int32_t dist_max,
                               uint8_t step_min, uint8_t step_max,
                               uint32_t step_ms, uint8_t buttons)
{
    int32_t dist = rand_range(dist_min, dist_max);
    if (dist <= 0) {
        return true;
    }
    /* 随机方向（8 方向均匀分布） */
    int32_t ang = rand_range(0, 7);
    static const int32_t dir_x[8] = { 1, 1, 0, -1, -1, -1, 0, 1};
    static const int32_t dir_y[8] = { 0, 1, 1, 1, 0, -1, -1, -1};
    int32_t dx = dir_x[ang];
    int32_t dy = dir_y[ang];
    /* 计算期望落点，并钳制在用户设定的有效活动范围内（MOUSE_POS_EFF_*），避免越界 */
    int32_t target_x = clamp_x(s_cur_x + dx * dist);
    int32_t target_y = clamp_y(s_cur_y + dy * dist);
    int32_t total_dx = target_x - s_cur_x;
    int32_t total_dy = target_y - s_cur_y;
    if (total_dx == 0 && total_dy == 0) {
        return true;   /* 已在边界，无法再向该方向移动 */
    }
    uint8_t chunk = (uint8_t)rand_range(step_min, step_max);
    if (chunk < 1) chunk = 1;
    if (chunk > ACTION_MOVE_CHUNK_MAX) chunk = ACTION_MOVE_CHUNK_MAX;
    bool ok = mouse_move_relative_chunk(total_dx, total_dy, chunk, step_ms, buttons);
    if (ok) {
        s_cur_x = target_x;
        s_cur_y = target_y;
    }
    return ok;
}

/* 滚轮：delta>0 上滚，<0 下滚。同样按 ≤127 分片。 */
static void mouse_wheel(int8_t delta)
{
    if (delta == 0) {
        return;
    }
    int8_t sign = (delta > 0) ? 1 : -1;
    int8_t rem = (int8_t)(delta > 0 ? delta : -delta);
    while (rem > 0) {
        /* rem 为 int8_t，最大值 127，单帧即可发送，无需再分片 */
        int8_t step = rem;
        ble_hid_send_mouse_full_blocking(0, 0, (int8_t)(sign * step), 0);
        rem -= step;
    }
}

/* 设置鼠标按键位（按住或松开左键等）。
 * 必须用阻塞发送：鼠标按键是电平状态，按下帧与松开帧都必须抵达主机才算一次
 * 有效点击；非阻塞帧在移动帧拥塞时会被丢弃，导致主机"看不到按下"。 */
static void mouse_button(uint8_t buttons)
{
    ble_hid_send_mouse_full_blocking(0, 0, 0, buttons);
}

/* 强制释放鼠标按键与所有键盘按键（含 Alt/Shift/Ctrl 修饰键）。
 * 每次动作的唯一出口都会调用；被中断时同样必须走到这里，
 * 否则 Alt 会一直处于按住状态，主机侧表现为“卡住 Alt”。 */
static void action_release_all(void)
{
    /* 释放鼠标按键 */
    ble_hid_send_mouse_full_blocking(0, 0, 0, 0);
    /* 键盘全 0 报文：修饰键字节与按键码均为 0，一次性释放包括 Alt 在内的所有按键，
     * 无需逐个键补发松开帧（省 BLE 带宽，也不会遗漏修饰键）。 */
    ble_hid_send_key_mods(0, 0, false);
}

/* ---------------- 子过程：鼠标复位 ----------------
 * 目标：把光标送回屏幕中心附近（允许偏差，靠经验值收敛）。
 *
 * 关键前提：BLE 鼠标是“相对位移”协议，设备读不到光标绝对位置；且 Windows 对
 * HID 相对位移有指针加速，按“半屏几何”回中心不可靠（推/回不抵消，落点漂移）。
 * 实测验证：1080@125% 与 2560@100% 两屏落点“感觉差不多、都偏出屏幕”，说明与
 * 屏幕数值无关，是系统加速导致，必须用经验值。
 *
 * 做法（两步，经验值，不依赖屏幕几何）：
 *  1) 撞角：朝 MOUSE_HOME_CORNER 配置角落方向发 MOUSE_HOME_PUSH_PX（分片）。
 *     只要 PUSH 足够大，真实光标越过边界被系统截停在角落（绝对已知点）。
 *  2) 回中心：朝撞角反方向发 MOUSE_HOME_BACK_X / MOUSE_HOME_BACK_Y（分片，X/Y 独立）。
 *     决定最终落点。撞角余量已在角落被截掉，故 BACK 不必等于 PUSH 的一半，
 *     需按实测落点调校（见 action_engine.h 注释）。
 * 返回 false 表示被中断。
 */

/* 复位撞角选择（与 action_engine.h 中 MOUSE_HOME_CORNER 对应） */
typedef enum {
    CORNER_TOP_RIGHT = 0,   /* 右上角：向右(+x) + 向上(-y) */
    CORNER_TOP_LEFT,        /* 左上角：向左(-x) + 向上(-y) */
    CORNER_BOTTOM_RIGHT,    /* 右下角：向右(+x) + 向下(+y) */
    CORNER_BOTTOM_LEFT,     /* 左下角：向左(-x) + 向下(+y) */
} mouse_corner_t;

/* 各角落对应的单位方向（每帧 dx, dy 符号） */
static const struct {
    int8_t dx;
    int8_t dy;
} s_corner_dir[4] = {
    [CORNER_TOP_RIGHT]    = { 127, -127},
    [CORNER_TOP_LEFT]     = {-127, -127},
    [CORNER_BOTTOM_RIGHT] = { 127,  127},
    [CORNER_BOTTOM_LEFT]  = {-127,  127},
};

static bool action_mouse_home(void)
{
    const motion_cfg_t *M = &config_store_active_profile()->motion;
    mouse_corner_t corner = (mouse_corner_t)(M->home_corner);
    if (corner > CORNER_BOTTOM_LEFT) {
        corner = CORNER_TOP_RIGHT;
    }
    int sign_x = (s_corner_dir[corner].dx > 0) ? 1 : -1;
    int sign_y = (s_corner_dir[corner].dy > 0) ? 1 : -1;

    int32_t push = (int32_t)M->home_push_px;
    int32_t back_x = (int32_t)M->home_back_x;
    int32_t back_y = (int32_t)M->home_back_y;

    ESP_LOGI(TAG, "复位: 角=%d sign=(%d,%d) PUSH=%d BACK_X=%d BACK_Y=%d",
             (int)corner, (int)sign_x, (int)sign_y, (int)push, (int)back_x, (int)back_y);

    /* 1) 撞角：朝角落方向推 PUSH（底层相对位移，不受活动范围钳制） */
    if (!mouse_move_relative(sign_x * push, sign_y * push, ACTION_HOME_STEP_MS, 0)) {
        return false;
    }

    /* 撞角产生密集 BLE 报文（约 PUSH/127 帧），此处短暂让出，使 BLE 发送队列排空、
     * 射频让给 WiFi，避免与 WiFi STA 重连扫描同时抢占资源而触发 wifi:mem fail / m f null。
     * 复位是各动作的标准前奏，几乎每次动作结束都会执行，故需平滑其 BLE 流量峰值。 */
    if (!action_delay_ms(40)) {
        return false;
    }

    /* 2) 回中心：朝撞角反方向发 BACK_X / BACK_Y（经验常量，X/Y 独立，决定落点） */
    if (!mouse_move_relative(-sign_x * back_x, -sign_y * back_y, ACTION_MOVE_STEP_MS, 0)) {
        return false;
    }

    /* 逻辑坐标重置为屏幕中心 (0,0)（仅用于后续动作取点，不影响复位落点） */
    s_cur_x = 0;
    s_cur_y = 0;
    return true;
}

/* ---------------- 动作原语 ----------------
 * 统一契约：若任何可中断调用返回 false，立即走 single exit 释放按键。
 */

/* 动作1：模拟鼠标拖拽 */
static void act_drag(void)
{
    const action_timing_t *T = ae_timing();
    int repeat = rand_range(T->drag_repeat_min, T->drag_repeat_max);
    ESP_LOGI(TAG, "[动作] 拖拽 重复=%d 次", repeat);
    action_engine_set_progress_total(repeat);

    /* 按住鼠标左键 */
    mouse_button(HID_MOUSE_BTN_LEFT);

    for (int i = 0; i < repeat; i++) {
        action_engine_tick_progress();
        /* 从当前位置沿随机方向拖一段随机距离（受 drag_distance/drag_step 控制） */
        if (!mouse_move_random(T->drag_distance_min, T->drag_distance_max,
                                T->drag_step_min, T->drag_step_max,
                                ACTION_MOVE_STEP_MS, HID_MOUSE_BTN_LEFT)) {
            goto exit_release;
        }
        int interval = rand_range(T->drag_interval_min, T->drag_interval_max);
        if (!action_delay_ms(interval)) {
            goto exit_release;
        }
    }

    /* 松开左键 */
    mouse_button(0);
    /* 鼠标复位 */
    if (!action_mouse_home()) {
        goto exit_release;
    }
    /* 动作自带随机延迟 */
    action_delay_ms(rand_range(T->drag_end_delay_min, T->drag_end_delay_max));

exit_release:
    action_release_all();
}

/* 动作2：模拟鼠标点击 */
static void act_click(void)
{
    const action_timing_t *T = ae_timing();
    int repeat = rand_range(T->click_repeat_min, T->click_repeat_max);
    ESP_LOGI(TAG, "[动作] 点击 重复=%d 次", repeat);
    action_engine_set_progress_total(repeat);

    for (int i = 0; i < repeat; i++) {
        action_engine_tick_progress();
        /* 先滑动到附近一个随机点（受 click_distance/click_step 控制），再单击 */
        if (!mouse_move_random(T->click_distance_min, T->click_distance_max,
                               T->click_step_min, T->click_step_max,
                               ACTION_MOVE_STEP_MS, 0)) {
            goto exit_release;
        }
        /* 单击：按下 + 松开（按下时长随机） */
        mouse_button(HID_MOUSE_BTN_LEFT);
        if (!action_delay_ms(rand_range(T->click_hold_min, T->click_hold_max))) {
            goto exit_release;
        }
        mouse_button(0);
        int interval = rand_range(T->click_interval_min, T->click_interval_max);
        if (!action_delay_ms(interval)) {
            goto exit_release;
        }
    }

    if (!action_mouse_home()) {
        goto exit_release;
    }
    action_delay_ms(rand_range(T->click_end_delay_min, T->click_end_delay_max));

exit_release:
    action_release_all();
}

/* 动作3：模拟鼠标滚轮（移动与滚轮交替：每次移动到一个新点后随即滚轮） */
static void act_wheel(void)
{
    const action_timing_t *T = ae_timing();
    int repeat = rand_range(T->wheel_repeat_min, T->wheel_repeat_max);
    ESP_LOGI(TAG, "[动作] 滚轮(移动+滚轮交替) 重复=%d 次", repeat);
    action_engine_set_progress_total(repeat);

    int wheel_sum = 0;

    for (int i = 0; i < repeat; i++) {
        action_engine_tick_progress();
        /* 1) 先沿随机方向滑动一段短距离（受 wheel_distance/wheel_step 控制） */
        mouse_move_random(T->wheel_distance_min, T->wheel_distance_max,
                          T->wheel_step_min, T->wheel_step_max,
                          ACTION_MOVE_STEP_MS, 0);

        /* 2) 移动完成后立即操作滚轮（随机上下、随机格数） */
        int dir  = (esp_random() & 0x1) ? 1 : -1;
        int tick = rand_range(T->wheel_tick_min, T->wheel_tick_max);
        int delta = dir * tick;
        wheel_sum += delta;
        mouse_wheel((int8_t)delta);

        /* 3) 间隔后进入下一轮（移动+滚轮） */
        if (!action_delay_ms(rand_range(T->wheel_interval_min, T->wheel_interval_max))) {
            goto exit_release;
        }
    }

    /* 滚轮归位：累计和精确归零 */
    if (wheel_sum != 0) {
        mouse_wheel((int8_t)(-wheel_sum));
    }

    if (!action_mouse_home()) {
        goto exit_release;
    }
    action_delay_ms(rand_range(T->wheel_end_delay_min, T->wheel_end_delay_max));

exit_release:
    action_release_all();
}

/* 动作4：模拟键盘方向键 */
static void act_arrow_keys(void)
{
    const action_timing_t *T = ae_timing();
    int repeat = rand_range(T->arrow_repeat_min, T->arrow_repeat_max);
    ESP_LOGI(TAG, "[动作] 方向键 重复=%d 次", repeat);
    action_engine_set_progress_total(repeat);

    const uint8_t arrows[4] = {
        HID_KEY_ARROW_UP, HID_KEY_ARROW_DOWN,
        HID_KEY_ARROW_LEFT, HID_KEY_ARROW_RIGHT,
    };

    for (int i = 0; i < repeat; i++) {
        action_engine_tick_progress();
        uint8_t k = arrows[rand_range(0, 3)];
        ble_hid_send_key(k, true);
        if (!action_delay_ms(10)) {
            goto exit_release;
        }
        ble_hid_send_key(k, false);
        if (!action_delay_ms(rand_range(T->arrow_interval_min, T->arrow_interval_max))) {
            goto exit_release;
        }
    }

    action_delay_ms(rand_range(T->arrow_end_delay_min, T->arrow_end_delay_max));

exit_release:
    action_release_all();
}

/* 动作7：打字（从单词表随机抽词，逐字符发送，词后追加空格）
 * 单词表为空格分隔的字符串，运行时按空格切分为单词数组（原地解析，零额外内存）。
 * 每次动作随机抽取 repeat 个词（允许重复），逐词逐字符发 HID 键盘报文，
 * 字符间延迟 word_char_delay，词后空格延迟 word_space_delay。 */
static void act_word(void)
{
    const action_timing_t *T = ae_timing();
    const config_t *cfg = config_store_get();
    if (cfg == NULL) {
        return;
    }
    const char *list = cfg->word.list;
    if (list[0] == '\0') {
        /* 单词表为空：回退到内置默认词表 */
        list = config_store_default_word_list();
        ESP_LOGW(TAG, "[动作] 打字：单词表为空，回退使用内置默认词表");
    }

    /* 统计单词数量（以空格切分，连续空格视为单个分隔） */
    int word_cnt = 0;
    bool in_word = false;
    for (const char *p = list; *p; ++p) {
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
            in_word = false;
        } else if (!in_word) {
            in_word = true;
            word_cnt++;
        }
    }
    if (word_cnt == 0) {
        ESP_LOGW(TAG, "[动作] 打字：单词表为空，跳过");
        return;
    }
    ESP_LOGI(TAG, "[动作] 打字 单词总数=%d", word_cnt);

    int repeat = rand_range(T->word_repeat_min, T->word_repeat_max);
    if (repeat < 1) repeat = 1;
    action_engine_set_progress_total(repeat);

    for (int r = 0; r < repeat; r++) {
        action_engine_tick_progress();
        /* 随机选一个词的起始下标（按词序计数） */
        int pick = rand_range(0, word_cnt - 1);
        const char *p = list;
        int idx = 0;
        in_word = false;
        const char *wstart = NULL;
        int wlen = 0;
        while (*p) {
            if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
                if (in_word) { /* 词结束 */
                    if (idx == pick) { wlen = (int)(p - wstart); break; }
                    in_word = false;
                }
            } else {
                if (!in_word) { in_word = true; wstart = p; if (idx == pick) { /* 开始记录 */ } idx++; }
                else if (idx - 1 == pick) { /* 继续记录当前词 */ }
            }
            p++;
        }
        /* 处理最后一个词（文件末尾无空格结尾的情况） */
        if (wlen == 0 && in_word && idx - 1 == pick) {
            wlen = (int)(p - wstart);
        }
        if (wlen <= 0 || wstart == NULL) {
            continue;
        }

        /* 逐字符发送该词 */
        for (int i = 0; i < wlen; i++) {
            char buf[2] = { wstart[i], '\0' };
            ble_hid_send_string(buf);
            if (!action_delay_ms(rand_range(T->word_char_delay_min, T->word_char_delay_max))) {
                goto exit_release;
            }
        }
        /* 词后追加空格 */
        ble_hid_send_string(" ");
        if (!action_delay_ms(rand_range(T->word_space_delay_min, T->word_space_delay_max))) {
            goto exit_release;
        }
    }

    action_delay_ms(rand_range(T->word_end_delay_min, T->word_end_delay_max));

exit_release:
    action_release_all();
}
/* 动作8：切换程序（Alt+Tab）
 * 组合键要点：修饰键写在键盘报文首字节，与 Tab 同帧发出主机才认作 Alt+Tab；
 * 松开 Tab 时 Alt 仍保持按住，直到发送“全 0 报文”才松开 Alt——此刻才真正切换窗口。
 * 参数：alt_tab_repeat（切换次数，0=本次不切换）、alt_tab_interval（每次按键间隔）、
 *       alt_tab_end_delay（动作结束后休息时间）。
 */
static void act_alt_tab(void)
{
    const action_timing_t *T = ae_timing();
    int repeat = rand_range(T->alt_tab_repeat_min, T->alt_tab_repeat_max);
    if (repeat < 0) {
        repeat = 0;
    }
    ESP_LOGI(TAG, "[动作] 切换程序 次数=%d", repeat);
    action_engine_set_progress_total(repeat);

    for (int i = 0; i < repeat; i++) {
        action_engine_tick_progress();
        /* Alt+Tab 按下：修饰键与 Tab 同帧 */
        ble_hid_send_key_mods(HID_KEY_TAB, HID_MODIFIER_LEFT_ALT, true);
        if (!action_delay_ms(ALT_TAB_HOLD_MS)) {
            goto exit_release;
        }
        /* 松开 Tab（修饰键字节仍为 Alt，Alt 保持按住，切换器不关闭） */
        ble_hid_send_key_mods(HID_KEY_TAB, HID_MODIFIER_LEFT_ALT, false);
        /* 每次按键后停留一个间隔：多次切换时为两次 Tab 的间隔，
         * 单次切换时为松开 Alt 前的停留，让任务切换器可见。 */
        if (!action_delay_ms(rand_range(T->alt_tab_interval_min, T->alt_tab_interval_max))) {
            goto exit_release;
        }
    }

    /* 松开 Alt 并释放全部按键，此时窗口切换生效 */
    action_release_all();
    action_delay_ms(rand_range(T->alt_tab_end_delay_min, T->alt_tab_end_delay_max));
    return;

exit_release:
    action_release_all();
}

static void act_rest(void)
{
    const action_timing_t *T = ae_timing();
    int delay = rand_range(T->rest_delay_min, T->rest_delay_max);
    ESP_LOGI(TAG, "[动作] 休息 %d ms", delay);
    s_rest_end_us = esp_timer_get_time() + (int64_t)delay * 1000;
    action_delay_ms((uint32_t)delay);
    s_rest_end_us = 0;
    action_release_all();
}

/* 动作6：模拟滑动鼠标（相对位移 + 中间步进）
 * 从当前逻辑位置出发，每次随机选一个方向（水平/垂直/对角）滑动一段随机总距离，
 * 距离由 move_distance 决定，每帧步进由 move_step（chunk）决定，避免瞬移闪烁；
 * 重复 move_repeat 次，每次之间间隔 move_interval，动作结束延迟 move_end_delay。
 * 逻辑坐标按活动范围钳制，更新 s_cur。 */
static void act_move(void)
{
    const action_timing_t *T = ae_timing();
    int repeat = rand_range(T->move_repeat_min, T->move_repeat_max);
    int step   = rand_range(T->move_step_min, T->move_step_max);
    if (step < 1) step = 1;
    if (step > 127) step = 127;
    ESP_LOGI(TAG, "[动作] 滑动 重复=%d 步进=%d", repeat, step);
    action_engine_set_progress_total(repeat);

    for (int i = 0; i < repeat; i++) {
        action_engine_tick_progress();
        int dist = rand_range(T->move_distance_min, T->move_distance_max);
        if (dist < 1) dist = 1;
        /* 随机方向：0=右 1=左 2=上 3=下 4=右上 5=右下 6=左上 7=左下 */
        uint8_t dir = (uint8_t)(esp_random() & 0x7);
        int32_t dx = 0, dy = 0;
        switch (dir) {
        case 0: dx =  dist; break;
        case 1: dx = -dist; break;
        case 2: dy = -dist; break;
        case 3: dy =  dist; break;
        case 4: dx =  dist; dy = -dist; break;
        case 5: dx =  dist; dy =  dist; break;
        case 6: dx = -dist; dy = -dist; break;
        default:dx = -dist; dy =  dist; break;
        }
        /* 目标逻辑坐标，钳制到活动范围（相对中心）。
         * 关键：若沿该方向会撞到边界（目标≈当前点，位移为0），则改为在活动范围内
         * 重新随机一个落点，确保每次循环都有真实位移，避免“只动一次就卡住”的观感。 */
        int32_t tx = clamp_x(s_cur_x + dx);
        int32_t ty = clamp_y(s_cur_y + dy);
        if (tx == s_cur_x && ty == s_cur_y) {
            /* 已贴边界：在有效矩形内随机取一个新落点（不依赖当前方向，保证有位移） */
            int32_t ex, ey;
            active_eff_limits(&ex, &ey);
            int32_t span_x = ex * 2;
            int32_t span_y = ey * 2;
            tx = (int32_t)(esp_random() % (uint32_t)(span_x + 1)) - ex;
            ty = (int32_t)(esp_random() % (uint32_t)(span_y + 1)) - ey;
        }
        /* 实际发送的相对位移 = 目标 - 当前（保证不越界） */
        if (!mouse_move_relative_chunk(tx - s_cur_x, ty - s_cur_y, (uint8_t)step, ACTION_MOVE_STEP_MS, 0)) {
            goto exit_release;
        }
        s_cur_x = tx;
        s_cur_y = ty;
        int interval = rand_range(T->move_interval_min, T->move_interval_max);
        if (!action_delay_ms(interval)) {
            goto exit_release;
        }
    }

    if (!action_mouse_home()) {
        goto exit_release;
    }
    action_delay_ms(rand_range(T->move_end_delay_min, T->move_end_delay_max));

exit_release:
    action_release_all();
}

/* ---------------- 调度 ---------------- */
/* 累计权重抽取（使用运行时可配的 s_weights） */
static action_id_t pick_action(void)
{
    int weights[ACT_COUNT] = {
        s_weights.drag, s_weights.click, s_weights.wheel,
        s_weights.arrow, s_weights.rest, s_weights.move, s_weights.word,
        s_weights.alt_tab,
    };
    int total = 0;
    for (int i = 0; i < ACT_COUNT; i++) {
        if (weights[i] < 0) {
            weights[i] = 0;   /* 防御：权重不接受负值 */
        }
        total += weights[i];
    }
    if (total <= 0) {
        return ACT_REST;      /* 全部禁用时退回休息，避免空转 */
    }
    int r = (int)(esp_random() % (uint32_t)total);
    int acc = 0;
    for (int i = 0; i < ACT_COUNT; i++) {
        acc += weights[i];
        if (r < acc) {
            return (action_id_t)i;
        }
    }
    return ACT_REST;
}

/* 按 action_id 执行对应动作原语（序列模式 / 单动作 / 周期复用） */
static void exec_action_by_id(uint8_t action_id)
{
    /* 记录当前动作（用于 Web 状态页展示） */
    action_engine_set_current_action((act_id_t)action_id);

    switch (action_id) {
    case ACT_DRAG:   act_drag();       break;
    case ACT_CLICK:  act_click();      break;
    case ACT_WHEEL:  act_wheel();      break;
    case ACT_ARROW:  act_arrow_keys(); break;
    case ACT_REST:   act_rest();       break;
    case ACT_MOVE:   act_move();       break;
    case ACT_WORD:   act_word();       break;
    case ACT_ALT_TAB: act_alt_tab();   break;
    default:         act_rest();       break;
    }
}

/* 原地洗牌：把 seq->actions 的顺序打乱写入 s_seq_shuffle（Fisher–Yates） */
static void build_shuffle_order(const action_seq_t *seq)
{
    uint8_t tmp[ACT_SEQ_MAX];
    uint8_t n = (seq->count > ACT_SEQ_MAX) ? ACT_SEQ_MAX : seq->count;
    for (uint8_t i = 0; i < n; i++) {
        tmp[i] = seq->actions[i];
    }
    /* Fisher–Yates 洗牌 */
    for (uint8_t i = n; i > 1; i--) {
        uint8_t j = (uint8_t)(esp_random() % i);
        uint8_t t = tmp[i - 1];
        tmp[i - 1] = tmp[j];
        tmp[j] = t;
    }
    memcpy(s_seq_shuffle, tmp, n);
    s_seq_shuffle_len = n;
}

/* 取序列中第 idx 个动作（按循环策略返回真实动作 ID） */
static uint8_t seq_action_at(uint8_t idx)
{
    uint8_t n = (s_sequence.count > ACT_SEQ_MAX) ? ACT_SEQ_MAX : s_sequence.count;
    if (n == 0) {
        return ACT_REST;
    }
    if (idx >= n) {
        idx = (uint8_t)(idx % n);
    }
    if (s_sequence.cycle == CYCLE_SHUFFLE && s_seq_shuffle_len > 0) {
        return s_seq_shuffle[idx];
    }
    return s_sequence.actions[idx];
}

/* 立即执行一轮序列（顺序/洗牌由配置决定），仅当蓝牙已连接时执行。
 * 供周期循环规则与 Web "执行一轮" 调用。 */
void action_engine_trigger_sequence_once(void)
{
    uint8_t n = (s_sequence.count > ACT_SEQ_MAX) ? ACT_SEQ_MAX : s_sequence.count;
    if (n == 0) {
        return;
    }
    if (!ble_hid_is_connected()) {
        ESP_LOGW(TAG, "序列触发：蓝牙未连接，跳过");
        return;
    }
    if (s_sequence.cycle == CYCLE_SHUFFLE) {
        build_shuffle_order(&s_sequence);
    }
    for (uint8_t i = 0; i < n; i++) {
        if (!ble_hid_is_connected()) {
            break;   /* 中途断连则停止本轮剩余动作 */
        }
        exec_action_by_id(seq_action_at(i));
    }
}

/* 立即执行单个动作一次（定时单动作规则 / Web 单动作触发） */
void action_engine_run_single(uint8_t action_id)
{
    if (!ble_hid_is_connected()) {
        ESP_LOGW(TAG, "单动作触发：蓝牙未连接，跳过 action=%d", action_id);
        return;
    }
    ESP_LOGI(TAG, "[定时单动作] 执行 action_id=%d", action_id);
    exec_action_by_id(action_id);
}

static void action_engine_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "动作引擎任务已启动，等待 KEY2 启动...");

    while (1) {
        EventBits_t bits = xEventGroupWaitBits(
            s_action_evt,
            RUN_BIT | STOP_BIT | DISCONNECT_BIT,
            pdFALSE, pdFALSE, portMAX_DELAY);

        /* 若未处于运行态（STOP 或断连），等待（LED 由独立 LED 任务控制） */
        if ((bits & RUN_BIT) == 0) {
            /* 兜底释放，确保停止后没有残留按下 */
            action_release_all();
            xEventGroupClearBits(s_action_evt, STOP_BIT | DISCONNECT_BIT);
            continue;
        }

        /* 运行额度到期检查：Key1 累加的时间耗尽则自动停止 */
        if (s_run_deadline_us != 0 &&
            esp_timer_get_time() >= s_run_deadline_us) {
            ESP_LOGI(TAG, ">>> 运行额度（%ld 分钟）已耗尽，自动停止 <<<",
                     (long)s_runtime_minutes);
            reset_runtime();
            xEventGroupClearBits(s_action_evt, RUN_BIT);
            xEventGroupSetBits(s_action_evt, STOP_BIT);
            action_release_all();
            xEventGroupClearBits(s_action_evt, STOP_BIT | DISCONNECT_BIT);
            continue;
        }

        /* 蓝牙未连接则周期性等待 */
        if (!ble_hid_is_connected()) {
            if (!action_delay_ms(500)) {
                action_release_all();
            }
            continue;
        }

        /* 动作开始前先鼠标归位，确保从屏幕中心出发（被中断则释放并退出本轮） */
        if (!action_mouse_home()) {
            action_release_all();
            continue;
        }

        /* 抽取并执行一个动作（按运行模式分支） */
        if (s_run_mode == RUN_MODE_SEQUENCE) {
            uint8_t n = (s_sequence.count > ACT_SEQ_MAX) ? ACT_SEQ_MAX : s_sequence.count;
            if (n == 0) {
                exec_action_by_id(ACT_REST);   /* 序列为空：退化为休息，避免空转 */
            } else {
                uint8_t act = seq_action_at(s_seq_cursor);
                exec_action_by_id(act);
                s_seq_cursor++;
                if (s_seq_cursor >= n) {
                    /* 一轮结束：回到头部 */
                    s_seq_cursor = 0;
                    if (s_sequence.cycle == CYCLE_SHUFFLE) {
                        build_shuffle_order(&s_sequence);
                    } else if (s_sequence.cycle == CYCLE_ONCE) {
                        /* 单次循环：一轮执行完后自动停止引擎 */
                        ESP_LOGI(TAG, ">>> 单次循环：一轮已执行完毕，自动停止 <<<");
                        s_seq_once_stop = true;
                        xEventGroupClearBits(s_action_evt, RUN_BIT);
                        xEventGroupSetBits(s_action_evt, STOP_BIT);
                        action_release_all();
                        xEventGroupClearBits(s_action_evt, STOP_BIT | DISCONNECT_BIT);
                        continue;
                    }
                }
            }
        } else {
            action_id_t act = pick_action();
            exec_action_by_id((uint8_t)act);
        }

        /* 每个动作结束后打印运行剩余时间 */
        log_remaining_time();

        /* 动作自带的延迟之后，额外追加 100ms 保护延迟 */
        if ((xEventGroupGetBits(s_action_evt) & (STOP_BIT | DISCONNECT_BIT)) == 0) {
            action_delay_ms(ACTION_GUARD_DELAY_MS);
        }
    }
}

/* ---------------- 独立周期任务：每分钟运行剩余时间日志 ----------------
 * 仅负责打日志提示（LED 闪烁频率由专门的 led_control_task 按剩余时间驱动），
 * 与动作引擎任务解耦，动作节奏不会阻塞本任务。
 * 规则：
 *   - 运行中且有到期点（Key1 额度 / 定时停止）：每分钟提示剩余时间；
 *   - 已停止：不再输出“（已停）”类剩余时间日志（避免刷屏）；
 *   - 已停止但存在“定时开启”规则：每分钟提示距离下次启动还有多久。 */
#define RUNTIME_TICK_PERIOD_MS   60000
static void runtime_tick_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(RUNTIME_TICK_PERIOD_MS));

        bool running = (s_action_evt != NULL) &&
                      ((xEventGroupGetBits(s_action_evt) & RUN_BIT) != 0);

        if (running) {
            /* 运行中：有到期点才提示剩余时间（非定时运行无剩余时间概念） */
            if (s_run_deadline_us == 0 && s_sched_stop_deadline_us == 0) {
                continue;
            }
            int64_t remain_us = get_remaining_us();
            if (remain_us <= 0) {
                continue;
            }
            int32_t remain_min = (int32_t)(remain_us / 1000000LL / 60);
            int32_t remain_sec = (int32_t)((remain_us / 1000000LL) % 60);
            ESP_LOGI(TAG, "[运行剩余时间] %ld 分 %ld 秒",
                     (long)remain_min, (long)remain_sec);
            continue;
        }

        /* 已停止：仅在有“定时开启”规则时提示距离启动还有多久，否则静默 */
        struct tm ti;
        if (!wifi_manager_get_local_time(&ti)) {
            continue;   /* 无时间源，无法计算定时开启倒计时 */
        }
        config_t *cfg = config_store_get();
        if (cfg == NULL) {
            continue;
        }
        int64_t now = esp_timer_get_time();
        int64_t day_sec = (int64_t)ti.tm_hour * 3600 + ti.tm_min * 60 + ti.tm_sec;
        int64_t day_us  = day_sec * 1000000LL;
        int64_t start_us = 0;   /* 最近一次未来“定时开启”绝对时刻 */
        for (uint8_t k = 0; k < cfg->timer_count && k < TIMER_RULE_MAX; k++) {
            timer_rule_t *sr = &cfg->timers[k];
            if (!sr->enabled || sr->type != TIMER_START_STOP || sr->ss_action != 0) {
                continue;
            }
            int64_t target_sec = (int64_t)sr->hour * 3600 + sr->minute * 60;
            int64_t delta_us = (target_sec * 1000000LL) - day_us;
            if (delta_us <= 0) {
                delta_us += 24LL * 3600 * 1000000LL;   /* 今天已过 → 取明天 */
            }
            int64_t ts = now + delta_us;
            if (start_us == 0 || ts < start_us) {
                start_us = ts;
            }
        }
        if (start_us == 0) {
            continue;   /* 没有生效的“定时开启”规则：静默 */
        }
        int64_t remain_us = start_us - now;
        if (remain_us <= 0) {
            continue;
        }
        int32_t remain_min = (int32_t)(remain_us / 1000000LL / 60);
        int32_t remain_sec = (int32_t)((remain_us / 1000000LL) % 60);
        ESP_LOGI(TAG, "[定时开启·距离启动] %ld 分 %ld 秒",
                 (long)remain_min, (long)remain_sec);
    }
}

/* ---------------- 对外接口 ---------------- */
esp_err_t action_engine_start_task(void)
{
    if (s_task_created) {
        return ESP_OK;
    }
    s_action_evt = xEventGroupCreate();
    if (s_action_evt == NULL) {
        ESP_LOGE(TAG, "EventGroup 创建失败");
        return ESP_ERR_NO_MEM;
    }

    s_led_evt = xEventGroupCreate();
    if (s_led_evt == NULL) {
        ESP_LOGE(TAG, "LED EventGroup 创建失败");
        vEventGroupDelete(s_action_evt);
        s_action_evt = NULL;
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreate(action_engine_task, "action_engine",
                                ACTION_TASK_STACK, NULL, ACTION_TASK_PRIORITY, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "动作引擎任务创建失败");
        vEventGroupDelete(s_action_evt);
        s_action_evt = NULL;
        return ESP_FAIL;
    }

    /* 独立周期任务：每分钟运行剩余时间日志（低优先级，不阻塞动作引擎） */
    BaseType_t ok2 = xTaskCreate(runtime_tick_task, "runtime_tick",
                                 ACTION_TASK_STACK, NULL,
                                 ACTION_TASK_PRIORITY - 1, NULL);
    if (ok2 != pdPASS) {
        ESP_LOGW(TAG, "运行剩余时间周期任务创建失败（每分钟提示将不可用）");
    }

    /* 独立 LED 控制任务：统一驱动状态 LED（按剩余时间频率/常亮/闪一下确认） */
    BaseType_t ok3 = xTaskCreate(led_control_task, "led_ctrl",
                                 ACTION_TASK_STACK, NULL,
                                 ACTION_TASK_PRIORITY - 1, NULL);
    if (ok3 != pdPASS) {
        ESP_LOGW(TAG, "LED 控制任务创建失败（状态灯将不可用）");
    }

    s_task_created = true;
    ESP_LOGI(TAG, "动作引擎任务已创建");
    return ESP_OK;
}

void action_engine_start(void)
{
    if (s_action_evt == NULL) {
        return;
    }
    /* 若当前处于计时运行状态，按 KEY2 切换为长时间运行（清掉计时额度，
     * LED 转为常亮）；否则按正常启动。 */
    if (s_run_deadline_us != 0) {
        ESP_LOGI(TAG, ">>> KEY2 已按下：由计时运行切换为长时间运行（常亮）<<<");
        reset_runtime();
    } else {
        ESP_LOGI(TAG, ">>> KEY2 已按下：动作引擎启动 <<<");
    }

    /* 置 RUN_BIT，清 STOP_BIT（并清 DISCONNECT 以便从断连恢复后继续） */
    xEventGroupClearBits(s_action_evt, STOP_BIT | DISCONNECT_BIT);
    xEventGroupSetBits(s_action_evt, RUN_BIT);
}

void action_engine_stop(void)
{
    if (s_action_evt == NULL) {
        return;
    }
    /* 清 RUN_BIT，置 STOP_BIT（唤醒可中断延迟） */
    xEventGroupClearBits(s_action_evt, RUN_BIT);
    xEventGroupSetBits(s_action_evt, STOP_BIT);
    reset_runtime();   /* 停止即清零运行额度计时器，避免残留 */
    s_rest_end_us = 0;  /* 停止时清休息倒计时，避免 status 残留旧值 */
    ESP_LOGI(TAG, ">>> KEY0 已按下：动作引擎停止 <<<");

    /* 停止时顺带提示：若配置了“定时开启”规则，告知距离下次启动还有多久 */
    struct tm ti;
    if (wifi_manager_get_local_time(&ti)) {
        config_t *cfg = config_store_get();
        if (cfg != NULL) {
            int64_t now = esp_timer_get_time();
            int64_t day_sec = (int64_t)ti.tm_hour * 3600 + ti.tm_min * 60 + ti.tm_sec;
            int64_t day_us  = day_sec * 1000000LL;
            int64_t start_us = 0;
            for (uint8_t k = 0; k < cfg->timer_count && k < TIMER_RULE_MAX; k++) {
                timer_rule_t *sr = &cfg->timers[k];
                if (!sr->enabled || sr->type != TIMER_START_STOP || sr->ss_action != 0) {
                    continue;
                }
                int64_t target_sec = (int64_t)sr->hour * 3600 + sr->minute * 60;
                int64_t delta_us = (target_sec * 1000000LL) - day_us;
                if (delta_us <= 0) {
                    delta_us += 24LL * 3600 * 1000000LL;
                }
                int64_t ts = now + delta_us;
                if (start_us == 0 || ts < start_us) {
                    start_us = ts;
                }
            }
            if (start_us != 0) {
                int64_t remain_us = start_us - now;
                if (remain_us > 0) {
                    int32_t remain_min = (int32_t)(remain_us / 1000000LL / 60);
                    int32_t remain_sec = (int32_t)((remain_us / 1000000LL) % 60);
                    ESP_LOGI(TAG, "[停止提示] 下次定时开启还有 %ld 分 %ld 秒",
                             (long)remain_min, (long)remain_sec);
                }
            }
        }
    }
}

bool action_engine_is_running(void)
{
    if (s_action_evt == NULL) {
        return false;
    }
    return (xEventGroupGetBits(s_action_evt) & RUN_BIT) != 0;
}

bool action_engine_stop_and_wait(uint32_t timeout_ms)
{
    if (s_action_evt == NULL) {
        return true;
    }
    xEventGroupClearBits(s_action_evt, RUN_BIT);
    xEventGroupSetBits(s_action_evt, STOP_BIT);
    reset_runtime();   /* 停止即清零运行额度计时器 */

    /* 等待引擎退出当前动作并释放按键：引擎在 STOP 分支会清 STOP_BIT */
    EventBits_t bits = xEventGroupWaitBits(
        s_action_evt, STOP_BIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    return (bits & STOP_BIT) == 0;
}

void action_engine_stop_and_reset(void)
{
    if (s_action_evt == NULL) {
        return;
    }

    /* 1) 先停止动作引擎（清 RUN、置 STOP、唤醒可中断延迟） */
    xEventGroupClearBits(s_action_evt, RUN_BIT);
    xEventGroupSetBits(s_action_evt, STOP_BIT);
    reset_runtime();   /* 停止即清零运行额度计时器 */
    ESP_LOGI(TAG, ">>> KEY3 已按下：停止动作并复位鼠标位置 <<<");

    /* 等待引擎退出当前动作并释放按键，避免复位与动作发送互相穿插 */
    xEventGroupWaitBits(
        s_action_evt, STOP_BIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(2000));

    /* 2) 复位鼠标位置（撞角 → 回到屏幕中心）。
     *    仅在蓝牙已连接时发送 HID 报文；未连接仅停动作，便于排查。 */
    if (!ble_hid_is_connected()) {
        ESP_LOGW(TAG, "蓝牙未连接，无法发送复位报文，仅已停止动作");
        return;
    }

    if (!action_mouse_home()) {
        ESP_LOGW(TAG, "复位过程被中断（STOP/断连）");
    } else {
        ESP_LOGI(TAG, "鼠标已复位到屏幕中心，可确认落点");
    }
}

/* ---------------- 独立 LED 控制任务 ----------------
 * 统一驱动状态 LED，动作引擎不再零散调用 led_status_set：
 *   - 停止（RUN_BIT 未置位）→ 灭
 *   - 非定时运行（无额度）→ 常亮
 *   - 定时运行 → 按剩余时间决定闪烁频率：每 1 分钟对应 T->led_freq_per_1min_ms 间隔
 *     （10 分钟 → 500ms；剩余越短闪得越快，下限 T->led_freq_per_1min_ms，
 *      上限 T->led_freq_max_ms 即最长 2s 闪一次）
 *   - 收到 LED_BLINK_ONCE_BIT（Key1 短按）→ 插入一次确认闪烁（亮-灭-亮-灭），
 *     该闪烁独立于频率节奏，不跟时间挂钩
 */
static void led_control_task(void *arg)
{
    (void)arg;
    const action_timing_t *T = ae_timing();
    bool running = false;
    bool timed   = false;       /* 是否定时运行（有额度） */
    int64_t remain_us = 0;
    uint32_t freq_interval_ms = T->led_freq_per_1min_ms;

    while (1) {
        if (s_action_evt != NULL) {
            running = (xEventGroupGetBits(s_action_evt) & RUN_BIT) != 0;
        } else {
            running = false;
        }
        remain_us = get_remaining_us();
        timed = running && (remain_us > 0);

        if (!running) {
            led_status_set(false);                  /* 停止：灭 */
            /* 等待“闪一下”通知（Key1 短按），并回应一次确认闪 */
            EventBits_t b = xEventGroupWaitBits(s_led_evt, LED_BLINK_ONCE_BIT,
                                                pdTRUE, pdFALSE, pdMS_TO_TICKS(200));
            if (b & LED_BLINK_ONCE_BIT) {
                xEventGroupClearBits(s_led_evt, LED_BLINK_ONCE_BIT);
                led_status_set(true);
                vTaskDelay(pdMS_TO_TICKS(T->led_blink_once_ms));
                led_status_set(false);
                vTaskDelay(pdMS_TO_TICKS(T->led_blink_once_gap_ms));
                led_status_set(true);
                vTaskDelay(pdMS_TO_TICKS(T->led_blink_once_ms));
                led_status_set(false);
            }
            continue;
        }

        if (!timed) {
            led_status_set(true);                   /* 非定时运行：常亮 */
            xEventGroupWaitBits(s_led_evt, LED_BLINK_ONCE_BIT,
                                pdTRUE, pdFALSE, pdMS_TO_TICKS(300));
            if (xEventGroupGetBits(s_led_evt) & LED_BLINK_ONCE_BIT) {
                xEventGroupClearBits(s_led_evt, LED_BLINK_ONCE_BIT);
                led_status_set(true);
                vTaskDelay(pdMS_TO_TICKS(T->led_blink_once_ms));
                led_status_set(false);
                vTaskDelay(pdMS_TO_TICKS(T->led_blink_once_gap_ms));
                led_status_set(true);
                vTaskDelay(pdMS_TO_TICKS(T->led_blink_once_ms));
                led_status_set(false);
            }
            continue;
        }

        /* 定时运行：按剩余时间频率闪烁 */
        int32_t remain_min = (int32_t)(remain_us / 1000000LL / 60);
        freq_interval_ms = (uint32_t)remain_min * T->led_freq_per_1min_ms;  /* 每 1 分钟 50ms */
        if (freq_interval_ms < T->led_freq_per_1min_ms) {
            freq_interval_ms = T->led_freq_per_1min_ms;   /* 下限：至少 50ms */
        }
        if (freq_interval_ms > T->led_freq_max_ms) {
            freq_interval_ms = T->led_freq_max_ms;        /* 上限：最长 2s 闪一次 */
        }

        led_status_set(true);                       /* 亮 */
        vTaskDelay(pdMS_TO_TICKS(T->led_blink_on_ms));
        led_status_set(false);                      /* 灭（间隔） */

        /* 等待间隔，期间可被“闪一下”通知打断 */
        EventBits_t b = xEventGroupWaitBits(s_led_evt, LED_BLINK_ONCE_BIT,
                                            pdTRUE, pdFALSE,
                                            pdMS_TO_TICKS(freq_interval_ms));
        if (b & LED_BLINK_ONCE_BIT) {
            xEventGroupClearBits(s_led_evt, LED_BLINK_ONCE_BIT);
            led_status_set(true);
            vTaskDelay(pdMS_TO_TICKS(T->led_blink_once_ms));
            led_status_set(false);
            vTaskDelay(pdMS_TO_TICKS(T->led_blink_once_gap_ms));
            led_status_set(true);
            vTaskDelay(pdMS_TO_TICKS(T->led_blink_once_ms));
            led_status_set(false);
        }
    }
}

/* ---------------- Key1 短按：增加运行额度 + LED 闪烁确认 ---------------- */
void action_engine_add_runtime(void)
{
    if (s_action_evt == NULL) {
        return;
    }

    int64_t now = esp_timer_get_time();

    /* 1) 累加额度：若仍有余额则从当前到期点续期，否则从现在算起 */
    s_runtime_minutes += KEY1_ADD_MINUTES;
    if (s_run_deadline_us != 0 && s_run_deadline_us > now) {
        s_run_deadline_us += (int64_t)KEY1_ADD_MINUTES * 60 * 1000000LL;
    } else {
        s_run_deadline_us = now + (int64_t)KEY1_ADD_MINUTES * 60 * 1000000LL;
    }

    /* 2) 若此前未运行则自动启动引擎（置 RUN_BIT），已运行则保持 */
    if ((xEventGroupGetBits(s_action_evt) & RUN_BIT) == 0) {
        xEventGroupClearBits(s_action_evt, STOP_BIT | DISCONNECT_BIT);
        xEventGroupSetBits(s_action_evt, RUN_BIT);
    }

    ESP_LOGI(TAG, ">>> KEY1 已按下：运行额度 +%d 分钟（累计 %ld 分钟）<<<",
             KEY1_ADD_MINUTES, (long)s_runtime_minutes);

    /* 3) 通知 LED 任务“闪一下”确认生效（不跟时间挂钩，频率由 LED 任务另行决定） */
    led_notify_blink_once();
}

/* ---------------- Web/NVS 配置接口 ---------------- */

void action_engine_apply_config(const config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }
    s_run_mode = cfg->run_mode;
    s_weights  = cfg->profiles[cfg->active_profile].weights;
    memcpy(&s_sequence, &cfg->sequence, sizeof(s_sequence));
    s_seq_cursor = 0;
    s_seq_shuffle_len = 0;
    s_seq_once_stop = false;
    if (s_sequence.cycle == CYCLE_SHUFFLE && s_sequence.count > 0) {
        build_shuffle_order(&s_sequence);
    }
    ESP_LOGI(TAG, "应用配置：mode=%d seq_count=%d", s_run_mode, s_sequence.count);
}

void action_engine_set_mode(run_mode_t mode)
{
    s_run_mode = (mode == RUN_MODE_SEQUENCE) ? RUN_MODE_SEQUENCE : RUN_MODE_RANDOM;
    s_seq_cursor = 0;
    s_seq_shuffle_len = 0;
    s_seq_once_stop = false;
    ESP_LOGI(TAG, "运行模式切换为 %d", s_run_mode);
}

void action_engine_set_weights(const action_weights_t *w)
{
    if (w == NULL) {
        return;
    }
    s_weights = *w;
    ESP_LOGI(TAG, "权重更新：drag=%d click=%d wheel=%d arrow=%d rest=%d move=%d word=%d alt_tab=%d",
             s_weights.drag, s_weights.click, s_weights.wheel,
             s_weights.arrow, s_weights.rest, s_weights.move, s_weights.word,
             s_weights.alt_tab);
}

void action_engine_set_sequence(const action_seq_t *seq)
{
    if (seq == NULL) {
        return;
    }
    memcpy(&s_sequence, seq, sizeof(s_sequence));
    s_seq_cursor = 0;
    s_seq_shuffle_len = 0;
    s_seq_once_stop = false;
    if (s_sequence.cycle == CYCLE_SHUFFLE && s_sequence.count > 0) {
        build_shuffle_order(&s_sequence);
    }
    ESP_LOGI(TAG, "序列更新：count=%d cycle=%d", s_sequence.count, s_sequence.cycle);
}

/* ---------------- 定时调度扫描任务 ----------------
 * 每 1 秒检查一次规则，低优先级，不阻塞动作引擎：
 *   - TIMER_START_STOP：到点（时:分匹配）置/清 RUN_BIT；
 *   - TIMER_SINGLE_ACTION：到点执行一次指定动作；
 *   - TIMER_PERIODIC：累计秒数到 period_min 触发一轮序列。
 * 为避免每分钟重复触发，记录"已触发分钟"去重（分钟级）。
 */
#define SCHED_TICK_MS       1000
#define SCHED_STACK         3072
#define SCHED_PRIORITY      (ACTION_TASK_PRIORITY - 2)

/* 每条规则上次的"触发分钟序号"（从启动算的累计分钟），用于去重 */
static int64_t s_last_fire_min[TIMER_RULE_MAX];
static int64_t s_boot_ms = 0;

static int64_t now_ms_since_boot(void)
{
    return (int64_t)(esp_timer_get_time() / 1000LL);
}

static void scheduler_task(void *arg)
{
    (void)arg;
    s_boot_ms = now_ms_since_boot();
    for (int i = 0; i < TIMER_RULE_MAX; i++) {
        s_last_fire_min[i] = -1;
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(SCHED_TICK_MS));

        config_t *cfg = config_store_get();
        if (cfg == NULL) {
            continue;
        }
        int64_t elapsed_min = (now_ms_since_boot() - s_boot_ms) / 60000LL;

        /* 取当前分钟（本分钟仅触发一次） */
        struct tm ti;
        bool have_time = wifi_manager_get_local_time(&ti);
        int cur_min_of_day = have_time ? (ti.tm_hour * 60 + ti.tm_min) : -1;

        /* —— 校时保护窗口 ——
         * 检测“是否已校时”的 false->true 边沿：一旦刚成功校时（浏览器或网络），
         * 启动 60 秒保护窗口，期间不触发任何定时任务。这样校时导致的时间跳变
         * 不会让定时规则“误以为过期”而补执行，也不会恰好命中触发分钟就立刻动作。
         * 保护窗口用 esp_timer 绝对时间，与其后真实时间怎样跳变都无关。 */
        bool synced_now = wifi_manager_time_synced();
        if (synced_now && !s_prev_synced) {
            s_timeset_guard_until = esp_timer_get_time() + (int64_t)TIMESET_GUARD_US;
            ESP_LOGI(TAG, "检测到刚校时，启动 %d 秒定时任务保护窗口",
                     (int)(TIMESET_GUARD_US / 1000000));
        }
        s_prev_synced = synced_now;
        if (synced_now && s_timeset_guard_until != 0 &&
            esp_timer_get_time() < s_timeset_guard_until) {
            continue;   /* 保护窗口内，本 tick 不处理任何定时规则 */
        }

        /* —— 计算最近一次“定时停止”任务的绝对停止时刻，用于运行中的 LED 倒计时 ——
         * 扫描所有启用、未到的 “TIMER_START_STOP + ss_action==1” 规则，取其中
         * 距离当前最近（今天若已过则取明天）的未来时刻，写入 s_sched_stop_deadline_us。
         * 当设备运行中且尚无 Key1 额度时，此停止时刻即作为实际到期点（LED 按剩余时间闪烁）；
         * 若同时有 Key1 额度，则二者取更早者（见 get_remaining_us）。
         * 真正的“到点停止”仍由下方规则匹配分支触发 action_engine_stop()。 */
        int64_t sched_stop_us = 0;
        if (have_time) {
            int64_t now = esp_timer_get_time();
            /* now 对应的“当天已过秒数”，用于把 hour:minute 折算成绝对时刻 */
            int64_t day_sec = (int64_t)ti.tm_hour * 3600 + ti.tm_min * 60 + ti.tm_sec;
            int64_t day_us  = day_sec * 1000000LL;
            for (uint8_t k = 0; k < cfg->timer_count && k < TIMER_RULE_MAX; k++) {
                timer_rule_t *sr = &cfg->timers[k];
                if (!sr->enabled || sr->type != TIMER_START_STOP || sr->ss_action != 1) {
                    continue;
                }
                int64_t target_sec = (int64_t)sr->hour * 3600 + sr->minute * 60;
                int64_t delta_us = (target_sec * 1000000LL) - day_us;
                if (delta_us <= 0) {
                    delta_us += 24LL * 3600 * 1000000LL;   /* 今天已过 → 取明天 */
                }
                int64_t ts = now + delta_us;
                if (sched_stop_us == 0 || ts < sched_stop_us) {
                    sched_stop_us = ts;
                }
            }
        }
        s_sched_stop_deadline_us = sched_stop_us;

        for (uint8_t i = 0; i < cfg->timer_count && i < TIMER_RULE_MAX; i++) {
            timer_rule_t *r = &cfg->timers[i];
            if (!r->enabled) {
                continue;
            }
            if (r->type == TIMER_START_STOP) {
                if (!have_time) {
                    continue;
                }
                /* 去重：本分钟已处理过则跳过 */
                if (s_last_fire_min[i] == elapsed_min && elapsed_min >= 0) {
                    continue;
                }
                if (cur_min_of_day == (r->hour * 60 + r->minute)) {
                    s_last_fire_min[i] = elapsed_min;
                    /* 用 ss_action 字段区分启动/停止：0=启动, 1=停止 */
                    if (r->ss_action == 0) {
                        ESP_LOGI(TAG, "[定时启停] 规则%d 到点启动", i);
                        action_engine_start();
                    } else {
                        ESP_LOGI(TAG, "[定时启停] 规则%d 到点停止", i);
                        s_sched_stop_deadline_us = 0;   /* 已停止，清除倒计时 */
                        action_engine_stop();
                    }
                }
            } else if (r->type == TIMER_SINGLE_ACTION) {
                if (!have_time) {
                    continue;
                }
                if (s_last_fire_min[i] == elapsed_min && elapsed_min >= 0) {
                    continue;
                }
                if (cur_min_of_day == (r->hour * 60 + r->minute)) {
                    s_last_fire_min[i] = elapsed_min;
                    ESP_LOGI(TAG, "[定时单动作] 规则%d 到点", i);
                    action_engine_run_single(r->action_id);
                }
            } else if (r->type == TIMER_PERIODIC) {
                /* 周期循环同样依赖已校时的真实时间：未校时（无有效本地时间）
                 * 时按 boot 计时触发会表现为“复位即自动运行”，违背“默认不启动”。
                 * 故与启停/单动作一致，必须 have_time 才生效。 */
                if (!have_time) {
                    continue;
                }
                if (r->period_min == 0) {
                    continue;
                }
                if (elapsed_min >= 0 && (elapsed_min % (int64_t)r->period_min) == 0
                    && s_last_fire_min[i] != elapsed_min) {
                    s_last_fire_min[i] = elapsed_min;
                    ESP_LOGI(TAG, "[周期循环] 规则%d 触发一轮(每%d分)", i, r->period_min);
                    action_engine_trigger_sequence_once();
                }
            }
        }
    }
}

esp_err_t action_engine_start_scheduler(void)
{
    BaseType_t ok = xTaskCreate(scheduler_task, "km_sched", SCHED_STACK, NULL,
                                SCHED_PRIORITY, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "定时调度任务创建失败");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "定时调度任务已创建");
    return ESP_OK;
}
