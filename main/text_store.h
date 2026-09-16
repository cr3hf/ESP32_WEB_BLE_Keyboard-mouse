/*
 * 长文本存储（text_store）
 *
 * “写文本”动作需要一段可长达 128K 字符的文本，远超 NVS（分区仅 24KB）容量，
 * 因此文本不放进 config_t / NVS，而是单独存放在一个 Raw Flash 分区 "textdb"，
 * 运行时整段读入 PSRAM 缓冲（失败则回退内部 RAM），供动作引擎逐字符输出。
 *
 * 存储布局（分区内）：
 *   [0..3]   magic  = 0x54585444 ("TXTD")，标识已初始化
 *   [4..7]   length = 文本字节数（小端），不含结尾 '\0'
 *   [8..]    文本原始字节（UTF-8/ASCII），按 4 字节对齐补零
 *
 * 首次启动（分区为空/无有效 magic）时，写入内置默认文本（来自工程根 Note.txt，
 * 构建期由 gen_default_text.py 生成 default_text.h）。
 */
#pragma once

#include <stddef.h>
#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 文本最大字节数（≥128K 字符；UTF-8 中文按多字节计，此处按字节上限）。
 * 缓冲实际分配 TEXT_MAX + 8，末尾留 '\0' 与 4 字节对齐补零余量。 */
#define TEXT_MAX  131072

/**
 * @brief 初始化文本存储：查找分区、分配 PSRAM 缓冲、加载文本。
 *        若分区为空则写入内置默认文本并持久化。
 *        需在 config_store_load() 之后、动作引擎/Web 启动之前调用。
 */
esp_err_t text_store_init(void);

/**
 * @brief 保存文本到 Flash 分区（同时更新运行时缓冲）。
 *
 * @param text 文本字节（可为任意二进制，通常是 UTF-8/ASCII），不以 '\0' 结尾
 * @param len  文本字节数；超过 TEXT_MAX 时截断到 TEXT_MAX
 * @return ESP_OK 成功；其它为分区/内存错误
 */
esp_err_t text_store_save(const char *text, size_t len);

/**
 * @brief 返回当前文本缓冲（保证以 '\0' 结尾）；未初始化时返回空串。
 */
const char *text_store_get(void);

/**
 * @brief 返回当前文本字节数（不含结尾 '\0'）。
 */
size_t text_store_len(void);

/**
 * @brief 文本存储是否可用（分区已找到且运行缓冲已分配）。
 *        为 false 通常表示设备上缺少 textdb 分区（分区表未更新）。
 */
bool text_store_is_ready(void);

#ifdef __cplusplus
}
#endif
