#include "web_ui.h"
#include "storage.h"
#include "jiggler.h"
#include "hotkey.h"
#include "wifi_manager.h"
#include "ble_hid_host.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

#define TAG "web_ui"
#define WS_MAX_CLIENTS  5

// Embedded web assets — single-file SPA built into the firmware
extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[]   asm("_binary_index_html_end");

static httpd_handle_t s_server;
static int s_ws_clients[WS_MAX_CLIENTS];
static int s_ws_count;

// ── Helpers ──────────────────────────────────────────────────────────────────

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
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
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, index_html_start,
                    (ssize_t)(index_html_end - index_html_start));
    return ESP_OK;
}

// ── WebSocket key-log ─────────────────────────────────────────────────────────

static esp_err_t handler_ws(httpd_req_t *req)
{
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
    strncpy(slot.label,   cJSON_GetObjectItem(json, "label")->valuestring,   sizeof(slot.label) - 1);
    strncpy(slot.payload, cJSON_GetObjectItem(json, "payload")->valuestring, sizeof(slot.payload) - 1);
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
    cJSON_AddBoolToObject(obj,   "connected", ble_hid_host_is_connected());
    cJSON_AddStringToObject(obj, "name",      name);
    cJSON_AddStringToObject(obj, "addr",      peer_addr);
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
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no OTA partition");
        return ESP_FAIL;
    }
    esp_ota_handle_t hdl;
    if (esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &hdl) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota_begin failed");
        return ESP_FAIL;
    }

    char buf[1024];
    int total = req->content_len, done = 0, n;
    s_ota_state = OTA_STATE_RUNNING;
    s_ota_pct   = 0;
    snprintf(s_ota_msg, sizeof(s_ota_msg), "Uploading...");

    while ((n = httpd_req_recv(req, buf, sizeof(buf))) > 0) {
        if (esp_ota_write(hdl, buf, n) != ESP_OK) {
            esp_ota_abort(hdl);
            s_ota_state = OTA_STATE_ERROR;
            snprintf(s_ota_msg, sizeof(s_ota_msg), "Write error");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write error");
            return ESP_FAIL;
        }
        done += n;
        if (total > 0) s_ota_pct = done * 100 / total;
    }
    if (esp_ota_end(hdl) != ESP_OK || esp_ota_set_boot_partition(part) != ESP_OK) {
        s_ota_state = OTA_STATE_ERROR;
        snprintf(s_ota_msg, sizeof(s_ota_msg), "Verify failed");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota_end failed");
        return ESP_FAIL;
    }
    s_ota_pct   = 100;
    s_ota_state = OTA_STATE_DONE;
    snprintf(s_ota_msg, sizeof(s_ota_msg), "Done. Rebooting...");
    send_ok(req);
    vTaskDelay(pdMS_TO_TICKS(600));
    esp_restart();
    return ESP_OK;
}

// Async task: download firmware from URL and flash it
static void ota_fetch_task(void *arg)
{
    char *url = (char *)arg;
    s_ota_state = OTA_STATE_RUNNING;
    s_ota_pct   = 0;
    snprintf(s_ota_msg, sizeof(s_ota_msg), "Connecting...");

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        snprintf(s_ota_msg, sizeof(s_ota_msg), "No OTA partition");
        s_ota_state = OTA_STATE_ERROR;
        goto done;
    }

    esp_http_client_config_t cfg = {
        .url                  = url,
        .crt_bundle_attach    = esp_crt_bundle_attach,
        .timeout_ms           = 20000,
        .buffer_size          = 4096,
        .buffer_size_tx       = 1024,
        .follow_redirects     = true,
        .max_redirection_count = 5,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        snprintf(s_ota_msg, sizeof(s_ota_msg), "Connect failed: %s", esp_err_to_name(err));
        s_ota_state = OTA_STATE_ERROR;
        esp_http_client_cleanup(client);
        goto done;
    }

    int content_len = esp_http_client_fetch_headers(client);
    int status      = esp_http_client_get_status_code(client);
    if (status != 200) {
        snprintf(s_ota_msg, sizeof(s_ota_msg), "HTTP %d", status);
        s_ota_state = OTA_STATE_ERROR;
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        goto done;
    }

    esp_ota_handle_t hdl;
    err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &hdl);
    if (err != ESP_OK) {
        snprintf(s_ota_msg, sizeof(s_ota_msg), "OTA begin: %s", esp_err_to_name(err));
        s_ota_state = OTA_STATE_ERROR;
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        goto done;
    }

    snprintf(s_ota_msg, sizeof(s_ota_msg), "Downloading...");
    char *buf    = malloc(4096);
    int   total  = 0, n;
    while ((n = esp_http_client_read(client, buf, 4096)) > 0) {
        err = esp_ota_write(hdl, buf, n);
        if (err != ESP_OK) break;
        total += n;
        if (content_len > 0) s_ota_pct = total * 100 / content_len;
    }
    free(buf);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || n < 0) {
        snprintf(s_ota_msg, sizeof(s_ota_msg), "Download error: %s", esp_err_to_name(err));
        esp_ota_abort(hdl);
        s_ota_state = OTA_STATE_ERROR;
        goto done;
    }
    if (esp_ota_end(hdl) != ESP_OK) {
        snprintf(s_ota_msg, sizeof(s_ota_msg), "Verify failed — wrong binary?");
        s_ota_state = OTA_STATE_ERROR;
        goto done;
    }
    if (esp_ota_set_boot_partition(part) != ESP_OK) {
        snprintf(s_ota_msg, sizeof(s_ota_msg), "Set boot partition failed");
        s_ota_state = OTA_STATE_ERROR;
        goto done;
    }
    s_ota_pct   = 100;
    snprintf(s_ota_msg, sizeof(s_ota_msg), "Done. Rebooting...");
    s_ota_state = OTA_STATE_DONE;
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();

done:
    free(url);
    vTaskDelete(NULL);
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

    cJSON *json  = cJSON_Parse(body);
    cJSON *url_j = json ? cJSON_GetObjectItem(json, "url") : NULL;
    if (!url_j || !cJSON_IsString(url_j) || url_j->valuestring[0] == '\0') {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "url required");
        return ESP_FAIL;
    }
    char *url = strdup(url_j->valuestring);
    cJSON_Delete(json);
    if (!url) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem"); return ESP_FAIL; }

    if (xTaskCreate(ota_fetch_task, "ota_fetch", 8192, url, 5, NULL) != pdPASS) {
        free(url);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "task failed");
        return ESP_FAIL;
    }
    return send_ok(req);
}

// ── Server start/stop ─────────────────────────────────────────────────────────

esp_err_t web_ui_init(void)
{
    // Register as key event listener so we can push to WebSocket clients
    hotkey_engine_set_event_cb(web_ui_push_key_event, NULL);
    return ESP_OK;
}

esp_err_t web_ui_start(void)
{
    if (s_server) return ESP_OK;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn    = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 20;

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
        { .uri = "/api/ble/scan",       .method = HTTP_GET,  .handler = handler_ble_scan },
        { .uri = "/api/ble/connect",    .method = HTTP_POST, .handler = handler_ble_connect },
        { .uri = "/api/ble/status",     .method = HTTP_GET,  .handler = handler_ble_status },
        { .uri = "/api/ble/disconnect", .method = HTTP_POST, .handler = handler_ble_disconnect },
        { .uri = "/api/ble/log",        .method = HTTP_GET,  .handler = handler_ble_log },
        { .uri = "/api/ota",        .method = HTTP_POST,   .handler = handler_ota_upload },
        { .uri = "/api/ota/fetch",  .method = HTTP_POST,   .handler = handler_ota_fetch },
        { .uri = "/api/ota/status", .method = HTTP_GET,    .handler = handler_ota_status },
        { .uri = "/api/time",      .method = HTTP_GET,    .handler = handler_time_get },
        { .uri = "/api/version",   .method = HTTP_GET,    .handler = handler_version_get },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(s_server, &routes[i]);
    }
    ESP_LOGI(TAG, "web UI started");
    return ESP_OK;
}

esp_err_t web_ui_stop(void)
{
    if (!s_server) return ESP_OK;
    httpd_stop(s_server);
    s_server   = NULL;
    s_ws_count = 0;
    ESP_LOGI(TAG, "web UI stopped");
    return ESP_OK;
}

bool web_ui_is_running(void) { return s_server != NULL; }
