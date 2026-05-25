#include "storage.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_check.h"
#include <string.h>

#define TAG "storage"
#define NS_WIFI    "wifi"
#define NS_HOTKEYS "hotkeys"
#define NS_JIGGLER "jiggler"
#define NS_BLE     "ble"
#define NS_BOARD   "board"
#define KEY_WIFI_CREDS   "creds"
#define KEY_JIG_CFG      "cfg"
#define KEY_BLE_PEER     "peer"
#define KEY_BOARD_CFG    "cfg"

esp_err_t storage_init(void)
{
    // nvs_flash_init() must already be called from app_main before this
    return ESP_OK;
}

esp_err_t storage_get_wifi_creds(wifi_creds_t *out)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS_WIFI, NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    size_t len = sizeof(*out);
    err = nvs_get_blob(h, KEY_WIFI_CREDS, out, &len);
    nvs_close(h);
    return err;
}

esp_err_t storage_set_wifi_creds(const wifi_creds_t *creds)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NS_WIFI, NVS_READWRITE, &h), TAG, "open failed");
    esp_err_t err = nvs_set_blob(h, KEY_WIFI_CREDS, creds, sizeof(*creds));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t storage_clear_wifi_creds(void)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NS_WIFI, NVS_READWRITE, &h), TAG, "open failed");
    esp_err_t err = nvs_erase_key(h, KEY_WIFI_CREDS);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

bool storage_has_wifi_creds(void)
{
    wifi_creds_t tmp;
    return storage_get_wifi_creds(&tmp) == ESP_OK;
}

esp_err_t storage_get_hotkey_slot(uint8_t index, hotkey_slot_t *out)
{
    if (index >= HOTKEY_SLOTS_MAX) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));  // zero-init so new fields default correctly on old blobs
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NS_HOTKEYS, NVS_READONLY, &h), TAG, "open failed");
    char key[8];
    snprintf(key, sizeof(key), "s%u", index);
    size_t len = sizeof(*out);
    esp_err_t err = nvs_get_blob(h, key, out, &len);
    nvs_close(h);
    return err;
}

esp_err_t storage_set_hotkey_slot(uint8_t index, const hotkey_slot_t *slot)
{
    if (index >= HOTKEY_SLOTS_MAX) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NS_HOTKEYS, NVS_READWRITE, &h), TAG, "open failed");
    char key[8];
    snprintf(key, sizeof(key), "s%u", index);
    esp_err_t err = nvs_set_blob(h, key, slot, sizeof(*slot));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t storage_delete_hotkey_slot(uint8_t index)
{
    if (index >= HOTKEY_SLOTS_MAX) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NS_HOTKEYS, NVS_READWRITE, &h), TAG, "open failed");
    char key[8];
    snprintf(key, sizeof(key), "s%u", index);

    // Scrub payload before deletion so it does not linger in flash as a stale NVS page.
    // NVS is log-structured: each write appends a new page; old pages are marked stale and
    // reclaimed by GC. Writing zeros then 0xFF makes both passes unrecoverable even if GC
    // has not yet run when the dump is taken.
    hotkey_slot_t slot;
    size_t len = sizeof(slot);
    if (nvs_get_blob(h, key, &slot, &len) == ESP_OK) {
        memset(slot.payload, 0x00, sizeof(slot.payload));
        nvs_set_blob(h, key, &slot, sizeof(slot));
        nvs_commit(h);
        memset(slot.payload, 0xFF, sizeof(slot.payload));
        nvs_set_blob(h, key, &slot, sizeof(slot));
        nvs_commit(h);
    }

    esp_err_t err = nvs_erase_key(h, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t storage_get_jiggler_config(jiggler_config_t *out)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NS_JIGGLER, NVS_READONLY, &h), TAG, "open failed");
    size_t len = sizeof(*out);
    esp_err_t err = nvs_get_blob(h, KEY_JIG_CFG, out, &len);
    nvs_close(h);
    return err;
}

esp_err_t storage_set_jiggler_config(const jiggler_config_t *cfg)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NS_JIGGLER, NVS_READWRITE, &h), TAG, "open failed");
    esp_err_t err = nvs_set_blob(h, KEY_JIG_CFG, cfg, sizeof(*cfg));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t storage_get_ble_peer(ble_peer_t *out)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS_BLE, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    size_t len = sizeof(*out);
    err = nvs_get_blob(h, KEY_BLE_PEER, out, &len);
    nvs_close(h);
    return err;
}

esp_err_t storage_set_ble_peer(const ble_peer_t *peer)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NS_BLE, NVS_READWRITE, &h), TAG, "open failed");
    esp_err_t err = nvs_set_blob(h, KEY_BLE_PEER, peer, sizeof(*peer));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t storage_clear_ble_peer(void)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NS_BLE, NVS_READWRITE, &h), TAG, "open failed");
    esp_err_t err = nvs_erase_key(h, KEY_BLE_PEER);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

#define NS_WEBHOOK "webhook"
#define NS_MQTT    "mqtt"

static void board_config_defaults(board_config_t *cfg)
{
    cfg->btn_gpio          = 0;
    cfg->led_type          = BOARD_LED_TYPE_RGB;
    cfg->rgb_gpio          = 48;
    cfg->rgb_brightness    = 20;
    cfg->simple_gpio       = -1;
    cfg->simple_active_high = 1;
}

esp_err_t storage_get_board_config(board_config_t *out)
{
    board_config_defaults(out);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS_BOARD, NVS_READONLY, &h);
    if (err != ESP_OK) return ESP_OK;  // no saved config — return defaults
    size_t len = sizeof(*out);
    board_config_t tmp;
    if (nvs_get_blob(h, KEY_BOARD_CFG, &tmp, &len) == ESP_OK)
        *out = tmp;
    nvs_close(h);
    return ESP_OK;
}

esp_err_t storage_set_board_config(const board_config_t *cfg)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NS_BOARD, NVS_READWRITE, &h), TAG, "open failed");
    esp_err_t err = nvs_set_blob(h, KEY_BOARD_CFG, cfg, sizeof(*cfg));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// ── Webhook ───────────────────────────────────────────────────────────────────

esp_err_t storage_get_webhook_enabled(bool *out)
{
    *out = false;
    nvs_handle_t h;
    if (nvs_open(NS_WEBHOOK, NVS_READONLY, &h) != ESP_OK) return ESP_OK;
    uint8_t v = 0;
    nvs_get_u8(h, "en", &v);
    nvs_close(h);
    *out = (v != 0);
    return ESP_OK;
}

esp_err_t storage_set_webhook_enabled(bool enabled)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NS_WEBHOOK, NVS_READWRITE, &h), TAG, "open failed");
    esp_err_t err = nvs_set_u8(h, "en", enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t storage_get_webhook_slot(uint8_t idx, webhook_slot_t *out)
{
    if (idx >= WEBHOOK_SLOTS_MAX) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    nvs_handle_t h;
    if (nvs_open(NS_WEBHOOK, NVS_READONLY, &h) != ESP_OK) return ESP_ERR_NVS_NOT_FOUND;
    char key[6]; snprintf(key, sizeof(key), "w%u", idx);
    size_t len = sizeof(*out);
    esp_err_t err = nvs_get_blob(h, key, out, &len);
    nvs_close(h);
    return err;
}

esp_err_t storage_set_webhook_slot(uint8_t idx, const webhook_slot_t *slot)
{
    if (idx >= WEBHOOK_SLOTS_MAX) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NS_WEBHOOK, NVS_READWRITE, &h), TAG, "open failed");
    char key[6]; snprintf(key, sizeof(key), "w%u", idx);
    esp_err_t err = nvs_set_blob(h, key, slot, sizeof(*slot));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t storage_delete_webhook_slot(uint8_t idx)
{
    if (idx >= WEBHOOK_SLOTS_MAX) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NS_WEBHOOK, NVS_READWRITE, &h), TAG, "open failed");
    char key[6]; snprintf(key, sizeof(key), "w%u", idx);
    esp_err_t err = nvs_erase_key(h, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// ── MQTT ──────────────────────────────────────────────────────────────────────

esp_err_t storage_get_mqtt_broker(mqtt_broker_config_t *out)
{
    memset(out, 0, sizeof(*out));
    nvs_handle_t h;
    if (nvs_open(NS_MQTT, NVS_READONLY, &h) != ESP_OK) return ESP_OK;
    size_t len = sizeof(*out);
    nvs_get_blob(h, "broker", out, &len);
    nvs_close(h);
    return ESP_OK;
}

esp_err_t storage_set_mqtt_broker(const mqtt_broker_config_t *cfg)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NS_MQTT, NVS_READWRITE, &h), TAG, "open failed");
    esp_err_t err = nvs_set_blob(h, "broker", cfg, sizeof(*cfg));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t mqtt_slot_get(const char *ns_key_prefix, uint8_t idx,
                                void *out, size_t sz)
{
    if (idx >= MQTT_SLOTS_MAX) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sz);
    nvs_handle_t h;
    if (nvs_open(NS_MQTT, NVS_READONLY, &h) != ESP_OK) return ESP_ERR_NVS_NOT_FOUND;
    char key[8]; snprintf(key, sizeof(key), "%s%u", ns_key_prefix, idx);
    esp_err_t err = nvs_get_blob(h, key, out, &(size_t){sz});
    nvs_close(h);
    return err;
}

static esp_err_t mqtt_slot_set(const char *ns_key_prefix, uint8_t idx,
                                const void *data, size_t sz)
{
    if (idx >= MQTT_SLOTS_MAX) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NS_MQTT, NVS_READWRITE, &h), TAG, "open failed");
    char key[8]; snprintf(key, sizeof(key), "%s%u", ns_key_prefix, idx);
    esp_err_t err = nvs_set_blob(h, key, data, sz);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t mqtt_slot_del(const char *ns_key_prefix, uint8_t idx)
{
    if (idx >= MQTT_SLOTS_MAX) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NS_MQTT, NVS_READWRITE, &h), TAG, "open failed");
    char key[8]; snprintf(key, sizeof(key), "%s%u", ns_key_prefix, idx);
    esp_err_t err = nvs_erase_key(h, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t storage_get_mqtt_out_slot(uint8_t idx, mqtt_out_slot_t *out)
{ return mqtt_slot_get("o", idx, out, sizeof(*out)); }

esp_err_t storage_set_mqtt_out_slot(uint8_t idx, const mqtt_out_slot_t *slot)
{ return mqtt_slot_set("o", idx, slot, sizeof(*slot)); }

esp_err_t storage_delete_mqtt_out_slot(uint8_t idx)
{ return mqtt_slot_del("o", idx); }

esp_err_t storage_get_mqtt_in_slot(uint8_t idx, mqtt_in_slot_t *out)
{ return mqtt_slot_get("i", idx, out, sizeof(*out)); }

esp_err_t storage_set_mqtt_in_slot(uint8_t idx, const mqtt_in_slot_t *slot)
{ return mqtt_slot_set("i", idx, slot, sizeof(*slot)); }

esp_err_t storage_delete_mqtt_in_slot(uint8_t idx)
{ return mqtt_slot_del("i", idx); }
