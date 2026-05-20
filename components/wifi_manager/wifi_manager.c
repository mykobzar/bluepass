#include "wifi_manager.h"
#include "storage.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>

#define TAG "wifi_mgr"

#define AP_SSID       "bluepass"
#define AP_PASS       ""                // open AP for initial setup
#define LED_GPIO      GPIO_NUM_2        // adjust to actual LED pin

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define MAX_RETRY          5

static EventGroupHandle_t s_wifi_events;
static int s_retry_count;
static wifi_mgr_state_t s_state = WIFI_MGR_STATE_DISCONNECTED;
static wifi_mgr_state_cb_t s_state_cb;
static void *s_state_cb_ctx;

static esp_timer_handle_t s_led_timer;
static bool s_led_on;
static volatile bool s_time_synced = false;
static bool s_sntp_started = false;

static void set_state(wifi_mgr_state_t state)
{
    s_state = state;
    if (s_state_cb) s_state_cb(state, s_state_cb_ctx);
}

static void led_blink_cb(void *arg)
{
    s_led_on = !s_led_on;
    gpio_set_level(LED_GPIO, s_led_on ? 1 : 0);
}

static void led_blink_start(void)
{
    esp_timer_start_periodic(s_led_timer, 500 * 1000); // 500 ms
}

static void led_blink_stop(bool on)
{
    esp_timer_stop(s_led_timer);
    gpio_set_level(LED_GPIO, on ? 1 : 0);
    s_led_on = on;
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            esp_wifi_connect();
            set_state(WIFI_MGR_STATE_CONNECTING);
            led_blink_start();
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            led_blink_start();
            if (s_retry_count < MAX_RETRY) {
                esp_wifi_connect();
                s_retry_count++;
                ESP_LOGI(TAG, "retry %d/%d", s_retry_count, MAX_RETRY);
            } else {
                xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
                set_state(WIFI_MGR_STATE_DISCONNECTED);
            }
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
        set_state(WIFI_MGR_STATE_CONNECTED);
        led_blink_stop(true);
        ESP_LOGI(TAG, "connected, syncing time...");
        wifi_manager_sync_time();
    }
}

esp_err_t wifi_manager_init(void)
{
    s_wifi_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init failed");

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        wifi_event_handler, NULL, NULL);

    const esp_timer_create_args_t led_args = {
        .callback = led_blink_cb,
        .name     = "led_blink",
    };
    esp_timer_create(&led_args, &s_led_timer);

    if (storage_has_wifi_creds()) {
        wifi_creds_t creds;
        storage_get_wifi_creds(&creds);

        wifi_config_t wifi_cfg = {0};
        strncpy((char *)wifi_cfg.sta.ssid,     creds.ssid,     sizeof(wifi_cfg.sta.ssid) - 1);
        strncpy((char *)wifi_cfg.sta.password,  creds.password, sizeof(wifi_cfg.sta.password) - 1);
        wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
        esp_wifi_start();
    }
    return ESP_OK;
}

esp_err_t wifi_manager_start_ap(void)
{
    wifi_config_t ap_cfg = {
        .ap = {
            .ssid            = AP_SSID,
            .ssid_len        = sizeof(AP_SSID) - 1,
            .password        = AP_PASS,
            .max_connection  = 4,
            .authmode        = WIFI_AUTH_OPEN,
        },
    };
    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    esp_wifi_start();
    set_state(WIFI_MGR_STATE_AP_MODE);
    ESP_LOGI(TAG, "AP started: SSID=%s", AP_SSID);
    return ESP_OK;
}

esp_err_t wifi_manager_stop_ap(void)
{
    esp_wifi_stop();
    set_state(WIFI_MGR_STATE_DISCONNECTED);
    return ESP_OK;
}

static void sntp_sync_cb(struct timeval *tv)
{
    s_time_synced = true;
    ESP_LOGI(TAG, "NTP sync complete");
}

esp_err_t wifi_manager_sync_time(void)
{
    if (s_sntp_started) {
        // Already running — just trigger a re-sync request
        esp_sntp_restart();
        return ESP_OK;
    }
    esp_sntp_set_time_sync_notification_cb(sntp_sync_cb);
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.google.com");
    esp_sntp_init();
    s_sntp_started = true;
    return ESP_OK;
}

wifi_mgr_state_t wifi_manager_get_state(void) { return s_state; }
bool wifi_manager_is_connected(void) { return s_state == WIFI_MGR_STATE_CONNECTED; }
bool wifi_manager_is_time_synced(void) { return s_time_synced; }

void wifi_manager_set_state_cb(wifi_mgr_state_cb_t cb, void *ctx)
{
    s_state_cb     = cb;
    s_state_cb_ctx = ctx;
}
