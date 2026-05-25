#include "ble_hid_device.h"
#include "ble_hid_host.h"
#include "esp_log.h"
#include "esp_check.h"
#include "nimble/nimble_port.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "os/os_mbuf.h"
#include <string.h>
#include <stdio.h>

#define TAG "ble_hid_dev"

// ── HID Report Descriptor ─────────────────────────────────────────────────────

static const uint8_t s_hid_report_map[] = {
    // Keyboard — Report ID 1
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x06,        // Usage (Keyboard)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        //   Report ID (1)
    0x05, 0x07,        //   Usage Page (Key Codes)
    0x19, 0xE0,        //   Usage Min (Left Ctrl)
    0x29, 0xE7,        //   Usage Max (Right GUI)
    0x15, 0x00,        //   Logical Min (0)
    0x25, 0x01,        //   Logical Max (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x08,        //   Report Count (8) — modifier bits
    0x81, 0x02,        //   Input (Data, Var, Abs)
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x08,        //   Report Size (8) — reserved byte
    0x81, 0x03,        //   Input (Const)
    0x95, 0x06,        //   Report Count (6)
    0x75, 0x08,        //   Report Size (8) — keycodes
    0x15, 0x00,        //   Logical Min (0)
    0x25, 0x73,        //   Logical Max (0x73)
    0x05, 0x07,        //   Usage Page (Key Codes)
    0x19, 0x00,        //   Usage Min (0)
    0x29, 0x73,        //   Usage Max (0x73)
    0x81, 0x00,        //   Input (Data, Array, Abs)
    0xC0,              // End Collection
    // Consumer Control — Report ID 2
    0x05, 0x0C,        // Usage Page (Consumer)
    0x09, 0x01,        // Usage (Consumer Control)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x02,        //   Report ID (2)
    0x15, 0x00,        //   Logical Min (0)
    0x26, 0xFF, 0x03,  //   Logical Max (0x3FF)
    0x19, 0x00,        //   Usage Min (0)
    0x2A, 0xFF, 0x03,  //   Usage Max (0x3FF)
    0x75, 0x10,        //   Report Size (16)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x00,        //   Input (Data, Array, Abs)
    0xC0,              // End Collection
};

// ── State ─────────────────────────────────────────────────────────────────────

static uint16_t s_kbd_handle;       // GATT attr handle for keyboard input report
static uint16_t s_consumer_handle;  // GATT attr handle for consumer input report
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static bool     s_connected;
static bool     s_advertising;

static ble_hid_device_conn_cb_t s_conn_cb;
static void *s_conn_ctx;

// ── ASCII → keycode map (same as usb_hid_device) ─────────────────────────────

#include "class/hid/hid.h"
#ifndef HID_MOD_L_SHIFT
#define HID_MOD_L_SHIFT 0x02
#endif

static const struct { uint8_t kc; uint8_t mod; } s_ascii_map[95] = {
    {HID_KEY_SPACE,         0},                // 0x20 ' '
    {HID_KEY_1,             HID_MOD_L_SHIFT},  // 0x21 '!'
    {HID_KEY_APOSTROPHE,    HID_MOD_L_SHIFT},  // 0x22 '"'
    {HID_KEY_3,             HID_MOD_L_SHIFT},  // 0x23 '#'
    {HID_KEY_4,             HID_MOD_L_SHIFT},  // 0x24 '$'
    {HID_KEY_5,             HID_MOD_L_SHIFT},  // 0x25 '%'
    {HID_KEY_7,             HID_MOD_L_SHIFT},  // 0x26 '&'
    {HID_KEY_APOSTROPHE,    0},                // 0x27 '\''
    {HID_KEY_9,             HID_MOD_L_SHIFT},  // 0x28 '('
    {HID_KEY_0,             HID_MOD_L_SHIFT},  // 0x29 ')'
    {HID_KEY_8,             HID_MOD_L_SHIFT},  // 0x2A '*'
    {HID_KEY_EQUAL,         HID_MOD_L_SHIFT},  // 0x2B '+'
    {HID_KEY_COMMA,         0},                // 0x2C ','
    {HID_KEY_MINUS,         0},                // 0x2D '-'
    {HID_KEY_PERIOD,        0},                // 0x2E '.'
    {HID_KEY_SLASH,         0},                // 0x2F '/'
    {HID_KEY_0, 0}, {HID_KEY_1, 0}, {HID_KEY_2, 0}, {HID_KEY_3, 0},
    {HID_KEY_4, 0}, {HID_KEY_5, 0}, {HID_KEY_6, 0}, {HID_KEY_7, 0},
    {HID_KEY_8, 0}, {HID_KEY_9, 0},               // 0x30–0x39
    {HID_KEY_SEMICOLON,     HID_MOD_L_SHIFT},  // 0x3A ':'
    {HID_KEY_SEMICOLON,     0},                // 0x3B ';'
    {HID_KEY_COMMA,         HID_MOD_L_SHIFT},  // 0x3C '<'
    {HID_KEY_EQUAL,         0},                // 0x3D '='
    {HID_KEY_PERIOD,        HID_MOD_L_SHIFT},  // 0x3E '>'
    {HID_KEY_SLASH,         HID_MOD_L_SHIFT},  // 0x3F '?'
    {HID_KEY_2,             HID_MOD_L_SHIFT},  // 0x40 '@'
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
    {HID_KEY_Y, HID_MOD_L_SHIFT}, {HID_KEY_Z, HID_MOD_L_SHIFT},  // 0x41–0x5A
    {HID_KEY_BRACKET_LEFT,  0},                // 0x5B '['
    {HID_KEY_BACKSLASH,     0},                // 0x5C '\\'
    {HID_KEY_BRACKET_RIGHT, 0},                // 0x5D ']'
    {HID_KEY_6,             HID_MOD_L_SHIFT},  // 0x5E '^'
    {HID_KEY_MINUS,         HID_MOD_L_SHIFT},  // 0x5F '_'
    {HID_KEY_GRAVE,         0},                // 0x60 '`'
    {HID_KEY_A, 0}, {HID_KEY_B, 0}, {HID_KEY_C, 0}, {HID_KEY_D, 0},
    {HID_KEY_E, 0}, {HID_KEY_F, 0}, {HID_KEY_G, 0}, {HID_KEY_H, 0},
    {HID_KEY_I, 0}, {HID_KEY_J, 0}, {HID_KEY_K, 0}, {HID_KEY_L, 0},
    {HID_KEY_M, 0}, {HID_KEY_N, 0}, {HID_KEY_O, 0}, {HID_KEY_P, 0},
    {HID_KEY_Q, 0}, {HID_KEY_R, 0}, {HID_KEY_S, 0}, {HID_KEY_T, 0},
    {HID_KEY_U, 0}, {HID_KEY_V, 0}, {HID_KEY_W, 0}, {HID_KEY_X, 0},
    {HID_KEY_Y, 0}, {HID_KEY_Z, 0},               // 0x61–0x7A
    {HID_KEY_BRACKET_LEFT,  HID_MOD_L_SHIFT},  // 0x7B '{'
    {HID_KEY_BACKSLASH,     HID_MOD_L_SHIFT},  // 0x7C '|'
    {HID_KEY_BRACKET_RIGHT, HID_MOD_L_SHIFT},  // 0x7D '}'
    {HID_KEY_GRAVE,         HID_MOD_L_SHIFT},  // 0x7E '~'
};

// ── GATT callbacks ────────────────────────────────────────────────────────────

// Report Reference: {report_id, report_type=1(Input)}
static const uint8_t s_kbd_rpt_ref[]      = {0x01, 0x01};
static const uint8_t s_consumer_rpt_ref[] = {0x02, 0x01};

static int gatt_cb(uint16_t conn_handle, uint16_t attr_handle,
                   struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    int id = (int)(uintptr_t)arg;
    switch (id) {
    case 0: { // HID Information: bcdHID=1.11, bCountryCode=0, flags=0x02 (normally connectable)
        static const uint8_t info[] = {0x11, 0x01, 0x00, 0x02};
        return os_mbuf_append(ctxt->om, info, sizeof(info)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    case 1: // Report Map
        return os_mbuf_append(ctxt->om, s_hid_report_map, sizeof(s_hid_report_map)) == 0
               ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    case 2: // HID Control Point — accept writes, ignore value
        return 0;
    case 3: { // Keyboard Input Report (read returns zeroed state)
        static const uint8_t zero[8] = {0};
        return os_mbuf_append(ctxt->om, zero, sizeof(zero)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    case 4: { // Consumer Input Report
        static const uint8_t zero[2] = {0};
        return os_mbuf_append(ctxt->om, zero, sizeof(zero)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    case 5: { // Protocol Mode — always Report Mode (1)
        static const uint8_t mode = 0x01;
        if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR)
            os_mbuf_append(ctxt->om, &mode, 1);
        return 0;
    }
    case 10: // Report Reference: keyboard
        return os_mbuf_append(ctxt->om, s_kbd_rpt_ref, sizeof(s_kbd_rpt_ref)) == 0
               ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    case 11: // Report Reference: consumer
        return os_mbuf_append(ctxt->om, s_consumer_rpt_ref, sizeof(s_consumer_rpt_ref)) == 0
               ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static int dis_cb(uint16_t conn_handle, uint16_t attr_handle,
                  struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    static const uint8_t pnp[] = {0x02, 0xE5, 0x02, 0x01, 0x00, 0x01, 0x00};
    const char *str = NULL;
    switch ((int)(uintptr_t)arg) {
    case 0: str = "bluepass"; break;
    case 1: str = "Bluepass KB"; break;
    case 2: return os_mbuf_append(ctxt->om, pnp, sizeof(pnp)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (str) os_mbuf_append(ctxt->om, str, strlen(str));
    return 0;
}

static int bas_cb(uint16_t conn_handle, uint16_t attr_handle,
                  struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    static const uint8_t level = 100;
    return os_mbuf_append(ctxt->om, &level, 1) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

// ── GATT service table ────────────────────────────────────────────────────────

static const struct ble_gatt_svc_def s_svcs[] = {
    // HID Service (0x1812)
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x1812),
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = BLE_UUID16_DECLARE(0x2A4A),  // HID Information
              .flags = BLE_GATT_CHR_F_READ,
              .access_cb = gatt_cb, .arg = (void*)0 },
            { .uuid = BLE_UUID16_DECLARE(0x2A4B),  // Report Map
              .flags = BLE_GATT_CHR_F_READ,
              .access_cb = gatt_cb, .arg = (void*)1 },
            { .uuid = BLE_UUID16_DECLARE(0x2A4C),  // HID Control Point
              .flags = BLE_GATT_CHR_F_WRITE_NO_RSP,
              .access_cb = gatt_cb, .arg = (void*)2 },
            { .uuid = BLE_UUID16_DECLARE(0x2A4E),  // Protocol Mode
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP,
              .access_cb = gatt_cb, .arg = (void*)5 },
            // Keyboard Input Report (Report ID 1)
            { .uuid       = BLE_UUID16_DECLARE(0x2A4D),
              .flags      = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
              .val_handle = &s_kbd_handle,
              .access_cb  = gatt_cb, .arg = (void*)3,
              .descriptors = (struct ble_gatt_dsc_def[]) {
                  { .uuid = BLE_UUID16_DECLARE(0x2908),
                    .att_flags = BLE_ATT_F_READ,
                    .access_cb = gatt_cb, .arg = (void*)10 },
                  { 0 },
              },
            },
            // Consumer Control Input Report (Report ID 2)
            { .uuid       = BLE_UUID16_DECLARE(0x2A4D),
              .flags      = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
              .val_handle = &s_consumer_handle,
              .access_cb  = gatt_cb, .arg = (void*)4,
              .descriptors = (struct ble_gatt_dsc_def[]) {
                  { .uuid = BLE_UUID16_DECLARE(0x2908),
                    .att_flags = BLE_ATT_F_READ,
                    .access_cb = gatt_cb, .arg = (void*)11 },
                  { 0 },
              },
            },
            { 0 },
        },
    },
    // Battery Service (0x180F)
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180F),
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = BLE_UUID16_DECLARE(0x2A19),
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
              .access_cb = bas_cb },
            { 0 },
        },
    },
    // Device Information Service (0x180A)
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180A),
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = BLE_UUID16_DECLARE(0x2A29), .flags = BLE_GATT_CHR_F_READ,
              .access_cb = dis_cb, .arg = (void*)0 },  // Manufacturer Name
            { .uuid = BLE_UUID16_DECLARE(0x2A24), .flags = BLE_GATT_CHR_F_READ,
              .access_cb = dis_cb, .arg = (void*)1 },  // Model Number
            { .uuid = BLE_UUID16_DECLARE(0x2A50), .flags = BLE_GATT_CHR_F_READ,
              .access_cb = dis_cb, .arg = (void*)2 },  // PnP ID
            { 0 },
        },
    },
    { 0 },
};

// ── GAP event handler (peripheral role) ──────────────────────────────────────

static int gap_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_connected   = true;
            s_advertising = false;
            ESP_LOGI(TAG, "host connected handle=%d", s_conn_handle);
            if (s_conn_cb) s_conn_cb(true, s_conn_ctx);
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        s_connected   = false;
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        ESP_LOGI(TAG, "host disconnected reason=%d", event->disconnect.reason);
        if (s_conn_cb) s_conn_cb(false, s_conn_ctx);
        ble_hid_device_start_advertising();
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (!s_connected) ble_hid_device_start_advertising();
        break;

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        struct ble_gap_conn_desc desc;
        ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
        ble_store_util_delete_peer(&desc.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    }
    return 0;
}

// ── Public API ────────────────────────────────────────────────────────────────

static void on_nimble_sync(void)
{
    ble_hid_device_start_advertising();
}

esp_err_t ble_hid_device_init(void)
{
    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(s_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg rc=%d", rc);
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(s_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs rc=%d", rc);
        return ESP_FAIL;
    }

    ble_hid_host_add_sync_hook(on_nimble_sync);
    return ESP_OK;
}

esp_err_t ble_hid_device_start_advertising(void)
{
    if (s_connected) return ESP_OK;

    ble_svc_gap_device_name_set("Bluepass KB");

    static const ble_uuid16_t hid_uuid = BLE_UUID16_INIT(0x1812);
    struct ble_hs_adv_fields fields = {
        .flags                 = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP,
        .appearance            = 0x03C1,  // Keyboard
        .appearance_is_present = 1,
        .uuids16               = (ble_uuid16_t *)&hid_uuid,
        .num_uuids16           = 1,
        .uuids16_is_complete   = 1,
    };
    struct ble_hs_adv_fields rsp = {
        .name             = (uint8_t *)"Bluepass KB",
        .name_len         = 11,
        .name_is_complete = 1,
    };

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) { ESP_LOGW(TAG, "adv_set_fields rc=%d", rc); return ESP_FAIL; }
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) { ESP_LOGW(TAG, "adv_rsp_set_fields rc=%d", rc); return ESP_FAIL; }

    uint8_t own_addr_type;
    ble_hs_id_infer_auto(0, &own_addr_type);

    struct ble_gap_adv_params params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };
    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_cb, NULL);
    if (rc == 0 || rc == BLE_HS_EALREADY) {
        s_advertising = true;
        ESP_LOGI(TAG, "advertising started");
        return ESP_OK;
    }
    ESP_LOGW(TAG, "adv_start rc=%d", rc);
    return ESP_FAIL;
}

esp_err_t ble_hid_device_send_report(const hid_keyboard_report_t *report)
{
    if (!s_connected || s_conn_handle == BLE_HS_CONN_HANDLE_NONE)
        return ESP_ERR_INVALID_STATE;

    uint8_t buf[8];
    buf[0] = report->modifier;
    buf[1] = 0;
    memcpy(buf + 2, report->keycode, 6);

    struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, sizeof(buf));
    if (!om) return ESP_ERR_NO_MEM;
    int rc = ble_gatts_notify_custom(s_conn_handle, s_kbd_handle, om);
    if (rc != 0 && rc != BLE_HS_ENOTCONN && rc != BLE_HS_EDISABLED)
        ESP_LOGD(TAG, "notify kbd rc=%d", rc);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t ble_hid_device_send_consumer(uint16_t usage_id)
{
    if (!s_connected || s_conn_handle == BLE_HS_CONN_HANDLE_NONE)
        return ESP_ERR_INVALID_STATE;

    uint8_t buf[2] = {usage_id & 0xFF, (usage_id >> 8) & 0xFF};
    struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, sizeof(buf));
    if (!om) return ESP_ERR_NO_MEM;
    int rc = ble_gatts_notify_custom(s_conn_handle, s_consumer_handle, om);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t ble_hid_device_send_release(void)
{
    static const hid_keyboard_report_t zero = {0};
    return ble_hid_device_send_report(&zero);
}

#define TYPE_INTER_KEY_MS 20

esp_err_t ble_hid_device_type_string(const char *str)
{
    while (*str) {
        unsigned char c = (unsigned char)*str++;
        if (c == '\n') {
            hid_keyboard_report_t r = {.keycode = {HID_KEY_ENTER}};
            ble_hid_device_send_report(&r);
            vTaskDelay(pdMS_TO_TICKS(TYPE_INTER_KEY_MS));
            ble_hid_device_send_release();
            vTaskDelay(pdMS_TO_TICKS(TYPE_INTER_KEY_MS));
            continue;
        }
        if (c < 0x20 || c > 0x7E) continue;
        uint8_t kc  = s_ascii_map[c - 0x20].kc;
        uint8_t mod = s_ascii_map[c - 0x20].mod;
        hid_keyboard_report_t r = {.modifier = mod, .keycode = {kc}};
        ble_hid_device_send_report(&r);
        vTaskDelay(pdMS_TO_TICKS(TYPE_INTER_KEY_MS));
        ble_hid_device_send_release();
        vTaskDelay(pdMS_TO_TICKS(TYPE_INTER_KEY_MS));
    }
    return ESP_OK;
}

bool ble_hid_device_is_connected(void)  { return s_connected; }
bool ble_hid_device_is_advertising(void) { return s_advertising; }

void ble_hid_device_set_conn_cb(ble_hid_device_conn_cb_t cb, void *ctx)
{
    s_conn_cb  = cb;
    s_conn_ctx = ctx;
}

void ble_hid_device_get_peer_addr(char *buf, size_t len)
{
    if (!s_connected || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        buf[0] = '\0';
        return;
    }
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(s_conn_handle, &desc) != 0) {
        buf[0] = '\0';
        return;
    }
    const uint8_t *a = desc.peer_id_addr.val;
    snprintf(buf, len, "%02x:%02x:%02x:%02x:%02x:%02x",
             a[0], a[1], a[2], a[3], a[4], a[5]);
}
