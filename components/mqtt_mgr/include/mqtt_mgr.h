#pragma once
#include "hid_defs.h"
#include "esp_err.h"
#include <stdbool.h>

esp_err_t mqtt_mgr_init(void);

// Called from the key-processing path. Returns true if a slot matched
// (keypress consumed — not passed through to USB).
bool mqtt_mgr_out_dispatch(const bluepass_hid_report_t *report);

// Reload config from NVS, reconnect if broker settings changed.
void mqtt_mgr_reload(void);

bool mqtt_mgr_is_connected(void);
