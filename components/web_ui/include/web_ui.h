#pragma once
#include "hotkey.h"
#include "esp_err.h"
#include <stdbool.h>

// Prepare the web UI (call once at startup; does not start the HTTP server)
esp_err_t web_ui_init(void);

// Start the HTTP server (called on GPIO0 short press)
esp_err_t web_ui_start(void);

// Stop the HTTP server
esp_err_t web_ui_stop(void);

bool web_ui_is_running(void);

// Push a key event to all connected WebSocket clients (key-log tab).
// Signature matches key_event_cb_t so it can be passed directly to
// hotkey_engine_set_event_cb().
void web_ui_push_key_event(const key_event_t *event, void *ctx);
