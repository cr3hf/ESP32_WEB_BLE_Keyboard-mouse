idf_component_register(SRCS "text_store.c" "web_server.c" "wifi_manager.c" "config_store.c" "action_engine.c" "led_status.c" "main.c"
                           
                           "ble_hid.c"
                           "hid_keys.c"
                           
                           "xl9555.c"
                           "esp_hid_gap.c"
                       INCLUDE_DIRS "."
                       REQUIRES bt esp_hid nvs_flash driver esp_timer
                                esp_wifi esp_netif esp_http_server esp_event
                                json app_update)

# 本地私有配置（个人信息）开关：
# 若 main/local_defs.h 存在（已被 .gitignore 忽略，不会上传），则启用 HAVE_LOCAL_DEFS，
# 改用其中的私有默认值；可用环境变量 CARINE_LOCAL=0 强制关闭，回落到公开默认值。
if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/local_defs.h AND NOT "$ENV{CARINE_LOCAL}" STREQUAL "0")
  target_compile_definitions(${COMPONENT_LIB} PRIVATE HAVE_LOCAL_DEFS)
  message(STATUS "已启用本地私有配置 (HAVE_LOCAL_DEFS)：AP/STA/登录/页面 使用本地默认值")
endif()

# 构建期从工程根目录 Note.txt 生成内置默认文本 default_text.h（供 text_store.c 使用）。
# 使用 IDF 自带的 python 解释器，避免依赖系统 python/python3 命令名差异。
idf_build_get_property(idf_python PYTHON)
set(DEFAULT_TEXT_H ${CMAKE_CURRENT_SOURCE_DIR}/default_text.h)
add_custom_command(
  OUTPUT  ${DEFAULT_TEXT_H}
  COMMAND ${idf_python} ${CMAKE_CURRENT_SOURCE_DIR}/gen_default_text.py
          ${CMAKE_CURRENT_SOURCE_DIR}/../Note.txt ${DEFAULT_TEXT_H}
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/gen_default_text.py
          ${CMAKE_CURRENT_SOURCE_DIR}/../Note.txt
  COMMENT "生成默认文本 default_text.h（来自 Note.txt）"
  VERBATIM)
add_custom_target(default_text_gen DEPENDS ${DEFAULT_TEXT_H})
add_dependencies(${COMPONENT_LIB} default_text_gen)
