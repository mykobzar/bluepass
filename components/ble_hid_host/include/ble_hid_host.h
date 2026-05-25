#pragma once
#include "hid_defs.h"
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Called on every HID report (keyboard or Consumer Control) from the BLE peripheral
typedef void (*ble_hid_report_cb_t)(const bluepass_hid_report_t *report, void *ctx);

// Called when BLE connection state changes
typedef void (*ble_connection_cb_t)(bool connected, void *ctx);

typedef struct {
    char name[32];
    uint8_t addr[6];
    uint8_t addr_type;
    int8_t rssi;
} ble_scan_result_t;

// Called for each BLE HID device found during scan
typedef void (*ble_scan_result_cb_t)(const ble_scan_result_t *result, void *ctx);

esp_err_t ble_hid_host_init(ble_hid_report_cb_t report_cb, void *report_ctx);

esp_err_t ble_hid_host_start_scan(uint32_t duration_ms,
                                   ble_scan_result_cb_t result_cb, void *ctx);
esp_err_t ble_hid_host_stop_scan(void);

esp_err_t ble_hid_host_connect(const uint8_t addr[6], uint8_t addr_type);
esp_err_t ble_hid_host_disconnect(void);

bool  ble_hid_host_is_connected(void);
int8_t ble_hid_host_get_rssi(void);
void  ble_hid_host_get_peer_name(char *buf, size_t len);
void  ble_hid_host_set_peer_name(const char *name);
void  ble_hid_host_get_peer_addr(char *buf, size_t len);

void ble_hid_host_set_connection_cb(ble_connection_cb_t cb, void *ctx);

// Register a callback invoked when the NimBLE host syncs (stack ready).
// Use this to start peripheral advertising. Must be called before ble_hid_host_init().
typedef void (*ble_sync_hook_t)(void);
void ble_hid_host_add_sync_hook(ble_sync_hook_t fn);

// Diagnostic log — returns recent BLE events as a newline-separated string
void ble_hid_host_get_log(char *buf, size_t len);
