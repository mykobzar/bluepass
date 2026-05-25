#include "usb_hid_host.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

// USB HID Host — requires CONFIG_TINYUSB_HOST_HID_ENABLED=y in sdkconfig.
// The ESP32-S3 USB OTG controller supports host mode; this is mutually exclusive
// with usb_hid_device (device mode) — only one can be initialized per boot.

#define TAG "usb_hid_host"

#ifdef CONFIG_TINYUSB_HOST_HID_ENABLED

#include "tinyusb.h"
#include "class/hid/hid_host.h"

static usb_hid_report_cb_t s_report_cb;
static void *s_report_ctx;
static bool s_connected;

// ── TinyUSB host HID callbacks ────────────────────────────────────────────────

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance,
                       uint8_t const *desc_report, uint16_t desc_len)
{
    ESP_LOGI(TAG, "HID device mounted dev=%u inst=%u", dev_addr, instance);
    s_connected = true;
    // Request boot protocol mode so we get standard 8-byte keyboard reports
    tuh_hid_set_protocol(dev_addr, instance, HID_PROTOCOL_BOOT);
    tuh_hid_receive_report(dev_addr, instance);
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance)
{
    ESP_LOGI(TAG, "HID device unmounted dev=%u inst=%u", dev_addr, instance);
    s_connected = false;
    if (s_report_cb) {
        // Send all-zero report to release any held keys
        bluepass_hid_report_t zero = {0};
        s_report_cb(&zero, s_report_ctx);
    }
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance,
                                  uint8_t const *report, uint16_t len)
{
    if (s_report_cb && len >= 8) {
        bluepass_hid_report_t r = {0};
        r.keyboard.modifier = report[0];
        // report[1] is reserved
        memcpy(r.keyboard.keycode, report + 2, 6);
        s_report_cb(&r, s_report_ctx);
    }
    tuh_hid_receive_report(dev_addr, instance);
}

// ── Host task ─────────────────────────────────────────────────────────────────

static void usb_host_task(void *arg)
{
    while (true) {
        tuh_task();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

esp_err_t usb_hid_host_init(usb_hid_report_cb_t report_cb, void *ctx)
{
    s_report_cb  = report_cb;
    s_report_ctx = ctx;

    const tinyusb_config_t tusb_cfg = {
        .port = TINYUSB_PORT_FULL_SPEED_0,
    };
    esp_err_t err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }

    xTaskCreate(usb_host_task, "usb_host", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "USB HID host initialized");
    return ESP_OK;
}

bool usb_hid_host_is_connected(void) { return s_connected; }

#else  // CONFIG_TINYUSB_HOST_HID_ENABLED not set

esp_err_t usb_hid_host_init(usb_hid_report_cb_t report_cb, void *ctx)
{
    ESP_LOGE(TAG, "USB host mode requires CONFIG_TINYUSB_HOST_HID_ENABLED=y in sdkconfig");
    return ESP_ERR_NOT_SUPPORTED;
}

bool usb_hid_host_is_connected(void) { return false; }

#endif
