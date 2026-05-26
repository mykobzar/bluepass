#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "driver/gpio.h"
#include "esp_timer.h"

#include "storage.h"
#include "ble_hid_host.h"
#include "ble_hid_device.h"
#include "usb_hid_device.h"
#include "usb_hid_host.h"
#include "hotkey.h"
#include "wifi_manager.h"
#include "web_ui.h"
#include "jiggler.h"
#include "webhook.h"
#include "mqtt_mgr.h"
#include "fido2.h"
#include "esp_ota_ops.h"

static void on_jiggler_state(bool enabled)
{
    wifi_manager_set_jiggler_active(enabled);
}

static void fido2_tx_wrapper(const uint8_t *buf)
{
    usb_hid_fido2_send(buf);
}

#define TAG "bluepass"

#define LONG_PRESS_US      (10 * 1000 * 1000)

static gpio_num_t s_btn_gpio = GPIO_NUM_0;

static esp_timer_handle_t s_btn_timer;
static int64_t s_btn_press_us;
static TaskHandle_t s_btn_task;

// ── HID output tables ─────────────────────────────────────────────────────────

static bool usb_is_ready(void) { return usb_hid_device_is_mounted(); }

static const hid_output_ops_t s_usb_out = {
    .send_report   = usb_hid_device_send_report,
    .send_consumer = usb_hid_device_send_consumer,
    .send_release  = usb_hid_device_send_release,
    .type_string   = usb_hid_device_type_string,
    .is_ready      = usb_is_ready,
};

static bool ble_dev_is_ready(void) { return ble_hid_device_is_connected(); }

static const hid_output_ops_t s_ble_out = {
    .send_report   = ble_hid_device_send_report,
    .send_consumer = ble_hid_device_send_consumer,
    .send_release  = ble_hid_device_send_release,
    .type_string   = ble_hid_device_type_string,
    .is_ready      = ble_dev_is_ready,
};

// ── Input callbacks ───────────────────────────────────────────────────────────

static void on_ble_report(const bluepass_hid_report_t *report, void *ctx)
{
    if (fido2_intercept_key(report->keyboard.modifier, report->keyboard.keycode[0])) return;
    hotkey_engine_process(report);
}

static void on_usb_report(const bluepass_hid_report_t *report, void *ctx)
{
    if (fido2_intercept_key(report->keyboard.modifier, report->keyboard.keycode[0])) return;
    hotkey_engine_process(report);
}

// ── Button handling ───────────────────────────────────────────────────────────

static void btn_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (fido2_pending_up()) {
            fido2_confirm_up();
            ESP_LOGI(TAG, "FIDO2 UP confirmed via button");
        } else if (web_ui_is_running()) {
            web_ui_stop();
            ESP_LOGI(TAG, "web UI stopped");
        } else {
            web_ui_start();
            ESP_LOGI(TAG, "web UI started");
        }
    }
}

static void btn_long_press_cb(void *arg)
{
    ESP_LOGI(TAG, "long press — resetting WiFi credentials");
    storage_clear_wifi_creds();
    wifi_manager_start_ap();
    web_ui_start();
}

static void IRAM_ATTR btn_isr(void *arg)
{
    int level = gpio_get_level(s_btn_gpio);
    int64_t now = esp_timer_get_time();

    if (level == 0) {
        s_btn_press_us = now;
        esp_timer_start_once(s_btn_timer, LONG_PRESS_US);
    } else {
        esp_timer_stop(s_btn_timer);
        int64_t held_us = now - s_btn_press_us;
        if (held_us < LONG_PRESS_US) {
            BaseType_t woken = pdFALSE;
            vTaskNotifyGiveFromISR(s_btn_task, &woken);
            portYIELD_FROM_ISR(woken);
        }
    }
}

static void gpio_init(void)
{
    const gpio_config_t btn_cfg = {
        .pin_bit_mask = 1ULL << s_btn_gpio,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .intr_type    = GPIO_INTR_ANYEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&btn_cfg));
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(s_btn_gpio, btn_isr, NULL));

    const esp_timer_create_args_t timer_args = {
        .callback = btn_long_press_cb,
        .name     = "btn_timer",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_btn_timer));
}

// ── Entry point ───────────────────────────────────────────────────────────────

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(storage_init());

    board_config_t board;
    storage_get_board_config(&board);
    s_btn_gpio = (gpio_num_t)board.btn_gpio;

    xTaskCreate(btn_task, "btn", 4096, NULL, 5, &s_btn_task);
    gpio_init();

    // Read connection mode and configure accordingly
    connection_mode_t mode;
    storage_get_connection_mode(&mode);
    ESP_LOGI(TAG, "connection mode: %d", (int)mode);

    // Wire output abstraction
    if (mode == CONN_MODE_BT_USB) {
        hotkey_engine_set_output(&s_usb_out);
    } else {
        hotkey_engine_set_output(&s_ble_out);
    }

    // Init BLE peripheral (for modes that output to a BT computer)
    if (mode == CONN_MODE_BT_BT || mode == CONN_MODE_USB_BT) {
        ESP_ERROR_CHECK(ble_hid_device_init());
    }

    // Init BLE central (for modes that input from a BT keyboard).
    // When report_cb is NULL, NimBLE still starts but central role is skipped.
    if (mode == CONN_MODE_BT_USB || mode == CONN_MODE_BT_BT) {
        ESP_ERROR_CHECK(ble_hid_host_init(on_ble_report, NULL));
    } else {
        // USB_BT: NimBLE needed for peripheral only — no central role
        ESP_ERROR_CHECK(ble_hid_host_init(NULL, NULL));
    }

    ESP_ERROR_CHECK(hotkey_engine_init());
    ESP_ERROR_CHECK(jiggler_init());
    jiggler_set_state_cb(on_jiggler_state);

    jiggler_config_t jig_cfg;
    if (storage_get_jiggler_config(&jig_cfg) == ESP_OK) {
        jiggler_configure(jig_cfg.keycode, jig_cfg.modifiers, jig_cfg.interval_ms);
        if (jig_cfg.enabled) jiggler_enable();
    }

    ESP_ERROR_CHECK(webhook_init());
    ESP_ERROR_CHECK(mqtt_mgr_init());
    ESP_ERROR_CHECK(web_ui_init());
    ESP_ERROR_CHECK(wifi_manager_init());

    if (!storage_has_wifi_creds()) {
        ESP_LOGI(TAG, "no WiFi credentials — starting setup AP");
        ESP_ERROR_CHECK(wifi_manager_start_ap());
        ESP_ERROR_CHECK(web_ui_start());
    }

    ESP_ERROR_CHECK(fido2_init());

    // USB init — mode determines device vs host.
    // TinyUSB switches USB PHY from JTAG to OTG; must be last.
    if (mode == CONN_MODE_BT_USB) {
        ESP_ERROR_CHECK(usb_hid_device_init());
        // Wire FIDO2 ↔ USB HID
        usb_hid_fido2_set_rx_cb(fido2_on_hid_rx);
        fido2_register_tx(fido2_tx_wrapper);
    } else if (mode == CONN_MODE_USB_BT) {
        ESP_ERROR_CHECK(usb_hid_host_init(on_usb_report, NULL));
    }
    // CONN_MODE_BT_BT: no USB at all

    esp_ota_mark_app_valid_cancel_rollback();
}
