/*
 * Web 服务实现
 *
 * 内置单页配置控制台，监听 0.0.0.0:80，AP 与 STA 网段均可访问。
 * 页面内嵌为 Flash 常量（static const char，ESP32 默认存 Flash）。
 * REST 接口用 cJSON 临时解析（用完释放），不长期占 RAM。
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_random.h"
#include "nvs.h"

#include "cJSON.h"

#include "web_server.h"
#include "config_store.h"
#include "action_engine.h"
#include "wifi_manager.h"
#include "defaults.h"
#include "ble_hid.h"

static const char *TAG = "WEB_SRV";

static httpd_handle_t s_server = NULL;

/* ---------------- 登录鉴权 ----------------
 * 默认用户名/密码（硬编码，仅为本地配置台做一层访问控制）。
 * 登录成功后生成一次性 token 存入 s_auth_token，前端在后续请求的
 * Authorization: Bearer <token> 头中携带；所有 api 接口均校验此 token。
 */
/* 出厂默认登录凭据（可在页面修改）；具体值来自 defaults.h / 本地 local_defs.h */
#define LOGIN_USER WEB_LOGIN_USER
#define LOGIN_PASS WEB_LOGIN_PASS

#define AUTH_NS        "auth"
#define AUTH_KEY_USER  "user"
#define AUTH_KEY_PASS  "pass"
#define AUTH_USER_MAX  32
#define AUTH_PASS_MAX  64

static char s_auth_user[AUTH_USER_MAX + 1] = WEB_LOGIN_USER;
static char s_auth_pass[AUTH_PASS_MAX + 1] = WEB_LOGIN_PASS;
static char s_auth_token[33] = "";   /* 32 位 hex token，空串表示未登录 */

/* 从 NVS("auth") 加载登录凭据；不存在则用默认 admin/admin 并写回 */
static void auth_store_load(void)
{
    nvs_handle_t h;
    if (nvs_open(AUTH_NS, NVS_READWRITE, &h) != ESP_OK) return;
    size_t len = sizeof(s_auth_user);
    if (nvs_get_str(h, AUTH_KEY_USER, s_auth_user, &len) != ESP_OK ||
        s_auth_user[0] == '\0' || len > sizeof(s_auth_user)) {
        strncpy(s_auth_user, LOGIN_USER, AUTH_USER_MAX);
        s_auth_user[AUTH_USER_MAX] = '\0';
        nvs_set_str(h, AUTH_KEY_USER, s_auth_user);
    }
    len = sizeof(s_auth_pass);
    if (nvs_get_str(h, AUTH_KEY_PASS, s_auth_pass, &len) != ESP_OK ||
        s_auth_pass[0] == '\0' || len > sizeof(s_auth_pass)) {
        strncpy(s_auth_pass, LOGIN_PASS, AUTH_PASS_MAX);
        s_auth_pass[AUTH_PASS_MAX] = '\0';
        nvs_set_str(h, AUTH_KEY_PASS, s_auth_pass);
    }
    nvs_commit(h);
    nvs_close(h);
}

/* 保存登录凭据到 NVS */
static void auth_store_save(const char *user, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open(AUTH_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, AUTH_KEY_USER, user);
    nvs_set_str(h, AUTH_KEY_PASS, pass);
    nvs_commit(h);
    nvs_close(h);
}

/* send_json 定义在文件后部，此处前向声明以便鉴权辅助函数复用 */
static esp_err_t send_json(httpd_req_t *req, cJSON *root, int code);

/* 校验请求是否携带有效 token；无效返回 false 并直接回复 401 */
static bool require_auth(httpd_req_t *req)
{
    if (s_auth_token[0] == '\0') {
        goto deny;
    }
    size_t hdr_len = httpd_req_get_hdr_value_len(req, "Authorization") + 1;
    if (hdr_len <= 1) {
        goto deny;
    }
    char *buf = malloc(hdr_len);
    if (!buf) {
        goto deny;
    }
    if (httpd_req_get_hdr_value_str(req, "Authorization", buf, hdr_len) != ESP_OK) {
        free(buf);
        goto deny;
    }
    bool ok = (strncmp(buf, "Bearer ", 7) == 0) && (strcmp(buf + 7, s_auth_token) == 0);
    free(buf);
    if (!ok) {
        goto deny;
    }
    return true;

deny:
    {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddBoolToObject(root, "auth", false);
        cJSON_AddStringToObject(root, "msg", "未登录或登录已失效");
        send_json(req, root, 401);
    }
    return false;
}

/* 生成 32 位 hex token（基于 esp_random），并写入 s_auth_token */
static void gen_auth_token(void)
{
    static const char hexd[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        uint32_t r = esp_random();
        s_auth_token[i] = hexd[r & 0xf];
    }
    s_auth_token[32] = '\0';
}

/* ---------------- 内嵌页面（深色玻璃拟态控制台，单页） ----------------
 * 使用相邻字符串字面量拼接（C 不支持 raw string）。
 * 属性统一用单引号，JS 字符串用反引号/单引号，避免与 C 字符串的 " 冲突。
 */
static const char PAGE_HTML[] =
"<!DOCTYPE html>\n"
"<html lang='zh-CN'>\n"
"<head>\n"
"<meta charset='UTF-8'>\n"
"<meta name='viewport' content='width=device-width, initial-scale=1.0'>\n"
"<title>" APP_TITLE "控制台</title>\n"
"<style>\n"
":root{\n"
"  --bg0:#0B1120; --bg1:#111827; --bg2:#1E293B;\n"
"  --cyan:#22D3EE; --blue:#3B82F6; --sky:#0EA5E9;\n"
"  --green:#22C55E; --red:#EF4444; --amber:#F59E0B;\n"
"  --text:#F1F5F9; --sub:#94A3B8;\n"
"}\n"
"*{box-sizing:border-box;margin:0;padding:0}\n"
"body{\n"
"  font-family:'PingFang SC','Microsoft YaHei',system-ui,sans-serif;\n"
"  color:var(--text);background:\n"
"    radial-gradient(1200px 800px at 10% -10%, rgba(34,211,238,.16), transparent 60%),\n"
"    radial-gradient(1000px 700px at 110% 10%, rgba(59,130,246,.18), transparent 55%),\n"
"    linear-gradient(160deg,#0B1120,#111827 55%,#1E293B);\n"
"  min-height:100vh;padding-bottom:40px;\n"
"}\n"
".wrap{max-width:1080px;margin:0 auto;padding:18px}\n"
".topbar{\n"
"  position:sticky;top:0;z-index:20;display:flex;flex-direction:column;gap:8px;\n"
"  padding:10px 20px;border-radius:18px;\n"
"  background:rgba(30,41,59,.55);backdrop-filter:blur(14px);\n"
"  border:1px solid rgba(34,211,238,.18);\n"
"  box-shadow:0 8px 30px rgba(2,8,23,.45);\n"
"}\n"
".topbar-head{display:flex;flex-wrap:wrap;gap:8px 18px;align-items:center;\n"
"  justify-content:space-between}\n"
".toolbar{display:flex;flex-wrap:wrap;gap:6px;align-items:center;\n"
"  padding-top:8px;border-top:1px solid rgba(148,163,184,.14)}\n"
".brand{display:flex;align-items:center;gap:12px}\n"
".logo{width:32px;height:32px;border-radius:11px;\n"
"  background:linear-gradient(135deg,var(--cyan),var(--blue));\n"
"  display:flex;align-items:center;justify-content:center;font-weight:700;color:#06121f}\n"
".brand h1{font-size:15px;font-weight:600;letter-spacing:.3px}\n"
".brand .sub{font-size:11px;color:var(--sub)}\n"
".stats{display:flex;flex-wrap:wrap;gap:10px;align-items:center}\n"
".chip{display:flex;align-items:center;gap:7px;font-size:12.5px;color:var(--sub);\n"
"  padding:6px 11px;border-radius:999px;background:rgba(15,23,42,.5);\n"
"  border:1px solid rgba(148,163,184,.16)}\n"
".dot{width:9px;height:9px;border-radius:50%;background:#475569;box-shadow:0 0 0 3px rgba(71,85,105,.25)}\n"
".dot.on{background:var(--green);box-shadow:0 0 10px rgba(34,197,94,.7)}\n"
".dot.warn{background:var(--amber);box-shadow:0 0 10px rgba(245,158,11,.7)}\n"
".dot.run{background:var(--cyan);box-shadow:0 0 10px rgba(34,211,238,.7)}\n"
".btn{cursor:pointer;border:none;border-radius:10px;padding:8px 13px;font-size:13px;\n"
"  font-weight:600;color:#06121f;transition:.18s transform,.18s box-shadow,.18s filter;\n"
"  background:linear-gradient(135deg,var(--cyan),var(--blue));\n"
"  box-shadow:0 6px 18px rgba(34,211,238,.28)}\n"
".btn:hover{transform:translateY(-1px) scale(1.02);filter:brightness(1.06)}\n"
".btn:active{transform:translateY(0) scale(.99)}\n"
".btn.ghost{background:rgba(148,163,184,.14);color:var(--text);\n"
"  box-shadow:none;border:1px solid rgba(148,163,184,.22)}\n"
".btn.save{background:linear-gradient(135deg,#22c55e,#16a34a);color:#04140a;\n"
"  box-shadow:0 6px 18px rgba(34,197,94,.35)}\n"
".btn.danger{background:linear-gradient(135deg,#fb7185,var(--red))}\n"
".grid{display:grid;grid-template-columns:1fr 1fr;gap:18px;margin-top:20px}\n"
"@media(max-width:860px){.grid{grid-template-columns:1fr}.stats{width:100%}}\n"
".card{background:rgba(30,41,59,.42);backdrop-filter:blur(12px);\n"
"  border:1px solid rgba(148,163,184,.14);border-radius:18px;padding:20px;\n"
"  box-shadow:0 10px 30px rgba(2,8,23,.35);transition:.2s border-color}\n"
".card:hover{border-color:rgba(34,211,238,.35)}\n"
".card h2{font-size:16px;font-weight:600;margin-bottom:4px}\n"
".card .hint{font-size:12px;color:var(--sub);margin-bottom:16px}\n"
".tabs{display:flex;gap:8px;margin-bottom:16px}\n"
".tab{flex:1;text-align:center;padding:9px;border-radius:11px;cursor:pointer;\n"
"  font-size:13.5px;font-weight:500;color:var(--sub);\n"
"  background:rgba(15,23,42,.5);border:1px solid transparent;transition:.18s}\n"
".tab.active{color:var(--text);background:rgba(34,211,238,.14);\n"
"  border-color:rgba(34,211,238,.4)}\n"
".pane{display:none}\n"
".pane.active{display:block;animation:fade .25s ease}\n"
"@keyframes fade{from{opacity:0;transform:translateY(6px)}to{opacity:1;transform:none}}\n"
".wrow{display:flex;align-items:center;gap:12px;margin-bottom:11px}\n"
".wrow label{width:78px;font-size:13.5px;color:var(--text)}\n"
".wrow input[type=range]{flex:1;accent-color:var(--cyan)}\n"
".wrow .val{width:42px;text-align:center;font-variant-numeric:tabular-nums;\n"
"  font-size:13.5px;color:var(--cyan)}\n"
".total{margin-top:8px;font-size:13px;color:var(--sub)}\n"
".total b{color:var(--text)}\n"
".actions-add{display:flex;flex-wrap:wrap;gap:8px;margin-bottom:14px}\n"
".addbtn{cursor:pointer;border:1px dashed rgba(34,211,238,.4);border-radius:10px;\n"
"  padding:8px 12px;font-size:13px;color:var(--cyan);background:rgba(34,211,238,.06);\n"
"  transition:.16s}\n"
".addbtn:hover{background:rgba(34,211,238,.16)}\n"
".seq-list{list-style:none;display:flex;flex-direction:column;gap:8px;min-height:40px}\n"
".seq-item{display:flex;align-items:center;gap:10px;padding:9px 12px;border-radius:11px;\n"
"  background:rgba(15,23,42,.5);border:1px solid rgba(148,163,184,.14)}\n"
".seq-item .idx{width:22px;height:22px;border-radius:7px;font-size:11.5px;\n"
"  display:flex;align-items:center;justify-content:center;color:#06121f;\n"
"  background:linear-gradient(135deg,var(--cyan),var(--blue));font-weight:700}\n"
".seq-item .name{flex:1;font-size:13.5px}\n"
".seq-item .mv{cursor:pointer;border:none;background:rgba(148,163,184,.16);color:var(--text);\n"
"  border-radius:8px;width:28px;height:28px;font-size:14px;transition:.16s}\n"
".seq-item .mv:hover{background:rgba(34,211,238,.3)}\n"
".seq-item .del{cursor:pointer;border:none;background:rgba(239,68,68,.18);color:#fecaca;\n"
"  border-radius:8px;width:28px;height:28px;font-size:14px;transition:.16s}\n"
".seq-item .del:hover{background:var(--red);color:#fff}\n"
".radios{display:flex;gap:18px;margin-top:14px}\n"
".radios label{display:flex;align-items:center;gap:7px;font-size:13px;color:var(--sub);cursor:pointer}\n"
".radios input{accent-color:var(--cyan)}\n"
".rule{border:1px solid rgba(148,163,184,.14);border-radius:13px;padding:13px;\n"
"  margin-bottom:12px;background:rgba(15,23,42,.4)}\n"
".rule .rh{display:flex;align-items:center;justify-content:space-between;margin-bottom:10px}\n"
".rule .rh .t{font-size:13.5px;font-weight:500}\n"
".rule .rgrid{display:grid;grid-template-columns:repeat(auto-fit,minmax(120px,1fr));gap:10px}\n"
".field label{display:block;font-size:11.5px;color:var(--sub);margin-bottom:5px}\n"
".field input,.field select{width:100%;padding:9px 10px;border-radius:10px;\n"
"  border:1px solid rgba(148,163,184,.2);background:rgba(2,8,23,.5);color:var(--text);\n"
"  font-size:13px;outline:none;transition:.16s}\n"
".field input:focus,.field select:focus{border-color:rgba(34,211,238,.5)}\n"
".field select option,select option{background:var(--bg2);color:var(--text);padding:6px 8px}\n"
".tcards{display:grid;grid-template-columns:repeat(auto-fill,minmax(220px,1fr));gap:12px;margin-top:10px}\n"
".tcard{background:rgba(2,8,23,.35);border:1px solid rgba(148,163,184,.15);border-radius:12px;padding:12px}\n"
".tch{font-size:13px;font-weight:600;color:var(--cyan);margin-bottom:10px}\n"
".trow{display:flex;align-items:center;gap:8px;margin-bottom:9px;font-size:12.5px}\n"
".trow label{flex:1;color:var(--text)}\n"
".trow .min input,.trow .max input{width:64px;padding:6px 7px;border-radius:8px;\n"
"  border:1px solid rgba(148,163,184,.2);background:rgba(2,8,23,.5);color:var(--text);font-size:12.5px;outline:none}\n"
".trow .sep{color:var(--sub);width:14px;text-align:center}\n"
".trow select{width:100%;padding:6px 7px;border-radius:8px;border:1px solid rgba(148,163,184,.2);background:rgba(2,8,23,.5);color:var(--text);font-size:12.5px;outline:none}\n"
".trow .u{color:var(--sub);width:26px;text-align:right;font-size:11px}\n"
".trow .min input.bad,.trow .max input.bad{border-color:#ef4444!important;background:rgba(239,68,68,.15)!important;color:#fecaca!important}\n"
".switch{position:relative;width:42px;height:24px;display:inline-block;cursor:pointer}\n"
".switch input{opacity:0;width:0;height:0}\n"
".slider{position:absolute;inset:0;background:rgba(148,163,184,.3);border-radius:999px;transition:.2s}\n"
".slider:before{content:'';position:absolute;width:18px;height:18px;left:3px;top:3px;\n"
"  background:#fff;border-radius:50%;transition:.2s}\n"
".switch input:checked + .slider{background:linear-gradient(135deg,var(--cyan),var(--blue))}\n"
".switch input:checked + .slider:before{transform:translateX(18px)}\n"
".toast{position:fixed;left:50%;bottom:30px;transform:translateX(-50%) translateY(20px);\n"
"  background:rgba(30,41,59,.95);border:1px solid rgba(34,211,238,.4);color:var(--text);\n"
"  padding:12px 20px;border-radius:12px;font-size:13.5px;opacity:0;pointer-events:none;\n"
"  transition:.25s;z-index:50;box-shadow:0 10px 30px rgba(2,8,23,.5)}\n"
".toast.show{opacity:1;transform:translateX(-50%) translateY(0)}\n"
".empty{font-size:12.5px;color:var(--sub);padding:8px 2px}\n"
".row2{display:grid;grid-template-columns:1fr 1fr;gap:10px}\n"
".add-rule{margin-top:6px}\n"
".dev-status{display:flex;flex-wrap:wrap;align-items:center;justify-content:center;gap:12px;margin-top:18px;\n"
"  padding:20px;border-radius:18px;font-size:14px;\n"
"  background:rgba(30,41,59,.42);backdrop-filter:blur(12px);\n"
"  border:1px solid rgba(148,163,184,.14);box-shadow:0 10px 30px rgba(2,8,23,.35);\n"
"  transition:.2s border-color}\n"
".dev-status:hover{border-color:rgba(34,211,238,.35)}\n"
".state-label{color:var(--sub);font-weight:600;font-size:14px}\n"
".state-val{font-weight:700;padding:6px 16px;border-radius:999px;font-size:14px;\n"
"  border:1px solid rgba(148,163,184,.25);font-variant-numeric:tabular-nums}\n"
".state-val.idle{color:var(--sub);background:rgba(100,116,139,.18)}\n"
".state-val.running{color:var(--green);background:rgba(34,197,94,.14);\n"
"  border-color:rgba(34,197,94,.5);box-shadow:0 0 12px rgba(34,197,94,.35)}\n"
"#login-mask{position:fixed;inset:0;z-index:9999;background:linear-gradient(135deg,#1f3a5f,#2c5364);\n"
"  display:flex;align-items:center;justify-content:center}\n"
"#login-mask.hidden{display:none}\n"
"#auth-mask{position:fixed;inset:0;z-index:9998;background:rgba(2,8,23,.6);backdrop-filter:blur(4px);display:flex;align-items:center;justify-content:center}\n"
"#auth-mask.hidden{display:none}\n"
".login-card{width:320px;max-width:90vw;background:#fff;border-radius:12px;padding:28px 26px;\n"
"  box-shadow:0 12px 40px rgba(0,0,0,.35)}\n"
".login-card h2{margin:0 0 4px;font-size:20px;color:#1f3a5f}\n"
".login-card .sub{margin:0 0 18px;font-size:12px;color:#888}\n"
".login-card label{display:block;font-size:13px;color:#555;margin:12px 0 5px}\n"
".login-card input{width:100%;box-sizing:border-box;padding:10px 12px;font-size:14px;\n"
"  border:1px solid #d0d7de;border-radius:8px;outline:none}\n"
".login-card input:focus{border-color:#2c5364}\n"
".login-card button{width:100%;margin-top:18px;padding:11px;font-size:15px;color:#fff;\n"
"  background:#2c5364;border:none;border-radius:8px;cursor:pointer}\n"
".login-card button:active{background:#1f3a5f}\n"
".login-msg{margin-top:12px;min-height:18px;font-size:13px;text-align:center}\n"
".login-msg.err{color:#c0392b}.login-msg.ok{color:#1a7f37}\n"
"</style>\n"
"</head>\n"
"<body>\n"
"<div id=\"login-mask\" class=\"hidden\">\n"
"  <div class=\"login-card\">\n"
"    <h2>" APP_TITLE " 配置台</h2>\n"
"    <p class=\"sub\">请登录后再查看与配置参数</p>\n"
"    <label for=\"li-user\">用户名</label>\n"
"    <input id=\"li-user\" type=\"text\" autocomplete=\"username\" placeholder=\"用户名\">\n"
"    <label for=\"li-pass\">密码</label>\n"
"    <input id=\"li-pass\" type=\"password\" autocomplete=\"current-password\" placeholder=\"密码\">\n"
"    <button id=\"li-btn\">登 录</button>\n"
"    <div class=\"login-msg\" id=\"li-msg\"></div>\n"
"  </div>\n"
"</div>\n"
"<div id=\"auth-mask\" class=\"hidden\">\n"
"  <div class=\"login-card\">\n"
"    <h2>修改登录凭据</h2>\n"
"    <p class=\"sub\">修改后立即生效并保存，下次登录请使用新用户名/密码</p>\n"
"    <label for=\"au-user\">新用户名</label>\n"
"    <input id=\"au-user\" type=\"text\" autocomplete=\"username\" placeholder=\"新用户名\">\n"
"    <label for=\"au-pass\">新密码</label>\n"
"    <input id=\"au-pass\" type=\"password\" autocomplete=\"new-password\" placeholder=\"新密码\">\n"
"    <label for=\"au-pass2\">确认新密码</label>\n"
"    <input id=\"au-pass2\" type=\"password\" autocomplete=\"new-password\" placeholder=\"再次输入新密码\">\n"
"    <button id=\"au-btn\">保 存</button>\n"
"    <div class=\"login-msg\" id=\"au-msg\"></div>\n"
"  </div>\n"
"</div>\n"
"<div class='wrap'>\n"
"  <div class='topbar'>\n"
"    <div class='topbar-head'>\n"
"      <div class='brand'>\n"
"        <div class='logo'>CK</div>\n"
"        <div>\n"
"          <h1>" APP_TITLE "控制台</h1>\n"
"          <div class='sub'>" APP_TITLE " · 本地配置 · 无需联网</div>\n"
"        </div>\n"
"      </div>\n"
"      <div class='stats'>\n"
"        <span class='chip'><span class='dot' id='d-ap'></span>AP <b id='ap-ip' style='color:var(--text)'>—</b></span>\n"
"        <span class='chip'><span class='dot' id='d-sta'></span>STA <b id='d-sta-ip' style='color:var(--text)'>—</b></span>\n"
"        <span class='chip'><span class='dot' id='d-bt'></span>蓝牙 <b id='bt' style='color:var(--text)'>—</b></span>\n"
"        <span class='chip'><span class='dot' id='d-run'></span>引擎 <b id='run' style='color:var(--text)'>—</b></span>\n"
"        <span class='chip' id='time-chip' title='灯亮表示已校时成功（北京时间 UTC+8）'><span class='dot' id='d-sync'></span>🕐 <b id='time' style='color:var(--text)'>—</b></span>\n"
"      </div>\n"
"    </div>\n"
"    <div class='toolbar'>\n"
"      <select id='profile-sel' class='btn ghost' title='选择要编辑的参数套：修改后请点“保存配置”才写入设备，切换不会自动保存'>\n"
"        <option value='0'>参数套 1</option>\n"
"        <option value='1'>参数套 2</option>\n"
"        <option value='2'>参数套 3</option>\n"
"      </select>\n"
"      <button class='btn save' id='save' title='保存全部参数（运行模式/权重/序列/定时/动作/WiFi/功率/单词）到 FLASH'>保存配置</button>\n"
"      <button class='btn primary' id='toggle'>启动</button>\n"
"      <button class='btn ghost' id='sync-browser-btn' title='用本机电脑时间一键校时（离线可用）'>校时(本机)</button>\n"
"      <button class='btn ghost' id='sync-btn' title='连上路由器后向 NTP 校时'>校时(NTP)</button>\n"
"      <button class='btn ghost' id='export-btn' title='导出全部参数为 JSON 文件'>导出</button>\n"
"      <button class='btn ghost' id='import-btn' title='从 JSON 文件导入参数'>导入</button>\n"
"      <input type='file' id='import-file' accept='.json,application/json' style='display:none'>\n"
"      <button class='btn danger' id='reset-default-btn' title='恢复出厂默认参数'>恢复默认</button>\n"
"      <button class='btn ghost' id='ble-reset-btn' title='断开当前蓝牙并清除配对，重新开始广播（等效按键长按重置，方便连接其他设备）'>复位蓝牙</button>\n"
"      <button class='btn danger' id='reboot-btn' title='软件重启设备（软复位），重启后配置保持不变'>重启</button>\n"
"      <button class='btn ghost' id='auth-btn' title='修改登录用户名/密码'>修改密码</button>\n"
"      <button class='btn ghost' id='logout-btn' title='退出当前登录'>退出登录</button>\n"
"    </div>\n"
"  </div>\n"
"  <div class='grid'>\n"
"    <div class='card'>\n"
"      <h2>运行模式与随机配置</h2>\n"
"      <div class='hint'>选择随机（按权重抽取）或序列（按编排顺序）模式。</div>\n"
"      <div class='tabs'>\n"
"        <div class='tab active' data-mode='0' id='tab-rand'>随机模式</div>\n"
"        <div class='tab' data-mode='1' id='tab-seq'>序列模式</div>\n"
"      </div>\n"
"      <div class='pane active' id='pane-rand'>\n"
"        <div id='weights'></div>\n"
"        <div class='total'>权重合计：<b id='wtotal'>0</b>（非 100 不强制，按相对比例抽取）</div>\n"
"      </div>\n"
"      <div class='pane' id='pane-seq'>\n"
"        <div class='empty'>序列模式请在右侧动作序列编排配置。当前循环策略：</div>\n"
"        <div class='radios' id='cycle-radios'>\n"
"          <label><input type='radio' name='cycle' value='0' checked> 顺序循环</label>\n"
"          <label><input type='radio' name='cycle' value='1'> 乱序洗牌</label>\n"
"          <label><input type='radio' name='cycle' value='2'> 单次（顺序一次后自动停止）</label>\n"
"        </div>\n"
"      </div>\n"
"    </div>\n"
"    <div class='card'>\n"
"      <h2>动作序列编排</h2>\n"
"      <div class='hint'>添加动作并排序，序列模式将按此顺序循环执行。</div>\n"
"      <div class='actions-add' id='add-bar'></div>\n"
"      <ul class='seq-list' id='seq-list'></ul>\n"
"      <div style='margin-top:14px'>\n"
"        <div class='radios'>\n"
"          <label><input type='radio' name='cycle2' value='0' checked> 顺序循环</label>\n"
"          <label><input type='radio' name='cycle2' value='1'> 乱序洗牌</label>\n"
"          <label><input type='radio' name='cycle2' value='2'> 单次（顺序一次后自动停止）</label>\n"
"        </div>\n"
"      </div>\n"
"    </div>\n"
"    <div class='card' style='grid-column:1/-1'>\n"
"      <h2>⌨️ 打字单词表</h2>\n"
"      <div class='hint'>“打字”动作会从下列单词中随机抽词，逐字符发送（词后自动追加空格）。词与词之间用空格分隔，可填入 C/C++ 关键字、Nordic 编程库 API、常见变量名等。修改后请点击右上角「保存配置」一并写入（与权重/序列/定时共享一份配置，避免单独保存互相冲掉）；若为空则使用内置默认单词表。</div>\n"
"      <textarea id='word-list' rows='6' style='width:100%;margin-top:10px;font-family:monospace;font-size:13px;padding:8px;border:1px solid #ccc;border-radius:6px;resize:vertical'></textarea>\n"
"    </div>\n"
"    <div class='card' style='grid-column:1/-1'>\n"
"      <div style='display:flex;justify-content:space-between;align-items:center'>\n"
"        <h2>⏰ 定时任务</h2>\n"
"        <button class='btn ghost' id='add-rule' style='padding:8px 13px;font-size:12.5px'>+ 新增</button>\n"
"      </div>\n"
"      <div class='hint'>定时启停（每天到点启/停引擎）、定时单动作、周期循环（最多 64 条）。AP 热点始终常开，可通过 192.168.4.1 访问本页。</div>\n"
"      <div id='rules' style='margin-top:12px;max-height:420px;overflow:auto;padding-right:6px'></div>\n"
"    </div>\n"
"    <div class='card' style='grid-column:1/-1'>\n"
"      <h2>📶 无线配网与射频功率</h2>\n"
"      <div class='hint'>STA 用于连接路由器（AP 热点不关闭）；射频功率可调，近距离 5M 建议低档以降发热，修改后保存即生效并持久化。</div>\n"
"      <div class='row2' style='align-items:start'>\n"
"        <div>\n"
"          <h2 style='font-size:14px'>WiFi 配网（连路由器）</h2>\n"
"          <div class='field' style='margin-top:12px'>\n"
"            <label>路由器 SSID</label>\n"
"            <input id='sta-ssid' placeholder='例如 Home-WiFi'>\n"
"          </div>\n"
"          <div class='field' style='margin-top:10px'>\n"
"            <label>密码</label>\n"
"            <input id='sta-pass' type='password' placeholder='路由器密码'>\n"
"          </div>\n"
"          <div class='field' style='margin-top:10px'>\n"
"            <label>IP 获取方式</label>\n"
"            <select id='sta-addr-mode'>\n"
"              <option value='dhcp'>DHCP 自动获取</option>\n"
"              <option value='static'>静态 IP</option>\n"
"            </select>\n"
"          </div>\n"
"          <div id='sta-static-box' style='display:none;margin-top:6px'>\n"
"            <div class='field' style='margin-top:8px'>\n"
"              <label>静态 IP</label>\n"
"              <input id='sta-ip' placeholder='例如 192.168.1.50'>\n"
"            </div>\n"
"            <div class='field' style='margin-top:8px'>\n"
"              <label>子网掩码</label>\n"
"              <input id='sta-netmask' placeholder='例如 255.255.255.0'>\n"
"            </div>\n"
"            <div class='field' style='margin-top:8px'>\n"
"              <label>网关</label>\n"
"              <input id='sta-gw' placeholder='例如 192.168.1.1'>\n"
"            </div>\n"
"            <div class='field' style='margin-top:8px'>\n"
"              <label>DNS（可选）</label>\n"
"              <input id='sta-dns' placeholder='例如 192.168.1.1'>\n"
"            </div>\n"
"          </div>\n"
"          <button class='btn' id='save-wifi' style='margin-top:12px;width:100%'>保存并连接路由器</button>\n"
"          <div class='empty' style='margin-top:10px'>保存后设备将并发连接路由器，AP 热点不关闭。连上后左侧 STA IP 会显示局域网地址。选静态 IP 时需确保地址在路由器网段且未被占用。</div>\n"
"        </div>\n"
"        <div>\n"
"          <h2 style='font-size:14px'>射频发射功率（可保存）</h2>\n"
"          <div class='field' style='margin-top:12px'>\n"
"            <label>WiFi 功率</label>\n"
"            <select id='wifi-power'>\n"
"              <option value='8'>2 dBm</option>\n"
"              <option value='20'>5 dBm</option>\n"
"              <option value='40'>10 dBm（默认）</option>\n"
"              <option value='56'>14 dBm</option>\n"
"              <option value='72'>18 dBm</option>\n"
"              <option value='80'>20 dBm</option>\n"
"            </select>\n"
"          </div>\n"
"          <div class='field' style='margin-top:10px'>\n"
"            <label>BLE 功率</label>\n"
"            <select id='ble-power'>\n"
"              <option value='0'>-12 dBm</option>\n"
"              <option value='1'>-9 dBm</option>\n"
"              <option value='2'>-6 dBm</option>\n"
"              <option value='3'>-3 dBm</option>\n"
"              <option value='4'>0 dBm</option>\n"
"              <option value='5'>+3 dBm（默认）</option>\n"
"              <option value='6'>+6 dBm</option>\n"
"              <option value='7'>+9 dBm</option>\n"
"            </select>\n"
"          </div>\n"
"          <div class='empty' style='margin-top:10px'>功率越低发热与功耗越小，覆盖范围越短。5M 内建议 WiFi 10dBm / BLE -3dBm。</div>\n"
"        </div>\n"
"      </div>\n"
"    </div>\n"
"    <div class='card' style='grid-column:1/-1'>\n"
"      <h2>⏱ 动作时间配置</h2>\n"
"      <div class='card-head' style='margin-top:6px'><button class='btn ghost' id='reset-timing'>恢复默认时间</button></div>\n"
"      <div id='timing' class='tcards'></div>\n"
"      <div class='empty' style='margin-top:10px'>每个动作的时间(ms)与重复次数：在 [最小,最大] 区间内随机取值；状态灯为固定时长。修改后请点击右上角「保存配置」才会写入（与权重/序列/定时共享一份配置，减少 FLASH 擦写）。</div>\n"
"      <div style='margin-top:18px;border-top:1px solid rgba(148,163,184,.14);padding-top:14px'>\n"
"        <h2 style='font-size:15px'>🖱️ 运动模式</h2>\n"
"        <div class='hint'>鼠标复位/活动范围等运动参数（每套参数独立）。修改后请点击右上角「保存配置」才会写入。</div>\n"
"        <div id='motion'></div>\n"
"      </div>\n"
"    </div>\n"
"  </div>\n"
"</div>\n"
"<div class='toast' id='toast'></div>\n"
"<footer class='dev-status'>\n"
"  <span class='state-label'>设备状态</span>\n"
"  <span class='state-val idle' id='cur-action'>空闲</span>\n"
"  <span class='state-label'>版本</span>\n"
"  <span class='state-val' id='ver'>—</span>\n"
"  <button class='btn ghost' id='ota-btn' title='选择固件 .bin 上传升级'>升级固件</button>\n"
"  <input type='file' id='ota-file' accept='.bin' style='display:none'>\n"
"  <span id='ota-msg' style='color:var(--sub);font-size:13px'></span>\n"
"</footer>\n"
"<script>\n"
"const ACT_NAMES=['拖拽','点击','滚轮','方向键','休息','滑动','打字','切换程序'];\n"
"const ACT_KEYS =['drag','click','wheel','arrow','rest','move','word','alt_tab'];\n"
"let cfg=null;\n"
"/* 安全绑定：元素不存在时跳过，避免单个缺失元素导致整个脚本崩溃（此前 reset-timing 缺失曾中断 loadAll） */\n"
"/* ev 支持 'onclick'/'click' 等写法，统一规范为 'on'+事件名 作为元素属性赋值 */\n"
"function on(id,ev,fn){const e=document.getElementById(id);if(!e){console.warn('元素缺失，跳过绑定：'+id);return;}\n"
"  const prop=ev.indexOf('on')===0?ev:('on'+ev);e[prop]=fn;}\n"
"function toast(msg){const t=document.getElementById('toast');t.textContent=msg;t.classList.add('show');\n"
"  setTimeout(()=>t.classList.remove('show'),2200);}\n"
"function debounce(fn,d){let t;return(...a)=>{clearTimeout(t);t=setTimeout(()=>fn(...a),d);};}\n"
"const TOKEN_KEY='ble_km_token';\n"
"function getToken(){return localStorage.getItem(TOKEN_KEY)||'';}\n"
"function setToken(v){localStorage.setItem(TOKEN_KEY,v);}\n"
"function clearToken(){localStorage.removeItem(TOKEN_KEY);}\n"
"async function api(path,opt){const ctl=new AbortController();const t=setTimeout(()=>ctl.abort(),4000);\n"  // 4s 超时，避免请求卡死堆积
"  const tk=getToken();const hdr=tk?{Authorization:'Bearer '+tk}:{};\n"
"  try{const r=await fetch(path,{...opt,headers:{...hdr,...(opt&&opt.headers)},signal:ctl.signal});\n"
"    let j=null;try{j=await r.json();}catch(e){j={};}\n"
"    if(typeof j!=='object'||j===null) j={};\n"
"    /* 鉴权由服务器保证：HTTP 200 即代表已登录；若响应体未带 ok 字段则补 ok:true */\n"
"    if(r.ok && typeof j.ok==='undefined') j.ok=true;\n"
"    else if(!r.ok && typeof j.ok==='undefined') j.ok=false;\n"
"    if(r.status===401){ handleUnauth(); } return j;}\n"
"  finally{clearTimeout(t);}}\n"
"function handleUnauth(){ clearToken(); showLogin('登录已失效，请重新登录'); }\n"
"function showLogin(msg){const m=document.getElementById('login-mask');if(m)m.classList.remove('hidden');\n"
"  if(msg){const e=document.getElementById('li-msg');if(e){e.textContent=msg;e.className='login-msg err';}}\n"
"  const wrap=document.querySelector('.wrap');if(wrap)wrap.style.visibility='hidden';\n"
"  const ft=document.querySelector('footer');if(ft)ft.style.visibility='hidden';}\n"
"function hideLogin(){const m=document.getElementById('login-mask');if(m)m.classList.add('hidden');\n"
"  const wrap=document.querySelector('.wrap');if(wrap)wrap.style.visibility='visible';\n"
"  const ft=document.querySelector('footer');if(ft)ft.style.visibility='visible';}\n"
"async function doLogin(){const u=document.getElementById('li-user').value.trim();\n"
"  const p=document.getElementById('li-pass').value;const msg=document.getElementById('li-msg');\n"
"  msg.textContent='登录中…';msg.className='login-msg';\n"
"  const r=await fetch('/api/login',{method:'POST',headers:{'Content-Type':'application/json'},\n"
"    body:JSON.stringify({user:u,pass:p})});\n"
"  let j={};try{j=await r.json();}catch(e){}\n"
"  if(j.ok && j.token){ setToken(j.token); msg.textContent='登录成功';msg.className='login-msg ok';\n"
"    hideLogin(); afterLogin(); }\n"
"  else { msg.textContent=(j.msg||'登录失败');msg.className='login-msg err'; }}\n"
"async function loadAll(){\n"
"  let loaded=false;\n"
"  for(let i=0;i<3 && !loaded;i++){\n"
"    try{ cfg=await api('/api/config'); loaded=true; }\n"
"    catch(e){ if(i<2) await new Promise(r=>setTimeout(r,600)); }\n"
"  }\n"
"  if(!loaded){ toast('配置加载失败，使用默认值');\n"
"    cfg=defaultConfigFull();\n"
"  }\n"
"  /* 与设备端“尾部补齐”迁移同理：缺失的新增参数用默认值补上，保证页面不出现 undefined */\n"
"  if(!cfg.profiles||!cfg.profiles.length) cfg.profiles=[defaultProfile(),defaultProfile(),defaultProfile()];\n"
"  cfg.active_profile=(cfg.active_profile|0); if(cfg.active_profile<0||cfg.active_profile>2) cfg.active_profile=0;\n"
"  applyProfileView();\n"
"  const psel=document.getElementById('profile-sel'); if(psel) psel.value=cfg.active_profile;\n"
"\n"
"  cfg.radio=cfg.radio||{wifi_power_025dbm:40,ble_power_level:5};\n"
"  renderWeights();renderTabs();renderSeq();renderRules();renderTiming();renderMotion();\n"
"  /* SSID/密码默认值：NVS 无配置时填充常用路由器 */\n"
"  document.getElementById('sta-ssid').value=cfg.wifi.sta_ssid||'';\n"
"  document.getElementById('sta-pass').value=cfg.wifi.sta_pass||'';\n"
"  const dhcp = (cfg.wifi.sta_dhcp!==false);\n"
"  document.getElementById('sta-addr-mode').value = dhcp ? 'dhcp' : 'static';\n"
"  document.getElementById('sta-ip').value      = cfg.wifi.sta_ip||'';\n"
"  document.getElementById('sta-netmask').value= cfg.wifi.sta_netmask||'';\n"
"  document.getElementById('sta-gw').value     = cfg.wifi.sta_gw||'';\n"
"  document.getElementById('sta-dns').value    = cfg.wifi.sta_dns||'';\n"
"  document.getElementById('sta-static-box').style.display = dhcp ? 'none' : 'block';\n"
"  document.getElementById('wifi-power').value=String(cfg.radio.wifi_power_025dbm);\n"
"  document.getElementById('ble-power').value=String(cfg.radio.ble_power_level);\n"
"  document.getElementById('word-list').value=(cfg.word_list||'');\n"
"}\n"
"function defaultTiming(){return {drag_repeat_min:1,drag_repeat_max:5,drag_distance_min:20,drag_distance_max:100,drag_step_min:10,drag_step_max:30,drag_interval_min:600,drag_interval_max:1500,drag_end_delay_min:500,drag_end_delay_max:5000,\n"
"  click_repeat_min:1,click_repeat_max:10,click_distance_min:10,click_distance_max:100,click_step_min:1,click_step_max:30,click_hold_min:20,click_hold_max:250,click_interval_min:100,click_interval_max:1000,click_end_delay_min:1000,click_end_delay_max:5000,\n"
"  wheel_repeat_min:1,wheel_repeat_max:5,wheel_distance_min:10,wheel_distance_max:100,wheel_step_min:1,wheel_step_max:30,wheel_tick_min:1,wheel_tick_max:8,wheel_interval_min:100,wheel_interval_max:500,wheel_end_delay_min:1000,wheel_end_delay_max:5000,\n"
"  arrow_repeat_min:1,arrow_repeat_max:20,arrow_interval_min:50,arrow_interval_max:800,arrow_end_delay_min:1000,arrow_end_delay_max:5000,\n"
"  rest_delay_min:1000,rest_delay_max:20000,\n"
"  move_repeat_min:1,move_repeat_max:20,move_distance_min:10,move_distance_max:30,move_step_min:1,move_step_max:15,\n"
"  move_interval_min:100,move_interval_max:500,move_end_delay_min:500,move_end_delay_max:1000,\n"
"  led_blink_on_ms:80,led_freq_per_1min_ms:50,led_freq_max_ms:2000,led_blink_once_ms:200,led_blink_once_gap_ms:200,\n"
"  word_repeat_min:1,word_repeat_max:5,word_char_delay_min:40,word_char_delay_max:700,word_space_delay_min:40,word_space_delay_max:1000,word_interval_min:500,word_interval_max:2000,word_end_delay_min:500,word_end_delay_max:1200,\n"
"  alt_tab_repeat_min:0,alt_tab_repeat_max:1,alt_tab_interval_min:500,alt_tab_interval_max:1000,alt_tab_end_delay_min:700,alt_tab_end_delay_max:1500};}\n"
"function defaultMotion(){return {screen_scale_pct:125,pos_limit_x:400,pos_limit_y:200,home_corner:0,home_push_px:2000,home_back_x:400,home_back_y:200};}\n"
"function defaultProfile(){return {weights:JSON.parse(JSON.stringify(DEFAULT_CONFIG.weights)),timing:defaultTiming(),motion:defaultMotion()};}\n"
"function defaultConfigFull(){const d=JSON.parse(JSON.stringify(DEFAULT_CONFIG));d.active_profile=0;d.motion=defaultMotion();d.profiles=[defaultProfile(),defaultProfile(),defaultProfile()];return d;}\n"
"function applyProfileView(){const p=cfg.profiles[cfg.active_profile];p.weights=Object.assign({},DEFAULT_CONFIG.weights,p.weights||{});p.timing=Object.assign({},defaultTiming(),p.timing||{});p.motion=Object.assign({},defaultMotion(),p.motion||{});cfg.weights=p.weights;cfg.timing=p.timing;cfg.motion=p.motion;}\n"
"let _refreshing=false;async function refreshStatus(){\n"  // 节流：上一次未完成则不叠加，避免请求堆积
"  if(_refreshing)return;_refreshing=true;\n"
"  try{\n"
"    const s=await api('/api/status');\n"
"    setChip('ap-ip',s.ap_ip,'d-ap',s.ap_ip!=='0.0.0.0');\n"
"    setChip('d-sta-ip',s.sta_ip,'d-sta',s.sta_ip!=='0.0.0.0');\n"
"    setChip('bt',s.bt?'已连接':'未连接','d-bt',s.bt);\n"
"    const run=s.running;setChip('run',run?'运行中':'停止','d-run',run,true);\n"
"    const b=document.getElementById('toggle');b.textContent=run?'停止':'启动';\n"
"    b.className=run?'btn danger':'btn';\n"
"    if(s.time){document.getElementById('time').textContent=s.time;}\n"
"    if(s.version){const ve=document.getElementById('ver');if(ve)ve.textContent=s.version;}\n"
"    const synced=!!s.time_synced;\n"
"    const sd=document.getElementById('d-sync');if(sd)sd.className='dot'+(synced?' on':'');\n"
"    const actEl=document.getElementById('cur-action');\n"
"    if(actEl){const an=(s.running&&s.action)?s.action:'空闲';\n"
"      let pr=(s.running&&s.progress)?('（'+s.progress+'次）'):'';\n"
"      if(s.running && an==='休息' && s.rest_remaining>0){pr='（剩 '+Math.ceil(s.rest_remaining)+'s）';}\n"
"      actEl.textContent=an+pr;\n"
"      actEl.className='state-val'+(s.running?' running':' idle');}\n"
"  }catch(e){ /* 网络异常时保持当前显示，下次轮询重试 */ }\n"
"  finally{_refreshing=false;}\n"
"}\n"
"function setChip(id,val,dot,on,run){document.getElementById(id).textContent=val;\n"
"  const d=document.getElementById(dot);d.className='dot'+(on?' on':'')+(run&&on?' run':'');}\n"
"function renderWeights(){\n"
" try{\n"
"  const wrap=document.getElementById('weights');wrap.innerHTML='';\n"
"  let total=0;\n"
"  ACT_KEYS.forEach((k,i)=>{\n"
"    const v=cfg.weights[k];total+=v;\n"
"    const row=document.createElement('div');row.className='wrow';\n"
"    row.innerHTML='<label>'+ACT_NAMES[i]+'</label>'\n"
"      +'<input type=range min=0 max=100 value='+v+' data-k='+k+'>'\n"
"      +'<span class=val>'+v+'</span>';\n"
"    const rng=row.querySelector('input');const val=row.querySelector('.val');\n"
"    rng.addEventListener('input',()=>{val.textContent=rng.value;cfg.weights[k]=+rng.value;\n"
"      document.getElementById('wtotal').textContent=sumWeight();scheduleSave();});\n"
"    wrap.appendChild(row);\n"
"  });\n"
"  document.getElementById('wtotal').textContent=total;\n"
" }catch(e){console.error('renderWeights',e);}\n"
"}\n"
"function sumWeight(){try{return ACT_KEYS.reduce((a,k)=>a+cfg.weights[k],0);}catch(e){return 100;}}\n"
"function renderTabs(){\n"
" try{\n"
"  document.querySelectorAll('.tab').forEach(t=>{\n"
"    t.classList.toggle('active',+t.dataset.mode===cfg.run_mode);\n"
"    t.onclick=()=>{cfg.run_mode=+t.dataset.mode;\n"
"      document.getElementById('tab-rand').classList.toggle('active',cfg.run_mode===0);\n"
"      document.getElementById('tab-seq').classList.toggle('active',cfg.run_mode===1);\n"
"      document.getElementById('pane-rand').classList.toggle('active',cfg.run_mode===0);\n"
"      document.getElementById('pane-seq').classList.toggle('active',cfg.run_mode===1);\n"
"      scheduleSave();};\n"
"  });\n"
"  document.querySelectorAll('input[name=cycle],input[name=cycle2]').forEach(r=>{\n"
"    r.checked=(+r.value===cfg.sequence.cycle);\n"
"    r.onchange=()=>{cfg.sequence.cycle=+r.value;\n"
"      document.querySelectorAll('input[name=cycle],input[name=cycle2]')\n"
"        .forEach(x=>x.checked=(+x.value===cfg.sequence.cycle));scheduleSave();};\n"
"  });\n"
" }catch(e){console.error('renderTabs',e);}\n"
"}\n"
"function renderSeq(){\n"
" try{\n"
"  const bar=document.getElementById('add-bar');bar.innerHTML='';\n"
"  ACT_NAMES.forEach((n,i)=>{const b=document.createElement('button');b.className='addbtn';\n"
"    b.textContent='+ '+n;b.onclick=()=>{if(cfg.sequence.actions.length>=64){toast('序列已满(64)');return;}\n"
"      cfg.sequence.actions.push(i);renderSeq();scheduleSave();};bar.appendChild(b);});\n"
"  const list=document.getElementById('seq-list');list.innerHTML='';\n"
"  if(cfg.sequence.actions.length===0){list.innerHTML='<div class=empty>暂无动作，点击上方按钮添加</div>';return;}\n"
"  cfg.sequence.actions.forEach((a,i)=>{\n"
"    const li=document.createElement('li');li.className='seq-item';\n"
"    li.innerHTML='<span class=idx>'+(i+1)+'</span><span class=name>'+ACT_NAMES[a]+'</span>'\n"
"      +'<button class=mv data-i='+i+' data-d=-1>↑</button>'\n"
"      +'<button class=mv data-i='+i+' data-d=1>↓</button>'\n"
"      +'<button class=del data-i='+i+'>✕</button>';\n"
"    li.querySelectorAll('[data-d]').forEach(mv=>{mv.onclick=(e)=>{const i2=+e.currentTarget.dataset.i,d=+e.currentTarget.dataset.d;\n"
"      const j=i2+d;if(j<0||j>=cfg.sequence.actions.length)return;\n"
"      const t=cfg.sequence.actions[i2];cfg.sequence.actions[i2]=cfg.sequence.actions[j];\n"
"      cfg.sequence.actions[j]=t;renderSeq();scheduleSave();}});\n"
"    li.querySelector('.del').onclick=(e)=>{cfg.sequence.actions.splice(+e.target.dataset.i,1);\n"
"      renderSeq();scheduleSave();};\n"
"    list.appendChild(li);\n"
"  });\n"
" }catch(e){console.error('renderSeq',e);}\n"
"}\n"
"function renderRules(){\n"
" try{\n"
"  const wrap=document.getElementById('rules');wrap.innerHTML='';\n"
"  if(!cfg.timers||cfg.timers.length===0){wrap.innerHTML=\"<div class=empty>暂无规则，点击「+ 新增」</div>\";return;}\n"
"  cfg.timers.forEach((r,i)=>{\n"
"    const div=document.createElement('div');div.className='rule';\n"
"    const typeName=['定时启停','定时单动作','周期循环'][r.type];\n"
"    const opts=ACT_NAMES.map((n,k)=>'<option value='+k+(r.action_id==k?' selected':'')+'>'+n+'</option>').join('');\n"
"    div.innerHTML=\n"
"      '<div class=rh><span class=t>'+typeName+' #'+(i+1)+'</span>'\n"
"      +'<label class=switch><input type=checkbox '+(r.enabled?'checked':'')+' data-f=enabled>'\n"
"      +'<span class=slider></span></label></div>'\n"
"      +'<div class=rgrid>'\n"
"      +'<div class=field><label>类型</label><select data-f=type>'\n"
"      +'<option value=0 '+(r.type==0?'selected':'')+'>定时启停</option>'\n"
"      +'<option value=1 '+(r.type==1?'selected':'')+'>定时单动作</option>'\n"
"      +'<option value=2 '+(r.type==2?'selected':'')+'>周期循环</option></select></div>'\n"
"      +'<div class=field data-show=hm><label>时刻(时)</label><input type=number min=0 max=23 value='+r.hour+' data-f=hour></div>'\n"
"      +'<div class=field data-show=hm><label>分</label><input type=number min=0 max=59 value='+r.minute+' data-f=minute></div>'\n"
"      +'<div class=field data-show=act><label>动作</label><select data-f=action_id>'+opts+'</select></div>'\n"
"      +'<div class=field data-show=startstop><label>启/停</label><select data-f=ss_action>'\n"
"      +'<option value=0 '+(r.ss_action==0?'selected':'')+'>启动</option>'\n"
"      +'<option value=1 '+(r.ss_action==1?'selected':'')+'>停止</option></select></div>'\n"
"      +'<div class=field data-show=period><label>周期(分钟)</label><input type=number min=1 max=1440 value='+(r.period_min||1)+' data-f=period_min></div>'\n"
"      +'<div class=field><label>删除</label><button class=btn ghost data-del='+i+' style=width:100%;padding:9px>删除该规则</button></div>'\n"
"      +'</div>';\n"
"    div.querySelectorAll('[data-f]').forEach(el=>{\n"
"      el.addEventListener('change',()=>{const f=el.dataset.f;\n"
"        if(f==='type'){cfg.timers[i].type=+el.value;renderRules();}\n"
"        else if(f==='enabled')cfg.timers[i].enabled=el.checked;\n"
"        else if(f==='hour')cfg.timers[i].hour=+el.value;\n"
"        else if(f==='minute')cfg.timers[i].minute=+el.value;\n"
"        else if(f==='action_id')cfg.timers[i].action_id=+el.value;\n"
"        else if(f==='ss_action')cfg.timers[i].ss_action=+el.value;\n"
"        else if(f==='period_min')cfg.timers[i].period_min=+el.value;\n"
"        scheduleSave();});\n"
"    });\n"
"    div.querySelector('[data-del]').onclick=()=>{cfg.timers.splice(+i,1);\n"
"      cfg.timer_count=cfg.timers.length;renderRules();scheduleSave();};\n"
"    wrap.appendChild(div);\n"
"  });\n"
"  wrap.querySelectorAll('.rule').forEach((rd,idx)=>{\n"
"    const t=cfg.timers[idx].type;\n"
"    rd.querySelector('[data-show=hm]').style.display=(t===0||t===1)?'':'none';\n"
"    rd.querySelector('[data-show=act]').style.display=(t===1)?'':'none';\n"
"    rd.querySelector('[data-show=startstop]').style.display=(t===0)?'':'none';\n"
"    rd.querySelector('[data-show=period]').style.display=(t===2)?'':'none';\n"
"  });\n"
" }catch(e){console.error('renderRules',e);}\n"
"}\n"
"function renderMotion(){\n"
" try{\n"
"  const wrap=document.getElementById('motion'); if(!wrap)return; wrap.innerHTML=''; wrap.className='tcards';\n"
"  const groups=[\n"
"   {title:'鼠标运动范围',rows:[['显示缩放(%)','screen_scale_pct',1,400,''],['活动范围X(±px)','pos_limit_x',1,4000,'px'],['活动范围Y(±px)','pos_limit_y',1,4000,'px']]},\n"
"   {title:'鼠标复位',rows:[['复位推程PUSH(px)','home_push_px',1,8000,'px'],['回中心X(px)','home_back_x',0,8000,'px'],['回中心Y(px)','home_back_y',0,8000,'px']]}\n"
"  ];\n"
"  groups.forEach(g=>{\n"
"   const card=document.createElement('div');card.className='tcard';\n"
"   card.innerHTML='<div class=tch>'+g.title+'</div>';\n"
"   g.rows.forEach(r=>{const [label,k,lo,hi,unit]=r;\n"
"    const row=document.createElement('div');row.className='trow';\n"
"    row.innerHTML='<label>'+label+'</label>'\n"
"     +'<span class=min><input type=number min='+lo+' max='+hi+' value='+(cfg.motion[k]||0)+' data-k='+k+'></span>'\n"
"     +'<span class=sep></span><span class=max></span><span class=u>'+unit+'</span>';\n"
"    row.querySelector('input').addEventListener('input',()=>{cfg.motion[k]=+row.querySelector('input').value;});\n"
"    card.appendChild(row);});\n"
"   wrap.appendChild(card);});\n"
"  const card=document.createElement('div');card.className='tcard';\n"
"  card.innerHTML='<div class=tch>复位撞角</div>';\n"
"  const cn=['右上角','左上角','右下角','左下角'];\n"
"  const row=document.createElement('div');row.className='trow';\n"
"  row.innerHTML='<label>复位撞角</label><span class=min style=\\'flex:1;max-width:none\\'><select data-k=home_corner>'+cn.map((n,i)=>'<option value='+i+' '+(cfg.motion.home_corner==i?'selected':'')+'>'+n+'</option>').join('')+'</select></span><span class=sep></span><span class=max></span>';\n"
"  row.querySelector('select').addEventListener('change',()=>{cfg.motion.home_corner=+row.querySelector('select').value;});\n"
"  card.appendChild(row);wrap.appendChild(card);\n"
" }catch(e){console.error('renderMotion',e);}\n"
"}\n"
"function buildConfigBody(){\n"
"  cfg.profiles[cfg.active_profile].weights=cfg.weights;\n"
"  cfg.profiles[cfg.active_profile].timing=cfg.timing;\n"
"  cfg.profiles[cfg.active_profile].motion=cfg.motion;\n"
"  return {active_profile:cfg.active_profile,profiles:cfg.profiles,run_mode:cfg.run_mode,weights:cfg.weights,timing:cfg.timing,motion:cfg.motion,\n"
"    sequence:{count:cfg.sequence.actions.length,actions:cfg.sequence.actions,cycle:cfg.sequence.cycle},\n"
"    timers:cfg.timers,timer_count:cfg.timers.length,\n"
"    wifi:{sta_enabled:cfg.wifi.sta_enabled,sta_ssid:cfg.wifi.sta_ssid,sta_pass:cfg.wifi.sta_pass,\n"
"      sta_dhcp:cfg.wifi.sta_dhcp,sta_ip:cfg.wifi.sta_ip,sta_netmask:cfg.wifi.sta_netmask,\n"
"      sta_gw:cfg.wifi.sta_gw,sta_dns:cfg.wifi.sta_dns},\n"
"    radio:{wifi_power_025dbm:cfg.radio.wifi_power_025dbm,ble_power_level:cfg.radio.ble_power_level},\n"
"    word_list:document.getElementById('word-list').value};\n"
"}\n"
"async function postConfig(){return await api('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},\n"
"    body:JSON.stringify(buildConfigBody())});}\n"
"/* 已禁用防抖自动保存：仅「保存配置」按钮写入 NVS，避免频繁擦写 FLASH */\n"
"const scheduleSave=()=>{};\n"
"on('save','onclick',async()=>{\n"
"  const bad=validateTiming();\n"
"  if(bad>0){\n"
"    const first=document.querySelector('#timing input.bad');\n"
"    if(first){first.scrollIntoView({behavior:'smooth',block:'center'});first.focus();}\n"
"    toast('有 '+bad+' 个动作参数不合法（已标红），请修正后再保存');return;}\n"
"  const j=await postConfig();\n"
"  if(j.ok){document.querySelectorAll('#timing input.bad').forEach(i=>i.classList.remove('bad'));}\n"
"  toast(j.ok?'已保存':('保存失败：'+(j.msg||'')));\n"
"});\n"
"/* ——— 导出/导入全部参数（JSON） ——— */\n"
"/* 重新渲染所有控件：导入新配置后调用，让页面与 cfg 同步 */\n"
"function renderAll(){\n"
"  /* 导入的历史 JSON 可能不含三套参数/运动模式，用默认值补齐 */\n"
"  if(!cfg.profiles||!cfg.profiles.length) cfg.profiles=[defaultProfile(),defaultProfile(),defaultProfile()];\n"
"  cfg.active_profile=(cfg.active_profile|0); if(cfg.active_profile<0||cfg.active_profile>2) cfg.active_profile=0;\n"
"  if(cfg.weights) cfg.profiles[cfg.active_profile].weights=Object.assign({},DEFAULT_CONFIG.weights,cfg.weights);\n"
"  if(cfg.timing) cfg.profiles[cfg.active_profile].timing=Object.assign({},defaultTiming(),cfg.timing);\n"
"  if(cfg.motion) cfg.profiles[cfg.active_profile].motion=Object.assign({},defaultMotion(),cfg.motion);\n"
"  applyProfileView();\n"
"  const psel=document.getElementById('profile-sel'); if(psel) psel.value=cfg.active_profile;\n"
"  renderWeights();renderTabs();renderSeq();renderRules();renderTiming();renderMotion();\n"
"  document.getElementById('sta-ssid').value=cfg.wifi.sta_ssid||'';\n"
"  document.getElementById('sta-pass').value=cfg.wifi.sta_pass||'';\n"
"  const dhcp = (cfg.wifi.sta_dhcp!==false);\n"
"  document.getElementById('sta-addr-mode').value = dhcp ? 'dhcp' : 'static';\n"
"  document.getElementById('sta-ip').value      = cfg.wifi.sta_ip||'';\n"
"  document.getElementById('sta-netmask').value= cfg.wifi.sta_netmask||'';\n"
"  document.getElementById('sta-gw').value     = cfg.wifi.sta_gw||'';\n"
"  document.getElementById('sta-dns').value    = cfg.wifi.sta_dns||'';\n"
"  document.getElementById('sta-static-box').style.display = dhcp ? 'none' : 'block';\n"
"  document.getElementById('wifi-power').value=String(cfg.radio.wifi_power_025dbm);\n"
"  document.getElementById('ble-power').value=String(cfg.radio.ble_power_level);\n"
"  document.getElementById('word-list').value=(cfg.word_list||'');\n"
"}\n"
"/* 导出：把当前内存中的完整 cfg 序列化为 JSON 并触发浏览器下载 */\n"
"on('export-btn','onclick',()=>{\n"
"  try{scheduleSave();}catch(e){}\n"
"  const data=JSON.stringify(cfg,null,2);\n"
"  const blob=new Blob([data],{type:'application/json'});\n"
"  const url=URL.createObjectURL(blob);\n"
"  const a=document.createElement('a');\n"
"  const ts=new Date().toISOString().replace(/[:.]/g,'-').slice(0,19);\n"
"  a.href=url;a.download='ble_km-config-'+ts+'.json';\n"
"  document.body.appendChild(a);a.click();a.remove();\n"
"  URL.revokeObjectURL(url);\n"
"  toast('已导出全部参数(JSON)');\n"
"});\n"
"/* 导入：选择 JSON 文件 -> 解析 -> 深合并到当前 cfg（等价于手动逐字段填写）-> 重渲染；不直接保存，仅点“保存配置”才生效 */\n"
"on('import-btn','onclick',()=>{document.getElementById('import-file').click();});\n"
"/* 深合并：对象递归合并，数组/基本类型直接覆盖；结果写入 target（返回 target） */\n"
"function deepMerge(target,src){if(!src||typeof src!=='object'||Array.isArray(src))return target;\n"
"  Object.keys(src).forEach(k=>{const v=src[k];\n"
"    if(v&&typeof v==='object'&&!Array.isArray(v)&&target[k]&&typeof target[k]==='object'&&!Array.isArray(target[k]))\n"
"      deepMerge(target[k],v);\n"
"    else target[k]=Array.isArray(v)?v.slice():v;});return target;}\n"
"on('import-file','onchange',(e)=>{\n"
"  const file=e.target.files&&e.target.files[0];e.target.value='';\n"
"  if(!file){return;}\n"
"  const reader=new FileReader();\n"
"  reader.onerror=()=>{toast('导入失败：文件读取错误');};\n"
"  reader.onload=()=>{\n"
"    let obj;\n"
"    try{obj=JSON.parse(reader.result);}catch(err){toast('导入失败：不是合法 JSON');return;}\n"
"    if(!obj||typeof obj!=='object'||Array.isArray(obj)){toast('导入失败：文件结构不正确');return;}\n"
"    try{ deepMerge(cfg,obj); renderAll();\n"
"      toast('已导入参数，请点“保存配置”生效'); }\n"
"    catch(err){ console.error('import',err); toast('导入失败：'+(err&&err.message||err)); }\n"
"  };\n"
"  reader.readAsText(file);\n"
"});\n"
"/* 参数套切换：只是切换“当前显示/编辑”的套，不自动保存到设备（避免“切换保存失败”）。\n"
"   三套参数始终都在内存里，UI 修改直接写进对应套的对象，切换不会丢数据；\n"
"   点“保存配置”会把全部三套写回设备，并用当前选中套作为生效套。 */\n"
"on('profile-sel','onchange',e=>{\n"
"  const idx=+e.target.value;\n"
"  cfg.active_profile=idx; applyProfileView();\n"
"  renderWeights();renderTiming();renderMotion();\n"
"  toast('已切换至参数套 '+(idx+1)+'（未保存，请点“保存配置”生效）');\n"
"});\n"
"on('reset-timing','onclick',async()=>{\n"
"  if(!confirm('确定将动作时间恢复为默认值？当前自定义值会被覆盖并保存。'))return;\n"
"  cfg.timing=defaultTiming(); cfg.profiles[cfg.active_profile].timing=cfg.timing; renderTiming();\n"
"  const j=await postConfig();toast(j.ok?'已恢复默认时间':'恢复失败');\n"
"});\n"
"/* 恢复全部默认参数：内置出厂默认值（与 ble_km-config-2026-08-26T06-43-13.json 一致） */\n"
"const DEFAULT_CONFIG={run_mode:0,weights:{drag:0,click:50,wheel:50,arrow:30,rest:35,move:50,word:0,alt_tab:0},sequence:{actions:[5,1,2,5,1,2,5,1,3,2,1,5,1,2,4],cycle:0},timers:[{type:0,enabled:false,hour:18,minute:32,action_id:0,ss_action:1,period_min:1}],wifi:{sta_enabled:false,sta_ssid:\"\",sta_pass:\"\"},radio:{wifi_power_025dbm:40,ble_power_level:5},timing:{drag_repeat_min:1,drag_repeat_max:5,drag_distance_min:20,drag_distance_max:100,drag_step_min:10,drag_step_max:30,drag_interval_min:600,drag_interval_max:1500,drag_end_delay_min:500,drag_end_delay_max:5000,click_repeat_min:1,click_repeat_max:10,click_distance_min:10,click_distance_max:100,click_step_min:1,click_step_max:30,click_hold_min:20,click_hold_max:250,click_interval_min:100,click_interval_max:1000,click_end_delay_min:1000,click_end_delay_max:5000,wheel_repeat_min:1,wheel_repeat_max:5,wheel_distance_min:10,wheel_distance_max:100,wheel_step_min:1,wheel_step_max:30,wheel_tick_min:1,wheel_tick_max:8,wheel_interval_min:100,wheel_interval_max:500,wheel_end_delay_min:1000,wheel_end_delay_max:5000,arrow_repeat_min:1,arrow_repeat_max:20,arrow_interval_min:50,arrow_interval_max:800,arrow_end_delay_min:1000,arrow_end_delay_max:5000,rest_delay_min:1000,rest_delay_max:20000,move_repeat_min:1,move_repeat_max:20,move_distance_min:10,move_distance_max:30,move_step_min:1,move_step_max:15,move_interval_min:100,move_interval_max:500,move_end_delay_min:500,move_end_delay_max:1000,led_blink_on_ms:80,led_freq_per_1min_ms:50,led_freq_max_ms:2000,led_blink_once_ms:200,led_blink_once_gap_ms:200,word_repeat_min:1,word_repeat_max:5,word_char_delay_min:40,word_char_delay_max:700,word_space_delay_min:40,word_space_delay_max:1000,word_interval_min:500,word_interval_max:2000,word_end_delay_min:500,word_end_delay_max:1200,alt_tab_repeat_min:0,alt_tab_repeat_max:1,alt_tab_interval_min:500,alt_tab_interval_max:1000,alt_tab_end_delay_min:700,alt_tab_end_delay_max:1500},motion:defaultMotion(),word_list:\"struct enum uint8_t int32_t bool const static void nrf_gpio_pin_set nrf_drv_timer_trigger nrf_saadc_sample sd_ble_gap_connect sdk_config.h app_timer_start vector string mutex handle context buffer pointer sensor_value device_handle callback flag retry CONFIG_NRFX_TIMER_ENABLED NRFX_UARTE_ENABLED nrfx_uarte_tx nrf_ble_scan_start std_map std_shared_ptr spi_transfer i2c_read power_state ble_conn_handle tx_queue rx_buffer \"};\n"
"on('reset-default-btn','onclick',()=>{\n"
"  if(!confirm('确定将所有参数恢复为出厂默认值？\\n当前所有自定义配置（运行模式/权重/序列/定时/动作/WiFi/功率/单词）都会被覆盖。恢复后请点“保存配置”生效。'))return;\n"
"  cfg=defaultConfigFull();\n"
"  renderAll();scheduleSave();\n"
"  toast('已恢复默认参数，请点“保存配置”生效');\n"
"});\n"
""
"on('ble-reset-btn','onclick',async()=>{\n"
"  if(!confirm('确定复位蓝牙？\\n将断开当前连接并清除配对，重新开始广播，方便你连接其他设备。'))return;\n"
"  try{\n"
"    const j=await api('/api/ble_reset',{method:'POST'});\n"
"    toast(j.ok?'蓝牙已复位，正在广播等待新设备配对':'复位失败：'+(j.msg||''));\n"
"    if(j.ok)setTimeout(refreshStatus,1200);\n"
"  }catch(e){toast('复位请求失败');}\n"
"});\n"
"on('reboot-btn','onclick',async()=>{\n"
"  if(!confirm('确定重启设备（软复位）？\\n当前所有已保存的配置保持不变，重启后页面需重新连接。'))return;\n"
"  toast('设备正在重启…');\n"
"  try{\n"
"    await api('/api/restart',{method:'POST'});\n"
"  }catch(e){/* 重启会断开连接，忽略失败 */}\n"
"});\n"
"on('add-rule','onclick',()=>{\n"
"  if(!cfg.timers)cfg.timers=[];if(cfg.timers.length>=64){toast('规则已满(64)');return;}\n"
"  cfg.timers.push({type:0,enabled:true,hour:9,minute:0,action_id:0,ss_action:0,period_min:1});\n"
"  cfg.timer_count=cfg.timers.length;renderRules();scheduleSave();\n"
"});\n"
"on('toggle','onclick',async()=>{\n"
"  const s=await api('/api/status');const act=s.running?'stop':'start';\n"
"  const j=await api('/api/control',{method:'POST',headers:{'Content-Type':'application/json'},\n"
"    body:JSON.stringify({action:act})});toast(j.ok?(act==='start'?'已启动':'已停止'):'操作失败');\n"
"  refreshStatus();\n"
"});\n"
"on('sync-btn','onclick',async()=>{\n"
"  const j=await api('/api/sync_time',{method:'POST',headers:{'Content-Type':'application/json'},body:'{}'});\n"
"  toast(j.ok?'已触发网络校时':'校时失败：请先连接WiFi');\n"
"  setTimeout(refreshStatus,1500);\n"
"});\n"
"on('sync-browser-btn','onclick',async()=>{\n"
"  const epoch=Math.floor(Date.now()/1000);\n"
"  const j=await api('/api/sync_browser_time',{method:'POST',headers:{'Content-Type':'application/json'},\n"
"    body:JSON.stringify({epoch})});\n"
"  toast(j.ok?('已用本机时间校时：'+new Date(epoch*1000).toLocaleString()):('校时失败：'+(j.msg||'')));\n"
"  setTimeout(refreshStatus,800);\n"
"});\n"
"on('ota-btn','onclick',()=>document.getElementById('ota-file').click());\n"
"on('ota-file','onchange',async(e)=>{\n"
"  const f=e.target.files[0];if(!f)return;\n"
"  if(!f.name.endsWith('.bin')){toast('请选择 .bin 固件文件');e.target.value='';return;}\n"
"  const msg=document.getElementById('ota-msg');const btn=document.getElementById('ota-btn');\n"
"  btn.disabled=true;msg.textContent='上传中 0%';\n"
"  try{\n"
"    const data=await f.arrayBuffer();\n"
"    const xhr=new XMLHttpRequest();\n"
"    xhr.open('POST','/api/ota');\n"
"    const tk=getToken();if(tk)xhr.setRequestHeader('Authorization','Bearer '+tk);\n"
"    xhr.upload.onprogress=ev=>{if(ev.lengthComputable)msg.textContent='上传中 '+Math.round(ev.loaded/ev.total*100)+'%';};\n"
"    const done=new Promise((res,rej)=>{\n"
"      xhr.onload=()=>res(xhr.status);\n"
"      xhr.onerror=()=>rej(new Error('网络错误'));});\n"
"    xhr.send(data);\n"
"    const st=await done;\n"
"    if(st===200){msg.textContent='升级成功，设备重启中… 8s 后自动刷新页面';toast('升级完成，正在重启');setTimeout(()=>location.reload(),8000);}\n"
"    else{msg.textContent='升级失败（HTTP '+st+'）';toast('升级失败');btn.disabled=false;}\n"
"  }catch(err){msg.textContent='升级出错：'+err.message;toast('升级出错');btn.disabled=false;}\n"
"  e.target.value='';\n"
"});\n"
"/* 切换 DHCP / 静态 IP：静态时显示地址输入框 */\n"
"on('sta-addr-mode','onchange',()=>{\n"
"  const m=document.getElementById('sta-addr-mode').value;\n"
"  document.getElementById('sta-static-box').style.display=(m==='static')?'block':'none';\n"
"});\n"
"on('save-wifi','onclick',async()=>{\n"
"  const ssid=document.getElementById('sta-ssid').value;\n"
"  const pass=document.getElementById('sta-pass').value;\n"
"  if(!ssid){toast('请填写 SSID');return;}\n"
"  const mode=document.getElementById('sta-addr-mode').value;\n"
"  const dhcp=(mode!=='static');\n"
"  const ip=document.getElementById('sta-ip').value.trim();\n"
"  const netmask=document.getElementById('sta-netmask').value.trim();\n"
"  const gw=document.getElementById('sta-gw').value.trim();\n"
"  const dns=document.getElementById('sta-dns').value.trim();\n"
"  if(!dhcp && (!ip||!netmask)){console.warn('静态IP分支但读取为空 ip=[%s] mask=[%s]，可能页面被刷新导致输入丢失，降级为DHCP',ip,netmask);toast('检测到已选静态IP但输入框为空（可能页面刚被刷新，输入已清空），已降级为DHCP。请重选静态IP并重新填写后保存');dhcp=true;ip='';netmask='';gw='';dns='';}\n"
"  cfg.wifi={sta_enabled:true,sta_ssid:ssid,sta_pass:pass,sta_dhcp:dhcp,\n"
"    sta_ip:dhcp?'':ip,sta_netmask:dhcp?'':netmask,sta_gw:dhcp?'':gw,sta_dns:dhcp?'':dns};\n"
"  const j=await api('/api/wifi',{method:'POST',headers:{'Content-Type':'application/json'},\n"
"    body:JSON.stringify({ssid,pass,dhcp,ip,netmask,gw,dns})});toast(j.ok?'已保存并尝试连接':'失败');refreshStatus();\n"
"});\n"
"on('wifi-power','onchange',(e)=>{cfg.radio.wifi_power_025dbm=+e.target.value;scheduleSave();});\n"
"on('ble-power','onchange',(e)=>{cfg.radio.ble_power_level=+e.target.value;scheduleSave();});\n"
"var TIMING_GROUPS=[\n"
"  {title:'拖拽',unit:'ms',rows:[\n"
"    ['重复次数(次)','drag_repeat_min','drag_repeat_max',1,999],\n"
"    ['移动距离(像素)','drag_distance_min','drag_distance_max',10,2000],\n"
"    ['中间步进(像素/帧)','drag_step_min','drag_step_max',1,127],\n"
"    ['动作间隔','drag_interval_min','drag_interval_max',50,5000],\n"
"    ['结束延迟','drag_end_delay_min','drag_end_delay_max',50,60000]]},\n"
"  {title:'点击',unit:'ms',rows:[\n"
"    ['重复次数(次)','click_repeat_min','click_repeat_max',1,999],\n"
"    ['移动距离(像素)','click_distance_min','click_distance_max',10,2000],\n"
"    ['中间步进(像素/帧)','click_step_min','click_step_max',1,127],\n"
"    ['按住时长','click_hold_min','click_hold_max',5,1000],\n"
"    ['动作间隔','click_interval_min','click_interval_max',50,5000],\n"
"    ['结束延迟','click_end_delay_min','click_end_delay_max',50,60000]]},\n"
"  {title:'滚轮',unit:'ms',rows:[\n"
"    ['重复次数(次)','wheel_repeat_min','wheel_repeat_max',1,999],\n"
"    ['移动距离(像素)','wheel_distance_min','wheel_distance_max',10,2000],\n"
"    ['中间步进(像素/帧)','wheel_step_min','wheel_step_max',1,127],\n"
"    ['单次格数','wheel_tick_min','wheel_tick_max',1,30],\n"
"    ['动作间隔','wheel_interval_min','wheel_interval_max',50,5000],\n"
"    ['结束延迟','wheel_end_delay_min','wheel_end_delay_max',50,60000]]},\n"
"  {title:'方向键',unit:'ms',rows:[\n"
"    ['重复次数(次)','arrow_repeat_min','arrow_repeat_max',1,999],\n"
"    ['动作间隔','arrow_interval_min','arrow_interval_max',50,5000],\n"
"    ['结束延迟','arrow_end_delay_min','arrow_end_delay_max',50,60000]]},\n"
"  {title:'休息',unit:'ms',rows:[\n"
"    ['休息时长','rest_delay_min','rest_delay_max',500,60000]]},\n"
"  {title:'滑动',unit:'',rows:[\n"
"    ['重复次数(次)','move_repeat_min','move_repeat_max',1,999],\n"
"    ['移动距离(像素)','move_distance_min','move_distance_max',10,2000],\n"
"    ['中间步进(像素/帧)','move_step_min','move_step_max',1,127],\n"
"    ['动作间隔','move_interval_min','move_interval_max',50,5000],\n"
"    ['结束延迟','move_end_delay_min','move_end_delay_max',50,60000]]},\n"
"  {title:'打字(单词)',unit:'ms',rows:[\n"
"    ['每次打字词数(个)','word_repeat_min','word_repeat_max',1,999],\n"
"    ['字符间隔','word_char_delay_min','word_char_delay_max',5,1000],\n"
"    ['词尾空格间隔','word_space_delay_min','word_space_delay_max',5,2000],\n"
"    ['词间间隔','word_interval_min','word_interval_max',50,5000],\n"
"    ['结束延迟','word_end_delay_min','word_end_delay_max',50,60000]]},\n"
"  {title:'切换程序(Alt+Tab)',unit:'ms',rows:[\n"
"    ['切换次数(次)','alt_tab_repeat_min','alt_tab_repeat_max',0,999],\n"
"    ['按键间隔','alt_tab_interval_min','alt_tab_interval_max',50,5000],\n"
"    ['动作后休息','alt_tab_end_delay_min','alt_tab_end_delay_max',50,60000]]},\n"
"  {title:'状态灯(LED)',unit:'ms',rows:[\n"
"    ['点亮时长','led_blink_on_ms',null,10,2000],\n"
"    ['每分钟闪烁间隔基准','led_freq_per_1min_ms',null,5,1000],\n"
"    ['闪烁间隔上限','led_freq_max_ms',null,50,10000],\n"
"    ['确认闪烁时长','led_blink_once_ms',null,10,2000],\n"
"    ['确认闪烁间隔','led_blink_once_gap_ms',null,10,2000]]}\n"
"];\n"
"function renderTiming(){\n"
" try{\n"
"  const wrap=document.getElementById('timing');wrap.innerHTML='';\n"
"  TIMING_GROUPS.forEach(g=>{\n"
"    const card=document.createElement('div');card.className='tcard';\n"
"    card.innerHTML='<div class=tch>'+g.title+'</div>';\n"
"    g.rows.forEach(r=>{\n"
"      const [label,minK,maxK,lo,hi]=r;const unit=g.unit;\n"
"      const row=document.createElement('div');row.className='trow';\n"
"      let h='<label>'+label+'</label>';\n"
"      h+='<span class=min><input type=number min='+lo+' max='+hi+' value='+(cfg.timing[minK]||0)+' data-k='+minK+' data-lo='+lo+' data-hi='+hi+'></span>';\n"
"      if(maxK){h+='<span class=sep>~</span><span class=max><input type=number min='+lo+' max='+hi+' value='+(cfg.timing[maxK]||0)+' data-k='+maxK+' data-lo='+lo+' data-hi='+hi+'></span>';}\n"
"      else{h+='<span class=sep></span><span class=max></span>';}\n"
"      h+='<span class=u>'+unit+'</span>';\n"
"      row.innerHTML=h;\n"
"      row.querySelectorAll('input').forEach(inp=>{inp.addEventListener('input',()=>{cfg.timing[inp.dataset.k]=+inp.value;scheduleSave();markTimingInput(inp);});});\n"
"      card.appendChild(row);\n"
"    });\n"
"    wrap.appendChild(card);\n"
"  });\n"
" }catch(e){console.error('renderTiming',e);}\n"
"}\n"
"/* 单个动作参数输入框即时校验：合法则去红，非法则标红。返回该框是否合法。 */\n"
"function markTimingInput(inp){\n"
"  const lo=+inp.dataset.lo, hi=+inp.dataset.hi;\n"
"  const raw=inp.value.trim();\n"
"  let ok=true;\n"
"  if(raw===''||isNaN(+raw))ok=false;            // 空或非法数字\n"
"  else{const v=+raw; if(v<lo||v>hi)ok=false;}   // 超出范围\n"
"  inp.classList.toggle('bad',!ok);\n"
"  return ok;\n"
"}\n"
"/* 全量校验所有动作参数框：标红非法的，返回非法数量 */\n"
"function validateTiming(){\n"
"  let bad=0;\n"
"  document.querySelectorAll('#timing input[type=number]').forEach(inp=>{\n"
"    if(!markTimingInput(inp))bad++;\n"
"  });\n"
"  return bad;\n"
"}\n"
"/* 状态/实时时间轮询：1s 一次，页面切到后台时暂停，减少对 STA 连接的持续占用 */\n"
"/* 登录门控：无有效 token 时只显示登录框，不加载配置/状态（查看与配置均受登录保护） */\n"
"function afterLogin(){ loadAll().catch(e=>console.error('loadAll',e)); refreshStatus();\n"
"  setTimeout(()=>{ if(cfg===null){ const t=document.getElementById('toast'); if(t){t.textContent='页面加载不完整，正在重试…';t.classList.add('show');} location.reload(); } },800); }\n"
"if(!getToken()){ showLogin(); }\n"
"else { (async()=>{ const s=await api('/api/status');\n"
"  if(s && s.ok){ hideLogin(); afterLogin(); } else { clearToken(); showLogin('登录已失效，请重新登录'); } })(); }\n"
"document.getElementById('li-btn').onclick=doLogin;\n"
"document.getElementById('li-pass').addEventListener('keydown',e=>{ if(e.key==='Enter') doLogin(); });\n"
"document.getElementById('li-user').addEventListener('keydown',e=>{ if(e.key==='Enter') document.getElementById('li-pass').focus(); });\n"
"document.getElementById('logout-btn').onclick=async()=>{ await fetch('/api/logout',{method:'POST'}); clearToken(); showLogin('已退出登录'); };\n"
""
"function openAuth(){const m=document.getElementById('auth-mask');if(m)m.classList.remove('hidden');\n"
"  const u=document.getElementById('au-user'),p=document.getElementById('au-pass'),p2=document.getElementById('au-pass2'),msg=document.getElementById('au-msg');\n"
"  if(u)u.value='';if(p)p.value='';if(p2)p2.value='';if(msg){msg.textContent='';msg.className='login-msg';}}\n"
"function closeAuth(){const m=document.getElementById('auth-mask');if(m)m.classList.add('hidden');}\n"
"async function doSetAuth(){\n"
"  const u=document.getElementById('au-user').value.trim();\n"
"  const p=document.getElementById('au-pass').value;\n"
"  const p2=document.getElementById('au-pass2').value;\n"
"  const msg=document.getElementById('au-msg');\n"
"  if(!u||!p){msg.textContent='用户名和密码均不能为空';msg.className='login-msg err';return;}\n"
"  if(p!==p2){msg.textContent='两次输入的密码不一致';msg.className='login-msg err';return;}\n"
"  msg.textContent='保存中…';msg.className='login-msg';\n"
"  const r=await api('/api/setauth',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({user:u,pass:p})});\n"
"  if(r.ok){msg.textContent='已保存，下次登录请使用新凭据';msg.className='login-msg ok';setTimeout(closeAuth,1200);}\n"
"  else{msg.textContent=(r.msg||'保存失败');msg.className='login-msg err';}}\n"
"const auBtn=document.getElementById('auth-btn');if(auBtn)auBtn.onclick=openAuth;\n"
"const auSave=document.getElementById('au-btn');if(auSave)auSave.onclick=doSetAuth;\n"
"['au-user','au-pass','au-pass2'].forEach(id=>{const el=document.getElementById(id);if(el)el.addEventListener('keydown',e=>{if(e.key==='Enter')doSetAuth();});});\n"
"const auMask=document.getElementById('auth-mask');if(auMask)auMask.addEventListener('click',e=>{if(e.target.id==='auth-mask')closeAuth();});\n"
"window.addEventListener('load',()=>{\n"
"  setTimeout(()=>{ setInterval(()=>{ if(!document.hidden) refreshStatus(); },1000); },1500);\n"
"});\n"
"</script>\n"
"</body>\n"
"</html>\n";

/* ---------------- 工具 ---------------- */
static esp_err_t send_json(httpd_req_t *req, cJSON *root, int http_status)
{
    char *s = cJSON_PrintUnformatted(root);
    if (s == NULL) {
        /* heap 紧张导致序列化失败时，必须释放已构建的 cJSON 树，否则会泄漏整棵 JSON
         * （含 /api/config 的 200 词大对象），使 heap 进一步下降、后续请求更易失败，
         * 形成“挂机一段时间后 heap 跌破阈值 → 成片 wifi:m f null”的正反馈泄漏。 */
        cJSON_Delete(root);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");  /* 禁止浏览器缓存，避免旧 JS 字段缺失导致保存漏字段 */
    httpd_resp_set_status(req, http_status == 200 ? "200" : "400");
    esp_err_t r = httpd_resp_send(req, s, strlen(s));
    free(s);
    /* 必须释放已构建的 cJSON 树：此前遗漏 cJSON_Delete(root)，导致每个 /api/status 等
     * 请求都泄漏整棵 JSON 对象（节点 + 字符串拷贝）。多开页面长时间轮询后累积泄漏
     * 使 heap 跌破阈值，触发成片 wifi:m f null / BLE Malloc failed。 */
    cJSON_Delete(root);
    return r;
}

/* ---------------- 页面处理器 ---------------- */
/* 分块（chunked）发送页面：PAGE_HTML 约 65KB，在 SoftAP 弱网下若用单次
   httpd_resp_send 容易因非阻塞 socket 发送缓冲区满而触发 EAGAIN(send:11) 并断开连接。
   改用 Transfer-Encoding: chunked + httpd_resp_send_chunk 分块推送，httpd 会在
   每次 EAGAIN 时按 send_wait_timeout 等待 socket 可写后继续，确保整页可靠送达。
   这与升级前(参考 .org 版本)完全一致；客户端中途断开(ECONNRESET)属正常现象，静默返回。 */
static esp_err_t handler_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Transfer-Encoding", "chunked");

    const char *p = PAGE_HTML;
    size_t total = strlen(PAGE_HTML);
    const size_t CHUNK = 1024;
    size_t off = 0;
    while (off < total) {
        size_t len = total - off;
        if (len > CHUNK) len = CHUNK;
        esp_err_t r = httpd_resp_send_chunk(req, p + off, len);
        if (r != ESP_OK) {
            if (r != ESP_ERR_HTTPD_RESP_SEND) {  /* 客户端断开静默，不刷屏 */
                ESP_LOGW(TAG, "页面分块发送中断 @%u/%u: %s", off, total, esp_err_to_name(r));
            }
            return r;
        }
        off += len;
    }
    /* 发送空块结束 chunked 传输 */
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* ---------------- /api/login ----------------
 * 校验用户名/密码，成功生成 token 并返回。
 * 请求体：{"user":"cr","pass":"chenrui"}
 */
static esp_err_t handler_login(httpd_req_t *req)
{
    int len = req->content_len;
    if (len <= 0 || len > 256) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddBoolToObject(e, "ok", false);
        cJSON_AddStringToObject(e, "msg", "请求体为空或过大");
        return send_json(req, e, 400);
    }
    char *body = malloc(len + 1);
    if (!body) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddBoolToObject(e, "ok", false);
        cJSON_AddStringToObject(e, "msg", "内存不足");
        return send_json(req, e, 500);
    }
    int rcvd = 0;
    while (rcvd < len) {
        int r = httpd_req_recv(req, body + rcvd, len - rcvd);
        if (r <= 0) {
            free(body);
            cJSON *e = cJSON_CreateObject();
            cJSON_AddBoolToObject(e, "ok", false);
            cJSON_AddStringToObject(e, "msg", "读取请求体失败");
            return send_json(req, e, 400);
        }
        rcvd += r;
    }
    body[rcvd] = '\0';

    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddBoolToObject(e, "ok", false);
        cJSON_AddStringToObject(e, "msg", "JSON 解析失败");
        return send_json(req, e, 400);
    }
    cJSON *ju = cJSON_GetObjectItem(j, "user");
    cJSON *jp = cJSON_GetObjectItem(j, "pass");
    const char *user = (ju && cJSON_IsString(ju)) ? ju->valuestring : NULL;
    const char *pass = (jp && cJSON_IsString(jp)) ? jp->valuestring : NULL;
    bool ok = user && pass && strcmp(user, s_auth_user) == 0 && strcmp(pass, s_auth_pass) == 0;
    cJSON_Delete(j);

    cJSON *ret = cJSON_CreateObject();
    if (ok) {
        gen_auth_token();
        cJSON_AddBoolToObject(ret, "ok", true);
        cJSON_AddStringToObject(ret, "token", s_auth_token);
    } else {
        cJSON_AddBoolToObject(ret, "ok", false);
        cJSON_AddStringToObject(ret, "msg", "用户名或密码错误");
    }
    return send_json(req, ret, ok ? 200 : 401);
}

/* ---------------- /api/logout ---------------- */
static esp_err_t handler_logout(httpd_req_t *req)
{
    s_auth_token[0] = '\0';
    cJSON *ret = cJSON_CreateObject();
    cJSON_AddBoolToObject(ret, "ok", true);
    return send_json(req, ret, 200);
}

/* ---------------- /api/setauth ----------------
 * 登录后修改用户名/密码（需鉴权）。成功后写入 NVS 持久化，当前会话保持有效。
 * 请求体：{"user":"新用户名","pass":"新密码"}
 */
static esp_err_t handler_setauth(httpd_req_t *req)
{
    if (!require_auth(req)) {
        return ESP_OK;   /* require_auth 已回复 401 */
    }
    int len = req->content_len;
    if (len <= 0 || len > 256) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddBoolToObject(e, "ok", false);
        cJSON_AddStringToObject(e, "msg", "请求体为空或过大");
        return send_json(req, e, 400);
    }
    char *body = malloc(len + 1);
    if (!body) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddBoolToObject(e, "ok", false);
        cJSON_AddStringToObject(e, "msg", "内存不足");
        return send_json(req, e, 500);
    }
    int rcvd = 0;
    while (rcvd < len) {
        int r = httpd_req_recv(req, body + rcvd, len - rcvd);
        if (r <= 0) {
            free(body);
            cJSON *e = cJSON_CreateObject();
            cJSON_AddBoolToObject(e, "ok", false);
            cJSON_AddStringToObject(e, "msg", "读取请求体失败");
            return send_json(req, e, 400);
        }
        rcvd += r;
    }
    body[rcvd] = '\0';
    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddBoolToObject(e, "ok", false);
        cJSON_AddStringToObject(e, "msg", "JSON 解析失败");
        return send_json(req, e, 400);
    }
    cJSON *ju = cJSON_GetObjectItem(j, "user");
    cJSON *jp = cJSON_GetObjectItem(j, "pass");
    char nu_buf[AUTH_USER_MAX + 1];
    char np_buf[AUTH_PASS_MAX + 1];
    bool have = (ju && cJSON_IsString(ju)) && (jp && cJSON_IsString(jp));
    if (have) {
        strncpy(nu_buf, ju->valuestring, AUTH_USER_MAX);
        nu_buf[AUTH_USER_MAX] = '\0';
        strncpy(np_buf, jp->valuestring, AUTH_PASS_MAX);
        np_buf[AUTH_PASS_MAX] = '\0';
    }
    cJSON_Delete(j);   /* 先拷贝到本地缓冲再释放，避免 use-after-free 读到已释放内存 */
    if (!have || strlen(nu_buf) == 0 || strlen(np_buf) == 0) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddBoolToObject(e, "ok", false);
        cJSON_AddStringToObject(e, "msg", "用户名和密码均不能为空");
        return send_json(req, e, 400);
    }
    if (strlen(nu_buf) > AUTH_USER_MAX || strlen(np_buf) > AUTH_PASS_MAX) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddBoolToObject(e, "ok", false);
        cJSON_AddStringToObject(e, "msg", "用户名或密码过长");
        return send_json(req, e, 400);
    }
    strncpy(s_auth_user, nu_buf, AUTH_USER_MAX);
    s_auth_user[AUTH_USER_MAX] = '\0';
    strncpy(s_auth_pass, np_buf, AUTH_PASS_MAX);
    s_auth_pass[AUTH_PASS_MAX] = '\0';
    auth_store_save(s_auth_user, s_auth_pass);
    cJSON *ret = cJSON_CreateObject();
    cJSON_AddBoolToObject(ret, "ok", true);
    return send_json(req, ret, 200);
}

/* ---------------- /api/factory_reset ----------------
 * 恢复出厂设置（需登录）：将 Web 登录账号重置为 admin/admin，并将全部参数恢复为默认值，
 * 立即写入 NVS（账号 + 配置），无需再点“保存配置”。
 */
static esp_err_t handler_factory_reset(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;
    strncpy(s_auth_user, LOGIN_USER, AUTH_USER_MAX);
    s_auth_user[AUTH_USER_MAX] = '\0';
    strncpy(s_auth_pass, LOGIN_PASS, AUTH_PASS_MAX);
    s_auth_pass[AUTH_PASS_MAX] = '\0';
    auth_store_save(s_auth_user, s_auth_pass);
    config_store_reset_to_defaults();
    cJSON *ret = cJSON_CreateObject();
    cJSON_AddBoolToObject(ret, "ok", true);
    cJSON_AddStringToObject(ret, "msg", "已恢复出厂设置（账号 admin/admin，参数已重置）");
    return send_json(req, ret, 200);
}

/* 供硬件长按等物理触发调用：恢复出厂（重置 Web 账号 + 全部参数），无鉴权，由物理触发保证安全 */
void web_factory_reset(void)
{
    strncpy(s_auth_user, LOGIN_USER, AUTH_USER_MAX);
    s_auth_user[AUTH_USER_MAX] = '\0';
    strncpy(s_auth_pass, LOGIN_PASS, AUTH_PASS_MAX);
    s_auth_pass[AUTH_PASS_MAX] = '\0';
    auth_store_save(s_auth_user, s_auth_pass);
    config_store_reset_to_defaults();
    ESP_LOGW(TAG, "硬件长按触发出厂复位：账号已重置为 admin/admin，参数已恢复默认");
}

/* favicon.ico：避免浏览器请求产生 404 日志（返回 204 No Content） */
static esp_err_t handler_favicon(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204");
    httpd_resp_set_type(req, "image/x-icon");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ---------------- /api/status ---------------- */
static esp_err_t handler_status(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;
    char ap[16], sta[16], mode[8];
    wifi_manager_get_ap_ip(ap, sizeof(ap));
    wifi_manager_get_sta_ip(sta, sizeof(sta));
    wifi_manager_get_mode_str(mode, sizeof(mode));
    (void)mode;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "ap_ip", ap);
    cJSON_AddStringToObject(root, "sta_ip", sta);
    cJSON_AddBoolToObject(root, "bt", ble_hid_is_connected());
    cJSON_AddBoolToObject(root, "running", action_engine_is_running());
    cJSON_AddStringToObject(root, "version", __DATE__ " " __TIME__);
    cJSON_AddStringToObject(root, "action",
        action_engine_is_running() ? action_engine_current_action_name() : "空闲");
    cJSON_AddStringToObject(root, "progress",
        action_engine_is_running() ? action_engine_current_action_progress() : "");
    int64_t rest_us = action_engine_current_rest_remaining_us();
    cJSON_AddNumberToObject(root, "rest_remaining", rest_us > 0 ? (double)(rest_us / 1000000LL) : 0);

    /* 当前板子时间（北京时间 UTC+8）+ 是否已联网校时 */
    char tbuf[24];
    wifi_manager_get_time_str(tbuf, sizeof(tbuf));
    cJSON_AddStringToObject(root, "time", tbuf);
    cJSON_AddBoolToObject(root, "time_synced", wifi_manager_time_synced());
    cJSON_AddStringToObject(root, "timezone", "Asia/Shanghai (UTC+8)");
    return send_json(req, root, 200);
}

/* GET /api/time：返回当前板子时间（北京时间），供页面显示确认 */
static esp_err_t handler_time(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;
    char tbuf[24];
    wifi_manager_get_time_str(tbuf, sizeof(tbuf));
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "time", tbuf);
    cJSON_AddBoolToObject(root, "synced", wifi_manager_time_synced());
    cJSON_AddStringToObject(root, "zone", "Asia/Shanghai (UTC+8)");
    return send_json(req, root, 200);
}

/* POST /api/sync_time：手动触发一次 SNTP 校时 */
static esp_err_t handler_sync_time(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;
    esp_err_t e = wifi_manager_sync_time();
    cJSON *root = cJSON_CreateObject();
    if (e == ESP_OK) {
        cJSON_AddBoolToObject(root, "ok", true);
        cJSON_AddStringToObject(root, "msg", "已触发网络校时，稍候刷新查看时间");
    } else {
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "msg", "校时失败：请先连接 WiFi（STA）");
    }
    return send_json(req, root, e == ESP_OK ? 200 : 400);
}

/* ---------------- 单套/单项参数 JSON 解析辅助 ---------------- */
static void json_parse_weights(cJSON *w, action_weights_t *wt)
{
    cJSON *f;
    if (!w) return;
    if ((f = cJSON_GetObjectItem(w, "drag")))    wt->drag = f->valueint;
    if ((f = cJSON_GetObjectItem(w, "click")))   wt->click = f->valueint;
    if ((f = cJSON_GetObjectItem(w, "wheel")))   wt->wheel = f->valueint;
    if ((f = cJSON_GetObjectItem(w, "arrow")))   wt->arrow = f->valueint;
    if ((f = cJSON_GetObjectItem(w, "rest")))    wt->rest = f->valueint;
    if ((f = cJSON_GetObjectItem(w, "move")))    wt->move = f->valueint;
    if ((f = cJSON_GetObjectItem(w, "word")))    wt->word = f->valueint;
    if ((f = cJSON_GetObjectItem(w, "alt_tab"))) wt->alt_tab = f->valueint;
}

static void json_parse_motion(cJSON *mo, motion_cfg_t *m)
{
    cJSON *f;
    if (!mo) return;
    if ((f = cJSON_GetObjectItem(mo, "screen_scale_pct"))) m->screen_scale_pct = f->valueint;
    if ((f = cJSON_GetObjectItem(mo, "pos_limit_x")))      m->pos_limit_x = f->valueint;
    if ((f = cJSON_GetObjectItem(mo, "pos_limit_y")))      m->pos_limit_y = f->valueint;
    if ((f = cJSON_GetObjectItem(mo, "home_corner")))      m->home_corner = f->valueint;
    if ((f = cJSON_GetObjectItem(mo, "home_push_px")))     m->home_push_px = f->valueint;
    if ((f = cJSON_GetObjectItem(mo, "home_back_x")))      m->home_back_x = f->valueint;
    if ((f = cJSON_GetObjectItem(mo, "home_back_y")))      m->home_back_y = f->valueint;
}

static void json_parse_timing(cJSON *tm, action_timing_t *t)
{
    cJSON *f;
    if (!tm) return;
    #define TINT(field) do { if ((f = cJSON_GetObjectItem(tm, #field))) t->field = f->valueint; } while (0)
    TINT(drag_repeat_min);   TINT(drag_repeat_max);
    TINT(drag_distance_min); TINT(drag_distance_max);
    TINT(drag_step_min);     TINT(drag_step_max);
    TINT(drag_interval_min); TINT(drag_interval_max);
    TINT(drag_end_delay_min);TINT(drag_end_delay_max);
    TINT(click_repeat_min);  TINT(click_repeat_max);
    TINT(click_distance_min);TINT(click_distance_max);
    TINT(click_step_min);    TINT(click_step_max);
    TINT(click_hold_min);    TINT(click_hold_max);
    TINT(click_interval_min);TINT(click_interval_max);
    TINT(click_end_delay_min);TINT(click_end_delay_max);
    TINT(wheel_repeat_min);  TINT(wheel_repeat_max);
    TINT(wheel_distance_min);TINT(wheel_distance_max);
    TINT(wheel_step_min);    TINT(wheel_step_max);
    TINT(wheel_tick_min);    TINT(wheel_tick_max);
    TINT(wheel_interval_min);TINT(wheel_interval_max);
    TINT(wheel_end_delay_min);TINT(wheel_end_delay_max);
    TINT(arrow_repeat_min);  TINT(arrow_repeat_max);
    TINT(arrow_interval_min);TINT(arrow_interval_max);
    TINT(arrow_end_delay_min);TINT(arrow_end_delay_max);
    TINT(rest_delay_min);    TINT(rest_delay_max);
    TINT(move_repeat_min);    TINT(move_repeat_max);
    TINT(move_distance_min);  TINT(move_distance_max);
    TINT(move_step_min);      TINT(move_step_max);
    TINT(move_interval_min);  TINT(move_interval_max);
    TINT(move_end_delay_min); TINT(move_end_delay_max);
    TINT(led_blink_on_ms);
    TINT(led_freq_per_1min_ms);
    TINT(led_freq_max_ms);
    TINT(led_blink_once_ms);
    TINT(led_blink_once_gap_ms);
    TINT(word_repeat_min);    TINT(word_repeat_max);
    TINT(word_char_delay_min);TINT(word_char_delay_max);
    TINT(word_space_delay_min);TINT(word_space_delay_max);
    TINT(word_interval_min);  TINT(word_interval_max);
    TINT(word_end_delay_min); TINT(word_end_delay_max);
    TINT(alt_tab_repeat_min); TINT(alt_tab_repeat_max);
    TINT(alt_tab_interval_min);TINT(alt_tab_interval_max);
    TINT(alt_tab_end_delay_min); TINT(alt_tab_end_delay_max);
    #undef TINT
}

static void json_parse_profile(cJSON *p, profile_t *out)
{
    if (!p) return;
    json_parse_weights(cJSON_GetObjectItem(p, "weights"), &out->weights);
    json_parse_timing(cJSON_GetObjectItem(p, "timing"), &out->timing);
    json_parse_motion(cJSON_GetObjectItem(p, "motion"), &out->motion);
}

/* ---------------- 读取配置 JSON 到 config_t ---------------- */
static bool parse_config_json(const char *body, config_t *out)
{
    cJSON *j = cJSON_Parse(body);
    if (j == NULL) {
        return false;
    }
    /* 基于当前配置的副本修改：body 中未出现的字段保留现有值，
       避免“只改某一部分”时把权重/序列/时序等其余设置冲掉 */
    *out = *config_store_get();

    cJSON *m = cJSON_GetObjectItem(j, "run_mode");
    out->run_mode = (m && m->valueint == RUN_MODE_SEQUENCE) ? RUN_MODE_SEQUENCE : RUN_MODE_RANDOM;

    /* 当前生效套序号（选择即生效，其余套只保存不生效） */
    cJSON *ap = cJSON_GetObjectItem(j, "active_profile");
    if (ap) {
        int v = ap->valueint;
        if (v < 0 || v >= PROFILE_COUNT) v = 0;
        out->active_profile = (uint8_t)v;
    }

    /* 顶层 weights 为兼容旧版前端：映射到当前生效套 */
    cJSON *w = cJSON_GetObjectItem(j, "weights");
    if (w) {
        json_parse_weights(w, &out->profiles[out->active_profile].weights);
    }

    cJSON *seq = cJSON_GetObjectItem(j, "sequence");
    if (seq) {
        cJSON *acts = cJSON_GetObjectItem(seq, "actions");
        if (cJSON_IsArray(acts)) {
            int n = cJSON_GetArraySize(acts);
            if (n > ACT_SEQ_MAX) {
                n = ACT_SEQ_MAX;
            }
            for (int i = 0; i < n; i++) {
                out->sequence.actions[i] = (uint8_t)cJSON_GetArrayItem(acts, i)->valueint;
            }
            out->sequence.count = (uint8_t)n;
        }
        cJSON *cyc = cJSON_GetObjectItem(seq, "cycle");
        if (cyc) {
            int v = cyc->valueint;
            out->sequence.cycle = (v == CYCLE_SHUFFLE) ? CYCLE_SHUFFLE
                              : (v == CYCLE_ONCE) ? CYCLE_ONCE : CYCLE_ORDERED;
        } else {
            out->sequence.cycle = CYCLE_ORDERED;
        }
    }

    cJSON *timers = cJSON_GetObjectItem(j, "timers");
    if (cJSON_IsArray(timers)) {
        int n = cJSON_GetArraySize(timers);
        if (n > TIMER_RULE_MAX) {
            n = TIMER_RULE_MAX;
        }
        for (int i = 0; i < n; i++) {
            cJSON *t = cJSON_GetArrayItem(timers, i);
            timer_rule_t *r = &out->timers[i];
            cJSON *f;
            r->type = (cJSON_GetObjectItem(t, "type")) ? (timer_type_t)cJSON_GetObjectItem(t, "type")->valueint : TIMER_START_STOP;
            r->enabled = cJSON_GetObjectItem(t, "enabled") ? cJSON_IsTrue(cJSON_GetObjectItem(t, "enabled")) : false;
            if ((f = cJSON_GetObjectItem(t, "hour")))    r->hour = (uint8_t)f->valueint;
            if ((f = cJSON_GetObjectItem(t, "minute")))  r->minute = (uint8_t)f->valueint;
            if ((f = cJSON_GetObjectItem(t, "action_id"))) r->action_id = (uint8_t)f->valueint;
            if ((f = cJSON_GetObjectItem(t, "ss_action"))) r->ss_action = (uint8_t)f->valueint;
            if ((f = cJSON_GetObjectItem(t, "period_min"))) r->period_min = (uint16_t)f->valueint;
        }
        out->timer_count = (uint8_t)n;
    }

    cJSON *wifi = cJSON_GetObjectItem(j, "wifi");
    if (wifi) {
        cJSON *ssid = cJSON_GetObjectItem(wifi, "sta_ssid");
        cJSON *pass = cJSON_GetObjectItem(wifi, "sta_pass");
        cJSON *en   = cJSON_GetObjectItem(wifi, "sta_enabled");
        /* 仅当页面显式传布尔时才覆盖使能位；空串 SSID 不得清空已保存的 SSID，
         * 否则“保存配置”会把之前通过 /api/wifi 连上的真实 SSID 覆盖为空，
         * 导致复位后 sta_ssid[0]=='\0' 而静默不自动连接。 */
        if (cJSON_IsBool(en)) {
            out->wifi.sta_enabled = cJSON_IsTrue(en);
        }
        if (ssid && ssid->valuestring && ssid->valuestring[0] != '\0') {
            strncpy(out->wifi.sta_ssid, ssid->valuestring, sizeof(out->wifi.sta_ssid) - 1);
        }
        if (pass && pass->valuestring) {
            strncpy(out->wifi.sta_pass, pass->valuestring, sizeof(out->wifi.sta_pass) - 1);
        }
        cJSON *dhcp = cJSON_GetObjectItem(wifi, "sta_dhcp");
        if (cJSON_IsBool(dhcp)) {
            out->wifi.sta_dhcp = cJSON_IsTrue(dhcp);
        }
        cJSON *ip = cJSON_GetObjectItem(wifi, "sta_ip");
        if (ip && ip->valuestring) {
            strncpy(out->wifi.sta_ip, ip->valuestring, sizeof(out->wifi.sta_ip) - 1);
            out->wifi.sta_ip[sizeof(out->wifi.sta_ip) - 1] = '\0';
        }
        cJSON *nm = cJSON_GetObjectItem(wifi, "sta_netmask");
        if (nm && nm->valuestring) {
            strncpy(out->wifi.sta_netmask, nm->valuestring, sizeof(out->wifi.sta_netmask) - 1);
            out->wifi.sta_netmask[sizeof(out->wifi.sta_netmask) - 1] = '\0';
        }
        cJSON *gw = cJSON_GetObjectItem(wifi, "sta_gw");
        if (gw && gw->valuestring) {
            strncpy(out->wifi.sta_gw, gw->valuestring, sizeof(out->wifi.sta_gw) - 1);
            out->wifi.sta_gw[sizeof(out->wifi.sta_gw) - 1] = '\0';
        }
        cJSON *dns = cJSON_GetObjectItem(wifi, "sta_dns");
        if (dns && dns->valuestring) {
            strncpy(out->wifi.sta_dns, dns->valuestring, sizeof(out->wifi.sta_dns) - 1);
            out->wifi.sta_dns[sizeof(out->wifi.sta_dns) - 1] = '\0';
        }
    }

    /* 射频发射功率（Web 可配；保存后重启自动应用，缺省用 config_store 默认值） */
    cJSON *radio = cJSON_GetObjectItem(j, "radio");
    if (radio) {
        cJSON *f;
        if ((f = cJSON_GetObjectItem(radio, "wifi_power_025dbm"))) {
            int v = f->valueint;
            if (v < 0) v = 0;
            if (v > 84) v = 84;
            out->radio.wifi_power_025dbm = v;
        }
        if ((f = cJSON_GetObjectItem(radio, "ble_power_level"))) {
            int v = f->valueint;
            if (v < 0) v = 0;
            if (v > 7) v = 7;   /* ESP_PWR_LVL 枚举 0~7（v5.3.5：-12~+9dBm） */
            out->radio.ble_power_level = v;
        }
    }

    /* 动作时间参数（映射到当前生效套；profiles 存在时优先用 profiles） */
    cJSON *tm = cJSON_GetObjectItem(j, "timing");
    if (tm) {
        json_parse_timing(tm, &out->profiles[out->active_profile].timing);
    }
    /* 运动模式（映射到当前生效套） */
    cJSON *mo = cJSON_GetObjectItem(j, "motion");
    if (mo) {
        json_parse_motion(mo, &out->profiles[out->active_profile].motion);
    }

    /* 3 套参数：若存在则逐套解析（覆盖上面的顶层兼容映射） */
    cJSON *profiles = cJSON_GetObjectItem(j, "profiles");
    if (cJSON_IsArray(profiles)) {
        int n = cJSON_GetArraySize(profiles);
        if (n > PROFILE_COUNT) n = PROFILE_COUNT;
        for (int i = 0; i < n; i++) {
            json_parse_profile(cJSON_GetArrayItem(profiles, i), &out->profiles[i]);
        }
    }

    /* 打字单词表（空格分隔，限制长度，缺省保留 config_store 默认值） */
    cJSON *wl = cJSON_GetObjectItem(j, "word_list");
    if (wl && wl->valuestring) {
        size_t n = strlen(wl->valuestring);
        if (n >= sizeof(out->word.list)) {
            n = sizeof(out->word.list) - 1;
        }
        memcpy(out->word.list, wl->valuestring, n);
        out->word.list[n] = '\0';
    }

    cJSON_Delete(j);
    return true;
}

/* ---------------- 单套/单项参数 JSON 序列化辅助 ---------------- */
static cJSON *json_weights_obj(const action_weights_t *wt)
{
    cJSON *w = cJSON_CreateObject();
    cJSON_AddNumberToObject(w, "drag", wt->drag);
    cJSON_AddNumberToObject(w, "click", wt->click);
    cJSON_AddNumberToObject(w, "wheel", wt->wheel);
    cJSON_AddNumberToObject(w, "arrow", wt->arrow);
    cJSON_AddNumberToObject(w, "rest", wt->rest);
    cJSON_AddNumberToObject(w, "move", wt->move);
    cJSON_AddNumberToObject(w, "word", wt->word);
    cJSON_AddNumberToObject(w, "alt_tab", wt->alt_tab);
    return w;
}

static cJSON *json_motion_obj(const motion_cfg_t *m)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "screen_scale_pct", m->screen_scale_pct);
    cJSON_AddNumberToObject(o, "pos_limit_x", m->pos_limit_x);
    cJSON_AddNumberToObject(o, "pos_limit_y", m->pos_limit_y);
    cJSON_AddNumberToObject(o, "home_corner", m->home_corner);
    cJSON_AddNumberToObject(o, "home_push_px", m->home_push_px);
    cJSON_AddNumberToObject(o, "home_back_x", m->home_back_x);
    cJSON_AddNumberToObject(o, "home_back_y", m->home_back_y);
    return o;
}

static cJSON *json_timing_obj(const action_timing_t *t)
{
    cJSON *o = cJSON_CreateObject();
    #define TADD(field) cJSON_AddNumberToObject(o, #field, t->field)
    TADD(drag_repeat_min);   TADD(drag_repeat_max);
    TADD(drag_distance_min); TADD(drag_distance_max);
    TADD(drag_step_min);     TADD(drag_step_max);
    TADD(drag_interval_min); TADD(drag_interval_max);
    TADD(drag_end_delay_min);TADD(drag_end_delay_max);
    TADD(click_repeat_min);  TADD(click_repeat_max);
    TADD(click_distance_min);TADD(click_distance_max);
    TADD(click_step_min);    TADD(click_step_max);
    TADD(click_hold_min);    TADD(click_hold_max);
    TADD(click_interval_min);TADD(click_interval_max);
    TADD(click_end_delay_min);TADD(click_end_delay_max);
    TADD(wheel_repeat_min);  TADD(wheel_repeat_max);
    TADD(wheel_distance_min);TADD(wheel_distance_max);
    TADD(wheel_step_min);    TADD(wheel_step_max);
    TADD(wheel_tick_min);    TADD(wheel_tick_max);
    TADD(wheel_interval_min);TADD(wheel_interval_max);
    TADD(wheel_end_delay_min);TADD(wheel_end_delay_max);
    TADD(arrow_repeat_min);  TADD(arrow_repeat_max);
    TADD(arrow_interval_min);TADD(arrow_interval_max);
    TADD(arrow_end_delay_min);TADD(arrow_end_delay_max);
    TADD(rest_delay_min);    TADD(rest_delay_max);
    TADD(move_repeat_min);    TADD(move_repeat_max);
    TADD(move_distance_min);  TADD(move_distance_max);
    TADD(move_step_min);      TADD(move_step_max);
    TADD(move_interval_min);  TADD(move_interval_max);
    TADD(move_end_delay_min); TADD(move_end_delay_max);
    TADD(led_blink_on_ms);
    TADD(led_freq_per_1min_ms);
    TADD(led_freq_max_ms);
    TADD(led_blink_once_ms);
    TADD(led_blink_once_gap_ms);
    TADD(word_repeat_min);    TADD(word_repeat_max);
    TADD(word_char_delay_min);TADD(word_char_delay_max);
    TADD(word_space_delay_min);TADD(word_space_delay_max);
    TADD(word_interval_min);  TADD(word_interval_max);
    TADD(word_end_delay_min); TADD(word_end_delay_max);
    TADD(alt_tab_repeat_min); TADD(alt_tab_repeat_max);
    TADD(alt_tab_interval_min);TADD(alt_tab_interval_max);
    TADD(alt_tab_end_delay_min); TADD(alt_tab_end_delay_max);
    #undef TADD
    return o;
}

static cJSON *json_profile(const profile_t *p)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "weights", json_weights_obj(&p->weights));
    cJSON_AddItemToObject(o, "timing", json_timing_obj(&p->timing));
    cJSON_AddItemToObject(o, "motion", json_motion_obj(&p->motion));
    return o;
}

/* 把 config_t 序列化为 cJSON（供 GET /api/config） */
static cJSON *config_to_json(const config_t *c)
{
    cJSON *root = cJSON_CreateObject();
    int ap_idx = (c->active_profile < PROFILE_COUNT) ? (int)c->active_profile : 0;
    const profile_t *ap = &c->profiles[ap_idx];
    cJSON_AddNumberToObject(root, "active_profile", c->active_profile);
    cJSON_AddNumberToObject(root, "run_mode", c->run_mode);

    cJSON_AddItemToObject(root, "weights", json_weights_obj(&ap->weights));

    cJSON *seq = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < c->sequence.count && i < ACT_SEQ_MAX; i++) {
        cJSON_AddItemToArray(arr, cJSON_CreateNumber(c->sequence.actions[i]));
    }
    cJSON_AddItemToObject(seq, "actions", arr);
    cJSON_AddNumberToObject(seq, "cycle", c->sequence.cycle);
    cJSON_AddItemToObject(root, "sequence", seq);

    cJSON *tm = cJSON_CreateArray();
    for (int i = 0; i < c->timer_count && i < TIMER_RULE_MAX; i++) {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddNumberToObject(t, "type", c->timers[i].type);
        cJSON_AddBoolToObject(t, "enabled", c->timers[i].enabled);
        cJSON_AddNumberToObject(t, "hour", c->timers[i].hour);
        cJSON_AddNumberToObject(t, "minute", c->timers[i].minute);
        cJSON_AddNumberToObject(t, "action_id", c->timers[i].action_id);
        cJSON_AddNumberToObject(t, "ss_action", c->timers[i].ss_action);
        cJSON_AddNumberToObject(t, "period_min", c->timers[i].period_min);
        cJSON_AddItemToArray(tm, t);
    }
    cJSON_AddItemToObject(root, "timers", tm);

    cJSON *wf = cJSON_CreateObject();
    cJSON_AddBoolToObject(wf, "sta_enabled", c->wifi.sta_enabled);
    cJSON_AddStringToObject(wf, "sta_ssid", c->wifi.sta_ssid);
    cJSON_AddStringToObject(wf, "sta_pass", c->wifi.sta_pass);
    cJSON_AddBoolToObject(wf, "sta_dhcp", c->wifi.sta_dhcp);
    cJSON_AddStringToObject(wf, "sta_ip", c->wifi.sta_ip);
    cJSON_AddStringToObject(wf, "sta_netmask", c->wifi.sta_netmask);
    cJSON_AddStringToObject(wf, "sta_gw", c->wifi.sta_gw);
    cJSON_AddStringToObject(wf, "sta_dns", c->wifi.sta_dns);
    cJSON_AddItemToObject(root, "wifi", wf);

    cJSON *radio = cJSON_CreateObject();
    cJSON_AddNumberToObject(radio, "wifi_power_025dbm", c->radio.wifi_power_025dbm);
    cJSON_AddNumberToObject(radio, "ble_power_level", c->radio.ble_power_level);
    cJSON_AddItemToObject(root, "radio", radio);

    cJSON_AddItemToObject(root, "timing", json_timing_obj(&ap->timing));
    cJSON_AddItemToObject(root, "motion", json_motion_obj(&ap->motion));

    /* 3 套参数（每套含 权重/时间/运动），供前端多套切换与导入导出 */
    cJSON *profiles = cJSON_CreateArray();
    for (int i = 0; i < PROFILE_COUNT; i++) {
        cJSON_AddItemToArray(profiles, json_profile(&c->profiles[i]));
    }
    cJSON_AddItemToObject(root, "profiles", profiles);

    /* 复制到局部缓冲并确保 '\0' 结尾，避免旧 NVS 数据未终止时越界读取 */
    char wlb[WORD_LIST_MAX];
    memcpy(wlb, c->word.list, WORD_LIST_MAX);
    wlb[WORD_LIST_MAX - 1] = '\0';
    if (strnlen(wlb, WORD_LIST_MAX) == 0) {
        /* 单词表为空：回退为内置默认词表 */
        cJSON_AddStringToObject(root, "word_list", config_store_default_word_list());
    } else {
        cJSON_AddStringToObject(root, "word_list", wlb);
    }

    return root;
}

/* ---------------- /api/config ---------------- */
/* httpd_req_recv 单次可能只返回部分请求体（TCP 分包），必须循环读到收满 len */
static bool recv_full_body(httpd_req_t *req, char *buf, int len)
{
    int received = 0;
    while (received < len) {
        int ret = httpd_req_recv(req, buf + received, len - received);
        if (ret <= 0) {           /* 超时(HTTPD_SOCK_ERR_TIMEOUT)或连接错误 */
            return false;
        }
        received += ret;
    }
    buf[received] = '\0';
    return true;
}

static esp_err_t handler_config_get(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;
    config_t *c = config_store_get();
    cJSON *root = config_to_json(c);
    /* 仅记录字节数，不重复序列化整份 JSON（原实现会多分配一份 2.4KB 字符串仅用于日志，
     * 多开页面并发时加倍占用 heap，是“多开网页后 heap 击穿”的诱因之一） */
    return send_json(req, root, 200);
}

static esp_err_t handler_config_post(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;
    int len = req->content_len;
    /* 上限 16384：包含 3 套 profile(每套 权重/时间/运动) + word_list(最长 768 字符，
       含中文时 UTF-8 多字节) + 64 条定时规则 + 全部动作时间参数，紧凑 JSON 可超 8KB，
       故放宽到 16KB；解析缓冲据此 malloc，ESP32-S3 内存充足 */
    if (len <= 0 || len > 16384) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddBoolToObject(e, "ok", false);
        char m[48];
        snprintf(m, sizeof(m), "请求体过大(%d)，上限 16384 字节", len);
        cJSON_AddStringToObject(e, "msg", m);
        return send_json(req, e, 400);
    }
    char *buf = malloc(len + 1);
    if (buf == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    if (!recv_full_body(req, buf, len)) {
        free(buf);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    config_t newcfg;
    if (!parse_config_json(buf, &newcfg)) {
        free(buf);
        cJSON *e = cJSON_CreateObject();
        cJSON_AddBoolToObject(e, "ok", false);
        cJSON_AddStringToObject(e, "msg", "json parse error");
        return send_json(req, e, 400);
    }
    free(buf);

    config_store_save(&newcfg);
    config_t *cur = config_store_get();
    *cur = newcfg;

    /* 实时应用到引擎 */
    action_engine_apply_config(cur);

    cJSON *ok = cJSON_CreateObject();
    cJSON_AddBoolToObject(ok, "ok", true);
    return send_json(req, ok, 200);
}

/* ---------------- /api/control ---------------- */
static esp_err_t handler_control(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;
    int len = req->content_len;
    if (len <= 0 || len > 256) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    char *buf = malloc(len + 1);
    if (buf == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    int ret = httpd_req_recv(req, buf, len);
    if (ret <= 0) {
        free(buf);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[ret] = '\0';
    cJSON *j = cJSON_Parse(buf);
    cJSON *ok = cJSON_CreateObject();
    if (j) {
        cJSON *a = cJSON_GetObjectItem(j, "action");
        if (a && strcmp(a->valuestring, "start") == 0) {
            action_engine_start();
        } else {
            action_engine_stop();
        }
        cJSON_Delete(j);
    } else {
        action_engine_stop();
    }
    cJSON_AddBoolToObject(ok, "ok", true);
    return send_json(req, ok, 200);
}

/* ---------------- /api/ota：网页上传固件升级 ----------------
 * 浏览器以 POST 原始二进制固件（Content-Type 任意，body 即 .bin）上传。
 * 流程：停止动作引擎 → 写入空闲 ota 分区 → 校验 → 设为启动分区 → 重启。
 * 防砖：新固件首次启动需调用 esp_ota_mark_app_valid_cancel_rollback() 确认，
 *       否则 bootloader 会回滚到上一分区。
 */
static esp_err_t handler_ota(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;
    const int len = req->content_len;
    if (len <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    /* OTA 前先停止动作引擎，避免升级过程中写 flash 与 HID 动作冲突 */
    action_engine_stop();

    const esp_partition_t *update_part = esp_ota_get_next_update_partition(NULL);
    if (update_part == NULL) {
        ESP_LOGE(TAG, "OTA 失败：未找到可用更新分区（分区表非 OTA 双分区？）");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    if ((size_t)len > update_part->size) {
        ESP_LOGE(TAG, "OTA 失败：固件 %d 字节超过分区大小 %u", len, update_part->size);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA 开始：写入分区 %s，大小 %d 字节", update_part->label, len);
    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(update_part, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA esp_ota_begin 失败: %s", esp_err_to_name(err));
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    char *buf = malloc(1024);
    if (buf == NULL) {
        esp_ota_abort(ota_handle);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int total = 0;
    while (total < len) {
        int to_read = len - total;
        if (to_read > 1024) to_read = 1024;
        int r = httpd_req_recv(req, buf, to_read);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;  /* 超时重试，依赖 recv_wait_timeout */
            ESP_LOGE(TAG, "OTA 接收中断 @%d/%d: %d", total, len, r);
            free(buf);
            esp_ota_abort(ota_handle);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        err = esp_ota_write(ota_handle, buf, r);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA esp_ota_write 失败: %s", esp_err_to_name(err));
            free(buf);
            esp_ota_abort(ota_handle);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        total += r;
    }
    free(buf);

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA esp_ota_end 失败（固件校验不过？）: %s", esp_err_to_name(err));
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA 设置启动分区失败: %s", esp_err_to_name(err));
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA 完成，即将重启进入新固件");
    cJSON *ok = cJSON_CreateObject();
    cJSON_AddBoolToObject(ok, "ok", true);
    cJSON_AddStringToObject(ok, "msg", "升级完成，设备即将重启");
    send_json(req, ok, 200);

    /* 等响应发完后重启 */
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;  /* 不会执行到这里 */
}

/* ---------------- /api/sync_browser_time ----------------
 * 离线校时：前端把本机 Date.now()/1000（epoch 秒）POST 上来，
 * 直接写系统时钟，无需联网。成功后 time_synced() 即返回 true。 */
static esp_err_t handler_sync_browser_time(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;
    int len = req->content_len;
    if (len <= 0 || len > 256) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    char *buf = malloc(len + 1);
    if (buf == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    int ret = httpd_req_recv(req, buf, len);
    if (ret <= 0) {
        free(buf);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[ret] = '\0';
    cJSON *j = cJSON_Parse(buf);
    cJSON *ok = cJSON_CreateObject();
    if (j) {
        cJSON *t = cJSON_GetObjectItem(j, "epoch");
        if (cJSON_IsNumber(t)) {
            esp_err_t e = wifi_manager_set_time((time_t)t->valuedouble);
            cJSON_AddBoolToObject(ok, "ok", e == ESP_OK);
            if (e != ESP_OK) {
                cJSON_AddStringToObject(ok, "msg", "校时失败（时间无效或未生效）");
            }
        } else {
            cJSON_AddBoolToObject(ok, "ok", false);
            cJSON_AddStringToObject(ok, "msg", "缺少 epoch 字段");
        }
        cJSON_Delete(j);
    } else {
        cJSON_AddBoolToObject(ok, "ok", false);
        cJSON_AddStringToObject(ok, "msg", "JSON 解析失败");
    }
    free(buf);
    return send_json(req, ok, 200);
}

/* ---------------- /api/wifi ---------------- */
static esp_err_t handler_wifi(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;
    int len = req->content_len;
    if (len <= 0 || len > 256) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    char *buf = malloc(len + 1);
    if (buf == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    int ret = httpd_req_recv(req, buf, len);
    if (ret <= 0) {
        free(buf);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[ret] = '\0';
    cJSON *j = cJSON_Parse(buf);
    cJSON *ok = cJSON_CreateObject();
    if (j) {
        cJSON *ssid = cJSON_GetObjectItem(j, "ssid");
        cJSON *pass = cJSON_GetObjectItem(j, "pass");
        if (ssid && pass) {
            cJSON *dhcp = cJSON_GetObjectItem(j, "dhcp");
            cJSON *ip   = cJSON_GetObjectItem(j, "ip");
            cJSON *nm   = cJSON_GetObjectItem(j, "netmask");
            cJSON *gw   = cJSON_GetObjectItem(j, "gw");
            cJSON *dns  = cJSON_GetObjectItem(j, "dns");
            bool use_dhcp = cJSON_IsFalse(dhcp) ? false : true;  /* 默认 DHCP */
            const char *ip_s  = (ip  && ip->valuestring)  ? ip->valuestring  : "";
            const char *nm_s  = (nm  && nm->valuestring)  ? nm->valuestring  : "";
            const char *gw_s  = (gw  && gw->valuestring)  ? gw->valuestring  : "";
            const char *dns_s = (dns && dns->valuestring) ? dns->valuestring : "";
            esp_err_t e = wifi_manager_set_sta_ex(ssid->valuestring, pass->valuestring,
                                                  use_dhcp, ip_s, gw_s, nm_s, dns_s);
            /* set_sta_ex 内部已同步保存到 NVS，这里无需重复保存 */
            cJSON_AddBoolToObject(ok, "ok", e == ESP_OK);
        } else {
            cJSON_AddBoolToObject(ok, "ok", false);
        }
        cJSON_Delete(j);
    } else {
        cJSON_AddBoolToObject(ok, "ok", false);
    }
    free(buf);
    return send_json(req, ok, 200);
}

/* ---------------- /api/ble_reset：网页按钮复位蓝牙（与按键长按重置等效） ---------------- */
static esp_err_t handler_ble_reset(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;
    /* 与按键重置蓝牙一致：先停动作引擎并释放按键，再断开当前主机、清除配对、重新广播 */
    action_engine_stop_and_wait(2000);
    esp_err_t e = ble_hid_reset();
    cJSON *ok = cJSON_CreateObject();
    cJSON_AddBoolToObject(ok, "ok", e == ESP_OK);
    cJSON_AddStringToObject(ok, "msg", e == ESP_OK ? "蓝牙已复位，正在广播等待新设备配对" : esp_err_to_name(e));
    return send_json(req, ok, 200);
}

/* ---------------- /api/restart：网页按钮软复位设备 ---------------- */
static esp_err_t handler_restart(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;
    cJSON *ok = cJSON_CreateObject();
    cJSON_AddBoolToObject(ok, "ok", true);
    cJSON_AddStringToObject(ok, "msg", "设备即将重启");
    esp_err_t r = send_json(req, ok, 200);
    /* 等待响应发完再重启，避免连接被异常断开导致前端收不到回包 */
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return r;
}

/* ---------------- 启动 ---------------- */
esp_err_t web_server_start(void)
{
    auth_store_load();   /* 加载登录凭据（NVS 无则默认 admin/admin 并写回） */
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 24;
    /* 限制并发连接数：系统常态空闲 heap 仅约 22KB，每个 httpd 任务占一份栈 + JSON 缓冲，
     * 多开网页(多标签并发 GET)会让并发任务峰值击穿 heap → BLE_INIT Malloc failed / wifi:m f null。
     * 限制为 4（仍允许 1 个浏览器多标签 + 1 个手机同时访问），其余连接排队或拒绝，保护 heap。 */
    config.max_open_sockets = 4;    /* httpd 上限为 7（内部占 3 个，最多 1 个用户并发连接） */
    config.stack_size = 8192;       /* POST /api/config 需 cJSON_Parse 解析含 200 词 word_list 的大 JSON，
                                        峰值栈 >4KB，4KB 会栈溢出导致保存配置时 panic 重启；8KB 留足余量。
                                        原 12KB 过浪费（多开网页时每连接 12KB 栈是 heap 击穿主因之一），8KB 兼顾安全与 heap。 */
    config.task_priority = 5;       /* 与动作引擎同级，避免被 BLE(6) 长期抢占导致响应排队 */
    config.recv_wait_timeout = 60;  /* OTA 上传固件(约1.25MB)需较长时间，放宽到 60s */
    config.send_wait_timeout = 10;  /* 保守值：给足发送窗口，避免中途 EAGAIN 丢连接 */

    httpd_uri_t uris[] = {
        { .uri = "/",            .method = HTTP_GET,  .handler = handler_root,       .user_ctx = NULL },
        { .uri = "/api/login",   .method = HTTP_POST, .handler = handler_login,      .user_ctx = NULL },
        { .uri = "/api/logout",  .method = HTTP_POST, .handler = handler_logout,     .user_ctx = NULL },
        { .uri = "/api/setauth", .method = HTTP_POST, .handler = handler_setauth,    .user_ctx = NULL },
        { .uri = "/api/factory_reset", .method = HTTP_POST, .handler = handler_factory_reset, .user_ctx = NULL },
        { .uri = "/api/status",  .method = HTTP_GET,  .handler = handler_status,     .user_ctx = NULL },
        { .uri = "/api/config",  .method = HTTP_GET,  .handler = handler_config_get, .user_ctx = NULL },
        { .uri = "/api/config",  .method = HTTP_POST, .handler = handler_config_post,.user_ctx = NULL },
        { .uri = "/api/control", .method = HTTP_POST, .handler = handler_control,    .user_ctx = NULL },
        { .uri = "/api/wifi",    .method = HTTP_POST, .handler = handler_wifi,       .user_ctx = NULL },
        { .uri = "/api/time",    .method = HTTP_GET,  .handler = handler_time,       .user_ctx = NULL },
        { .uri = "/api/sync_time", .method = HTTP_POST, .handler = handler_sync_time, .user_ctx = NULL },
        { .uri = "/api/sync_browser_time", .method = HTTP_POST, .handler = handler_sync_browser_time, .user_ctx = NULL },
        { .uri = "/api/ota",     .method = HTTP_POST, .handler = handler_ota,         .user_ctx = NULL },
        { .uri = "/api/ble_reset", .method = HTTP_POST, .handler = handler_ble_reset,  .user_ctx = NULL },
        { .uri = "/api/restart",   .method = HTTP_POST, .handler = handler_restart,    .user_ctx = NULL },
        { .uri = "/favicon.ico", .method = HTTP_GET,  .handler = handler_favicon,    .user_ctx = NULL },
    };

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP 服务启动失败(%s)", esp_err_to_name(err));
        return err;
    }
    for (int i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_server, &uris[i]);
    }
    ESP_LOGI(TAG, "Web 服务已启动：http://192.168.4.1/ (AP/STA 双网段均可访问)");
    return ESP_OK;
}

esp_err_t web_server_stop(void)
{
    if (s_server) {
        esp_err_t e = httpd_stop(s_server);
        s_server = NULL;
        return e;
    }
    return ESP_OK;
}
