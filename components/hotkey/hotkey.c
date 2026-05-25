#include "hotkey.h"
#include "storage.h"
#include "totp.h"
#include "jiggler.h"
#include "webhook.h"
#include "mqtt_mgr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>
#include <stdio.h>

#define TAG "hotkey"
#define TYPE_QUEUE_LEN 4

static hotkey_slot_t  s_slots[HOTKEY_SLOTS_MAX];
static int            s_slot_count;
static bool           s_had_consumer;
static hid_output_ops_t s_out;

void hotkey_engine_set_output(const hid_output_ops_t *ops)
{
    s_out = *ops;
}

static key_event_cb_t s_event_cb;
static void *s_event_ctx;

static uint8_t s_jig_on_mod,  s_jig_on_kc;
static uint8_t s_jig_off_mod, s_jig_off_kc;

// Queue for async string typing (avoids blocking the BLE callback)
static QueueHandle_t s_type_queue;
typedef struct {
    char    text[HOTKEY_PAYLOAD_MAX + 8];
    uint8_t restore_mods;  // if non-zero, re-send this modifier state after typing
} type_job_t;

static void typing_task(void *arg)
{
    type_job_t job;
    while (true) {
        if (xQueueReceive(s_type_queue, &job, portMAX_DELAY)) {
            if (s_out.send_release) s_out.send_release();
            vTaskDelay(pdMS_TO_TICKS(20));
            if (s_out.type_string) s_out.type_string(job.text);
            if (job.restore_mods && s_out.send_report) {
                hid_keyboard_report_t mod_report = {
                    .modifier = job.restore_mods,
                    .reserved = 0,
                    .keycode  = {0},
                };
                s_out.send_report(&mod_report);
            }
        }
    }
}

esp_err_t hotkey_engine_init(void)
{
    s_type_queue = xQueueCreate(TYPE_QUEUE_LEN, sizeof(type_job_t));
    if (!s_type_queue) return ESP_ERR_NO_MEM;

    xTaskCreate(typing_task, "typist", 4096, NULL, 5, NULL);
    hotkey_engine_reload();
    return ESP_OK;
}

void hotkey_engine_reload(void)
{
    s_slot_count = 0;
    for (int i = 0; i < HOTKEY_SLOTS_MAX; i++) {
        hotkey_slot_t slot;
        if (storage_get_hotkey_slot(i, &slot) == ESP_OK && slot.active) {
            s_slots[s_slot_count++] = slot;
        }
    }

    jiggler_config_t jcfg = {0};
    if (storage_get_jiggler_config(&jcfg) == ESP_OK) {
        s_jig_on_mod  = jcfg.on_modifiers;
        s_jig_on_kc   = jcfg.on_keycode;
        s_jig_off_mod = jcfg.off_modifiers;
        s_jig_off_kc  = jcfg.off_keycode;
    }

    ESP_LOGI(TAG, "loaded %d hotkey slots", s_slot_count);
}

static bool kb_combo_matches(const bluepass_hid_report_t *report, uint8_t mod, uint8_t kc)
{
    if (!kc) return false;
    if (report->consumer_code != 0) return false;
    if (report->keyboard.modifier != mod) return false;
    for (int i = 0; i < 6; i++) {
        if (report->keyboard.keycode[i] == kc) return true;
    }
    return false;
}

static bool report_matches(const bluepass_hid_report_t *report,
                            const hotkey_slot_t *s)
{
    if (s->consumer_code != 0) {
        return report->consumer_code == s->consumer_code;
    }
    // match_mode 0 (default): exact keycode + modifiers
    // match_mode 1: keycode only — fire regardless of which modifiers are held
    if (s->match_mode == 0 && report->keyboard.modifier != s->modifiers) return false;
    for (int i = 0; i < 6; i++) {
        if (report->keyboard.keycode[i] == s->keycode) return true;
    }
    return false;
}

static bool is_all_zero(const bluepass_hid_report_t *report)
{
    if (report->consumer_code != 0) return false;
    if (report->keyboard.modifier != 0) return false;
    for (int i = 0; i < 6; i++) if (report->keyboard.keycode[i]) return false;
    return true;
}

static void notify_event(const bluepass_hid_report_t *report, bool substituted, bool failed)
{
    if (!s_event_cb) return;
    key_event_t ev = {
        .timestamp_ms = esp_timer_get_time() / 1000,
        .report       = *report,
        .substituted  = substituted,
        .failed       = failed,
    };
    s_event_cb(&ev, s_event_ctx);
}

static void fire_substitution(const bluepass_hid_report_t *report,
                               const hotkey_slot_t *s, int slot_idx)
{
    bool failed = false;
    // replace_mode 0 (default): replace all — consume combo, type payload only
    // replace_mode 1: keep modifiers — re-send held modifier keys after typing
    uint8_t restore_mods = (s->replace_mode == 1) ? report->keyboard.modifier : 0;

    switch (s->type) {
    case SLOT_TYPE_PASSWORD:
    case SLOT_TYPE_TEXT: {
        type_job_t job = {0};
        strncpy(job.text, s->payload, sizeof(job.text) - 1);
        job.restore_mods = restore_mods;
        xQueueSend(s_type_queue, &job, 0);
        break;
    }
    case SLOT_TYPE_TOTP: {
        uint32_t code;
        if (totp_generate(s->payload, &code) == ESP_OK) {
            type_job_t job = {0};
            snprintf(job.text, sizeof(job.text), "%06" PRIu32, code);
            job.restore_mods = restore_mods;
            xQueueSend(s_type_queue, &job, 0);
        } else {
            ESP_LOGW(TAG, "TOTP slot %d failed — clock synced?", slot_idx);
            failed = true;
        }
        break;
    }
    }
    notify_event(report, true, failed);
}

void hotkey_engine_process(const bluepass_hid_report_t *report)
{
    if (is_all_zero(report)) {
        if (s_out.send_report) s_out.send_report(&report->keyboard);
        if (s_had_consumer) {
            if (s_out.send_consumer) s_out.send_consumer(0);
            s_had_consumer = false;
        }
        notify_event(report, false, false);
        return;
    }

    // Jiggler on/off hotkeys — checked before regular slots
    if (kb_combo_matches(report, s_jig_on_mod, s_jig_on_kc)) {
        // Toggle mode: off hotkey is empty — on hotkey acts as toggle
        if (s_jig_off_kc == 0) {
            jiggler_toggle();
        } else {
            jiggler_enable();
        }
        jiggler_config_t jcfg = {0};
        if (storage_get_jiggler_config(&jcfg) == ESP_OK) {
            jcfg.enabled = jiggler_is_enabled();
            storage_set_jiggler_config(&jcfg);
        }
        notify_event(report, true, false);
        return;
    }
    if (kb_combo_matches(report, s_jig_off_mod, s_jig_off_kc)) {
        jiggler_disable();
        jiggler_config_t jcfg = {0};
        if (storage_get_jiggler_config(&jcfg) == ESP_OK) {
            jcfg.enabled = false;
            storage_set_jiggler_config(&jcfg);
        }
        notify_event(report, true, false);
        return;
    }

    if (webhook_dispatch(report)) {
        notify_event(report, true, false);
        return;
    }
    if (mqtt_mgr_out_dispatch(report)) {
        notify_event(report, true, false);
        return;
    }

    for (int i = 0; i < s_slot_count; i++) {
        if (!report_matches(report, &s_slots[i])) continue;
        fire_substitution(report, &s_slots[i], i);
        return;
    }

    // No match — pass through to output
    if (report->consumer_code == 0) {
        if (s_out.send_report) s_out.send_report(&report->keyboard);
        if (s_had_consumer) {
            if (s_out.send_consumer) s_out.send_consumer(0);
            s_had_consumer = false;
        }
    } else {
        if (s_out.send_consumer) s_out.send_consumer(report->consumer_code);
        s_had_consumer = true;
    }
    notify_event(report, false, false);
}

void hotkey_engine_set_event_cb(key_event_cb_t cb, void *ctx)
{
    s_event_cb  = cb;
    s_event_ctx = ctx;
}
