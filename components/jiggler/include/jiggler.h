#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

esp_err_t jiggler_init(void);

// Configure the jiggler (does not enable it — call jiggler_enable() separately)
esp_err_t jiggler_configure(uint8_t keycode, uint8_t modifiers, uint32_t interval_ms);

esp_err_t jiggler_enable(void);
esp_err_t jiggler_disable(void);
esp_err_t jiggler_toggle(void);
bool      jiggler_is_enabled(void);
