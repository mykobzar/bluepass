#include "web_ui.h"
#include "esp_flash_encrypt.h"
#include "storage.h"
#include "fido2.h"
#include "jiggler.h"
#include "hotkey.h"
#include "wifi_manager.h"
#include "ble_hid_host.h"
#include "ble_hid_device.h"
#include "usb_hid_host.h"
#include "webhook.h"
#include "mqtt_mgr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_wifi.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "cJSON.h"
#include "mbedtls/sha256.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

typedef struct { char *url; bool erase_nvs; } ota_fetch_arg_t;

#define TAG "web_ui"
#define WS_MAX_CLIENTS      5
#define WEB_IDLE_TIMEOUT_US (5LL * 60 * 1000 * 1000)  // 5 minutes

// Embedded web assets — single-file SPA built into the firmware
extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[]   asm("_binary_index_html_end");

static httpd_handle_t     s_server;
static int                s_ws_clients[WS_MAX_CLIENTS];
static int                s_ws_count;
static esp_timer_handle_t s_idle_timer;

static void idle_stop_task(void *arg)
{
    ESP_LOGI(TAG, "idle timeout — stopping web UI");
    web_ui_stop();
    vTaskDelete(NULL);
}

static void idle_timer_cb(void *arg)
{
    xTaskCreate(idle_stop_task, "web_idle", 2048, NULL, 4, NULL);
}

static void bump_idle_timer(void)
{
    if (!s_idle_timer || !s_server) return;
    esp_timer_stop(s_idle_timer);
    esp_timer_start_once(s_idle_timer, WEB_IDLE_TIMEOUT_US);
}

// ── Helpers ──────────────────────────────────────────────────────────────────

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    bump_idle_timer();
    char *body = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, body);
    free(body);
    return ESP_OK;
}

static esp_err_t send_ok(httpd_req_t *req)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "status", "ok");
    send_json(req, r);
    cJSON_Delete(r);
    return ESP_OK;
}

// ── Static SPA ───────────────────────────────────────────────────────────────

static esp_err_t handler_index(httpd_req_t *req)
{
    bump_idle_timer();
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, index_html_start,
                    (ssize_t)(index_html_end - index_html_start));
    return ESP_OK;
}

// ── WebSocket key-log ─────────────────────────────────────────────────────────

static esp_err_t handler_ws(httpd_req_t *req)
{
    bump_idle_timer();
    if (req->method == HTTP_GET) {
        // New WS handshake — record client fd
        int fd = httpd_req_to_sockfd(req);
        if (s_ws_count < WS_MAX_CLIENTS) {
            s_ws_clients[s_ws_count++] = fd;
        }
        return ESP_OK;
    }
    return ESP_OK;
}

void web_ui_push_key_event(const key_event_t *event, void *ctx)
{
    if (event->substituted && !event->failed)
        wifi_manager_led_blink_once();

    if (!s_server || s_ws_count == 0) return;
    const bluepass_hid_report_t *r = &event->report;
    if (r->keyboard.modifier == 0 && r->keyboard.keycode[0] == 0
        && r->consumer_code == 0) return;

    char buf[80];
    (void)ctx;
    snprintf(buf, sizeof(buf),
             "{\"ts\":%lld,\"mod\":%u,\"kc\":%u,\"cc\":%u,\"sub\":%d,\"err\":%d}",
             event->timestamp_ms,
             r->keyboard.modifier,
             r->keyboard.keycode[0],
             r->consumer_code,
             event->substituted ? 1 : 0,
             event->failed ? 1 : 0);

    httpd_ws_frame_t frame = {
        .type    = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)buf,
        .len     = strlen(buf),
    };
    for (int i = s_ws_count - 1; i >= 0; i--) {
        if (httpd_ws_send_frame_async(s_server, s_ws_clients[i], &frame) != ESP_OK) {
            // Client gone — compact list
            s_ws_clients[i] = s_ws_clients[--s_ws_count];
        }
    }
}

// ── Hotkey slots API ─────────────────────────────────────────────────────────
// GET  /api/slots        → JSON array of all slots (password payload redacted)
// PUT  /api/slots/{n}    → create/update slot n
// DELETE /api/slots/{n}  → delete slot n

static esp_err_t handler_slots_get(httpd_req_t *req)
{
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < HOTKEY_SLOTS_MAX; i++) {
        hotkey_slot_t slot;
        if (storage_get_hotkey_slot(i, &slot) != ESP_OK || !slot.active) continue;
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(obj, "index", i);
        cJSON_AddNumberToObject(obj, "type", slot.type);
        cJSON_AddStringToObject(obj, "label", slot.label);
        cJSON_AddNumberToObject(obj, "modifiers", slot.modifiers);
        cJSON_AddNumberToObject(obj, "keycode", slot.keycode);
        cJSON_AddNumberToObject(obj, "consumer_code", slot.consumer_code);
        cJSON_AddNumberToObject(obj, "match_mode",   slot.match_mode);
        cJSON_AddNumberToObject(obj, "replace_mode", slot.replace_mode);
        if (slot.type == SLOT_TYPE_TEXT) {
            cJSON_AddStringToObject(obj, "payload", slot.payload);
        } else {
            cJSON_AddStringToObject(obj, "payload", "");
        }
        cJSON_AddItemToArray(arr, obj);
    }
    send_json(req, arr);
    cJSON_Delete(arr);
    return ESP_OK;
}

static esp_err_t handler_slots_put(httpd_req_t *req)
{
    // Parse slot index from URI: /api/slots/{n}
    const char *uri = req->uri;
    int index = -1;
    sscanf(uri + strlen("/api/slots/"), "%d", &index);
    if (index < 0 || index >= HOTKEY_SLOTS_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid slot index");
        return ESP_FAIL;
    }

    char body[512];
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
        return ESP_FAIL;
    }
    body[received] = '\0';

    cJSON *json = cJSON_Parse(body);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }

    hotkey_slot_t slot = {0};
    slot.active    = true;
    slot.type      = (hotkey_slot_type_t)cJSON_GetObjectItem(json, "type")->valueint;
    slot.modifiers = (uint8_t)cJSON_GetObjectItem(json, "modifiers")->valueint;
    slot.keycode   = (uint8_t)cJSON_GetObjectItem(json, "keycode")->valueint;
    cJSON *cc_j = cJSON_GetObjectItem(json, "consumer_code");
    slot.consumer_code = cc_j ? (uint16_t)cc_j->valueint : 0;
    cJSON *mm_j = cJSON_GetObjectItem(json, "match_mode");
    slot.match_mode   = mm_j ? (uint8_t)mm_j->valueint : 0;
    cJSON *rm_j = cJSON_GetObjectItem(json, "replace_mode");
    slot.replace_mode = rm_j ? (uint8_t)rm_j->valueint : 0;
    strncpy(slot.label, cJSON_GetObjectItem(json, "label")->valuestring, sizeof(slot.label) - 1);
    cJSON *payload_j = cJSON_GetObjectItem(json, "payload");
    if (payload_j && payload_j->valuestring && payload_j->valuestring[0] != '\0') {
        strncpy(slot.payload, payload_j->valuestring, sizeof(slot.payload) - 1);
    } else {
        // Empty payload — preserve existing value if slot already stored
        hotkey_slot_t existing = {0};
        if (storage_get_hotkey_slot((uint8_t)index, &existing) == ESP_OK) {
            memcpy(slot.payload, existing.payload, sizeof(slot.payload));
        }
    }
    cJSON_Delete(json);

    esp_err_t err = storage_set_hotkey_slot(index, &slot);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "storage error");
        return err;
    }
    hotkey_engine_reload();
    return send_ok(req);
}

static esp_err_t handler_slots_delete(httpd_req_t *req)
{
    int index = -1;
    sscanf(req->uri + strlen("/api/slots/"), "%d", &index);
    if (index < 0 || index >= HOTKEY_SLOTS_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid slot index");
        return ESP_FAIL;
    }
    storage_delete_hotkey_slot(index);
    hotkey_engine_reload();
    return send_ok(req);
}

// ── Jiggler API ───────────────────────────────────────────────────────────────
// GET  /api/jiggler   → {enabled, keycode, modifiers, interval_ms}
// POST /api/jiggler   → update config + optionally toggle

static esp_err_t handler_jiggler_get(httpd_req_t *req)
{
    jiggler_config_t cfg = {0};
    storage_get_jiggler_config(&cfg);
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(obj,   "enabled",      jiggler_is_enabled());
    cJSON_AddNumberToObject(obj, "keycode",       cfg.keycode);
    cJSON_AddNumberToObject(obj, "modifiers",     cfg.modifiers);
    cJSON_AddNumberToObject(obj, "interval_ms",   cfg.interval_ms);
    cJSON_AddNumberToObject(obj, "on_modifiers",  cfg.on_modifiers);
    cJSON_AddNumberToObject(obj, "on_keycode",    cfg.on_keycode);
    cJSON_AddNumberToObject(obj, "off_modifiers", cfg.off_modifiers);
    cJSON_AddNumberToObject(obj, "off_keycode",   cfg.off_keycode);
    send_json(req, obj);
    cJSON_Delete(obj);
    return ESP_OK;
}

static esp_err_t handler_jiggler_post(httpd_req_t *req)
{
    char body[512];
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty"); return ESP_FAIL; }
    body[n] = '\0';
    cJSON *json = cJSON_Parse(body);
    if (!json) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON"); return ESP_FAIL; }

    jiggler_config_t cfg = {0};
    storage_get_jiggler_config(&cfg);  // load current as base

    cJSON *item;
    if ((item = cJSON_GetObjectItem(json, "keycode")))       cfg.keycode       = item->valueint;
    if ((item = cJSON_GetObjectItem(json, "modifiers")))     cfg.modifiers     = item->valueint;
    if ((item = cJSON_GetObjectItem(json, "interval_ms")))   cfg.interval_ms   = item->valueint;
    if ((item = cJSON_GetObjectItem(json, "on_modifiers")))  cfg.on_modifiers  = item->valueint;
    if ((item = cJSON_GetObjectItem(json, "on_keycode")))    cfg.on_keycode    = item->valueint;
    if ((item = cJSON_GetObjectItem(json, "off_modifiers"))) cfg.off_modifiers = item->valueint;
    if ((item = cJSON_GetObjectItem(json, "off_keycode")))   cfg.off_keycode   = item->valueint;
    if ((item = cJSON_GetObjectItem(json, "enabled")))       cfg.enabled       = item->valueint ? 1 : 0;

    storage_set_jiggler_config(&cfg);
    jiggler_configure(cfg.keycode, cfg.modifiers, cfg.interval_ms);
    cfg.enabled ? jiggler_enable() : jiggler_disable();
    hotkey_engine_reload();  // pick up new on/off hotkeys

    cJSON_Delete(json);
    return send_ok(req);
}

// ── WiFi API ──────────────────────────────────────────────────────────────────
// GET  /api/wifi  → {"configured":bool,"connected":bool,"ssid":"..."}
// POST /api/wifi  → {"ssid":"...","password":"..."} → save + reboot

static void do_restart(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(400));
    esp_restart();
    vTaskDelete(NULL);
}

static esp_err_t handler_wifi_get(httpd_req_t *req)
{
    bool configured = storage_has_wifi_creds();
    wifi_creds_t creds = {0};
    if (configured) storage_get_wifi_creds(&creds);

    cJSON *obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(obj,   "configured", configured);
    cJSON_AddBoolToObject(obj,   "connected",  wifi_manager_is_connected());
    cJSON_AddStringToObject(obj, "ssid",       creds.ssid);
    if (wifi_manager_is_connected()) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
            cJSON_AddNumberToObject(obj, "rssi", ap.rssi);
    }
    send_json(req, obj);
    cJSON_Delete(obj);
    return ESP_OK;
}

static esp_err_t handler_wifi_post(httpd_req_t *req)
{
    char body[256];
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty"); return ESP_FAIL; }
    body[n] = '\0';

    cJSON *json = cJSON_Parse(body);
    if (!json) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON"); return ESP_FAIL; }

    cJSON *ssid_j = cJSON_GetObjectItem(json, "ssid");
    cJSON *pass_j = cJSON_GetObjectItem(json, "password");
    if (!ssid_j || !ssid_j->valuestring || ssid_j->valuestring[0] == '\0') {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required");
        return ESP_FAIL;
    }

    wifi_creds_t creds = {0};
    strncpy(creds.ssid, ssid_j->valuestring, sizeof(creds.ssid) - 1);
    if (pass_j && pass_j->valuestring)
        strncpy(creds.password, pass_j->valuestring, sizeof(creds.password) - 1);
    cJSON_Delete(json);

    esp_err_t err = storage_set_wifi_creds(&creds);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "storage error");
        return err;
    }
    send_ok(req);
    xTaskCreate(do_restart, "restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

// ── Connection mode ───────────────────────────────────────────────────────────
// GET  /api/connection → {"mode":0,"keyboard":"bt","computer":"usb"}
// POST /api/connection → {"mode":1}  → saves to NVS, reboots

static const char *conn_keyboard_str(connection_mode_t m)
{ return (m == CONN_MODE_USB_BT) ? "usb" : "bt"; }

static const char *conn_computer_str(connection_mode_t m)
{ return (m == CONN_MODE_BT_USB) ? "usb" : "bt"; }

static esp_err_t handler_connection_get(httpd_req_t *req)
{
    connection_mode_t mode;
    storage_get_connection_mode(&mode);
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "mode",     (int)mode);
    cJSON_AddStringToObject(obj, "keyboard", conn_keyboard_str(mode));
    cJSON_AddStringToObject(obj, "computer", conn_computer_str(mode));
    send_json(req, obj);
    cJSON_Delete(obj);
    return ESP_OK;
}

static esp_err_t handler_connection_set(httpd_req_t *req)
{
    char buf[64];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body"); return ESP_FAIL; }
    buf[len] = '\0';
    cJSON *j = cJSON_Parse(buf);
    if (!j) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad JSON"); return ESP_FAIL; }

    cJSON *mode_j = cJSON_GetObjectItem(j, "mode");
    if (!mode_j) { cJSON_Delete(j); httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing mode"); return ESP_FAIL; }

    int mode_val = mode_j->valueint;
    cJSON_Delete(j);

    if (mode_val < 0 || mode_val > (int)CONN_MODE_USB_BT) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid mode");
        return ESP_FAIL;
    }

    storage_set_connection_mode((connection_mode_t)mode_val);
    send_ok(req);
    xTaskCreate(do_restart, "restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

// GET /api/ble/device/status → {"advertising":bool,"connected":bool,"peer_addr":"..."}
static esp_err_t handler_ble_device_status(httpd_req_t *req)
{
    char addr[20] = {0};
    ble_hid_device_get_peer_addr(addr, sizeof(addr));
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(obj, "advertising", ble_hid_device_is_advertising());
    cJSON_AddBoolToObject(obj, "connected",   ble_hid_device_is_connected());
    cJSON_AddStringToObject(obj, "peer_addr", addr);
    send_json(req, obj);
    cJSON_Delete(obj);
    return ESP_OK;
}

// POST /api/ble/device/advertise → restart advertising
static esp_err_t handler_ble_device_advertise(httpd_req_t *req)
{
    ble_hid_device_start_advertising();
    return send_ok(req);
}

// GET /api/usb/host/status → {"connected":bool}
static esp_err_t handler_usb_host_status(httpd_req_t *req)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(obj, "connected", usb_hid_host_is_connected());
    send_json(req, obj);
    cJSON_Delete(obj);
    return ESP_OK;
}

// ── BLE pairing ───────────────────────────────────────────────────────────────
// GET /api/ble/scan   — blocks ~4 s while scanning, returns JSON array of devices
// POST /api/ble/connect — {"addr":"aa:bb:cc:dd:ee:ff","addr_type":0}

#define BLE_SCAN_MS       4000
#define BLE_MAX_RESULTS   20

static ble_scan_result_t s_scan_buf[BLE_MAX_RESULTS];
static int               s_scan_count;

static void on_scan_result(const ble_scan_result_t *r, void *ctx)
{
    if (s_scan_count >= BLE_MAX_RESULTS) return;
    // Deduplicate by address
    for (int i = 0; i < s_scan_count; i++) {
        if (memcmp(s_scan_buf[i].addr, r->addr, 6) == 0) return;
    }
    s_scan_buf[s_scan_count++] = *r;
}

static esp_err_t handler_ble_scan(httpd_req_t *req)
{
    s_scan_count = 0;
    memset(s_scan_buf, 0, sizeof(s_scan_buf));

    esp_err_t err = ble_hid_host_start_scan(BLE_SCAN_MS, on_scan_result, NULL);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "scan failed");
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(BLE_SCAN_MS + 200));

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < s_scan_count; i++) {
        const ble_scan_result_t *r = &s_scan_buf[i];
        cJSON *obj = cJSON_CreateObject();
        char addr_s[18];
        snprintf(addr_s, sizeof(addr_s), "%02x:%02x:%02x:%02x:%02x:%02x",
                 r->addr[0], r->addr[1], r->addr[2],
                 r->addr[3], r->addr[4], r->addr[5]);
        cJSON_AddStringToObject(obj, "name",      r->name[0] ? r->name : "(unknown)");
        cJSON_AddStringToObject(obj, "addr",      addr_s);
        cJSON_AddNumberToObject(obj, "addr_type", r->addr_type);
        cJSON_AddNumberToObject(obj, "rssi",      r->rssi);
        cJSON_AddItemToArray(arr, obj);
    }
    send_json(req, arr);
    cJSON_Delete(arr);
    return ESP_OK;
}

static esp_err_t handler_ble_connect(httpd_req_t *req)
{
    char body[192];
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty"); return ESP_FAIL; }
    body[n] = '\0';

    cJSON *json = cJSON_Parse(body);
    if (!json) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON"); return ESP_FAIL; }

    cJSON *addr_j = cJSON_GetObjectItem(json, "addr");
    cJSON *type_j = cJSON_GetObjectItem(json, "addr_type");
    cJSON *name_j = cJSON_GetObjectItem(json, "name");
    if (!addr_j || !addr_j->valuestring || !type_j) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing addr/addr_type");
        return ESP_FAIL;
    }

    uint8_t addr[6];
    sscanf(addr_j->valuestring, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
           &addr[0], &addr[1], &addr[2], &addr[3], &addr[4], &addr[5]);
    uint8_t addr_type = (uint8_t)type_j->valueint;
    if (name_j && name_j->valuestring)
        ble_hid_host_set_peer_name(name_j->valuestring);
    cJSON_Delete(json);

    esp_err_t err = ble_hid_host_connect(addr, addr_type);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "connect failed");
        return err;
    }
    return send_ok(req);
}

static esp_err_t handler_ble_status(httpd_req_t *req)
{
    char name[32] = {0};
    char peer_addr[18] = {0};
    ble_hid_host_get_peer_name(name, sizeof(name));
    ble_hid_host_get_peer_addr(peer_addr, sizeof(peer_addr));

    cJSON *obj = cJSON_CreateObject();
    bool connected = ble_hid_host_is_connected();
    cJSON_AddBoolToObject(obj,   "connected", connected);
    cJSON_AddStringToObject(obj, "name",      name);
    cJSON_AddStringToObject(obj, "addr",      peer_addr);
    if (connected)
        cJSON_AddNumberToObject(obj, "rssi", ble_hid_host_get_rssi());
    send_json(req, obj);
    cJSON_Delete(obj);
    return ESP_OK;
}

static esp_err_t handler_ble_disconnect(httpd_req_t *req)
{
    ble_hid_host_disconnect();
    return send_ok(req);
}

static esp_err_t handler_ble_log(httpd_req_t *req)
{
    bump_idle_timer();
    static char log_buf[2048];
    ble_hid_host_get_log(log_buf, sizeof(log_buf));
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, log_buf);
    return ESP_OK;
}

// ── Version API ───────────────────────────────────────────────────────────────
// GET /api/version → {"version":"0.9.0","idf_ver":"v5.2.x","date":"May 20 2025","time":"12:00:00"}

static esp_err_t handler_version_get(httpd_req_t *req)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "version", desc->version);
    cJSON_AddStringToObject(obj, "idf_ver", desc->idf_ver);
    cJSON_AddStringToObject(obj, "date",    desc->date);
    cJSON_AddStringToObject(obj, "time",    desc->time);
    send_json(req, obj);
    cJSON_Delete(obj);
    return ESP_OK;
}

// ── Time API ─────────────────────────────────────────────────────────────────
// GET /api/time → {"epoch":1748000000,"synced":true}

static esp_err_t handler_time_get(httpd_req_t *req)
{
    time_t now = time(NULL);
    bool synced = wifi_manager_is_time_synced();
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "epoch", (double)now);
    cJSON_AddBoolToObject(obj,   "synced", synced);
    send_json(req, obj);
    cJSON_Delete(obj);
    return ESP_OK;
}

// ── Security API ─────────────────────────────────────────────────────────────
// GET  /api/security         → {"flash_enc":"disabled"|"development"|"release"}
// POST /api/security/release → transition Development → Release (burns eFuse, irreversible)

static esp_err_t handler_security_get(httpd_req_t *req)
{
    esp_flash_enc_mode_t mode = esp_get_flash_encryption_mode();
    const char *mode_str = (mode == ESP_FLASH_ENC_MODE_RELEASE)     ? "release"
                         : (mode == ESP_FLASH_ENC_MODE_DEVELOPMENT)  ? "development"
                                                                      : "disabled";
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "flash_enc", mode_str);
    send_json(req, obj);
    cJSON_Delete(obj);
    return ESP_OK;
}

static esp_err_t handler_security_release(httpd_req_t *req)
{
    esp_flash_enc_mode_t mode = esp_get_flash_encryption_mode();
    if (mode == ESP_FLASH_ENC_MODE_DISABLED) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "Flash encryption is not enabled in this firmware");
        return ESP_FAIL;
    }
    if (mode == ESP_FLASH_ENC_MODE_RELEASE) {
        return send_ok(req);  // already in release mode
    }
    // Development → Release: burn eFuse bits (irreversible)
    esp_flash_encryption_set_release_mode();
    return send_ok(req);
}

// ── Board config ──────────────────────────────────────────────────────────────

static esp_err_t handler_board_get(httpd_req_t *req)
{
    board_config_t cfg;
    storage_get_board_config(&cfg);
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "btn_gpio",           cfg.btn_gpio);
    cJSON_AddNumberToObject(obj, "led_type",           cfg.led_type);
    cJSON_AddNumberToObject(obj, "rgb_gpio",           cfg.rgb_gpio);
    cJSON_AddNumberToObject(obj, "rgb_brightness",     cfg.rgb_brightness);
    cJSON_AddNumberToObject(obj, "simple_gpio",        cfg.simple_gpio);
    cJSON_AddBoolToObject  (obj, "simple_active_high", cfg.simple_active_high);
    send_json(req, obj);
    cJSON_Delete(obj);
    return ESP_OK;
}

static esp_err_t handler_board_set(httpd_req_t *req)
{
    char buf[256];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body"); return ESP_FAIL; }
    buf[len] = '\0';
    cJSON *j = cJSON_Parse(buf);
    if (!j) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad JSON"); return ESP_FAIL; }

    board_config_t cfg;
    storage_get_board_config(&cfg);

    cJSON *v;
    if ((v = cJSON_GetObjectItem(j, "btn_gpio"))           && cJSON_IsNumber(v)) cfg.btn_gpio           = (int32_t)v->valuedouble;
    if ((v = cJSON_GetObjectItem(j, "led_type"))           && cJSON_IsNumber(v)) cfg.led_type           = (uint8_t)v->valuedouble;
    if ((v = cJSON_GetObjectItem(j, "rgb_gpio"))           && cJSON_IsNumber(v)) cfg.rgb_gpio           = (int32_t)v->valuedouble;
    if ((v = cJSON_GetObjectItem(j, "rgb_brightness"))     && cJSON_IsNumber(v)) cfg.rgb_brightness     = (uint8_t)v->valuedouble;
    if ((v = cJSON_GetObjectItem(j, "simple_gpio"))        && cJSON_IsNumber(v)) cfg.simple_gpio        = (int32_t)v->valuedouble;
    if ((v = cJSON_GetObjectItem(j, "simple_active_high")) && cJSON_IsBool(v))   cfg.simple_active_high = cJSON_IsTrue(v) ? 1 : 0;

    cJSON_Delete(j);
    storage_set_board_config(&cfg);
    return send_ok(req);
}

// ── Logout ────────────────────────────────────────────────────────────────────
// POST /api/logout — send response first, then stop the web server via a task

static esp_err_t handler_logout(httpd_req_t *req)
{
    send_ok(req);
    xTaskCreate(idle_stop_task, "logout", 2048, NULL, 4, NULL);
    return ESP_OK;
}

// ── OTA ───────────────────────────────────────────────────────────────────────
// POST /api/ota          — upload .bin directly from browser
// POST /api/ota/fetch    — {"url":"https://..."} device downloads from GitHub
// GET  /api/ota/status   — {"state":"idle|running|done|error","progress":0-100,"message":"..."}

typedef enum { OTA_STATE_IDLE, OTA_STATE_RUNNING, OTA_STATE_DONE, OTA_STATE_ERROR } ota_state_t;
static volatile ota_state_t s_ota_state  = OTA_STATE_IDLE;
static volatile int         s_ota_pct    = 0;
static char                 s_ota_msg[128];

static esp_err_t handler_ota_status(httpd_req_t *req)
{
    static const char *names[] = { "idle", "running", "done", "error" };
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "state",    names[s_ota_state]);
    cJSON_AddNumberToObject(obj, "progress", s_ota_pct);
    cJSON_AddStringToObject(obj, "message",  s_ota_msg);
    send_json(req, obj);
    cJSON_Delete(obj);
    return ESP_OK;
}

// Upload binary from browser — streams straight into OTA partition
static esp_err_t handler_ota_upload(httpd_req_t *req)
{
    if (s_ota_state == OTA_STATE_RUNNING) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "OTA already running");
        return ESP_FAIL;
    }
    char qs[32] = {0};
    char val[8] = {0};
    bool erase_nvs = false;
    if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) == ESP_OK)
        if (httpd_query_key_value(qs, "erase_nvs", val, sizeof(val)) == ESP_OK)
            erase_nvs = (val[0] == '1');
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no OTA partition");
        return ESP_FAIL;
    }
    esp_ota_mark_app_valid_cancel_rollback();
    esp_ota_handle_t hdl;
    if (esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &hdl) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota_begin failed");
        return ESP_FAIL;
    }

    // Heap buffer: one flash sector per recv → fewer erases, less stack pressure
    char *buf = malloc(4096);
    if (!buf) {
        esp_ota_abort(hdl);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }
    int total = req->content_len, done = 0, n;
    s_ota_state = OTA_STATE_RUNNING;
    s_ota_pct   = 0;
    snprintf(s_ota_msg, sizeof(s_ota_msg), "Uploading...");

    while ((n = httpd_req_recv(req, buf, 4096)) > 0) {
        if (esp_ota_write(hdl, buf, n) != ESP_OK) {
            free(buf);
            esp_ota_abort(hdl);
            s_ota_state = OTA_STATE_ERROR;
            snprintf(s_ota_msg, sizeof(s_ota_msg), "Write error");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write error");
            return ESP_FAIL;
        }
        done += n;
        if (total > 0) s_ota_pct = done * 100 / total;
    }
    free(buf);
    if (esp_ota_end(hdl) != ESP_OK || esp_ota_set_boot_partition(part) != ESP_OK) {
        s_ota_state = OTA_STATE_ERROR;
        snprintf(s_ota_msg, sizeof(s_ota_msg), "Verify failed");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota_end failed");
        return ESP_FAIL;
    }
    s_ota_pct   = 100;
    s_ota_state = OTA_STATE_DONE;
    if (erase_nvs) nvs_flash_erase();
    snprintf(s_ota_msg, sizeof(s_ota_msg), "Done. Rebooting...");
    send_ok(req);
    vTaskDelay(pdMS_TO_TICKS(600));
    esp_restart();
    return ESP_OK;
}

// Async task: download firmware from URL and flash it via esp_https_ota
static void ota_fetch_task(void *arg)
{
    ota_fetch_arg_t *a   = (ota_fetch_arg_t *)arg;
    char            *url = a->url;
    bool         erase   = a->erase_nvs;
    free(a);
    s_ota_state = OTA_STATE_RUNNING;
    s_ota_pct   = 0;
    snprintf(s_ota_msg, sizeof(s_ota_msg), "Connecting...");

    esp_ota_mark_app_valid_cancel_rollback();

    esp_http_client_config_t http_cfg = {
        .url                   = url,
        .crt_bundle_attach     = esp_crt_bundle_attach,
        .timeout_ms            = 60000,
        .keep_alive_enable     = true,
        .max_redirection_count = 5,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_https_ota_handle_t hdl;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &hdl);
    if (err != ESP_OK) {
        snprintf(s_ota_msg, sizeof(s_ota_msg), "OTA begin: %s", esp_err_to_name(err));
        s_ota_state = OTA_STATE_ERROR;
        goto done;
    }

    snprintf(s_ota_msg, sizeof(s_ota_msg), "Downloading...");
    while (1) {
        err = esp_https_ota_perform(hdl);
        if (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            int img_size = esp_https_ota_get_image_size(hdl);
            int img_read = esp_https_ota_get_image_len_read(hdl);
            if (img_size > 0) s_ota_pct = img_read * 100 / img_size;
            continue;
        }
        break;
    }

    if (!esp_https_ota_is_complete_data_received(hdl)) {
        snprintf(s_ota_msg, sizeof(s_ota_msg), "Download incomplete — connection lost?");
        esp_https_ota_abort(hdl);
        s_ota_state = OTA_STATE_ERROR;
        goto done;
    }

    err = esp_https_ota_finish(hdl);
    if (err != ESP_OK) {
        snprintf(s_ota_msg, sizeof(s_ota_msg), "OTA finish: %s", esp_err_to_name(err));
        s_ota_state = OTA_STATE_ERROR;
        goto done;
    }

    s_ota_pct = 100;
    if (erase) nvs_flash_erase();
    snprintf(s_ota_msg, sizeof(s_ota_msg), "Done. Rebooting...");
    s_ota_state = OTA_STATE_DONE;
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();

done:
    free(url);
    vTaskDelete(NULL);
}

// GET /api/ota/check — device queries GitHub tags and returns {latest, url} or {error}
static esp_err_t handler_ota_check(httpd_req_t *req)
{
    if (!wifi_manager_is_connected()) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"WiFi not connected\"}");
        return ESP_OK;
    }

    esp_http_client_config_t cfg = {
        .url                   = "https://api.github.com/repos/mykobzar/bluepass/tags?per_page=1",
        .crt_bundle_attach     = esp_crt_bundle_attach,
        .timeout_ms            = 15000,
        .buffer_size           = 2048,
        .buffer_size_tx        = 512,
        .max_redirection_count = 3,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "User-Agent", "bluepass-firmware");

    httpd_resp_set_type(req, "application/json");

    if (esp_http_client_open(client, 0) != ESP_OK) {
        esp_http_client_cleanup(client);
        httpd_resp_sendstr(req, "{\"error\":\"connect failed\"}");
        return ESP_OK;
    }

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    char *body  = malloc(2048);
    int   total = 0;
    if (status == 200 && body) {
        int n;
        while (total < 2047 &&
               (n = esp_http_client_read(client, body + total, 2047 - total)) > 0)
            total += n;
        if (total > 0) body[total] = '\0';
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (status != 200 || !body || total <= 0) {
        free(body);
        httpd_resp_sendstr(req, "{\"error\":\"GitHub API error\"}");
        return ESP_OK;
    }

    cJSON *arr   = cJSON_Parse(body);
    free(body);

    if (!arr || !cJSON_IsArray(arr) || cJSON_GetArraySize(arr) == 0) {
        cJSON_Delete(arr);
        httpd_resp_sendstr(req, "{\"error\":\"no tags found\"}");
        return ESP_OK;
    }

    cJSON *tag_obj = cJSON_GetArrayItem(arr, 0);
    cJSON *name    = cJSON_GetObjectItem(tag_obj, "name");

    if (!name || !cJSON_IsString(name)) {
        cJSON_Delete(arr);
        httpd_resp_sendstr(req, "{\"error\":\"tag parse failed\"}");
        return ESP_OK;
    }

    const char *tag = name->valuestring;
    const char *ver = (tag[0] == 'v') ? tag + 1 : tag;
    char url[192];
    snprintf(url, sizeof(url),
        "https://raw.githubusercontent.com/mykobzar/bluepass/%s/firmware/bluepass-%s.bin",
        tag, ver);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "latest", tag);
    cJSON_AddStringToObject(resp, "url",    url);
    cJSON_Delete(arr);

    char *js = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    httpd_resp_sendstr(req, js);
    free(js);
    return ESP_OK;
}

// Device fetches from URL in background
static esp_err_t handler_ota_fetch(httpd_req_t *req)
{
    if (s_ota_state == OTA_STATE_RUNNING) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "OTA already running");
        return ESP_FAIL;
    }
    if (!wifi_manager_is_connected()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "WiFi not connected");
        return ESP_FAIL;
    }
    char body[512];
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty"); return ESP_FAIL; }
    body[n] = '\0';

    cJSON *json     = cJSON_Parse(body);
    cJSON *url_j    = json ? cJSON_GetObjectItem(json, "url") : NULL;
    cJSON *erase_j  = json ? cJSON_GetObjectItem(json, "erase_nvs") : NULL;
    if (!url_j || !cJSON_IsString(url_j) || url_j->valuestring[0] == '\0') {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "url required");
        return ESP_FAIL;
    }
    ota_fetch_arg_t *a = malloc(sizeof(ota_fetch_arg_t));
    if (!a) { cJSON_Delete(json); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem"); return ESP_FAIL; }
    a->url       = strdup(url_j->valuestring);
    a->erase_nvs = erase_j && cJSON_IsTrue(erase_j);
    cJSON_Delete(json);
    if (!a->url) { free(a); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem"); return ESP_FAIL; }

    if (xTaskCreate(ota_fetch_task, "ota_fetch", 8192, a, 5, NULL) != pdPASS) {
        free(a->url); free(a);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "task failed");
        return ESP_FAIL;
    }
    return send_ok(req);
}

// ── Webhook API ───────────────────────────────────────────────────────────────
// GET  /api/webhook          → {enabled, slots:[...]}
// POST /api/webhook          → {enabled:bool}
// PUT  /api/webhook/slots/{n} → create/update slot n
// DELETE /api/webhook/slots/{n} → delete slot n

static esp_err_t handler_webhook_get(httpd_req_t *req)
{
    bool enabled = false;
    storage_get_webhook_enabled(&enabled);
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(obj, "enabled", enabled);
    cJSON *arr = cJSON_AddArrayToObject(obj, "slots");
    for (int i = 0; i < WEBHOOK_SLOTS_MAX; i++) {
        webhook_slot_t s = {0};
        if (storage_get_webhook_slot(i, &s) != ESP_OK || !s.active) continue;
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "index",     i);
        cJSON_AddStringToObject(item, "label",     s.label);
        cJSON_AddNumberToObject(item, "modifiers", s.modifiers);
        cJSON_AddNumberToObject(item, "keycode",   s.keycode);
        cJSON_AddStringToObject(item, "url",       s.url);
        cJSON_AddStringToObject(item, "value",     s.value);
        cJSON_AddItemToArray(arr, item);
    }
    send_json(req, obj);
    cJSON_Delete(obj);
    return ESP_OK;
}

static esp_err_t handler_webhook_post(httpd_req_t *req)
{
    char body[64];
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty"); return ESP_FAIL; }
    body[n] = '\0';
    cJSON *json = cJSON_Parse(body);
    if (!json) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON"); return ESP_FAIL; }
    cJSON *en = cJSON_GetObjectItem(json, "enabled");
    if (en) storage_set_webhook_enabled(cJSON_IsTrue(en) ? true : false);
    cJSON_Delete(json);
    webhook_reload();
    return send_ok(req);
}

static esp_err_t handler_webhook_slot_put(httpd_req_t *req)
{
    int index = -1;
    sscanf(req->uri + strlen("/api/webhook/slots/"), "%d", &index);
    if (index < 0 || index >= WEBHOOK_SLOTS_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid index");
        return ESP_FAIL;
    }
    char body[512];
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty"); return ESP_FAIL; }
    body[n] = '\0';
    cJSON *json = cJSON_Parse(body);
    if (!json) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON"); return ESP_FAIL; }

    webhook_slot_t s = {0};
    s.active = true;
    cJSON *v;
    if ((v = cJSON_GetObjectItem(json, "modifiers")) && cJSON_IsNumber(v)) s.modifiers = (uint8_t)v->valueint;
    if ((v = cJSON_GetObjectItem(json, "keycode"))   && cJSON_IsNumber(v)) s.keycode   = (uint8_t)v->valueint;
    if ((v = cJSON_GetObjectItem(json, "label"))     && cJSON_IsString(v)) strncpy(s.label, v->valuestring, sizeof(s.label) - 1);
    if ((v = cJSON_GetObjectItem(json, "url"))       && cJSON_IsString(v)) strncpy(s.url,   v->valuestring, sizeof(s.url)   - 1);
    if ((v = cJSON_GetObjectItem(json, "value"))     && cJSON_IsString(v)) strncpy(s.value, v->valuestring, sizeof(s.value) - 1);
    cJSON_Delete(json);

    esp_err_t err = storage_set_webhook_slot(index, &s);
    if (err != ESP_OK) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "storage"); return err; }
    webhook_reload();
    return send_ok(req);
}

static esp_err_t handler_webhook_slot_delete(httpd_req_t *req)
{
    int index = -1;
    sscanf(req->uri + strlen("/api/webhook/slots/"), "%d", &index);
    if (index < 0 || index >= WEBHOOK_SLOTS_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid index");
        return ESP_FAIL;
    }
    storage_delete_webhook_slot(index);
    webhook_reload();
    return send_ok(req);
}

// ── MQTT API ──────────────────────────────────────────────────────────────────
// GET  /api/mqtt              → {broker_url, username, out_enabled, in_enabled, connected, out_slots:[...], in_slots:[...]}
// POST /api/mqtt              → broker config + enable flags
// PUT  /api/mqtt/out/{n}      → create/update out slot n
// DELETE /api/mqtt/out/{n}    → delete out slot n
// PUT  /api/mqtt/in/{n}       → create/update in slot n
// DELETE /api/mqtt/in/{n}     → delete in slot n

static esp_err_t handler_mqtt_get(httpd_req_t *req)
{
    mqtt_broker_config_t broker = {0};
    storage_get_mqtt_broker(&broker);
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "broker_url",   broker.broker_url);
    cJSON_AddStringToObject(obj, "username",     broker.username);
    cJSON_AddBoolToObject  (obj, "out_enabled",  broker.out_enabled);
    cJSON_AddBoolToObject  (obj, "in_enabled",   broker.in_enabled);
    cJSON_AddBoolToObject  (obj, "connected",    mqtt_mgr_is_connected());

    cJSON *out_arr = cJSON_AddArrayToObject(obj, "out_slots");
    for (int i = 0; i < MQTT_SLOTS_MAX; i++) {
        mqtt_out_slot_t s = {0};
        if (storage_get_mqtt_out_slot(i, &s) != ESP_OK || !s.active) continue;
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "index",     i);
        cJSON_AddStringToObject(item, "label",     s.label);
        cJSON_AddNumberToObject(item, "modifiers", s.modifiers);
        cJSON_AddNumberToObject(item, "keycode",   s.keycode);
        cJSON_AddStringToObject(item, "topic",     s.topic);
        cJSON_AddStringToObject(item, "value",     s.value);
        cJSON_AddItemToArray(out_arr, item);
    }

    cJSON *in_arr = cJSON_AddArrayToObject(obj, "in_slots");
    for (int i = 0; i < MQTT_SLOTS_MAX; i++) {
        mqtt_in_slot_t s = {0};
        if (storage_get_mqtt_in_slot(i, &s) != ESP_OK || !s.active) continue;
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "index",       i);
        cJSON_AddStringToObject(item, "label",       s.label);
        cJSON_AddStringToObject(item, "topic",       s.topic);
        cJSON_AddStringToObject(item, "match_value", s.match_value);
        cJSON_AddNumberToObject(item, "modifiers",   s.modifiers);
        cJSON_AddNumberToObject(item, "keycode",     s.keycode);
        cJSON_AddItemToArray(in_arr, item);
    }

    send_json(req, obj);
    cJSON_Delete(obj);
    return ESP_OK;
}

static esp_err_t handler_mqtt_post(httpd_req_t *req)
{
    char body[512];
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty"); return ESP_FAIL; }
    body[n] = '\0';
    cJSON *json = cJSON_Parse(body);
    if (!json) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON"); return ESP_FAIL; }

    mqtt_broker_config_t broker = {0};
    storage_get_mqtt_broker(&broker);

    cJSON *v;
    if ((v = cJSON_GetObjectItem(json, "broker_url")) && cJSON_IsString(v))
        strncpy(broker.broker_url, v->valuestring, sizeof(broker.broker_url) - 1);
    if ((v = cJSON_GetObjectItem(json, "username"))   && cJSON_IsString(v))
        strncpy(broker.username,   v->valuestring, sizeof(broker.username) - 1);
    if ((v = cJSON_GetObjectItem(json, "password"))   && cJSON_IsString(v) && v->valuestring[0])
        strncpy(broker.password,   v->valuestring, sizeof(broker.password) - 1);
    if ((v = cJSON_GetObjectItem(json, "out_enabled")) && cJSON_IsBool(v))
        broker.out_enabled = cJSON_IsTrue(v) ? 1 : 0;
    if ((v = cJSON_GetObjectItem(json, "in_enabled"))  && cJSON_IsBool(v))
        broker.in_enabled  = cJSON_IsTrue(v) ? 1 : 0;
    cJSON_Delete(json);

    storage_set_mqtt_broker(&broker);
    mqtt_mgr_reload();
    return send_ok(req);
}

static esp_err_t handler_mqtt_out_put(httpd_req_t *req)
{
    int index = -1;
    sscanf(req->uri + strlen("/api/mqtt/out/"), "%d", &index);
    if (index < 0 || index >= MQTT_SLOTS_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid index");
        return ESP_FAIL;
    }
    char body[512];
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty"); return ESP_FAIL; }
    body[n] = '\0';
    cJSON *json = cJSON_Parse(body);
    if (!json) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON"); return ESP_FAIL; }

    mqtt_out_slot_t s = {0};
    s.active = true;
    cJSON *v;
    if ((v = cJSON_GetObjectItem(json, "modifiers")) && cJSON_IsNumber(v)) s.modifiers = (uint8_t)v->valueint;
    if ((v = cJSON_GetObjectItem(json, "keycode"))   && cJSON_IsNumber(v)) s.keycode   = (uint8_t)v->valueint;
    if ((v = cJSON_GetObjectItem(json, "label"))     && cJSON_IsString(v)) strncpy(s.label, v->valuestring, sizeof(s.label) - 1);
    if ((v = cJSON_GetObjectItem(json, "topic"))     && cJSON_IsString(v)) strncpy(s.topic, v->valuestring, sizeof(s.topic) - 1);
    if ((v = cJSON_GetObjectItem(json, "value"))     && cJSON_IsString(v)) strncpy(s.value, v->valuestring, sizeof(s.value) - 1);
    cJSON_Delete(json);

    esp_err_t err = storage_set_mqtt_out_slot(index, &s);
    if (err != ESP_OK) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "storage"); return err; }
    mqtt_mgr_reload();
    return send_ok(req);
}

static esp_err_t handler_mqtt_out_delete(httpd_req_t *req)
{
    int index = -1;
    sscanf(req->uri + strlen("/api/mqtt/out/"), "%d", &index);
    if (index < 0 || index >= MQTT_SLOTS_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid index");
        return ESP_FAIL;
    }
    storage_delete_mqtt_out_slot(index);
    mqtt_mgr_reload();
    return send_ok(req);
}

static esp_err_t handler_mqtt_in_put(httpd_req_t *req)
{
    int index = -1;
    sscanf(req->uri + strlen("/api/mqtt/in/"), "%d", &index);
    if (index < 0 || index >= MQTT_SLOTS_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid index");
        return ESP_FAIL;
    }
    char body[512];
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty"); return ESP_FAIL; }
    body[n] = '\0';
    cJSON *json = cJSON_Parse(body);
    if (!json) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON"); return ESP_FAIL; }

    mqtt_in_slot_t s = {0};
    s.active = true;
    cJSON *v;
    if ((v = cJSON_GetObjectItem(json, "modifiers"))   && cJSON_IsNumber(v)) s.modifiers = (uint8_t)v->valueint;
    if ((v = cJSON_GetObjectItem(json, "keycode"))     && cJSON_IsNumber(v)) s.keycode   = (uint8_t)v->valueint;
    if ((v = cJSON_GetObjectItem(json, "label"))       && cJSON_IsString(v)) strncpy(s.label,       v->valuestring, sizeof(s.label)       - 1);
    if ((v = cJSON_GetObjectItem(json, "topic"))       && cJSON_IsString(v)) strncpy(s.topic,       v->valuestring, sizeof(s.topic)       - 1);
    if ((v = cJSON_GetObjectItem(json, "match_value")) && cJSON_IsString(v)) strncpy(s.match_value, v->valuestring, sizeof(s.match_value) - 1);
    cJSON_Delete(json);

    esp_err_t err = storage_set_mqtt_in_slot(index, &s);
    if (err != ESP_OK) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "storage"); return err; }
    mqtt_mgr_reload();
    return send_ok(req);
}

static esp_err_t handler_mqtt_in_delete(httpd_req_t *req)
{
    int index = -1;
    sscanf(req->uri + strlen("/api/mqtt/in/"), "%d", &index);
    if (index < 0 || index >= MQTT_SLOTS_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid index");
        return ESP_FAIL;
    }
    storage_delete_mqtt_in_slot(index);
    mqtt_mgr_reload();
    return send_ok(req);
}

// ── Passkey (FIDO2/CTAP2) API ─────────────────────────────────────────────────
// GET    /api/passkey        → config + rk_count + pending_up
// POST   /api/passkey        → {enabled, confirm_modifiers, confirm_keycode}
// POST   /api/passkey/pin    → {pin:"..."} set/change PIN (plain text, local WiFi only)
// POST   /api/passkey/key    → regenerate master key
// DELETE /api/passkey        → factory reset
// GET    /api/passkey/rk     → list resident keys (meta only, no private keys)
// DELETE /api/passkey/rk/{n} → delete resident key slot n

static esp_err_t handler_passkey_get(httpd_req_t *req)
{
    fido2_config_t cfg = {0};
    storage_get_fido2_config(&cfg);
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddBoolToObject  (obj, "enabled",           cfg.enabled);
    cJSON_AddNumberToObject(obj, "confirm_modifiers", cfg.confirm_modifiers);
    cJSON_AddNumberToObject(obj, "confirm_keycode",   cfg.confirm_keycode);
    cJSON_AddNumberToObject(obj, "rk_count",          cfg.rk_count);
    cJSON_AddBoolToObject  (obj, "pin_set",           storage_fido2_has_pin());
    cJSON_AddBoolToObject  (obj, "pending_up",        fido2_pending_up());
    send_json(req, obj);
    cJSON_Delete(obj);
    return ESP_OK;
}

static esp_err_t handler_passkey_post(httpd_req_t *req)
{
    char body[256];
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty"); return ESP_FAIL; }
    body[n] = '\0';
    cJSON *json = cJSON_Parse(body);
    if (!json) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON"); return ESP_FAIL; }

    fido2_config_t cfg = {0};
    storage_get_fido2_config(&cfg);
    cJSON *v;
    if ((v = cJSON_GetObjectItem(json, "enabled"))           && cJSON_IsBool(v))   cfg.enabled           = cJSON_IsTrue(v) ? 1 : 0;
    if ((v = cJSON_GetObjectItem(json, "confirm_modifiers")) && cJSON_IsNumber(v)) cfg.confirm_modifiers = (uint8_t)v->valueint;
    if ((v = cJSON_GetObjectItem(json, "confirm_keycode"))   && cJSON_IsNumber(v)) cfg.confirm_keycode   = (uint8_t)v->valueint;
    cJSON_Delete(json);

    esp_err_t err = storage_set_fido2_config(&cfg);
    if (err != ESP_OK) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "storage"); return err; }
    return send_ok(req);
}

static esp_err_t handler_passkey_pin(httpd_req_t *req)
{
    char body[128];
    int n = httpd_req_recv(req, body, sizeof(body) - 1);
    if (n <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty"); return ESP_FAIL; }
    body[n] = '\0';
    cJSON *json = cJSON_Parse(body);
    if (!json) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON"); return ESP_FAIL; }

    cJSON *pin_j = cJSON_GetObjectItem(json, "pin");
    if (!pin_j || !cJSON_IsString(pin_j) || strlen(pin_j->valuestring) < 4) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "PIN must be at least 4 characters");
        return ESP_FAIL;
    }

    // Store SHA256(SHA256(pin))[0:16] — same derivation as CTAP2 clientPIN
    const char *pin = pin_j->valuestring;
    size_t pin_len = strlen(pin);
    cJSON_Delete(json);

    uint8_t h1[32], h2[32];
    mbedtls_sha256((const uint8_t *)pin, pin_len, h1, 0);
    mbedtls_sha256(h1, 32, h2, 0);
    memset(h1, 0, sizeof(h1));

    esp_err_t err = storage_set_fido2_pin_hash(h2);
    memset(h2, 0, sizeof(h2));
    if (err != ESP_OK) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "storage"); return err; }

    // Reset retries on PIN change
    fido2_config_t cfg = {0};
    storage_get_fido2_config(&cfg);
    cfg.pin_retries = 8;
    storage_set_fido2_config(&cfg);

    return send_ok(req);
}

static esp_err_t handler_passkey_regen_key(httpd_req_t *req)
{
    esp_err_t err = fido2_regen_master_key();
    if (err != ESP_OK) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "regen failed"); return err; }
    return send_ok(req);
}

static esp_err_t handler_passkey_delete(httpd_req_t *req)
{
    esp_err_t err = fido2_factory_reset();
    if (err != ESP_OK) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "reset failed"); return err; }
    return send_ok(req);
}

static esp_err_t handler_passkey_rk_get(httpd_req_t *req)
{
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < FIDO2_RK_MAX; i++) {
        fido2_rk_t rk = {0};
        if (storage_get_fido2_rk(i, &rk) != ESP_OK || !rk.active) continue;
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(obj, "index",        i);
        cJSON_AddNumberToObject(obj, "sign_count",   rk.sign_count);
        cJSON_AddStringToObject(obj, "user_name",    rk.user_name);
        cJSON_AddStringToObject(obj, "display_name", rk.display_name);
        // Encode rp_id_hash as hex
        char hex[65] = {0};
        for (int j = 0; j < 32; j++) snprintf(hex + j*2, 3, "%02x", rk.rp_id_hash[j]);
        cJSON_AddStringToObject(obj, "rp_id_hash", hex);
        cJSON_AddItemToArray(arr, obj);
    }
    send_json(req, arr);
    cJSON_Delete(arr);
    return ESP_OK;
}

static esp_err_t handler_passkey_rk_delete(httpd_req_t *req)
{
    int index = -1;
    sscanf(req->uri + strlen("/api/passkey/rk/"), "%d", &index);
    if (index < 0 || index >= FIDO2_RK_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid index");
        return ESP_FAIL;
    }
    storage_delete_fido2_rk(index);
    return send_ok(req);
}

static esp_err_t handler_passkey_diag_get(httpd_req_t *req)
{
    char *log = malloc(1056);
    if (!log) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }
    fido2_diag_get(log, 1056);
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "log", log);
    free(log);
    send_json(req, obj);
    cJSON_Delete(obj);
    return ESP_OK;
}

static esp_err_t handler_passkey_diag_clear(httpd_req_t *req)
{
    fido2_diag_clear();
    return send_ok(req);
}

// ── Server start/stop ─────────────────────────────────────────────────────────

esp_err_t web_ui_init(void)
{
    hotkey_engine_set_event_cb(web_ui_push_key_event, NULL);
    const esp_timer_create_args_t idle_args = {
        .callback = idle_timer_cb,
        .name     = "web_idle",
    };
    return esp_timer_create(&idle_args, &s_idle_timer);
}

esp_err_t web_ui_start(void)
{
    if (s_server) return ESP_OK;

    wifi_manager_led_on();

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn     = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 56;
    cfg.stack_size        = 8192;   // default 4096 overflows during OTA (buf[1024] + flash writes)
    cfg.recv_wait_timeout = 30;     // seconds; default 5 is too short for ~1.3 MB upload over WiFi
    cfg.send_wait_timeout = 30;

    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &cfg), TAG, "httpd start failed");

    static const httpd_uri_t routes[] = {
        { .uri = "/",              .method = HTTP_GET,    .handler = handler_index },
        { .uri = "/ws",            .method = HTTP_GET,    .handler = handler_ws,
          .is_websocket = true },
        { .uri = "/api/slots",     .method = HTTP_GET,    .handler = handler_slots_get },
        { .uri = "/api/slots/*",   .method = HTTP_PUT,    .handler = handler_slots_put },
        { .uri = "/api/slots/*",   .method = HTTP_DELETE, .handler = handler_slots_delete },
        { .uri = "/api/jiggler",   .method = HTTP_GET,    .handler = handler_jiggler_get },
        { .uri = "/api/jiggler",   .method = HTTP_POST,   .handler = handler_jiggler_post },
        { .uri = "/api/wifi",       .method = HTTP_GET,    .handler = handler_wifi_get },
        { .uri = "/api/wifi",       .method = HTTP_POST,   .handler = handler_wifi_post },
        { .uri = "/api/connection",              .method = HTTP_GET,  .handler = handler_connection_get },
        { .uri = "/api/connection",              .method = HTTP_POST, .handler = handler_connection_set },
        { .uri = "/api/ble/scan",                .method = HTTP_GET,  .handler = handler_ble_scan },
        { .uri = "/api/ble/connect",             .method = HTTP_POST, .handler = handler_ble_connect },
        { .uri = "/api/ble/status",              .method = HTTP_GET,  .handler = handler_ble_status },
        { .uri = "/api/ble/disconnect",          .method = HTTP_POST, .handler = handler_ble_disconnect },
        { .uri = "/api/ble/log",                 .method = HTTP_GET,  .handler = handler_ble_log },
        { .uri = "/api/ble/device/status",       .method = HTTP_GET,  .handler = handler_ble_device_status },
        { .uri = "/api/ble/device/advertise",    .method = HTTP_POST, .handler = handler_ble_device_advertise },
        { .uri = "/api/usb/host/status",         .method = HTTP_GET,  .handler = handler_usb_host_status },
        { .uri = "/api/ota",        .method = HTTP_POST,   .handler = handler_ota_upload },
        { .uri = "/api/ota/check",  .method = HTTP_GET,    .handler = handler_ota_check },
        { .uri = "/api/ota/fetch",  .method = HTTP_POST,   .handler = handler_ota_fetch },
        { .uri = "/api/ota/status", .method = HTTP_GET,    .handler = handler_ota_status },
        { .uri = "/api/time",              .method = HTTP_GET,  .handler = handler_time_get },
        { .uri = "/api/version",           .method = HTTP_GET,  .handler = handler_version_get },
        { .uri = "/api/security",          .method = HTTP_GET,  .handler = handler_security_get },
        { .uri = "/api/security/release",  .method = HTTP_POST, .handler = handler_security_release },
        { .uri = "/api/board",             .method = HTTP_GET,  .handler = handler_board_get },
        { .uri = "/api/board",             .method = HTTP_POST, .handler = handler_board_set },
        { .uri = "/api/logout",            .method = HTTP_POST, .handler = handler_logout },
        { .uri = "/api/webhook",           .method = HTTP_GET,    .handler = handler_webhook_get },
        { .uri = "/api/webhook",           .method = HTTP_POST,   .handler = handler_webhook_post },
        { .uri = "/api/webhook/slots/*",   .method = HTTP_PUT,    .handler = handler_webhook_slot_put },
        { .uri = "/api/webhook/slots/*",   .method = HTTP_DELETE, .handler = handler_webhook_slot_delete },
        { .uri = "/api/mqtt",              .method = HTTP_GET,    .handler = handler_mqtt_get },
        { .uri = "/api/mqtt",              .method = HTTP_POST,   .handler = handler_mqtt_post },
        { .uri = "/api/mqtt/out/*",        .method = HTTP_PUT,    .handler = handler_mqtt_out_put },
        { .uri = "/api/mqtt/out/*",        .method = HTTP_DELETE, .handler = handler_mqtt_out_delete },
        { .uri = "/api/mqtt/in/*",         .method = HTTP_PUT,    .handler = handler_mqtt_in_put },
        { .uri = "/api/mqtt/in/*",         .method = HTTP_DELETE, .handler = handler_mqtt_in_delete },
        { .uri = "/api/passkey",           .method = HTTP_GET,    .handler = handler_passkey_get },
        { .uri = "/api/passkey",           .method = HTTP_POST,   .handler = handler_passkey_post },
        { .uri = "/api/passkey",           .method = HTTP_DELETE, .handler = handler_passkey_delete },
        { .uri = "/api/passkey/pin",       .method = HTTP_POST,   .handler = handler_passkey_pin },
        { .uri = "/api/passkey/key",       .method = HTTP_POST,   .handler = handler_passkey_regen_key },
        { .uri = "/api/passkey/rk",        .method = HTTP_GET,    .handler = handler_passkey_rk_get },
        { .uri = "/api/passkey/rk/*",      .method = HTTP_DELETE, .handler = handler_passkey_rk_delete },
        { .uri = "/api/passkey/diag",      .method = HTTP_GET,    .handler = handler_passkey_diag_get },
        { .uri = "/api/passkey/diag",      .method = HTTP_DELETE, .handler = handler_passkey_diag_clear },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(s_server, &routes[i]);
    }
    bump_idle_timer();
    ESP_LOGI(TAG, "web UI started");
    return ESP_OK;
}

esp_err_t web_ui_stop(void)
{
    if (!s_server) return ESP_OK;
    esp_timer_stop(s_idle_timer);
    httpd_stop(s_server);
    s_server   = NULL;
    s_ws_count = 0;
    wifi_manager_led_off();
    ESP_LOGI(TAG, "web UI stopped");
    return ESP_OK;
}

bool web_ui_is_running(void) { return s_server != NULL; }
