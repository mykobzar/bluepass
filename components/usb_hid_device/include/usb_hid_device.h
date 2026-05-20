#pragma once
#include "hid_defs.h"
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

esp_err_t usb_hid_device_init(void);

// Pass a raw HID keyboard report directly to the USB host (pass-through path)
esp_err_t usb_hid_device_send_report(const hid_keyboard_report_t *report);

// Send a HID Consumer Control usage ID (e.g. PlayPause = 0x00CD). Pass 0 to release.
esp_err_t usb_hid_device_send_consumer(uint16_t usage_id);

// Release all keys
esp_err_t usb_hid_device_send_release(void);

// Type an ASCII/UTF-8 string, handling shift modifier automatically.
// Blocks until all characters are sent (delays ~10 ms between key events).
esp_err_t usb_hid_device_type_string(const char *str);

// Type a single Unicode code point via OS-specific input method.
// Uses Ctrl+Shift+U sequence (X11/GTK) — adjust if targeting other OS.
esp_err_t usb_hid_device_type_unicode(uint32_t codepoint);

bool usb_hid_device_is_mounted(void);
