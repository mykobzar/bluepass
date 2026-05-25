#pragma once
#include "hid_defs.h"
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

// Output abstraction — set before hotkey_engine_init() or at any time to switch modes.
// All function pointers are required and must not be NULL.
typedef struct {
    esp_err_t (*send_report)  (const hid_keyboard_report_t *report);
    esp_err_t (*send_consumer)(uint16_t usage_id);
    esp_err_t (*send_release) (void);
    esp_err_t (*type_string)  (const char *str);
    bool      (*is_ready)     (void);
} hid_output_ops_t;

void hotkey_engine_set_output(const hid_output_ops_t *ops);

typedef struct {
    int64_t timestamp_ms;
    bluepass_hid_report_t report;
    bool substituted;
    bool failed;      // substitution attempted but failed (e.g. TOTP clock not synced)
} key_event_t;

// Called from web_ui to stream the key log over WebSocket
typedef void (*key_event_cb_t)(const key_event_t *event, void *ctx);

esp_err_t hotkey_engine_init(void);

// Called from the BLE HID report callback — processes the incoming report,
// either passes it through to usb_hid_device or fires a substitution.
void hotkey_engine_process(const bluepass_hid_report_t *report);

// Reload hotkey table from NVS (call after web UI modifies slots)
void hotkey_engine_reload(void);

// Register a callback to receive all key events (for the key-log tab)
void hotkey_engine_set_event_cb(key_event_cb_t cb, void *ctx);
