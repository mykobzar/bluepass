#include "mqtt_mgr.h"
#include "storage.h"
#include "usb_hid_device.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "mqtt_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

#define TAG "mqtt_mgr"

static esp_mqtt_client_handle_t s_client;
static bool                     s_connected;
static mqtt_broker_config_t     s_broker;
static mqtt_out_slot_t          s_out[MQTT_SLOTS_MAX];
static mqtt_in_slot_t           s_in[MQTT_SLOTS_MAX];

// ── helpers ───────────────────────────────────────────────────────────────────

static void load_slots(void)
{
    for (int i = 0; i < MQTT_SLOTS_MAX; i++) {
        if (storage_get_mqtt_out_slot(i, &s_out[i]) != ESP_OK)
            memset(&s_out[i], 0, sizeof(s_out[i]));
        if (storage_get_mqtt_in_slot(i, &s_in[i]) != ESP_OK)
            memset(&s_in[i], 0, sizeof(s_in[i]));
    }
}

static void subscribe_all(void)
{
    for (int i = 0; i < MQTT_SLOTS_MAX; i++) {
        if (s_in[i].active && s_in[i].topic[0])
            esp_mqtt_client_subscribe(s_client, s_in[i].topic, 0);
    }
}

static void handle_incoming(const char *topic, int topic_len,
                             const char *data, int data_len)
{
    for (int i = 0; i < MQTT_SLOTS_MAX; i++) {
        const mqtt_in_slot_t *s = &s_in[i];
        if (!s->active || !s->topic[0] || !s->keycode) continue;
        if (strncmp(s->topic, topic, topic_len) != 0 || s->topic[topic_len] != '\0')
            continue;
        // match_value empty = any value triggers
        if (s->match_value[0] &&
            (strncmp(s->match_value, data, data_len) != 0 ||
             s->match_value[data_len] != '\0'))
            continue;

        hid_keyboard_report_t report = {
            .modifier = s->modifiers,
            .reserved = 0,
            .keycode  = { s->keycode, 0, 0, 0, 0, 0 },
        };
        usb_hid_device_send_report(&report);
        vTaskDelay(pdMS_TO_TICKS(50));
        hid_keyboard_report_t release = {0};
        usb_hid_device_send_report(&release);
        ESP_LOGI(TAG, "in: topic=%.*s → kc=0x%02x mod=0x%02x",
                 topic_len, topic, s->keycode, s->modifiers);
    }
}

// ── MQTT event handler ────────────────────────────────────────────────────────

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)event_data;
    switch (event_id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        ESP_LOGI(TAG, "connected");
        subscribe_all();
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGI(TAG, "disconnected");
        break;
    case MQTT_EVENT_DATA:
        if (s_broker.in_enabled)
            handle_incoming(ev->topic, ev->topic_len, ev->data, ev->data_len);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG, "error");
        break;
    default:
        break;
    }
}

// ── WiFi event handlers ───────────────────────────────────────────────────────

static void on_wifi_connected(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (!s_broker.out_enabled && !s_broker.in_enabled) return;
    if (!s_broker.broker_url[0]) return;
    if (s_client) return;  // already running

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = s_broker.broker_url,
    };
    if (s_broker.username[0]) {
        cfg.credentials.username  = s_broker.username;
        cfg.credentials.authentication.password = s_broker.password;
    }
    s_client = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_client);
    ESP_LOGI(TAG, "starting MQTT → %s", s_broker.broker_url);
}

static void on_wifi_disconnected(void *arg, esp_event_base_t base,
                                  int32_t id, void *data)
{
    if (!s_client) return;
    esp_mqtt_client_stop(s_client);
    esp_mqtt_client_destroy(s_client);
    s_client    = NULL;
    s_connected = false;
    ESP_LOGI(TAG, "WiFi lost — MQTT stopped");
}

// ── Public API ────────────────────────────────────────────────────────────────

esp_err_t mqtt_mgr_init(void)
{
    storage_get_mqtt_broker(&s_broker);
    load_slots();

    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               on_wifi_connected, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                               on_wifi_disconnected, NULL));
    return ESP_OK;
}

void mqtt_mgr_reload(void)
{
    mqtt_broker_config_t new_broker;
    storage_get_mqtt_broker(&new_broker);
    load_slots();

    bool broker_changed = memcmp(&new_broker, &s_broker, sizeof(s_broker)) != 0;
    s_broker = new_broker;

    if (broker_changed && s_client) {
        on_wifi_disconnected(NULL, NULL, 0, NULL);
        // Reconnect only if WiFi is up
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
            on_wifi_connected(NULL, NULL, 0, NULL);
    } else if (!s_client && (s_broker.out_enabled || s_broker.in_enabled)) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
            on_wifi_connected(NULL, NULL, 0, NULL);
    } else if (s_client && !s_broker.out_enabled && !s_broker.in_enabled) {
        on_wifi_disconnected(NULL, NULL, 0, NULL);
    } else if (s_client && s_broker.in_enabled) {
        subscribe_all();  // re-subscribe after slot changes
    }
    ESP_LOGI(TAG, "reload: out_en=%d in_en=%d broker=%s",
             s_broker.out_enabled, s_broker.in_enabled, s_broker.broker_url);
}

bool mqtt_mgr_is_connected(void) { return s_connected; }

bool mqtt_mgr_out_dispatch(const bluepass_hid_report_t *report)
{
    if (!s_broker.out_enabled || !s_client || !s_connected) return false;
    if (report->consumer_code != 0) return false;

    bool matched = false;
    for (int i = 0; i < MQTT_SLOTS_MAX; i++) {
        const mqtt_out_slot_t *s = &s_out[i];
        if (!s->active || !s->keycode || !s->topic[0]) continue;
        if (report->keyboard.modifier != s->modifiers) continue;
        bool kc_found = false;
        for (int k = 0; k < 6; k++)
            if (report->keyboard.keycode[k] == s->keycode) { kc_found = true; break; }
        if (!kc_found) continue;

        int msg_id = esp_mqtt_client_publish(s_client, s->topic, s->value,
                                             (int)strlen(s->value), 0, 0);
        ESP_LOGI(TAG, "publish %s=%s (msg_id=%d)", s->topic, s->value, msg_id);
        matched = true;
    }
    return matched;
}
