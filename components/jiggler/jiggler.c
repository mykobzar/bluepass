#include "jiggler.h"
#include "usb_hid_device.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>

#define TAG "jiggler"

static struct {
    esp_timer_handle_t timer;
    uint8_t keycode;
    uint8_t modifiers;
    uint32_t interval_ms;
    bool enabled;
} s_jig;

static void (*s_state_cb)(bool enabled) = NULL;

void jiggler_set_state_cb(void (*cb)(bool enabled)) { s_state_cb = cb; }

static void jiggler_timer_cb(void *arg)
{
    hid_keyboard_report_t report = {
        .modifier = s_jig.modifiers,
        .reserved = 0,
        .keycode  = {s_jig.keycode, 0, 0, 0, 0, 0},
    };
    usb_hid_device_send_report(&report);
    // Brief delay then release — handled in next timer fire is sufficient,
    // but we must release now so the host sees a key-up event.
    usb_hid_device_send_release();
}

esp_err_t jiggler_init(void)
{
    s_jig.keycode     = HID_KEY_F15;  // F15 — invisible on most systems
    s_jig.modifiers   = 0;
    s_jig.interval_ms = 60000;        // default: once per minute
    s_jig.enabled     = false;

    const esp_timer_create_args_t args = {
        .callback = jiggler_timer_cb,
        .name     = "jiggler",
    };
    return esp_timer_create(&args, &s_jig.timer);
}

esp_err_t jiggler_configure(uint8_t keycode, uint8_t modifiers, uint32_t interval_ms)
{
    bool was_enabled = s_jig.enabled;
    if (was_enabled) jiggler_disable();

    s_jig.keycode     = keycode;
    s_jig.modifiers   = modifiers;
    s_jig.interval_ms = interval_ms;

    if (was_enabled) jiggler_enable();
    return ESP_OK;
}

esp_err_t jiggler_enable(void)
{
    if (s_jig.enabled) return ESP_OK;
    esp_err_t err = esp_timer_start_periodic(s_jig.timer,
                                              (uint64_t)s_jig.interval_ms * 1000);
    if (err == ESP_OK) {
        s_jig.enabled = true;
        if (s_state_cb) s_state_cb(true);
        ESP_LOGI(TAG, "enabled, interval=%"PRIu32"ms, keycode=0x%02X",
                 s_jig.interval_ms, s_jig.keycode);
    }
    return err;
}

esp_err_t jiggler_disable(void)
{
    if (!s_jig.enabled) return ESP_OK;
    esp_timer_stop(s_jig.timer);
    s_jig.enabled = false;
    if (s_state_cb) s_state_cb(false);
    ESP_LOGI(TAG, "disabled");
    return ESP_OK;
}

esp_err_t jiggler_toggle(void)
{
    return s_jig.enabled ? jiggler_disable() : jiggler_enable();
}

bool jiggler_is_enabled(void) { return s_jig.enabled; }
