#include "ble_hid_host.h"
#include "storage.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "services/gap/ble_svc_gap.h"
void ble_store_config_init(void);  // not declared in public header
#include "os/os_mbuf.h"
#include <string.h>
#include <stdarg.h>

#define TAG "ble_hid"

// ── Diagnostic ring buffer ────────────────────────────────────────────────────

#define BLE_LOG_SIZE 4096
static char   s_ble_log[BLE_LOG_SIZE];
static size_t s_ble_log_pos;

static void blog(const char *fmt, ...) __attribute__((format(printf,1,2)));
static void blog(const char *fmt, ...)
{
#if CONFIG_BLE_DEBUG_LOG
    char tmp[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    ESP_LOGI(TAG, "%s", tmp);
    size_t n = strlen(tmp);
    if (s_ble_log_pos + n + 2 >= BLE_LOG_SIZE) {
        s_ble_log_pos = 0;
        memset(s_ble_log, 0, BLE_LOG_SIZE);
    }
    memcpy(s_ble_log + s_ble_log_pos, tmp, n);
    s_ble_log[s_ble_log_pos + n]     = '\n';
    s_ble_log[s_ble_log_pos + n + 1] = '\0';
    s_ble_log_pos += n + 1;
#else
    (void)fmt;
#endif
}

#define HID_SVC_UUID         0x1812
#define REPORT_CHR_UUID      0x2A4D
#define PROTO_MODE_CHR_UUID  0x2A4E
#define HID_CTRL_PT_CHR_UUID 0x2A4C
#define CCCD_UUID            0x2902
#define MAX_REPORTS          8

static struct {
    ble_hid_report_cb_t  report_cb;
    void                *report_ctx;
    ble_connection_cb_t  conn_cb;
    void                *conn_ctx;
    ble_scan_result_cb_t scan_cb;
    void                *scan_ctx;
    uint16_t             conn_handle;
    bool                 connected;
    char                 peer_name[32];
    // Stored for auto-reconnect
    uint8_t              peer_addr[6];
    uint8_t              peer_addr_type;
    bool                 peer_known;
} s_ctx;

static esp_timer_handle_t s_reconnect_timer;
static int                s_reconnect_attempts;

#define SYNC_HOOKS_MAX 4
static ble_sync_hook_t s_sync_hooks[SYNC_HOOKS_MAX];
static int             s_sync_hook_count;
#define RECONNECT_DELAY_US    1500000
#define RECONNECT_RETRY_US    3000000

// Consumer Control polling (Input reports that have no CCCD)
#define MAX_POLL_HANDLES  4
#define POLL_INTERVAL_US  20000   // 20 ms

static struct {
    uint16_t handles[MAX_POLL_HANDLES];
    uint16_t last_val[MAX_POLL_HANDLES];
    int      count;
    int      current;
} s_poll;

static esp_timer_handle_t s_poll_timer;

// Discovery state — reset on each new discovery session.
// s_disc_gen is incremented each session so stale callbacks are ignored.
static uint8_t s_disc_gen;

static struct {
    uint16_t hid_svc_end;
    uint16_t proto_mode_handle;
    uint16_t ctrl_pt_handle;
    uint16_t report_val_handles[MAX_REPORTS];
    uint8_t  report_props[MAX_REPORTS];    // BLE_GATT_CHR_PROP_* flags from discovery
    int      report_count;
    // Per-characteristic state populated during descriptor scan
    uint16_t cccd_handles[MAX_REPORTS];    // CCCD descriptor handle (0 if none)
    uint8_t  report_ref_type[MAX_REPORTS]; // 0=unknown, 1=Input, 2=Output, 3=Feature
    bool     waiting_ref_read;             // true while async Report Reference read is in flight
    // Handles of Input Reports we successfully subscribed to (for NOTIFY_RX filtering)
    uint16_t subscribed_handles[MAX_REPORTS];
    int      subscribed_count;
} s_gatt;

// ── Consumer Control polling ──────────────────────────────────────────────────

static int on_poll_read(uint16_t conn_handle,
                        const struct ble_gatt_error *error,
                        struct ble_gatt_attr *attr, void *arg)
{
    int idx = (int)(uintptr_t)arg;
    if (error->status != 0 || !attr || !attr->om) return 0;

    uint8_t  buf[4] = {0};
    uint16_t len    = OS_MBUF_PKTLEN(attr->om);
    if (len > sizeof(buf)) len = sizeof(buf);
    os_mbuf_copydata(attr->om, 0, len, buf);

    // Parse 16-bit consumer usage code (LE).
    // Some reports include a 1-byte Report ID prefix.
    uint16_t cc;
    if (len >= 3) {
        cc = (uint16_t)buf[1] | ((uint16_t)buf[2] << 8); // skip report ID byte
    } else if (len >= 2) {
        cc = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    } else {
        cc = buf[0];
    }

    if (cc == s_poll.last_val[idx]) return 0;
    s_poll.last_val[idx] = cc;

    if (cc != 0 && s_ctx.report_cb) {
        bluepass_hid_report_t report = {0};
        report.consumer_code = cc;
        blog("CONSUMER cc=0x%04x h=%u", cc, s_poll.handles[idx]);
        s_ctx.report_cb(&report, s_ctx.report_ctx);
    }
    return 0;
}

static void poll_timer_cb(void *arg)
{
    if (!s_ctx.connected || s_poll.count == 0) return;
    int idx = s_poll.current;
    s_poll.current = (s_poll.current + 1) % s_poll.count;
    ble_gattc_read(s_ctx.conn_handle, s_poll.handles[idx],
                   on_poll_read, (void *)(uintptr_t)idx);
}

// ── Post-discovery HID init ───────────────────────────────────────────────────

static void on_hid_ready(uint16_t conn_handle)
{
    if (s_gatt.ctrl_pt_handle) {
        uint8_t cp = 0x00;
        ble_gattc_write_no_rsp_flat(conn_handle, s_gatt.ctrl_pt_handle,
                                     &cp, sizeof(cp));
    }
    blog("HID READY subscribed=%d poll=%d", s_gatt.subscribed_count, s_poll.count);
    if (s_poll.count > 0) {
        esp_timer_start_periodic(s_poll_timer, POLL_INTERVAL_US);
    }
}

// ── GATT discovery chain ──────────────────────────────────────────────────────
// arg encodes: high byte = discovery generation, low byte = report index

static void subscribe_report(uint16_t conn_handle, uint8_t gen, int idx);

// Returns CCCD value to write (0x0001=NOTIFY, 0x0002=INDICATE) based on props.
// Returns 0 if neither supported (characteristic should be polled instead).
static uint16_t cccd_val_for(int idx)
{
    uint8_t p = s_gatt.report_props[idx];
    if (p & BLE_GATT_CHR_PROP_NOTIFY)   return 0x0001;
    if (p & BLE_GATT_CHR_PROP_INDICATE) return 0x0002;
    return 0;
}

static int on_write_cccd(uint16_t conn_handle,
                          const struct ble_gatt_error *error,
                          struct ble_gatt_attr *attr, void *arg)
{
    uint8_t gen = (uint8_t)((uintptr_t)arg >> 8);
    int     idx = (uint8_t)((uintptr_t)arg & 0xFF);
    if (gen != s_disc_gen) return 0;

    if (error->status != 0) {
        blog("CCCD write FAILED report=%d err=0x%x → poll fallback", idx, error->status);
        // Subscribe failed — fall back to periodic polling
        if (s_poll.count < MAX_POLL_HANDLES)
            s_poll.handles[s_poll.count++] = s_gatt.report_val_handles[idx];
    } else {
        blog("CCCD ok report=%d handle=%u", idx, s_gatt.report_val_handles[idx]);
        if (s_gatt.subscribed_count < MAX_REPORTS)
            s_gatt.subscribed_handles[s_gatt.subscribed_count++] =
                s_gatt.report_val_handles[idx];
    }
    subscribe_report(conn_handle, gen, idx + 1);
    return 0;
}

// Called after async read of Report Reference descriptor (UUID 0x2908).
// ref[0] = Report ID, ref[1] = Report Type (1=Input, 2=Output, 3=Feature).
static int on_read_report_ref(uint16_t conn_handle,
                               const struct ble_gatt_error *error,
                               struct ble_gatt_attr *attr, void *arg)
{
    uint8_t gen = (uint8_t)((uintptr_t)arg >> 8);
    int     idx = (uint8_t)((uintptr_t)arg & 0xFF);
    if (gen != s_disc_gen) return 0;

    if (error->status == 0 && attr && attr->om) {
        uint8_t ref[2] = {0, 0};
        os_mbuf_copydata(attr->om, 0, 2, ref);
        s_gatt.report_ref_type[idx] = ref[1];
        blog("report_ref idx=%d id=%u type=%u", idx, ref[0], ref[1]);
    }
    s_gatt.waiting_ref_read = false;

    // Subscribe if this is an Input report (type=1) and has a CCCD.
    // If type is still unknown (no Report Reference found), assume Input.
    uint8_t rtype = s_gatt.report_ref_type[idx];
    bool is_input = (rtype == 1 || rtype == 0);
    if (is_input && s_gatt.cccd_handles[idx]) {
        uint16_t ccval = cccd_val_for(idx);
        if (ccval) {
            uint16_t val = htole16(ccval);
            blog("CCCD write report=%d val=0x%04x (NOTIFY=%d INDICATE=%d)",
                 idx, ccval, !!(ccval & 1), !!(ccval & 2));
            ble_gattc_write_flat(conn_handle, s_gatt.cccd_handles[idx],
                                 &val, sizeof(val), on_write_cccd, arg);
        } else {
            // Char has CCCD but neither NOTIFY nor INDICATE — poll
            if (s_poll.count < MAX_POLL_HANDLES) {
                s_poll.handles[s_poll.count++] = s_gatt.report_val_handles[idx];
                blog("poll report=%d handle=%u (no notify/indicate)", idx, s_gatt.report_val_handles[idx]);
            }
            subscribe_report(conn_handle, gen, idx + 1);
        }
    } else {
        blog("skip report=%d type=%u (not Input or no CCCD)", idx, rtype);
        if (is_input && !s_gatt.cccd_handles[idx] &&
            s_poll.count < MAX_POLL_HANDLES) {
            s_poll.handles[s_poll.count++] = s_gatt.report_val_handles[idx];
            blog("poll report=%d handle=%u", idx, s_gatt.report_val_handles[idx]);
        }
        subscribe_report(conn_handle, gen, idx + 1);
    }
    return 0;
}

static int on_disc_dscs(uint16_t conn_handle,
                         const struct ble_gatt_error *error,
                         uint16_t chr_val_handle,
                         const struct ble_gatt_dsc *dsc, void *arg)
{
    uint8_t gen = (uint8_t)((uintptr_t)arg >> 8);
    int     idx = (uint8_t)((uintptr_t)arg & 0xFF);
    if (gen != s_disc_gen) return 0;

    if (error->status == BLE_HS_EDONE) {
        if (!s_gatt.waiting_ref_read) {
            uint8_t rtype = s_gatt.report_ref_type[idx];
            bool is_input = (rtype == 1 || rtype == 0);
            if (is_input && s_gatt.cccd_handles[idx]) {
                uint16_t ccval = cccd_val_for(idx);
                if (ccval) {
                    uint16_t val = htole16(ccval);
                    ble_gattc_write_flat(conn_handle, s_gatt.cccd_handles[idx],
                                         &val, sizeof(val), on_write_cccd, arg);
                } else {
                    if (s_poll.count < MAX_POLL_HANDLES)
                        s_poll.handles[s_poll.count++] = s_gatt.report_val_handles[idx];
                    subscribe_report(conn_handle, gen, idx + 1);
                }
            } else {
                if (is_input && !s_gatt.cccd_handles[idx] &&
                    s_poll.count < MAX_POLL_HANDLES) {
                    s_poll.handles[s_poll.count++] = s_gatt.report_val_handles[idx];
                    blog("poll report=%d handle=%u", idx, s_gatt.report_val_handles[idx]);
                }
                subscribe_report(conn_handle, gen, idx + 1);
            }
        }
        // else: on_read_report_ref will advance the chain after the read completes
        return 0;
    }
    if (error->status != 0 || !dsc) return 0;

    uint16_t uuid = ble_uuid_u16(&dsc->uuid.u);
    if (uuid == 0x2908) { // Report Reference — read value to determine Input/Output/Feature
        s_gatt.waiting_ref_read = true;
        ble_gattc_read(conn_handle, dsc->handle, on_read_report_ref, arg);
    } else if (uuid == CCCD_UUID) {
        s_gatt.cccd_handles[idx] = dsc->handle;
    }
    return 0;
}

static void subscribe_report(uint16_t conn_handle, uint8_t gen, int idx)
{
    if (gen != s_disc_gen) return;
    if (idx >= s_gatt.report_count) {
        on_hid_ready(conn_handle);
        return;
    }
    // Reset per-characteristic state before descriptor scan
    s_gatt.cccd_handles[idx]    = 0;
    s_gatt.report_ref_type[idx] = 0;
    s_gatt.waiting_ref_read     = false;

    void *arg = (void *)(uintptr_t)(((uint32_t)gen << 8) | (uint8_t)idx);
    ble_gattc_disc_all_dscs(conn_handle,
                             s_gatt.report_val_handles[idx],
                             s_gatt.hid_svc_end,
                             on_disc_dscs, arg);
}

static int on_disc_chrs(uint16_t conn_handle,
                         const struct ble_gatt_error *error,
                         const struct ble_gatt_chr *chr, void *arg)
{
    uint8_t gen = (uint8_t)(uintptr_t)arg;
    if (gen != s_disc_gen) return 0;

    if (error->status == BLE_HS_EDONE) {
        ESP_LOGI(TAG, "chr discovery done, %d report(s)", s_gatt.report_count);
        subscribe_report(conn_handle, gen, 0);
        return 0;
    }
    if (error->status != 0 || !chr) return 0;

    uint16_t uuid = ble_uuid_u16(&chr->uuid.u);
    if (uuid == REPORT_CHR_UUID && s_gatt.report_count < MAX_REPORTS) {
        blog("Report chr val=%u props=0x%02x", chr->val_handle, chr->properties);
        s_gatt.report_props[s_gatt.report_count]        = chr->properties;
        s_gatt.report_val_handles[s_gatt.report_count++] = chr->val_handle;
    } else if (uuid == PROTO_MODE_CHR_UUID) {
        s_gatt.proto_mode_handle = chr->val_handle;
    } else if (uuid == HID_CTRL_PT_CHR_UUID) {
        s_gatt.ctrl_pt_handle = chr->val_handle;
    }
    return 0;
}

static int on_disc_svcs(uint16_t conn_handle,
                         const struct ble_gatt_error *error,
                         const struct ble_gatt_svc *svc, void *arg)
{
    uint8_t gen = (uint8_t)(uintptr_t)arg;
    if (gen != s_disc_gen) return 0;
    if (error->status == BLE_HS_EDONE || !svc) return 0;
    if (error->status != 0) return 0;

    if (ble_uuid_u16(&svc->uuid.u) == HID_SVC_UUID) {
        ESP_LOGI(TAG, "HID service found: handles %u–%u",
                 svc->start_handle, svc->end_handle);
        s_gatt.hid_svc_end = svc->end_handle;
        ble_gattc_disc_all_chrs(conn_handle, svc->start_handle,
                                 svc->end_handle, on_disc_chrs, arg);
    }
    return 0;
}

static void start_discovery(uint16_t conn_handle)
{
    s_disc_gen++;
    memset(&s_gatt, 0, sizeof(s_gatt));
    esp_timer_stop(s_poll_timer);
    memset(&s_poll, 0, sizeof(s_poll));
    // subscribed_count etc. are zeroed by memset
    void *arg = (void *)(uintptr_t)s_disc_gen;
    int rc = ble_gattc_disc_all_svcs(conn_handle, on_disc_svcs, arg);
    blog("discovery start gen=%u rc=%d", s_disc_gen, rc);
}

// ── Auto-reconnect ────────────────────────────────────────────────────────────

static int gap_event_handler(struct ble_gap_event *event, void *arg);  // forward

static void schedule_reconnect(uint64_t delay_us)
{
    esp_timer_stop(s_reconnect_timer);
    esp_timer_start_once(s_reconnect_timer, delay_us);
}

static void reconnect_timer_cb(void *arg)
{
    if (!s_ctx.peer_known || s_ctx.connected) return;
    s_reconnect_attempts++;
    blog("reconnect attempt %d", s_reconnect_attempts);
    ble_addr_t peer = { .type = s_ctx.peer_addr_type };
    memcpy(peer.val, s_ctx.peer_addr, 6);
    int rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &peer, 30000, NULL,
                             gap_event_handler, NULL);
    if (rc != 0) {
        blog("gap_connect rc=%d, retry in 3s", rc);
        schedule_reconnect(RECONNECT_RETRY_US);
    }
}

// ── GAP event handler ─────────────────────────────────────────────────────────

static int gap_event_handler(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        // LL link is up but host sync may not be done — just store the handle.
        // Wait for LINK_ESTAB (38) before initiating SM or GATT.
        if (event->connect.status == 0) {
            s_ctx.conn_handle = event->connect.conn_handle;
            blog("CONNECT handle=%d (waiting for LINK_ESTAB)", s_ctx.conn_handle);
        } else {
            blog("CONNECT failed status=%d", event->connect.status);
            if (s_ctx.peer_known)
                schedule_reconnect(RECONNECT_RETRY_US);
        }
        break;

    case BLE_GAP_EVENT_LINK_ESTAB:
        if (event->link_estab.status == 0) {
            s_ctx.connected = true;
            s_reconnect_attempts = 0;
            blog("LINK_ESTAB handle=%d cached=%d",
                 event->link_estab.conn_handle, s_ctx.conn_handle);
            // Persist this peer so we can reconnect after reboot
            ble_peer_t p;
            memcpy(p.addr, s_ctx.peer_addr, 6);
            p.addr_type = s_ctx.peer_addr_type;
            strncpy(p.name, s_ctx.peer_name, sizeof(p.name) - 1);
            p.name[sizeof(p.name) - 1] = '\0';
            storage_set_ble_peer(&p);
            int sec_rc = ble_gap_security_initiate(s_ctx.conn_handle);
            blog("security_initiate rc=%d", sec_rc);
            if (s_ctx.conn_cb) s_ctx.conn_cb(true, s_ctx.conn_ctx);
        } else {
            blog("LINK_ESTAB failed status=%d", event->link_estab.status);
            s_ctx.conn_handle = BLE_HS_CONN_HANDLE_NONE;
            if (s_ctx.peer_known)
                schedule_reconnect(RECONNECT_RETRY_US);
        }
        break;

    case BLE_GAP_EVENT_CONN_UPDATE_REQ:
        // Accept whatever connection parameters the peripheral requests
        *event->conn_update_req.self_params = *event->conn_update_req.peer_params;
        blog("CONN_UPDATE_REQ accepted itvl=%u lat=%u",
             event->conn_update_req.peer_params->itvl_max,
             event->conn_update_req.peer_params->latency);
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE:
        blog("CONN_UPDATE status=%d", event->conn_update.status);
        break;

    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        struct ble_sm_io pkey = { .action = event->passkey.params.action };
        blog("PASSKEY action=%d", pkey.action);
        if (pkey.action == BLE_SM_IOACT_NUMCMP)
            pkey.numcmp_accept = 1;
        ble_sm_inject_io(event->passkey.conn_handle, &pkey);
        break;
    }

    case BLE_GAP_EVENT_ENC_CHANGE:
        blog("ENC_CHANGE status=%d", event->enc_change.status);
        if (event->enc_change.status == 0) {
            start_discovery(s_ctx.conn_handle);
        } else {
            blog("ENC_CHANGE failed - keyboard rejected our LTK");
        }
        break;

    case BLE_GAP_EVENT_PARING_COMPLETE:
        blog("PAIRING_COMPLETE status=%d", event->pairing_complete.status);
        break;

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        struct ble_gap_conn_desc desc;
        ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
        ble_store_util_delete_peer(&desc.peer_id_addr);
        blog("REPEAT_PAIRING: deleted stale bond, retrying");
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    case BLE_GAP_EVENT_DISCONNECT:
        esp_timer_stop(s_poll_timer);
        s_ctx.connected   = false;
        s_ctx.conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_disc_gen++;
        blog("DISCONNECT reason=%d", event->disconnect.reason);
        if (s_ctx.conn_cb) s_ctx.conn_cb(false, s_ctx.conn_ctx);
        // Auto-reconnect: defer via timer so the stack can finish cleanup
        if (s_ctx.peer_known) {
            s_reconnect_attempts = 0;
            blog("scheduling reconnect in 1.5s");
            schedule_reconnect(RECONNECT_DELAY_US);
        }
        break;

    case BLE_GAP_EVENT_DISC:
        if (s_ctx.scan_cb) {
            ble_scan_result_t r = {0};
            r.rssi      = event->disc.rssi;
            r.addr_type = event->disc.addr.type;
            memcpy(r.addr, event->disc.addr.val, 6);
            if (event->disc.length_data > 0) {
                struct ble_hs_adv_fields fields;
                ble_hs_adv_parse_fields(&fields, event->disc.data,
                                        event->disc.length_data);
                if (fields.name) {
                    size_t n = fields.name_len < sizeof(r.name) - 1
                             ? fields.name_len : sizeof(r.name) - 1;
                    memcpy(r.name, fields.name, n);
                }
            }
            s_ctx.scan_cb(&r, s_ctx.scan_ctx);
        }
        break;

    case BLE_GAP_EVENT_NOTIFY_RX: {
        struct os_mbuf *om = event->notify_rx.om;
        uint8_t buf[10] = {0};
        uint16_t len = OS_MBUF_PKTLEN(om);
        if (len > sizeof(buf)) len = sizeof(buf);
        os_mbuf_copydata(om, 0, len, buf);
        uint16_t attr_h = event->notify_rx.attr_handle;
        blog("NOTIFY h=%u len=%d %02x %02x %02x %02x %02x %02x %02x %02x %02x",
             attr_h, len,
             buf[0],buf[1],buf[2],buf[3],buf[4],buf[5],buf[6],buf[7],buf[8]);

        if (!s_ctx.report_cb || len < 2) break;

        // Only process notifications from Input Report characteristics we subscribed to
        bool is_subscribed = false;
        for (int i = 0; i < s_gatt.subscribed_count; i++) {
            if (s_gatt.subscribed_handles[i] == attr_h) { is_subscribed = true; break; }
        }
        if (!is_subscribed) break;

        bluepass_hid_report_t report = {0};

        // Heuristic: Consumer Control reports are short (2-3 bytes).
        // Keyboard reports are always 7–9 bytes.
        // len==2: [cc_lo, cc_hi]
        // len==3: [report_id, cc_lo, cc_hi]
        if (len <= 3) {
            uint16_t cc;
            if (len == 3) {
                cc = (uint16_t)buf[1] | ((uint16_t)buf[2] << 8);
            } else {
                cc = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
            }
            report.consumer_code = cc;
            blog("CONSUMER_NOTIFY h=%u cc=0x%04x", attr_h, cc);
            s_ctx.report_cb(&report, s_ctx.report_ctx);
            break;
        }

        // Keyboard report
        uint8_t *kc;
        uint8_t  kc_len;
        if (len >= 9) {
            report.keyboard.modifier = buf[1];
            kc     = buf + 3;
            kc_len = (uint8_t)(len - 3);
        } else if (len == 8) {
            report.keyboard.modifier = buf[0];
            kc     = buf + 2;
            kc_len = 6;
        } else {
            report.keyboard.modifier = buf[0];
            kc     = buf + 1;
            kc_len = (uint8_t)(len - 1);
        }
        if (kc_len > 6) kc_len = 6;
        memcpy(report.keyboard.keycode, kc, kc_len);
        s_ctx.report_cb(&report, s_ctx.report_ctx);
        break;
    }

    default:
        blog("GAP evt=%d", event->type);
        break;
    }
    return 0;
}

// ── NimBLE host task ──────────────────────────────────────────────────────────

static void ble_host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "BLE host reset, reason=%d", reason);
    s_ctx.connected = false;
}

static void on_sync(void)
{
    ESP_LOGI(TAG, "BLE host synced");
    uint8_t addr_type;
    ble_hs_id_infer_auto(0, &addr_type);

    // If a peer was saved in NVS from a previous session, restore it and reconnect.
    // Skipped when report_cb is NULL (peripheral-only mode, no central role).
    if (s_ctx.report_cb && !s_ctx.peer_known) {
        ble_peer_t p;
        if (storage_get_ble_peer(&p) == ESP_OK) {
            memcpy(s_ctx.peer_addr, p.addr, 6);
            s_ctx.peer_addr_type = p.addr_type;
            strncpy(s_ctx.peer_name, p.name, sizeof(s_ctx.peer_name) - 1);
            s_ctx.peer_known     = true;
            s_reconnect_attempts = 0;
            blog("restored peer from NVS, scheduling reconnect");
            schedule_reconnect(RECONNECT_DELAY_US);
        }
    }

    for (int i = 0; i < s_sync_hook_count; i++)
        s_sync_hooks[i]();
}

// ── Public API ────────────────────────────────────────────────────────────────

esp_err_t ble_hid_host_init(ble_hid_report_cb_t report_cb, void *report_ctx)
{
    s_ctx.report_cb   = report_cb;
    s_ctx.report_ctx  = report_ctx;
    s_ctx.conn_handle = BLE_HS_CONN_HANDLE_NONE;

    esp_timer_create_args_t ta = {
        .callback = reconnect_timer_cb,
        .name     = "ble_reconnect",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&ta, &s_reconnect_timer),
                        TAG, "reconnect timer create failed");

    esp_timer_create_args_t pa = {
        .callback = poll_timer_cb,
        .name     = "ble_poll",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&pa, &s_poll_timer),
                        TAG, "poll timer create failed");

    ESP_RETURN_ON_ERROR(nimble_port_init(), TAG, "nimble_port_init failed");

    ble_hs_cfg.reset_cb        = on_reset;
    ble_hs_cfg.sync_cb         = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    // Required: sets store_read_cb / store_write_cb to NVS-backed implementations.
    // Without this every ble_store_read returns BLE_HS_ENOTSUP (8) and SM fails.
    ble_store_config_init();

    // "Just Works" bonding — no passkey, no MITM, LE Secure Connections preferred
    ble_hs_cfg.sm_io_cap         = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding        = 1;
    ble_hs_cfg.sm_mitm           = 0;
    ble_hs_cfg.sm_sc             = 1;
    ble_hs_cfg.sm_our_key_dist   = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    nimble_port_freertos_init(ble_host_task);
    return ESP_OK;
}

esp_err_t ble_hid_host_start_scan(uint32_t duration_ms,
                                   ble_scan_result_cb_t result_cb, void *ctx)
{
    s_ctx.scan_cb  = result_cb;
    s_ctx.scan_ctx = ctx;

    struct ble_gap_disc_params params = {
        .passive           = 0,
        .filter_duplicates = 1,
        .itvl              = BLE_GAP_SCAN_FAST_INTERVAL_MAX,
        .window            = BLE_GAP_SCAN_FAST_WINDOW,
    };
    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, duration_ms, &params,
                          gap_event_handler, NULL);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t ble_hid_host_stop_scan(void)
{
    ble_gap_disc_cancel();
    return ESP_OK;
}

esp_err_t ble_hid_host_connect(const uint8_t addr[6], uint8_t addr_type)
{
    memcpy(s_ctx.peer_addr, addr, 6);
    s_ctx.peer_addr_type = addr_type;
    s_ctx.peer_known     = true;

    ble_addr_t peer = { .type = addr_type };
    memcpy(peer.val, addr, 6);
    int rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &peer, 30000, NULL,
                             gap_event_handler, NULL);
    blog("gap_connect addr_type=%d rc=%d", addr_type, rc);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t ble_hid_host_disconnect(void)
{
    if (!s_ctx.connected) return ESP_OK;
    // Cancel auto-reconnect and forget this peer (explicit user action)
    s_ctx.peer_known = false;
    esp_timer_stop(s_reconnect_timer);
    storage_clear_ble_peer();
    int rc = ble_gap_terminate(s_ctx.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

bool ble_hid_host_is_connected(void) { return s_ctx.connected; }

void ble_hid_host_get_peer_name(char *buf, size_t len)
{
    strncpy(buf, s_ctx.peer_name, len - 1);
    buf[len - 1] = '\0';
}

void ble_hid_host_set_peer_name(const char *name)
{
    strncpy(s_ctx.peer_name, name, sizeof(s_ctx.peer_name) - 1);
    s_ctx.peer_name[sizeof(s_ctx.peer_name) - 1] = '\0';
}

void ble_hid_host_get_peer_addr(char *buf, size_t len)
{
    if (!s_ctx.connected || s_ctx.conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        buf[0] = '\0';
        return;
    }
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(s_ctx.conn_handle, &desc) != 0) {
        buf[0] = '\0';
        return;
    }
    const uint8_t *a = desc.peer_id_addr.val;
    snprintf(buf, len, "%02x:%02x:%02x:%02x:%02x:%02x",
             a[0], a[1], a[2], a[3], a[4], a[5]);
}

int8_t ble_hid_host_get_rssi(void)
{
    if (!s_ctx.connected || s_ctx.conn_handle == BLE_HS_CONN_HANDLE_NONE)
        return 0;
    int8_t rssi = 0;
    ble_gap_conn_rssi(s_ctx.conn_handle, &rssi);
    return rssi;
}

void ble_hid_host_set_connection_cb(ble_connection_cb_t cb, void *ctx)
{
    s_ctx.conn_cb  = cb;
    s_ctx.conn_ctx = ctx;
}

void ble_hid_host_get_log(char *buf, size_t len)
{
    strncpy(buf, s_ble_log, len - 1);
    buf[len - 1] = '\0';
}

void ble_hid_host_add_sync_hook(ble_sync_hook_t fn)
{
    if (s_sync_hook_count < SYNC_HOOKS_MAX)
        s_sync_hooks[s_sync_hook_count++] = fn;
}
