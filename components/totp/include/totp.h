#pragma once
#include "esp_err.h"
#include <stdint.h>

// Generate a 6-digit TOTP code using the current system time.
// secret is a base32-encoded string (standard RFC 4648 alphabet, no padding required).
// Returns ESP_ERR_INVALID_STATE if the system clock is not synchronized.
esp_err_t totp_generate(const char *base32_secret, uint32_t *code_out);

// Generate a TOTP code for an explicit Unix timestamp (for testing).
esp_err_t totp_generate_at(const char *base32_secret, int64_t unix_time, uint32_t *code_out);

// Seconds remaining in the current 30-second TOTP window
uint8_t totp_seconds_remaining(void);
