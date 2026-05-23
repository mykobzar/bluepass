#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#define HOTKEY_SLOTS_MAX      16
#define HOTKEY_PAYLOAD_MAX    128   // password / arbitrary text / base32 TOTP secret
#define HOTKEY_LABEL_MAX      32

typedef enum {
    SLOT_TYPE_PASSWORD = 0,  // payload is a secret password (never shown in UI)
    SLOT_TYPE_TEXT     = 1,  // payload is arbitrary text (shown in UI)
    SLOT_TYPE_TOTP     = 2,  // payload is base32 TOTP secret
} hotkey_slot_type_t;

typedef struct {
    uint8_t  modifiers;                   // HID modifier byte for trigger (keyboard)
    uint8_t  keycode;                     // HID keycode for trigger (keyboard)
    uint16_t consumer_code;               // HID Consumer Usage ID; if non-zero overrides modifier+keycode
    hotkey_slot_type_t type;
    char payload[HOTKEY_PAYLOAD_MAX];     // secret / text / TOTP base32 key
    char label[HOTKEY_LABEL_MAX];         // display name (shown in UI)
    bool active;
    uint8_t match_mode;    // 0 = exact (keycode + modifiers, default); 1 = keycode only
    uint8_t replace_mode;  // 0 = replace all (default); 1 = keep modifiers after substitution
} hotkey_slot_t;

typedef struct {
    char ssid[33];
    char password[65];
} wifi_creds_t;

typedef struct {
    uint8_t addr[6];
    uint8_t addr_type;
    char    name[32];
} ble_peer_t;

typedef struct {
    bool enabled;
    uint8_t keycode;          // key to send periodically
    uint8_t modifiers;
    uint32_t interval_ms;
    uint8_t on_modifiers;     // hotkey to enable jiggler
    uint8_t on_keycode;
    uint8_t off_modifiers;    // hotkey to disable jiggler
    uint8_t off_keycode;
} jiggler_config_t;

esp_err_t storage_init(void);

esp_err_t storage_get_wifi_creds(wifi_creds_t *out);
esp_err_t storage_set_wifi_creds(const wifi_creds_t *creds);
esp_err_t storage_clear_wifi_creds(void);
bool      storage_has_wifi_creds(void);

esp_err_t storage_get_hotkey_slot(uint8_t index, hotkey_slot_t *out);
esp_err_t storage_set_hotkey_slot(uint8_t index, const hotkey_slot_t *slot);
esp_err_t storage_delete_hotkey_slot(uint8_t index);

esp_err_t storage_get_jiggler_config(jiggler_config_t *out);
esp_err_t storage_set_jiggler_config(const jiggler_config_t *cfg);

esp_err_t storage_get_ble_peer(ble_peer_t *out);
esp_err_t storage_set_ble_peer(const ble_peer_t *peer);
esp_err_t storage_clear_ble_peer(void);

typedef enum {
    BOARD_LED_TYPE_NONE   = 0,
    BOARD_LED_TYPE_RGB    = 1,  // WS2812 via RMT
    BOARD_LED_TYPE_SIMPLE = 2,  // plain GPIO output
} board_led_type_t;

typedef struct {
    int32_t btn_gpio;           // button GPIO, default 0
    uint8_t led_type;           // board_led_type_t, default BOARD_LED_TYPE_RGB
    int32_t rgb_gpio;           // WS2812 LED GPIO, default 48
    uint8_t rgb_brightness;     // 1-100 percent, default 4
    int32_t simple_gpio;        // plain LED GPIO, default -1 (not configured)
    uint8_t simple_active_high; // 1=active high, 0=active low, default 1
} board_config_t;

esp_err_t storage_get_board_config(board_config_t *out);
esp_err_t storage_set_board_config(const board_config_t *cfg);
