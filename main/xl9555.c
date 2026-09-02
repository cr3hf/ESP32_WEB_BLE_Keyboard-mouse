#include "xl9555.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

static const char *TAG = "XL9555";

static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_dev = NULL;
static uint8_t s_addr = XL9555_I2C_ADDR;
static bool    s_available = false;   /* XL9555 是否可用（探测成功且配置好） */

/* 写单个寄存器 */
static esp_err_t xl9555_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 100);
}

/* 读单个寄存器 */
static esp_err_t xl9555_read_reg(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, 100);
}

esp_err_t xl9555_init(void)
{
    if (s_dev != NULL) {
        return ESP_OK; /* 已初始化 */
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port    = XL9555_I2C_PORT,
        .sda_io_num  = XL9555_SDA_GPIO,
        .scl_io_num  = XL9555_SCL_GPIO,
        .clk_source  = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {
            .enable_internal_pullup = true,
        },
    };
    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C 总线创建失败: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 自动探测 XL9555 地址：A0/A1/A2 组合对应 0x20~0x27 */
    uint8_t found = 0;
    for (uint8_t a = 0x20; a <= 0x27; a++) {
        if (i2c_master_probe(s_bus, a, 100) == ESP_OK) {
            ESP_LOGI(TAG, "I2C 扫描到设备: 0x%02X", a);
            if (found == 0) {
                found = a;
            }
        }
    }
    if (found == 0) {
        ESP_LOGW(TAG, "未扫描到 XL9555（0x20~0x27 无响应）：板子未焊接扩展 IO，硬件按键将禁用（不影响其余功能）");
        s_available = false;
        return ESP_OK;   /* 兼容：无 XL9555 时不报错，仅跳过按键扫描 */
    }
    s_addr = found;

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = s_addr,
        .scl_speed_hz    = 100000,
    };
    ret = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "XL9555 设备添加失败(地址 0x%02X): %s", s_addr, esp_err_to_name(ret));
        return ret;
    }

    /* 把 Port0 / Port1 全部配置为输入（1=输入），确保按键引脚可读 */
    esp_err_t r0 = xl9555_write_reg(XL9555_REG_CFG0, 0xFF);
    esp_err_t r1 = xl9555_write_reg(XL9555_REG_CFG1, 0xFF);
    if (r0 != ESP_OK || r1 != ESP_OK) {
        ESP_LOGE(TAG, "配置寄存器写入失败: cfg0=%s cfg1=%s",
                 esp_err_to_name(r0), esp_err_to_name(r1));
        return (r0 != ESP_OK) ? r0 : r1;
    }

    /* 回读确认配置真正生效 */
    uint8_t cfg0 = 0, cfg1 = 0, in0 = 0, in1 = 0;
    xl9555_read_reg(XL9555_REG_CFG0, &cfg0);
    xl9555_read_reg(XL9555_REG_CFG1, &cfg1);
    xl9555_read_reg(XL9555_REG_INPUT0, &in0);
    xl9555_read_reg(XL9555_REG_INPUT1, &in1);

    ESP_LOGI(TAG, "XL9555 初始化完成 (地址 0x%02X, SDA=GPIO%d, SCL=GPIO%d)",
             s_addr, XL9555_SDA_GPIO, XL9555_SCL_GPIO);
    ESP_LOGI(TAG, "配置回读: CFG0=0x%02X CFG1=0x%02X (应为 0xFF=全输入)", cfg0, cfg1);
    ESP_LOGI(TAG, "输入回读: IN0=0x%02X IN1=0x%02X (按键在 IN1 的 bit7~bit4)", in0, in1);

    if (cfg1 != 0xFF) {
        ESP_LOGW(TAG, "CFG1 回读异常，可能不是 XL9555/PCA9555 兼容器件");
    }
    s_available = true;
    return ESP_OK;
}

bool xl9555_is_available(void)
{
    return s_available;
}

esp_err_t xl9555_read_p4_p7(uint8_t *p4_p7)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t val = 0xFF;
    /* 按键挂在第二组 Port1，读输入端口 1（寄存器 0x01） */
    esp_err_t ret = xl9555_read_reg(XL9555_REG_INPUT1, &val);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "读取 XL9555 Input1 失败: %s", esp_err_to_name(ret));
        return ret;
    }
    /* val 的 bit7..bit4 = P1_7..P1_4；按键按下=低电平 */
    *p4_p7 = (val >> 4) & 0x0F;
    return ESP_OK;
}
