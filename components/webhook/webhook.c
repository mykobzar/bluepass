#include "webhook.h"
#include "storage.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>
#include <stdio.h>

#define TAG "webhook"

typedef struct {
    char url[WEBHOOK_URL_MAX + WEBHOOK_VALUE_MAX * 3 + 10];
} wh_job_t;

static QueueHandle_t      s_queue;
static webhook_slot_t     s_slots[WEBHOOK_SLOTS_MAX];
static bool               s_enabled;

static void http_task(void *arg)
{
    wh_job_t job;
    while (true) {
        if (!xQueueReceive(s_queue, &job, portMAX_DELAY)) continue;

        esp_http_client_config_t cfg = {
            .url            = job.url,
            .method         = HTTP_METHOD_GET,
            .timeout_ms     = 5000,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };
        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        esp_err_t err = esp_http_client_perform(client);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "GET %s → %d", job.url,
                     esp_http_client_get_status_code(client));
        } else {
            ESP_LOGW(TAG, "GET %s failed: %s", job.url, esp_err_to_name(err));
        }
        esp_http_client_cleanup(client);
    }
}

esp_err_t webhook_init(void)
{
    s_queue = xQueueCreate(4, sizeof(wh_job_t));
    if (!s_queue) return ESP_ERR_NO_MEM;
    xTaskCreate(http_task, "wh_http", 4096, NULL, 4, NULL);
    webhook_reload();
    return ESP_OK;
}

void webhook_reload(void)
{
    storage_get_webhook_enabled(&s_enabled);
    for (int i = 0; i < WEBHOOK_SLOTS_MAX; i++) {
        if (storage_get_webhook_slot(i, &s_slots[i]) != ESP_OK)
            memset(&s_slots[i], 0, sizeof(s_slots[i]));
    }
    ESP_LOGI(TAG, "reload: enabled=%d", s_enabled);
}

// URL-encode value into dst (dst must be at least 3*len+1 bytes)
static void url_encode(const char *src, char *dst, size_t dst_sz)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t di = 0;
    for (const char *p = src; *p && di + 3 < dst_sz; p++) {
        unsigned char c = (unsigned char)*p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[di++] = (char)c;
        } else {
            dst[di++] = '%';
            dst[di++] = hex[c >> 4];
            dst[di++] = hex[c & 0xF];
        }
    }
    dst[di] = '\0';
}

bool webhook_dispatch(const bluepass_hid_report_t *report)
{
    if (!s_enabled) return false;
    if (report->consumer_code != 0) return false;

    bool matched = false;
    for (int i = 0; i < WEBHOOK_SLOTS_MAX; i++) {
        const webhook_slot_t *s = &s_slots[i];
        if (!s->active || !s->keycode) continue;
        if (report->keyboard.modifier != s->modifiers) continue;
        bool kc_found = false;
        for (int k = 0; k < 6; k++)
            if (report->keyboard.keycode[k] == s->keycode) { kc_found = true; break; }
        if (!kc_found) continue;

        wh_job_t job;
        if (s->value[0]) {
            char enc[WEBHOOK_VALUE_MAX * 3 + 1];
            url_encode(s->value, enc, sizeof(enc));
            snprintf(job.url, sizeof(job.url), "%s?value=%s", s->url, enc);
        } else {
            snprintf(job.url, sizeof(job.url), "%s", s->url);
        }
        if (xQueueSend(s_queue, &job, 0) != pdTRUE)
            ESP_LOGW(TAG, "queue full, dropping webhook");
        else
            ESP_LOGI(TAG, "queued: %s", job.url);
        matched = true;
    }
    return matched;
}
