#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

esp_err_t fido2_init(void);

// Register the function that sends 64-byte HID packets to the host
void fido2_register_tx(void (*tx_cb)(const uint8_t *buf));

// Receive a 64-byte CTAPHID packet from the USB host (called by usb_hid_device callback)
void fido2_on_hid_rx(const uint8_t *buf);

bool fido2_is_enabled(void);

// True while waiting for user presence confirmation
bool fido2_pending_up(void);

// Confirm user presence — called from button handler or matched hotkey
void fido2_confirm_up(void);

// Returns true and consumes the key if UP is pending and the key matches the configured confirm key
bool fido2_intercept_key(uint8_t modifiers, uint8_t keycode);

// Regenerate the 256-bit master credential key; invalidates all existing credentials
esp_err_t fido2_regen_master_key(void);

// Full factory reset: clear all RKs, PIN, sign counter, regenerate master key
esp_err_t fido2_factory_reset(void);

// Diagnostic log: copy current log into buf (null-terminated); clear on request
void fido2_diag_get(char *buf, size_t maxlen);
void fido2_diag_clear(void);
