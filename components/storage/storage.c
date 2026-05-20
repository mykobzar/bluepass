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
#define KEY_WIFI_CREDS   "creds"
#define KEY_JIG_CFG      "cfg"
#define KEY_BLE_PEER     "peer"

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
