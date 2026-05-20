#pragma once
#include "esp_err.h"
#include <stdbool.h>

typedef enum {
    WIFI_MGR_STATE_DISCONNECTED,
    WIFI_MGR_STATE_CONNECTING,
    WIFI_MGR_STATE_CONNECTED,
    WIFI_MGR_STATE_AP_MODE,
} wifi_mgr_state_t;

typedef void (*wifi_mgr_state_cb_t)(wifi_mgr_state_t state, void *ctx);

// Initialize WiFi stack and register event handlers.
// Automatically starts STA connection if credentials exist in storage,
// or AP mode if they don't.
esp_err_t wifi_manager_init(void);

wifi_mgr_state_t wifi_manager_get_state(void);
bool             wifi_manager_is_connected(void);

// Start soft-AP for initial WiFi credential setup
esp_err_t wifi_manager_start_ap(void);
esp_err_t wifi_manager_stop_ap(void);

// Sync system time via SNTP (called automatically after STA connect)
esp_err_t wifi_manager_sync_time(void);

// True once SNTP has successfully set the system clock
bool wifi_manager_is_time_synced(void);

void wifi_manager_set_state_cb(wifi_mgr_state_cb_t cb, void *ctx);

// LED control API:
//   led_on / led_off  — called by web UI to signal its active/inactive state
//   set_jiggler_active — called when jiggler is enabled/disabled
//   led_blink_once    — one-shot pulse on substitution
void wifi_manager_led_on(void);
void wifi_manager_led_off(void);
void wifi_manager_set_jiggler_active(bool active);
void wifi_manager_led_blink_once(void);
