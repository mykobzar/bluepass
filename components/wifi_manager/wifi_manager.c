#include "wifi_manager.h"
#include "storage.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "led_strip.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>

#define TAG "wifi_mgr"

#define AP_SSID  "bluepass"
#define AP_PASS  ""                 // open AP for initial setup

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define MAX_RETRY          5

// ── LED blink periods ─────────────────────────────────────────────────────────
#define LED_FAST_US    (100 * 1000)   // 100 ms per toggle → 5 Hz   (WiFi error)
#define LED_JIG_ON_US  (200 * 1000)   // 200 ms flash                (jiggler on-phase)
#define LED_JIG_OFF_US (2800 * 1000)  // 2.8 s dark                  (jiggler off-phase, 3 s cycle)
#define LED_PULSE_US   (150 * 1000)   // 150 ms one-shot pulse        (substitution)

// Full-scale RGB colours scaled by brightness at runtime
#define RED_R_F    255
#define RED_G_F    0
#define RED_B_F    0

#define GREEN_R_F  0
#define GREEN_G_F  255
#define GREEN_B_F  0

#define BLUE_R_F   0
#define BLUE_G_F   0
#define BLUE_B_F   255

#define WHITE_R_F  255
#define WHITE_G_F  255
#define WHITE_B_F  255

static uint8_t s_brightness = 20;  // 1-100 %

// WS2812 PWM is linear but human eye follows a power curve (gamma ~2.2).
// Squaring the normalised input (gamma=2 approximation) gives a perceptually
// even brightness ramp: perceived 50% → 25% PWM, 10% → 1% PWM.
static uint8_t scale(uint8_t v) {
    uint32_t linear = (uint32_t)s_brightness * 255 / 100;  // perceptual % → 0-255
    uint32_t pwm    = linear * linear / 255;                // gamma-2 correction
    return (uint8_t)((uint32_t)v * pwm / 255);
}

#define RED_R    scale(RED_R_F)
#define RED_G    scale(RED_G_F)
#define RED_B    scale(RED_B_F)
#define GREEN_R  scale(GREEN_R_F)
#define GREEN_G  scale(GREEN_G_F)
#define GREEN_B  scale(GREEN_B_F)
#define BLUE_R   scale(BLUE_R_F)
#define BLUE_G   scale(BLUE_G_F)
#define BLUE_B   scale(BLUE_B_F)
#define WHITE_R  scale(WHITE_R_F)
#define WHITE_G  scale(WHITE_G_F)
#define WHITE_B  scale(WHITE_B_F)

static EventGroupHandle_t s_wifi_events;
static int s_retry_count;
static wifi_mgr_state_t s_state = WIFI_MGR_STATE_DISCONNECTED;
static wifi_mgr_state_cb_t s_state_cb;
static void *s_state_cb_ctx;

static esp_timer_handle_t s_led_timer;    // periodic blink
static esp_timer_handle_t s_pulse_timer;  // one-shot substitution pulse
static led_strip_handle_t s_led_strip;

// Simple LED state
static board_led_type_t s_led_type = BOARD_LED_TYPE_RGB;
static gpio_num_t       s_simple_gpio = GPIO_NUM_NC;
static bool             s_simple_active_high = true;

static bool    s_led_on;
static uint8_t s_blink_r, s_blink_g, s_blink_b;
static bool    s_web_ui_active  = false;
static bool    s_jiggler_active = false;

static volatile bool s_time_synced = false;
static bool s_sntp_started = false;

// ── LED helpers ───────────────────────────────────────────────────────────────

static void led_set(uint8_t r, uint8_t g, uint8_t b)
{
    if (s_led_type == BOARD_LED_TYPE_RGB) {
        if (!s_led_strip) return;
        if (r == 0 && g == 0 && b == 0) {
            led_strip_clear(s_led_strip);
        } else {
            led_strip_set_pixel(s_led_strip, 0, r, g, b);
            led_strip_refresh(s_led_strip);
        }
    } else if (s_led_type == BOARD_LED_TYPE_SIMPLE && s_simple_gpio != GPIO_NUM_NC) {
        bool on = (r != 0 || g != 0 || b != 0);
        gpio_set_level(s_simple_gpio, on == s_simple_active_high ? 1 : 0);
    }
}

// ── LED state machine ─────────────────────────────────────────────────────────
//
// Priority (highest first):
//   1. Web UI active       → solid blue
//   2. WiFi not connected  → red fast blink (100 ms)
//   3. Jiggler active      → green slow blink (500 ms)
//   4. Connected idle      → off

static void led_apply(void)
{
    if (!s_led_timer) return;

    esp_timer_stop(s_led_timer);

    if (s_web_ui_active) {
        led_set(BLUE_R, BLUE_G, BLUE_B);
        s_led_on = true;
        return;
    }

    if (s_state != WIFI_MGR_STATE_CONNECTED) {
        s_blink_r = RED_R; s_blink_g = RED_G; s_blink_b = RED_B;
        s_led_on = false;
        led_set(0, 0, 0);
        esp_timer_start_once(s_led_timer, LED_FAST_US);
        return;
    }

    if (s_jiggler_active) {
        s_blink_r = GREEN_R; s_blink_g = GREEN_G; s_blink_b = GREEN_B;
        s_led_on = false;
        led_set(0, 0, 0);
        esp_timer_start_once(s_led_timer, LED_JIG_OFF_US);
        return;
    }

    led_set(0, 0, 0);
    s_led_on = false;
}

static void led_blink_cb(void *arg)
{
    // Web UI solid-blue takes priority — stale callback must not override it.
    if (s_web_ui_active) {
        led_set(BLUE_R, BLUE_G, BLUE_B);
        s_led_on = true;
        return;
    }
    s_led_on = !s_led_on;
    led_set(s_led_on ? s_blink_r : 0,
            s_led_on ? s_blink_g : 0,
            s_led_on ? s_blink_b : 0);

    // Restart one-shot for next toggle (periodic timer replaced by self-rescheduling one-shot)
    if (s_jiggler_active && s_state == WIFI_MGR_STATE_CONNECTED) {
        esp_timer_start_once(s_led_timer, s_led_on ? LED_JIG_ON_US : LED_JIG_OFF_US);
    } else if (s_state != WIFI_MGR_STATE_CONNECTED) {
        esp_timer_start_once(s_led_timer, LED_FAST_US);
    }
}

static void led_pulse_end_cb(void *arg)
{
    led_apply();
}

// ── WiFi state ────────────────────────────────────────────────────────────────

static void set_state(wifi_mgr_state_t state)
{
    s_state = state;
    led_apply();
    if (s_state_cb) s_state_cb(state, s_state_cb_ctx);
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            esp_wifi_connect();
            set_state(WIFI_MGR_STATE_CONNECTING);
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
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
        ESP_LOGI(TAG, "connected, syncing time...");
        wifi_manager_sync_time();
    }
}

// ── Init ──────────────────────────────────────────────────────────────────────

esp_err_t wifi_manager_init(void)
{
    s_wifi_events = xEventGroupCreate();

    // Load board config and init LED
    board_config_t board;
    storage_get_board_config(&board);
    s_led_type         = (board_led_type_t)board.led_type;
    s_brightness       = (board.rgb_brightness > 0 && board.rgb_brightness <= 100)
                         ? board.rgb_brightness : 4;
    s_simple_gpio      = (board.simple_gpio >= 0) ? (gpio_num_t)board.simple_gpio : GPIO_NUM_NC;
    s_simple_active_high = board.simple_active_high;

    if (s_led_type == BOARD_LED_TYPE_RGB) {
        led_strip_config_t strip_cfg = {
            .strip_gpio_num = (int)board.rgb_gpio,
            .max_leds       = 1,
            .led_model      = LED_MODEL_WS2812,
            .flags.invert_out = false,
        };
        led_strip_rmt_config_t rmt_cfg = {
            .clk_src       = RMT_CLK_SRC_DEFAULT,
            .resolution_hz = 10 * 1000 * 1000,
        };
        esp_err_t strip_err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_led_strip);
        if (strip_err != ESP_OK) {
            ESP_LOGE(TAG, "LED strip init failed: %s", esp_err_to_name(strip_err));
            s_led_strip = NULL;
        } else {
            led_strip_clear(s_led_strip);
        }
    } else if (s_led_type == BOARD_LED_TYPE_SIMPLE && s_simple_gpio != GPIO_NUM_NC) {
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << s_simple_gpio,
            .mode         = GPIO_MODE_OUTPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        gpio_config(&io);
        gpio_set_level(s_simple_gpio, s_simple_active_high ? 0 : 1);  // off
    }

    ESP_ERROR_CHECK(esp_netif_init());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init failed");

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        wifi_event_handler, NULL, NULL);

    const esp_timer_create_args_t blink_args = {
        .callback = led_blink_cb,
        .name     = "led_blink",
    };
    esp_timer_create(&blink_args, &s_led_timer);

    const esp_timer_create_args_t pulse_args = {
        .callback = led_pulse_end_cb,
        .name     = "led_pulse",
    };
    esp_timer_create(&pulse_args, &s_pulse_timer);

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

    led_apply();
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

// ── LED public API ────────────────────────────────────────────────────────────

void wifi_manager_led_on(void)
{
    s_web_ui_active = true;
    if (s_led_timer)   esp_timer_stop(s_led_timer);
    if (s_pulse_timer) esp_timer_stop(s_pulse_timer);
    led_set(BLUE_R, BLUE_G, BLUE_B);
    s_led_on = true;
}

void wifi_manager_led_off(void)
{
    s_web_ui_active = false;
    led_apply();
}

void wifi_manager_set_jiggler_active(bool active)
{
    s_jiggler_active = active;
    led_apply();
}

void wifi_manager_led_blink_once(void)
{
    if (s_web_ui_active) return;
    esp_timer_stop(s_led_timer);
    esp_timer_stop(s_pulse_timer);
    led_set(WHITE_R, WHITE_G, WHITE_B);
    s_led_on = true;
    esp_timer_start_once(s_pulse_timer, LED_PULSE_US);
}
