/*
 * 随机动作引擎
 *
 * 按下 KEY2 启动、KEY0 停止；按概率随机抽取并执行一组鼠标/键盘动作。
 * 每个动作执行期间 GPIO1 状态 LED 点亮（低电平）。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

#include "config_store.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- 屏幕分辨率（仅用于动作内"活动范围"折算，复位已改为经验值） ---------------- */
#ifndef SCREEN_W
#define SCREEN_W  2560
#endif
#ifndef SCREEN_H
#define SCREEN_H  1440
#endif

/* ---------------- 屏幕缩放（系统显示缩放百分比，如 125% 屏） ----------------
 * 主机开启显示缩放后，同样的 HID 相对位移在屏幕上产生的实际移动量会变化，
 * 从而影响“活动范围”边界。将其做成宏，按缩放折算有效活动范围：
 *   有效范围 = 基准范围 × 100 / SCREEN_SCALE_PCT
 * 缩放越大，允许的逻辑位移越小，真实光标越不易移出用户期望的物理边界。
 * 默认 100（无缩放）。125% 缩放屏设为 125。
 */
#ifndef SCREEN_SCALE_PCT
#define SCREEN_SCALE_PCT   125
#endif

/* ---------------- 鼠标活动范围限制（相对屏幕中心，单位像素） ----------------
 * 所有动作内的鼠标移动都会被钳制在该矩形范围内，且是“全程（跨多次移动、
 * 跨所有动作）”的限制——逻辑光标坐标 s_cur 永远落在此范围内。
 * 以屏幕中心为原点：x ∈ [-MOUSE_POS_LIMIT_X, +MOUSE_POS_LIMIT_X]
 *                    y ∈ [-MOUSE_POS_LIMIT_Y, +MOUSE_POS_LIMIT_Y]
 * 默认：水平 ±400，垂直 ±200（基准值，未折算缩放）。取值偏小可避免动作内
 * 光标跳动幅度过大显得“夸张”。实际生效的是折算后的 MOUSE_POS_EFF_X / MOUSE_POS_EFF_Y。
 */
#ifndef MOUSE_POS_LIMIT_X
#define MOUSE_POS_LIMIT_X   400
#endif
#ifndef MOUSE_POS_LIMIT_Y
#define MOUSE_POS_LIMIT_Y   200
#endif

/* 折算屏幕缩放后的有效活动半范围（供 clamp / 随机取点使用） */
#define MOUSE_POS_EFF_X   ((MOUSE_POS_LIMIT_X) * 100 / (SCREEN_SCALE_PCT))
#define MOUSE_POS_EFF_Y   ((MOUSE_POS_LIMIT_Y) * 100 / (SCREEN_SCALE_PCT))

/* ---------------- 鼠标复位撞角配置 ----------------
 * 复位子过程会先朝某个角方向持续移动，直到光标撞出屏幕边界，
 * 再回到屏幕中心。不同的多屏布局需要撞不同的角：
 *   - 单屏 / 副屏在主屏右侧：撞右上角（向右 + 向上）
 *   - 副屏在主屏左侧（如左 3440x1440 副屏）：撞左上角（向左 + 向上）
 *   - 副屏在主屏下方：撞右下角
 *   - 副屏在主屏上方：撞左下角
 *
 * 可选值（见 action_engine.c 中 mouse_corner_t）：
 *   CORNER_TOP_RIGHT / CORNER_TOP_LEFT / CORNER_BOTTOM_RIGHT / CORNER_BOTTOM_LEFT
 * 默认撞右上角（CORNER_TOP_RIGHT），适用于单屏或副屏在右侧；
 * 副屏在左侧时改为 CORNER_TOP_LEFT，副屏在下/上时对应 CORNER_BOTTOM_RIGHT / CORNER_BOTTOM_LEFT，按需修改。
 */
#ifndef MOUSE_HOME_CORNER
#define MOUSE_HOME_CORNER   CORNER_TOP_RIGHT
#endif

/* ---------------- 复位经验值（实测调校，不依赖屏幕几何） ----------------
 * 实测发现：Windows 对 HID 相对位移有指针加速，按“半屏几何”回中心不可靠，
 * 推/回不抵消，落点漂移。因此复位改为“撞角推到边界（被系统截停，绝对已知点）
 * + 朝中心回一个经验常量”。下面是“发送的位移计数”而非物理像素，需实测收敛。
 * 撞角 PUSH 用单值（X/Y 同量，只要够大越过边界即可）；回程 BACK 必须 X/Y 分开，
 * 因为屏幕宽高比 ≠1 且系统对 X/Y 加速可能不同，单值会导致落点呈“正方形”偏移。
 * 调校方法（KEY3 单次复位观察落点）：
 *   - 落点偏左右（水平未回够）→ 调大 MOUSE_HOME_BACK_X；偏上下 → 调大/小 MOUSE_HOME_BACK_Y。
 *   - 撞角没到边界（落点整体偏撞角反方向）→ MOUSE_HOME_PUSH 给小了，调大。
 * 实测参考（1080@125% 屏）：BACK_Y=200 时垂直居中；BACK_X 需更大才能让水平居中。
 * 默认针对 2560×1440 / 100%：PUSH=2000（足够越界），BACK_X/BACK_Y 初值待实测。
 */
#ifndef MOUSE_HOME_PUSH_PX
#define MOUSE_HOME_PUSH_PX   2000
#endif
/* 回程经验值：X/Y 独立（屏幕宽高比 + 系统加速不对称） */
#ifndef MOUSE_HOME_BACK_X
#define MOUSE_HOME_BACK_X    400
#endif
#ifndef MOUSE_HOME_BACK_Y
#define MOUSE_HOME_BACK_Y    200
#endif

/* ---------------- 动作概率权重 ---------------- */
#define ACT_W_DRAG     0   /* 模拟鼠标拖拽 */
#define ACT_W_CLICK    50   /* 模拟鼠标点击 */
#define ACT_W_WHEEL    50   /* 模拟鼠标滚轮 */
#define ACT_W_ARROW    30   /* 模拟键盘方向键 */
#define ACT_W_REST     35   /* 模拟休息 */
#define ACT_W_MOVE     50   /* 模拟滑动鼠标 */
#define ACT_W_WORD     0   /* 打字（随机单词） */
#define ACT_W_ALT_TAB  0   /* 切换程序（Alt+Tab），默认禁用 */

/* ---------------- 动作1：拖拽 参数（动作说明.md §1） ----------------
 * 注：当前动作采用"随机目标点 + 受限步进移动"实现，下面 DIST/STEP 距离类宏
 *     已不再被动作代码直接引用（保留以备后续改回角度/距离式移动）。
 */
#define DRAG_DIST_MIN       20
#define DRAG_DIST_MAX       100
#define DRAG_STEP_MIN       1
#define DRAG_STEP_MAX       20
#define DRAG_REPEAT_MIN     1
#define DRAG_REPEAT_MAX     5
#define DRAG_INTERVAL_MIN   600     /* ms：每次拖动间隔下限 */
#define DRAG_INTERVAL_MAX   1500    /* ms：每次拖动间隔上限 */
#define DRAG_END_DELAY_MIN  500     /* ms：动作结束延迟下限 */
#define DRAG_END_DELAY_MAX  5000    /* ms：动作结束延迟上限 */

/* ---------------- 动作2：点击 参数（动作说明.md §2） ---------------- */
#define CLICK_DIST_MIN      10
#define CLICK_DIST_MAX      100
#define CLICK_STEP_MIN      1
#define CLICK_STEP_MAX      30
#define CLICK_REPEAT_MIN    1
#define CLICK_REPEAT_MAX    10
#define CLICK_INTERVAL_MIN  100     /* ms */
#define CLICK_INTERVAL_MAX  1000    /* ms */
#define CLICK_END_DELAY_MIN 1000    /* ms */
#define CLICK_END_DELAY_MAX 5000    /* ms */
#define CLICK_HOLD_MIN      20      /* ms：左键按下保持时长下限 */
#define CLICK_HOLD_MAX      250     /* ms：左键按下保持时长上限 */

/* ---------------- 动作3：滚轮 参数（动作说明.md §3） ---------------- */
#define WHEEL_STEP_MIN      1
#define WHEEL_STEP_MAX      30
#define WHEEL_REPEAT_MIN    1
#define WHEEL_REPEAT_MAX    5
#define WHEEL_INTERVAL_MIN  100      /* ms */
#define WHEEL_INTERVAL_MAX  500     /* ms */
#define WHEEL_TICK_MIN      1       /* 单次滚动步进 */
#define WHEEL_TICK_MAX      8
#define WHEEL_END_DELAY_MIN 1000     /* ms */
#define WHEEL_END_DELAY_MAX 5000    /* ms */

/* ---------------- 动作4：方向键 参数（动作说明.md §4） ---------------- */
#define ARROW_REPEAT_MIN    1
#define ARROW_REPEAT_MAX    20
#define ARROW_INTERVAL_MIN  50      /* ms */
#define ARROW_INTERVAL_MAX  800    /* ms */
#define ARROW_END_DELAY_MIN 1000     /* ms */
#define ARROW_END_DELAY_MAX 5000    /* ms */

/* ---------------- 动作5：休息 参数（动作说明.md §5） ---------------- */
#define REST_DELAY_MIN      1000    /* ms */
#define REST_DELAY_MAX      20000   /* ms */

/* ---------------- 动作8：切换程序（Alt+Tab）参数 ----------------
 * - 切换次数（默认 0~1 次）：0=本次不切换程序，1=切换一次。
 * - 每次按键间隔（默认 500~1000ms）：多次切换时为两次 Tab 之间的等待；
 *   单次切换时为“松开 Alt 前的停留”，让 Windows 任务切换器可见。
 * - 动作后休息（默认 700~1500ms）：松开 Alt、完成窗口切换后的休息时间。
 */
/* Alt+Tab 参数“建议默认范围”（非硬限制）：此处仅作代码内软参考上限，
 * 实际的随机默认值在 config_store.c 设置（0~1/500~1000/700~1500），
 * 页面输入上限已放宽至与其他动作一致（次数 0~999、间隔 50~5000、休息 50~60000）。 */
#ifndef ALT_TAB_REPEAT_MIN
#define ALT_TAB_REPEAT_MIN      0       /* 次：切换次数下限（0=本次不切换） */
#endif
#ifndef ALT_TAB_REPEAT_MAX
#define ALT_TAB_REPEAT_MAX      999     /* 次：切换次数上限（与其他动作重复次数一致） */
#endif
#ifndef ALT_TAB_INTERVAL_MIN
#define ALT_TAB_INTERVAL_MIN    50      /* ms：每次按键间隔下限（与其他动作间隔一致） */
#endif
#ifndef ALT_TAB_INTERVAL_MAX
#define ALT_TAB_INTERVAL_MAX    5000    /* ms：每次按键间隔上限（与其他动作间隔一致） */
#endif
#ifndef ALT_TAB_END_DELAY_MIN
#define ALT_TAB_END_DELAY_MIN   50      /* ms：动作后休息下限（与其他动作结束延迟一致） */
#endif
#ifndef ALT_TAB_END_DELAY_MAX
#define ALT_TAB_END_DELAY_MAX   60000   /* ms：动作后休息上限（与其他动作结束延迟一致） */
#endif
#define ALT_TAB_HOLD_MS         10      /* ms：Tab 按下保持时长（与方向键一致） */

/* ---------------- 公共保护延迟：每个动作之后额外追加 ---------------- */
#define ACTION_GUARD_DELAY_MS   100

/* ---------------- 分片位移与帧间延迟 ---------------- */
#define ACTION_MOVE_CHUNK_MAX   25      /* 单帧最大相对位移计数（越小移动越细腻/慢） */
#define ACTION_MOVE_STEP_MS     28      /* 分片位移帧间延迟（可中断，越大整体越慢） */
#define ACTION_HOME_STEP_MS     12      /* 复位撞角帧间延迟（太快会突兀，略慢于动作） */
#define ACTION_POST_BLOCKING_MS 100     /* 阻塞投递超时 */

/* ---------------- 运行时间额度（Key1 短按 +10分钟） ----------------
 * Key1 每短按一次，运行额度 +KEY1_ADD_MINUTES 分钟；额度为 0 时引擎自动停止。
 * 短按后若未运行则自动启动，已运行则累加。
 * LED 由独立 led_control_task 统一驱动：定时运行时按剩余时间频率闪烁
 * （每 1 分钟对应 50ms 间隔，如 10 分钟→500ms，上限 2s）；非定时运行常亮；停止则灭。
 * Key1 短按仅通知 LED 任务“闪一下”确认（不跟时间挂钩）。
 */
#ifndef KEY1_ADD_MINUTES
#define KEY1_ADD_MINUTES       10      /* Key1 单次短按增加的运行分钟数 */
#endif

/* ---------------- 任务栈与优先级 ---------------- */
#define ACTION_TASK_STACK      4096
#define ACTION_TASK_PRIORITY   5

/**
 * @brief 创建动作引擎任务与 EventGroup，初始为停止态。
 *        必须在 keys_start() 之前调用（按键回调会操作 EventGroup）。
 */
esp_err_t action_engine_start_task(void);

/**
 * @brief KEY2：启动动作引擎（置 RUN_BIT，清 STOP_BIT）
 */
void action_engine_start(void);

/**
 * @brief KEY0：停止动作引擎（置 STOP_BIT，清 RUN_BIT，唤醒可中断延迟）
 */
void action_engine_stop(void);

/**
 * @brief 查询引擎当前是否处于运行状态
 */
bool action_engine_is_running(void);

/**
 * @brief 蓝牙重置前调用：停止引擎并等待其确认已释放全部按键。
 *
 * @param timeout_ms 等待超时（毫秒）
 * @return true=已停止且按键已释放，false=超时
 */
bool action_engine_stop_and_wait(uint32_t timeout_ms);

/**
 * @brief 停止动作并复位鼠标位置（供 KEY3 手动确认落点使用）。
 *
 * 无论当前是否处于运行状态都会执行：
 *   - 先停止动作引擎并释放全部按键（等价于 action_engine_stop()）；
 *   - 再执行鼠标复位（撞角 → 回到屏幕中心），便于用户确认复位落点。
 * 仅在蓝牙已连接时才会真正发送 HID 位移报文；未连接时仅停动作并打印告警。
 */
void action_engine_stop_and_reset(void);

/**
 * @brief Key1 短按：增加运行额度（默认 +10 分钟），并用 LED 闪烁两次确认生效。
 *
 * 行为：
 *   - 运行额度 += KEY1_ADD_MINUTES 分钟；若此前未运行则自动启动引擎；
 *   - 通知 LED 任务“闪一下”确认生效（不跟时间挂钩，具体闪烁频率由 LED 任务
 *     按剩余时间另行决定）；不破坏正常运行状态。
 */
void action_engine_add_runtime(void);

/* ---------------- 以下为 WiFi/Web 配置扩展接口 ---------------- */

/**
 * @brief 应用一份配置到引擎（运行模式、权重、序列、定时）。
 *        由 main.c 启动后调用一次，加载 NVS 中的用户配置。
 */
void action_engine_apply_config(const config_t *cfg);

/**
 * @brief 设置当前正在执行的动作（在调度器执行每个动作前调用）。
 *        用于 Web 状态页展示“正在执行的动作”。
 */
void action_engine_set_current_action(act_id_t id);

/**
 * @brief 返回当前/最近一次执行的动作的中文名（供 /api/status 展示）。
 *        引擎未运行时返回“空闲”。
 */
const char *action_engine_current_action_name(void);

/**
 * @brief 设置当前动作的重复总次数（动作开始执行时调用）。
 */
void action_engine_set_progress_total(int total);

/**
 * @brief 每执行一次重复时调用，推进当前进度计数。
 */
void action_engine_tick_progress(void);

/**
 * @brief 返回当前动作进度字符串（如 "2/10"）；无进度信息时返回空串。
 *        供 /api/status 展示“当前动作 + 重复次数”。
 */
const char *action_engine_current_action_progress(void);

/**
 * @brief 返回当前“休息”剩余时间（微秒）；不在休息或已结束时返回 0。
 *        供 /api/status 在状态为“休息”时展示倒计时。
 */
int64_t action_engine_current_rest_remaining_us(void);

/**
 * @brief 设置运行模式（随机 / 序列）。Web 实时切换。
 */
void action_engine_set_mode(run_mode_t mode);

/**
 * @brief 设置随机权重（覆盖编译期宏默认）。Web 实时修改。
 */
void action_engine_set_weights(const action_weights_t *w);

/**
 * @brief 设置动作序列（排布 + 循环策略）。Web 实时修改。
 */
void action_engine_set_sequence(const action_seq_t *seq);

/**
 * @brief 立即执行单个动作一次（供定时单动作规则调用）。
 *        仅当蓝牙已连接时发送；未连接则跳过并告警。
 */
void action_engine_run_single(uint8_t action_id);

/**
 * @brief 立即执行一轮序列（供周期循环规则调用）。
 *        复用序列模式调度（顺序/洗牌），仅当蓝牙已连接时执行。
 */
void action_engine_trigger_sequence_once(void);

/**
 * @brief 启动定时调度扫描任务（每 1 秒检查一次规则）。
 *        仅在 action_engine_start_task() 之后调用一次。
 */
esp_err_t action_engine_start_scheduler(void);

#ifdef __cplusplus
}
#endif
