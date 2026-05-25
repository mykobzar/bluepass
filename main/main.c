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
#include "usb_hid_device.h"
#include "hotkey.h"
#include "wifi_manager.h"
#include "web_ui.h"
#include "jiggler.h"
#include "webhook.h"
#include "mqtt_mgr.h"
#include "esp_ota_ops.h"

static void on_jiggler_state(bool enabled)
{
    wifi_manager_set_jiggler_active(enabled);
}

#define TAG "bluepass"

#define LONG_PRESS_US      (10 * 1000 * 1000)  // 10 seconds in microseconds

static gpio_num_t s_btn_gpio = GPIO_NUM_0;  // default; overridden from board config

static esp_timer_handle_t s_btn_timer;
static int64_t s_btn_press_us;
static TaskHandle_t s_btn_task;

// ── BLE report callback ───────────────────────────────────────────────────────

static void on_ble_report(const bluepass_hid_report_t *report, void *ctx)
{
    hotkey_engine_process(report);
}

// ── Button handling ───────────────────────────────────────────────────────────

// Runs in a normal task context — safe to call any function
static void btn_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (web_ui_is_running()) {
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

// ── GPIO / LED init ───────────────────────────────────────────────────────────

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
    // NVS must be initialized before anything else touches flash
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

    ESP_ERROR_CHECK(ble_hid_host_init(on_ble_report, NULL));
    ESP_ERROR_CHECK(hotkey_engine_init());
    ESP_ERROR_CHECK(jiggler_init());
    jiggler_set_state_cb(on_jiggler_state);

    // Restore jiggler config including enabled state
    jiggler_config_t jig_cfg;
    if (storage_get_jiggler_config(&jig_cfg) == ESP_OK) {
        jiggler_configure(jig_cfg.keycode, jig_cfg.modifiers, jig_cfg.interval_ms);
        if (jig_cfg.enabled) jiggler_enable();
    }

    ESP_ERROR_CHECK(webhook_init());
    ESP_ERROR_CHECK(mqtt_mgr_init());
    ESP_ERROR_CHECK(web_ui_init());
    ESP_ERROR_CHECK(wifi_manager_init());

    // If no WiFi credentials stored, start AP + web UI immediately
    if (!storage_has_wifi_creds()) {
        ESP_LOGI(TAG, "no WiFi credentials — starting setup AP");
        ESP_ERROR_CHECK(wifi_manager_start_ap());
        ESP_ERROR_CHECK(web_ui_start());
    }

    // TinyUSB last: it switches the USB PHY mux from JTAG/Serial to OTG,
    // killing the serial monitor. All critical inits must complete first.
    ESP_ERROR_CHECK(usb_hid_device_init());

    // Confirm this boot was successful so the bootloader doesn't roll back,
    // and so future esp_ota_begin() calls are not blocked.
    esp_ota_mark_app_valid_cancel_rollback();
}
