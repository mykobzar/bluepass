#pragma once
#include "hid_defs.h"
#include "esp_err.h"
#include <stdbool.h>

// Called on every HID keyboard report received from a USB keyboard
typedef void (*usb_hid_report_cb_t)(const bluepass_hid_report_t *report, void *ctx);

// Initialize TinyUSB in host mode and register the report callback.
// Requires CONFIG_TINYUSB_HOST_HID_ENABLED=y in sdkconfig.
esp_err_t usb_hid_host_init(usb_hid_report_cb_t report_cb, void *ctx);

bool usb_hid_host_is_connected(void);
