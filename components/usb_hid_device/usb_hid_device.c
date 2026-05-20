#include "usb_hid_device.h"
#include "esp_log.h"
#include "esp_check.h"
#include "tinyusb.h"
#include "class/hid/hid_device.h"
#include "device/usbd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

#define TAG "usb_hid"
#define REPORT_ID_KEYBOARD 1
#define REPORT_ID_CONSUMER 2
#define EPNUM_HID          0x81   // Endpoint 1, IN direction
#define TYPE_INTER_KEY_MS  10     // ms between key-down and key-up when typing

// ── HID descriptors ──────────────────────────────────────────────────────────

static const uint8_t s_hid_report_desc[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(REPORT_ID_KEYBOARD)),
    TUD_HID_REPORT_DESC_CONSUMER(HID_REPORT_ID(REPORT_ID_CONSUMER)),
};

static const tusb_desc_device_t s_desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x303A,  // Espressif
    .idProduct          = 0x4002,  // Bluepass HID keyboard
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)
static const uint8_t s_config_desc[] = {
    // bConfigurationValue, bNumInterfaces, iConfiguration, bmAttributes, bMaxPower (×2 mA)
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    // bInterfaceNumber, iInterface, bInterfaceProtocol, wDescriptorLength, epIN addr, epSize, bInterval
    TUD_HID_DESCRIPTOR(0, 0, HID_ITF_PROTOCOL_NONE,
                       sizeof(s_hid_report_desc),
                       EPNUM_HID, CFG_TUD_HID_EP_BUFSIZE, 5)
};

static const char *s_string_desc[] = {
    (const char[]){0x09, 0x04},  // 0: supported language — English (0x0409)
    "bluepass",                   // 1: Manufacturer
    "bluepass",                   // 2: Product
    "000000",                     // 3: Serial
};

// ── ASCII → HID keycode table (printable 0x20–0x7E) ──────────────────────────

static const struct { uint8_t kc; uint8_t mod; } s_ascii_map[95] = {
    {HID_KEY_SPACE,       0},                  // 0x20 ' '
    {HID_KEY_1,           HID_MOD_L_SHIFT},    // 0x21 '!'
    {HID_KEY_APOSTROPHE,  HID_MOD_L_SHIFT},    // 0x22 '"'
    {HID_KEY_3,           HID_MOD_L_SHIFT},    // 0x23 '#'
    {HID_KEY_4,           HID_MOD_L_SHIFT},    // 0x24 '$'
    {HID_KEY_5,           HID_MOD_L_SHIFT},    // 0x25 '%'
    {HID_KEY_7,           HID_MOD_L_SHIFT},    // 0x26 '&'
    {HID_KEY_APOSTROPHE,  0},                  // 0x27 '\''
    {HID_KEY_9,           HID_MOD_L_SHIFT},    // 0x28 '('
    {HID_KEY_0,           HID_MOD_L_SHIFT},    // 0x29 ')'
    {HID_KEY_8,           HID_MOD_L_SHIFT},    // 0x2A '*'
    {HID_KEY_EQUAL,       HID_MOD_L_SHIFT},    // 0x2B '+'
    {HID_KEY_COMMA,       0},                  // 0x2C ','
    {HID_KEY_MINUS,       0},                  // 0x2D '-'
    {HID_KEY_PERIOD,      0},                  // 0x2E '.'
    {HID_KEY_SLASH,       0},                  // 0x2F '/'
    {HID_KEY_0,           0},                  // 0x30 '0'
    {HID_KEY_1,           0},                  // 0x31 '1'
    {HID_KEY_2,           0},                  // 0x32 '2'
    {HID_KEY_3,           0},                  // 0x33 '3'
    {HID_KEY_4,           0},                  // 0x34 '4'
    {HID_KEY_5,           0},                  // 0x35 '5'
    {HID_KEY_6,           0},                  // 0x36 '6'
    {HID_KEY_7,           0},                  // 0x37 '7'
    {HID_KEY_8,           0},                  // 0x38 '8'
    {HID_KEY_9,           0},                  // 0x39 '9'
    {HID_KEY_SEMICOLON,   HID_MOD_L_SHIFT},    // 0x3A ':'
    {HID_KEY_SEMICOLON,   0},                  // 0x3B ';'
    {HID_KEY_COMMA,       HID_MOD_L_SHIFT},    // 0x3C '<'
    {HID_KEY_EQUAL,       0},                  // 0x3D '='
    {HID_KEY_PERIOD,      HID_MOD_L_SHIFT},    // 0x3E '>'
    {HID_KEY_SLASH,       HID_MOD_L_SHIFT},    // 0x3F '?'
    {HID_KEY_2,           HID_MOD_L_SHIFT},    // 0x40 '@'
    // A–Z (0x41–0x5A)
    {HID_KEY_A, HID_MOD_L_SHIFT}, {HID_KEY_B, HID_MOD_L_SHIFT},
    {HID_KEY_C, HID_MOD_L_SHIFT}, {HID_KEY_D, HID_MOD_L_SHIFT},
    {HID_KEY_E, HID_MOD_L_SHIFT}, {HID_KEY_F, HID_MOD_L_SHIFT},
    {HID_KEY_G, HID_MOD_L_SHIFT}, {HID_KEY_H, HID_MOD_L_SHIFT},
    {HID_KEY_I, HID_MOD_L_SHIFT}, {HID_KEY_J, HID_MOD_L_SHIFT},
    {HID_KEY_K, HID_MOD_L_SHIFT}, {HID_KEY_L, HID_MOD_L_SHIFT},
    {HID_KEY_M, HID_MOD_L_SHIFT}, {HID_KEY_N, HID_MOD_L_SHIFT},
    {HID_KEY_O, HID_MOD_L_SHIFT}, {HID_KEY_P, HID_MOD_L_SHIFT},
    {HID_KEY_Q, HID_MOD_L_SHIFT}, {HID_KEY_R, HID_MOD_L_SHIFT},
    {HID_KEY_S, HID_MOD_L_SHIFT}, {HID_KEY_T, HID_MOD_L_SHIFT},
    {HID_KEY_U, HID_MOD_L_SHIFT}, {HID_KEY_V, HID_MOD_L_SHIFT},
    {HID_KEY_W, HID_MOD_L_SHIFT}, {HID_KEY_X, HID_MOD_L_SHIFT},
    {HID_KEY_Y, HID_MOD_L_SHIFT}, {HID_KEY_Z, HID_MOD_L_SHIFT},
    {HID_KEY_BRACKET_LEFT,  0},                // 0x5B '['
    {HID_KEY_BACKSLASH,     0},                // 0x5C '\\'
    {HID_KEY_BRACKET_RIGHT, 0},                // 0x5D ']'
    {HID_KEY_6,             HID_MOD_L_SHIFT},  // 0x5E '^'
    {HID_KEY_MINUS,         HID_MOD_L_SHIFT},  // 0x5F '_'
    {HID_KEY_GRAVE,         0},                // 0x60 '`'
    // a–z (0x61–0x7A)
    {HID_KEY_A, 0}, {HID_KEY_B, 0}, {HID_KEY_C, 0}, {HID_KEY_D, 0},
    {HID_KEY_E, 0}, {HID_KEY_F, 0}, {HID_KEY_G, 0}, {HID_KEY_H, 0},
    {HID_KEY_I, 0}, {HID_KEY_J, 0}, {HID_KEY_K, 0}, {HID_KEY_L, 0},
    {HID_KEY_M, 0}, {HID_KEY_N, 0}, {HID_KEY_O, 0}, {HID_KEY_P, 0},
    {HID_KEY_Q, 0}, {HID_KEY_R, 0}, {HID_KEY_S, 0}, {HID_KEY_T, 0},
    {HID_KEY_U, 0}, {HID_KEY_V, 0}, {HID_KEY_W, 0}, {HID_KEY_X, 0},
    {HID_KEY_Y, 0}, {HID_KEY_Z, 0},
    {HID_KEY_BRACKET_LEFT,  HID_MOD_L_SHIFT},  // 0x7B '{'
    {HID_KEY_BACKSLASH,     HID_MOD_L_SHIFT},  // 0x7C '|'
    {HID_KEY_BRACKET_RIGHT, HID_MOD_L_SHIFT},  // 0x7D '}'
    {HID_KEY_GRAVE,         HID_MOD_L_SHIFT},  // 0x7E '~'
};

// ── Init ──────────────────────────────────────────────────────────────────────

esp_err_t usb_hid_device_init(void)
{
    const tinyusb_config_t tusb_cfg = {
        .port = TINYUSB_PORT_FULL_SPEED_0,
        .task = {
            .size    = 4096,
            .priority = 5,
            .xCoreID = 0,
        },
        .descriptor = {
            .device            = &s_desc_device,
            .string            = s_string_desc,
            .string_count      = sizeof(s_string_desc) / sizeof(s_string_desc[0]),
            .full_speed_config = s_config_desc,
        },
    };
    return tinyusb_driver_install(&tusb_cfg);
}

bool usb_hid_device_is_mounted(void) { return tud_mounted(); }

// ── Report sending ────────────────────────────────────────────────────────────

// Wait up to ~20 ms for the USB HID endpoint to be ready (host ACKed previous report).
// Without this, BLE auto-repeat bursts drop reports and the host sees spurious key-up events.
static bool wait_hid_ready(void)
{
    for (int i = 0; i < 10; i++) {
        if (tud_hid_ready()) return true;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return false;
}

esp_err_t usb_hid_device_send_report(const hid_keyboard_report_t *report)
{
    if (!wait_hid_ready()) return ESP_ERR_INVALID_STATE;
    tud_hid_keyboard_report(REPORT_ID_KEYBOARD, report->modifier, report->keycode);
    return ESP_OK;
}

esp_err_t usb_hid_device_send_consumer(uint16_t usage_id)
{
    if (!wait_hid_ready()) return ESP_ERR_INVALID_STATE;
    tud_hid_report(REPORT_ID_CONSUMER, &usage_id, sizeof(usage_id));
    return ESP_OK;
}

esp_err_t usb_hid_device_send_release(void)
{
    if (!wait_hid_ready()) return ESP_ERR_INVALID_STATE;
    const uint8_t empty[6] = {0};
    tud_hid_keyboard_report(REPORT_ID_KEYBOARD, 0, empty);
    return ESP_OK;
}

esp_err_t usb_hid_device_type_string(const char *str)
{
    while (*str) {
        unsigned char c = (unsigned char)*str++;
        if (c == '\n') {
            const uint8_t kc[6] = {HID_KEY_ENTER};
            if (!wait_hid_ready()) return ESP_ERR_INVALID_STATE;
            tud_hid_keyboard_report(REPORT_ID_KEYBOARD, 0, kc);
            vTaskDelay(pdMS_TO_TICKS(TYPE_INTER_KEY_MS));
            usb_hid_device_send_release();
            vTaskDelay(pdMS_TO_TICKS(TYPE_INTER_KEY_MS));
            continue;
        }
        if (c < 0x20 || c > 0x7E) continue;

        uint8_t kc  = s_ascii_map[c - 0x20].kc;
        uint8_t mod = s_ascii_map[c - 0x20].mod;
        const uint8_t keys[6] = {kc, 0, 0, 0, 0, 0};
        if (!wait_hid_ready()) return ESP_ERR_INVALID_STATE;
        tud_hid_keyboard_report(REPORT_ID_KEYBOARD, mod, keys);
        vTaskDelay(pdMS_TO_TICKS(TYPE_INTER_KEY_MS));
        usb_hid_device_send_release();
        vTaskDelay(pdMS_TO_TICKS(TYPE_INTER_KEY_MS));
    }
    return ESP_OK;
}

// Uses Ctrl+Shift+U followed by hex digits (X11/GTK Unicode entry)
esp_err_t usb_hid_device_type_unicode(uint32_t codepoint)
{
    char hex[9];
    snprintf(hex, sizeof(hex), "%"PRIx32, codepoint);

    const uint8_t u_key[6] = {HID_KEY_U};
    if (!wait_hid_ready()) return ESP_ERR_INVALID_STATE;
    tud_hid_keyboard_report(REPORT_ID_KEYBOARD, HID_MOD_L_CTRL | HID_MOD_L_SHIFT, u_key);
    vTaskDelay(pdMS_TO_TICKS(TYPE_INTER_KEY_MS));
    usb_hid_device_send_release();
    vTaskDelay(pdMS_TO_TICKS(TYPE_INTER_KEY_MS));

    usb_hid_device_type_string(hex);

    const uint8_t enter[6] = {HID_KEY_ENTER};
    if (!wait_hid_ready()) return ESP_ERR_INVALID_STATE;
    tud_hid_keyboard_report(REPORT_ID_KEYBOARD, 0, enter);
    vTaskDelay(pdMS_TO_TICKS(TYPE_INTER_KEY_MS));
    usb_hid_device_send_release();
    return ESP_OK;
}

// ── TinyUSB required callbacks ────────────────────────────────────────────────

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    return s_hid_report_desc;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                                hid_report_type_t report_type,
                                uint8_t *buffer, uint16_t reqlen)
{
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                            hid_report_type_t report_type,
                            uint8_t const *buffer, uint16_t bufsize)
{
}
