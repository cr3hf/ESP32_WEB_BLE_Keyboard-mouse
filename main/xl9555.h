#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* XL9555（PCA9555 兼容 16 位 I/O 扩展）通过 I2C 连接
 * ESP32 侧：SDA=GPIO41，SCL=GPIO42
 * 4 个按键挂在 XL9555 的【第二组 Port1】的 P1_4~P1_7：
 *   Key0 -> P1_7, Key1 -> P1_6, Key2 -> P1_5, Key3 -> P1_4
 * 按键按下接地 -> 对应位为低电平(0)，松开由外部上拉为高(1)
 */
#define XL9555_I2C_PORT       0
#define XL9555_SDA_GPIO       41
#define XL9555_SCL_GPIO       42
#define XL9555_I2C_ADDR       0x20    /* A0/A1/A2 全接地时的 7 位地址；init 会自动扫描 0x20~0x27 */

/* XL9555 寄存器（与 PCA9555 兼容） */
#define XL9555_REG_INPUT0     0x00    /* 输入端口 0：P0_0~P0_7 */
#define XL9555_REG_INPUT1     0x01    /* 输入端口 1：P1_0~P1_7 <- 按键在这一组 */
#define XL9555_REG_CFG0       0x06    /* 配置端口 0：1=输入，0=输出 */
#define XL9555_REG_CFG1       0x07    /* 配置端口 1 */

/** @brief 初始化 I2C 总线与 XL9555 设备（自动探测地址，配置 P0/P1 全部为输入）
 *  @note 若板上未焊接 XL9555（I2C 0x20~0x27 无响应），本函数不报错，
 *        仅记录警告并返回 ESP_OK，同时标记器件不可用；按键扫描任务会据此跳过硬件按键。
 */
esp_err_t xl9555_init(void);

/** @brief 查询 XL9555 是否可用（探测成功且已配置好）
 *  @return true=可用（硬件按键有效），false=不可用（板子未焊接，跳过按键扫描）
 */
bool xl9555_is_available(void);

/** @brief 读取 XL9555 第二组 P1_4~P1_7 电平
 *  @param[out] p4_p7 低 4 位依次为 bit3=P1_7, bit2=P1_6, bit1=P1_5, bit0=P1_4；
 *                    实际返回的是 Input1 寄存器高 4 位右移后的值，1=高(松开)，0=低(按下)
 */
esp_err_t xl9555_read_p4_p7(uint8_t *p4_p7);

#ifdef __cplusplus
}
#endif
