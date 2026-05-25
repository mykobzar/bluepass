#pragma once
#include "hid_defs.h"
#include "esp_err.h"
#include <stdbool.h>

esp_err_t webhook_init(void);

// Called from the key-processing path. Returns true if a slot matched
// (keypress consumed — not passed through to USB).
bool webhook_dispatch(const bluepass_hid_report_t *report);

// Reload enabled flag and slots from NVS (call after web UI saves config).
void webhook_reload(void);
