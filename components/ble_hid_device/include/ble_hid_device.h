#pragma once
#include "hid_defs.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

// Called when the BLE host (computer) connects or disconnects
typedef void (*ble_hid_device_conn_cb_t)(bool connected, void *ctx);

// Register GATT HID services — must be called before ble_hid_host_init().
// Internally calls ble_hid_host_add_sync_hook() to start advertising on sync.
esp_err_t ble_hid_device_init(void);

// Start BLE advertising (also called automatically on sync and disconnect)
esp_err_t ble_hid_device_start_advertising(void);

// Send a raw keyboard HID report to the connected BT host
esp_err_t ble_hid_device_send_report(const hid_keyboard_report_t *report);

// Send a Consumer Control usage ID (0 = release)
esp_err_t ble_hid_device_send_consumer(uint16_t usage_id);

// Release all keys
esp_err_t ble_hid_device_send_release(void);

// Type a UTF-8 string by synthesising key events (same semantics as usb_hid_device)
esp_err_t ble_hid_device_type_string(const char *str);

bool ble_hid_device_is_connected(void);
bool ble_hid_device_is_advertising(void);

void ble_hid_device_set_conn_cb(ble_hid_device_conn_cb_t cb, void *ctx);
void ble_hid_device_get_peer_addr(char *buf, size_t len);
