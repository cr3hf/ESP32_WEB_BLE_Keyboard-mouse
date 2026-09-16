/*
 * 长文本存储实现
 *
 * 见 text_store.h：分区 "textdb" 内 [magic][length][text...]，运行时缓冲在 PSRAM。
 */
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_partition.h"
#include "esp_heap_caps.h"

#include "text_store.h"
#include "default_text.h"   /* 构建期由 gen_default_text.py 从 Note.txt 生成 */

static const char *TAG = "TEXT_STORE";

#define TEXT_PART_NAME   "textdb"
#define TEXT_PART_SUBTYPE 0x40          /* 自定义 data 子类型，与分区表一致 */
#define TEXT_MAGIC        0x54585444u   /* "TXTD" */

/* 分区内头部：magic + length（小端） */
#define TEXT_HDR_SIZE     8

static const esp_partition_t *s_part = NULL;
static char  *s_text = NULL;    /* PSRAM 缓冲，容量 TEXT_MAX + 8，始终以 '\0' 结尾 */
static size_t s_len  = 0;       /* 当前文本字节数（不含 '\0'） */

/* 4 字节对齐向上取整 */
static size_t align4(size_t n)
{
    return (n + 3u) & ~(size_t)3u;
}

/* 把文本写入分区：先擦除覆盖扇区，再写头部与正文（正文按 4 字节对齐补零）。 */
static esp_err_t text_flash_write(const char *text, size_t len)
{
    if (s_part == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    size_t body = align4(len);
    size_t total = TEXT_HDR_SIZE + body;
    size_t erase = (total + 4095u) / 4096u * 4096u;

    esp_err_t err = esp_partition_erase_range(s_part, 0, erase);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "擦除分区失败: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t hdr[TEXT_HDR_SIZE];
    uint32_t magic = TEXT_MAGIC;
    uint32_t u32len = (uint32_t)len;
    memcpy(&hdr[0], &magic, 4);
    memcpy(&hdr[4], &u32len, 4);
    err = esp_partition_write(s_part, 0, hdr, sizeof(hdr));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "写入头部失败: %s", esp_err_to_name(err));
        return err;
    }

    if (body > 0) {
        /* 直接以 s_text 为源：其容量 TEXT_MAX+8，且已在 len~body 间补零 */
        err = esp_partition_write(s_part, TEXT_HDR_SIZE, s_text, body);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "写入正文失败: %s", esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}

/* 从分区读取文本到 s_text；返回 true 表示读到有效文本（length 合法）。 */
static bool text_flash_read(void)
{
    uint8_t hdr[TEXT_HDR_SIZE];
    esp_err_t err = esp_partition_read(s_part, 0, hdr, sizeof(hdr));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "读取头部失败: %s", esp_err_to_name(err));
        return false;
    }
    uint32_t magic = 0, u32len = 0;
    memcpy(&magic, &hdr[0], 4);
    memcpy(&u32len, &hdr[4], 4);
    if (magic != TEXT_MAGIC || u32len > TEXT_MAX) {
        return false;   /* 未初始化或数据异常 */
    }
    if (u32len > 0) {
        err = esp_partition_read(s_part, TEXT_HDR_SIZE, s_text, u32len);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "读取正文失败: %s", esp_err_to_name(err));
            return false;
        }
    }
    s_len = u32len;
    s_text[s_len] = '\0';
    return true;
}

esp_err_t text_store_init(void)
{
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                      (esp_partition_subtype_t)TEXT_PART_SUBTYPE,
                                      TEXT_PART_NAME);
    if (s_part == NULL) {
        ESP_LOGE(TAG, "未找到分区 \"%s\"：设备分区表里没有该分区。请用 `idf.py flash` 完整烧录"
                      "（含分区表；网页 OTA 升级不会更新分区表）后重试", TEXT_PART_NAME);
        return ESP_ERR_NOT_FOUND;
    }

    if (s_text == NULL) {
        /* 优先分配 PSRAM，失败回退内部 RAM */
        s_text = heap_caps_malloc(TEXT_MAX + 8, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_text == NULL) {
            ESP_LOGW(TAG, "PSRAM 分配失败，回退内部 RAM");
            s_text = malloc(TEXT_MAX + 8);
        }
        if (s_text == NULL) {
            ESP_LOGE(TAG, "文本缓冲分配失败（%u 字节）", (unsigned)(TEXT_MAX + 8));
            return ESP_ERR_NO_MEM;
        }
    }
    s_text[0] = '\0';
    s_len = 0;

    if (text_flash_read()) {
        ESP_LOGI(TAG, "文本加载成功：%u 字节", (unsigned)s_len);
        return ESP_OK;
    }

    /* 分区为空：写入内置默认文本（Note.txt 内容） */
    size_t dlen = strlen(DEFAULT_TEXT);
    if (dlen > TEXT_MAX) {
        dlen = TEXT_MAX;
    }
    memcpy(s_text, DEFAULT_TEXT, dlen);
    s_text[dlen] = '\0';
    s_len = dlen;
    esp_err_t err = text_flash_write(s_text, s_len);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "分区为空，已写入默认文本：%u 字节", (unsigned)s_len);
    } else {
        ESP_LOGW(TAG, "默认文本写入失败(%s)，仅使用运行内存副本", esp_err_to_name(err));
    }
    return ESP_OK;
}

esp_err_t text_store_save(const char *text, size_t len)
{
    if (s_text == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (text == NULL) {
        len = 0;
    }
    if (len > TEXT_MAX) {
        len = TEXT_MAX;
    }
    if (len > 0) {
        memcpy(s_text, text, len);
    }
    s_text[len] = '\0';
    /* 补齐到 4 字节对齐：清零 len..align4(len) 的字节，供分区写入直接使用 */
    size_t body = align4(len);
    if (body > len) {
        memset(s_text + len, 0, body - len);
        s_text[len] = '\0';   /* 保证运行内存里仍是 C 字符串 */
    }
    s_len = len;

    esp_err_t err = text_flash_write(s_text, s_len);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "文本已保存：%u 字节", (unsigned)s_len);
    }
    return err;
}

const char *text_store_get(void)
{
    return (s_text != NULL) ? s_text : "";
}

size_t text_store_len(void)
{
    return s_len;
}

bool text_store_is_ready(void)
{
    return (s_part != NULL) && (s_text != NULL);
}
