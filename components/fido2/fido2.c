#include "fido2.h"
#include "storage.h"
#include "wifi_manager.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "mbedtls/sha256.h"
#include "mbedtls/ecp.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/ecdh.h"
#include "mbedtls/gcm.h"
#include "mbedtls/aes.h"
#include "mbedtls/md.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#define TAG "fido2"

// ── CTAPHID constants ─────────────────────────────────────────────────────────

#define CTAPHID_PING       0x01
#define CTAPHID_CBOR       0x10
#define CTAPHID_INIT       0x06
#define CTAPHID_WINK       0x08
#define CTAPHID_KEEPALIVE  0x3B
#define CTAPHID_ERROR      0x3F
#define CTAPHID_CANCEL     0x11

#define CTAPHID_STATUS_PROCESSING 0x01
#define CTAPHID_STATUS_UPNEEDED   0x02

#define CTAPHID_CID_BROADCAST 0xFFFFFFFFU

#define CTAP2_CMD_MAKE_CREDENTIAL    0x01
#define CTAP2_CMD_GET_ASSERTION      0x02
#define CTAP2_CMD_GET_INFO           0x04
#define CTAP2_CMD_CLIENT_PIN         0x06
#define CTAP2_CMD_RESET              0x07
#define CTAP2_CMD_GET_NEXT_ASSERTION 0x08

#define CTAP1_ERR_INVALID_COMMAND    0x01
#define CTAP2_OK                     0x00
#define CTAP2_ERR_INVALID_CBOR       0x12
#define CTAP2_ERR_MISSING_PARAMETER  0x14
#define CTAP2_ERR_CRED_EXCLUDED      0x19
#define CTAP2_ERR_UNSUPPORTED_ALG    0x26
#define CTAP2_ERR_OPERATION_DENIED   0x27
#define CTAP2_ERR_KEY_STORE_FULL     0x28
#define CTAP2_ERR_NO_CREDENTIALS     0x2E
#define CTAP2_ERR_ACTION_TIMEOUT     0x3A
#define CTAP2_ERR_UP_REQUIRED        0x3B
#define CTAP2_ERR_PIN_INVALID        0x31
#define CTAP2_ERR_PIN_BLOCKED        0x32
#define CTAP2_ERR_PIN_AUTH_INVALID   0x33
#define CTAP2_ERR_PIN_AUTH_BLOCKED   0x34
#define CTAP2_ERR_PIN_NOT_SET        0x35
#define CTAP2_ERR_PIN_REQUIRED           0x36
#define CTAP2_ERR_PIN_POLICY_VIOLATION   0x37
#define CTAP2_ERR_NOT_ALLOWED            0x30

#define CTAP2_PIN_PROTO          1
#define CTAP2_SUBCMD_GET_RETRIES     0x01
#define CTAP2_SUBCMD_GET_KEY_AGREE   0x02
#define CTAP2_SUBCMD_SET_PIN         0x03
#define CTAP2_SUBCMD_CHANGE_PIN      0x04
#define CTAP2_SUBCMD_GET_PIN_TOKEN   0x05

#define CRED_ID_LEN   108   // nonce[12] + ciphertext[80] + tag[16]
#define CRED_PLAIN_LEN 80
#define PIN_RETRIES_MAX 8
#define UP_TIMEOUT_MS   30000

static const uint8_t BLUEPASS_AAGUID[16] = {
    0xB1,0x6E,0x1F,0x39,0x4C,0x3B,0x4F,0x2A,
    0x8B,0x1D,0x7E,0x6C,0x09,0x5A,0x3D,0x2E
};

// ── Credential plain structure (80 bytes, AES-GCM encrypted) ─────────────────

typedef struct __attribute__((packed)) {
    uint8_t  priv_key[32];    // ECDSA P-256 private key scalar (big-endian)
    uint8_t  rp_id_hash[32];  // SHA-256 of rpId string
    uint8_t  flags;           // bit0=is_resident, bit1=uv_at_creation
    uint8_t  _pad[15];        // zeroed
} cred_plain_t;               // 80 bytes

// ── Static state ──────────────────────────────────────────────────────────────

static QueueHandle_t   s_rx_queue;        // 64-byte HID packets from host
static SemaphoreHandle_t s_up_sem;        // UP confirmation signal
static volatile bool   s_up_pending;
static volatile bool   s_enabled;

// Session ECDH key for ClientPIN (regenerated per getKeyAgreement call)
static mbedtls_ecp_group s_pin_grp;
static mbedtls_mpi        s_pin_d;
static mbedtls_ecp_point  s_pin_Q;
static bool               s_pin_key_valid;

// Active PIN token (16 bytes), valid until PIN changes or authenticator reset
static uint8_t  s_pin_token[16];
static bool     s_pin_token_valid;

// Send callback registered by usb_hid_device
static void (*s_tx_cb)(const uint8_t *buf);

// ── RTC crash log (survives software reset; shows last steps before a crash) ─

#define CRASH_MAGIC 0xC0FFEE42u
RTC_NOINIT_ATTR static char     s_crash_buf[512];
RTC_NOINIT_ATTR static uint16_t s_crash_len;
RTC_NOINIT_ATTR static uint32_t s_crash_magic;

static void crash_mark(const char *s) {
    if (s_crash_magic != CRASH_MAGIC) return;
    uint16_t n = (uint16_t)strlen(s);
    if ((uint32_t)s_crash_len + n < sizeof(s_crash_buf) - 1u) {
        memcpy(s_crash_buf + s_crash_len, s, n);
        s_crash_len += n;
        s_crash_buf[s_crash_len] = '\0';
    }
}

// ── Diagnostic log (read via GET /api/passkey/diag) ──────────────────────────
// diag_append is called from both fido2_task and the TinyUSB task (via
// fido2_on_hid_rx), so all buffer access must be guarded by a spinlock.

#define DIAG_SIZE 1024
static char          s_diag_buf[DIAG_SIZE];
static size_t        s_diag_len = 0;
static uint32_t      s_diag_seq = 0;
static portMUX_TYPE  s_diag_mux = portMUX_INITIALIZER_UNLOCKED;

static void diag_append(const char *fmt, ...) {
    char tmp[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    taskENTER_CRITICAL(&s_diag_mux);
    if (s_diag_len + (size_t)n >= DIAG_SIZE) {
        // Drop oldest line to make room
        char *nl = memchr(s_diag_buf, '\n', s_diag_len);
        if (nl) {
            size_t skip = (size_t)(nl - s_diag_buf) + 1;
            memmove(s_diag_buf, s_diag_buf + skip, s_diag_len - skip);
            s_diag_len -= skip;
        } else {
            s_diag_len = 0; // no newline found, clear entirely
        }
    }
    memcpy(s_diag_buf + s_diag_len, tmp, (size_t)n);
    s_diag_len += (size_t)n;
    s_diag_buf[s_diag_len] = '\0';
    taskEXIT_CRITICAL(&s_diag_mux);
}

static const char *ctap_status_name(uint8_t s) {
    switch (s) {
    case 0x00: return "OK";
    case 0x01: return "invalidCmd";
    case 0x12: return "invalidCBOR";
    case 0x14: return "missingParam";
    case 0x27: return "operationDenied";
    case 0x30: return "notAllowed";
    case 0x31: return "pinInvalid";
    case 0x32: return "pinBlocked";
    case 0x33: return "pinAuthInvalid";
    case 0x34: return "pinAuthBlocked";
    case 0x35: return "pinNotSet";
    case 0x36: return "pinRequired";
    case 0x37: return "pinPolicyViolation";
    case 0x3A: return "actionTimeout";
    case 0x3B: return "upRequired";
    default:   return "err";
    }
}

// Called from ctap2_respond — logs the response for the current command
static const char *s_diag_cmd = "";  // set before each cmd_* call

void fido2_diag_get(char *buf, size_t maxlen) {
    taskENTER_CRITICAL(&s_diag_mux);
    size_t n = s_diag_len < maxlen - 1 ? s_diag_len : maxlen - 1;
    memcpy(buf, s_diag_buf, n);
    buf[n] = '\0';
    taskEXIT_CRITICAL(&s_diag_mux);
}

void fido2_diag_clear(void) {
    taskENTER_CRITICAL(&s_diag_mux);
    s_diag_len = 0;
    s_diag_buf[0] = '\0';
    s_diag_seq = 0;
    taskEXIT_CRITICAL(&s_diag_mux);
}

// ── CBOR encoder ─────────────────────────────────────────────────────────────

typedef struct { uint8_t *buf; size_t pos; size_t cap; bool err; } cbor_enc_t;

static void ce_init(cbor_enc_t *e, uint8_t *buf, size_t cap)
    { e->buf = buf; e->pos = 0; e->cap = cap; e->err = false; }

static void ce_byte(cbor_enc_t *e, uint8_t b)
    { if (e->pos < e->cap) e->buf[e->pos++] = b; else e->err = true; }

static void ce_bytes(cbor_enc_t *e, const void *d, size_t n) {
    if (e->pos + n <= e->cap) { memcpy(e->buf + e->pos, d, n); e->pos += n; }
    else e->err = true;
}

static void ce_tv(cbor_enc_t *e, uint8_t mt, uint64_t v) {
    mt <<= 5;
    if      (v <= 23)         { ce_byte(e, mt|(uint8_t)v); }
    else if (v <= 0xFF)       { ce_byte(e, mt|24); ce_byte(e,(uint8_t)v); }
    else if (v <= 0xFFFF)     { ce_byte(e, mt|25); ce_byte(e,(uint8_t)(v>>8)); ce_byte(e,(uint8_t)v); }
    else if (v <= 0xFFFFFFFF) { ce_byte(e, mt|26);
        for(int i=3;i>=0;i--) ce_byte(e,(uint8_t)(v>>(i*8))); }
    else                      { ce_byte(e, mt|27);
        for(int i=7;i>=0;i--) ce_byte(e,(uint8_t)(v>>(i*8))); }
}

static void ce_uint(cbor_enc_t *e, uint64_t v)       { ce_tv(e, 0, v); }
static void ce_nint(cbor_enc_t *e, int64_t v)         { ce_tv(e, 1, (uint64_t)(-1-v)); }
static void ce_bstr(cbor_enc_t *e, const void *d, size_t n) { ce_tv(e,2,n); ce_bytes(e,d,n); }
static void ce_tstr(cbor_enc_t *e, const char *s)    { size_t n=strlen(s); ce_tv(e,3,n); ce_bytes(e,s,n); }
static void ce_arr(cbor_enc_t *e, size_t n)           { ce_tv(e, 4, n); }
static void ce_map(cbor_enc_t *e, size_t n)           { ce_tv(e, 5, n); }
static void ce_bool(cbor_enc_t *e, bool b)            { ce_byte(e, b ? 0xF5 : 0xF4); }

// ── CBOR decoder ─────────────────────────────────────────────────────────────

typedef struct { const uint8_t *buf; size_t pos; size_t len; bool err; } cbor_dec_t;

static void cd_init(cbor_dec_t *d, const uint8_t *buf, size_t len)
    { d->buf=buf; d->pos=0; d->len=len; d->err=false; }

static uint8_t cd_byte(cbor_dec_t *d)
    { if (d->pos < d->len) return d->buf[d->pos++]; d->err=true; return 0; }

static uint64_t cd_arg(cbor_dec_t *d, uint8_t ai) {
    if (ai<=23) return ai;
    if (ai==24) return cd_byte(d);
    if (ai==25) { uint64_t v=(uint64_t)cd_byte(d)<<8; return v|cd_byte(d); }
    if (ai==26) { uint64_t v=0; for(int i=0;i<4;i++) v=(v<<8)|cd_byte(d); return v; }
    if (ai==27) { uint64_t v=0; for(int i=0;i<8;i++) v=(v<<8)|cd_byte(d); return v; }
    d->err=true; return 0;
}

typedef struct { uint8_t type; uint64_t val; } cd_item_t;

static cd_item_t cd_item(cbor_dec_t *d) {
    uint8_t b = cd_byte(d); return (cd_item_t){ b>>5, cd_arg(d, b&0x1F) };
}

static void cd_skip(cbor_dec_t *d) {
    cd_item_t it = cd_item(d);
    size_t n = (size_t)it.val;
    switch(it.type) {
    case 2: case 3: d->pos += n; if (d->pos > d->len) d->err=true; break;
    case 4: for(size_t i=0;i<n&&!d->err;i++) cd_skip(d); break;
    case 5: for(size_t i=0;i<n*2&&!d->err;i++) cd_skip(d); break;
    default: break;
    }
}

// Find value of uint key in a CBOR map; returns false if not found
static bool cd_map_uint(const uint8_t *buf, size_t len, uint64_t key, cbor_dec_t *out) {
    cbor_dec_t d; cd_init(&d, buf, len);
    cd_item_t map = cd_item(&d);
    if (d.err || map.type != 5) return false;
    for (uint64_t i = 0; i < map.val && !d.err; i++) {
        cd_item_t k = cd_item(&d);
        if (d.err) break;
        if (k.type == 0 && k.val == key) {
            cd_init(out, d.buf + d.pos, d.len - d.pos);
            return true;
        }
        cd_skip(&d);
    }
    return false;
}

// Find value of text key in a CBOR map
static bool cd_map_tstr(const uint8_t *buf, size_t len, const char *key, cbor_dec_t *out) {
    cbor_dec_t d; cd_init(&d, buf, len);
    cd_item_t map = cd_item(&d);
    if (d.err || map.type != 5) return false;
    size_t kl = strlen(key);
    for (uint64_t i = 0; i < map.val && !d.err; i++) {
        cd_item_t k = cd_item(&d);
        if (d.err) break;
        if (k.type == 3 && k.val == kl &&
            d.pos + kl <= d.len &&
            memcmp(d.buf + d.pos, key, kl) == 0) {
            d.pos += kl;
            cd_init(out, d.buf + d.pos, d.len - d.pos);
            return true;
        }
        if (k.type == 3) d.pos += (size_t)k.val;
        cd_skip(&d); // skip value
    }
    return false;
}

static const uint8_t *cd_bstr(cbor_dec_t *d, size_t *len_out) {
    cd_item_t it = cd_item(d);
    if (d->err || it.type != 2) { d->err=true; return NULL; }
    *len_out = (size_t)it.val;
    const uint8_t *p = d->buf + d->pos;
    d->pos += *len_out;
    if (d->pos > d->len) { d->err=true; return NULL; }
    return p;
}

static const char *cd_tstr(cbor_dec_t *d, size_t *len_out) {
    cd_item_t it = cd_item(d);
    if (d->err || it.type != 3) { d->err=true; return NULL; }
    *len_out = (size_t)it.val;
    const char *p = (const char *)(d->buf + d->pos);
    d->pos += *len_out;
    if (d->pos > d->len) { d->err=true; return NULL; }
    return p;
}

// ── Crypto helpers ────────────────────────────────────────────────────────────

static int rng_func(void *ctx, uint8_t *buf, size_t len)
    { (void)ctx; esp_fill_random(buf, len); return 0; }

static void sha256(const uint8_t *in, size_t ilen, uint8_t out[32])
    { mbedtls_sha256(in, ilen, out, 0); }

static void hmac_sha256(const uint8_t *key, size_t klen,
                        const uint8_t *data, size_t dlen, uint8_t out[32]) {
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
    mbedtls_md_hmac_starts(&ctx, key, klen);
    mbedtls_md_hmac_update(&ctx, data, dlen);
    mbedtls_md_hmac_finish(&ctx, out);
    mbedtls_md_free(&ctx);
}

// AES-256-CBC decrypt (PIN protocol 1; IV always 0)
static bool aes_cbc_decrypt(const uint8_t key[32], const uint8_t *in, size_t len, uint8_t *out) {
    uint8_t iv[16] = {0};
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    bool ok = (mbedtls_aes_setkey_dec(&ctx, key, 256) == 0 &&
               mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_DECRYPT, len, iv, in, out) == 0);
    mbedtls_aes_free(&ctx);
    return ok;
}

// AES-256-CBC encrypt (PIN protocol 1; IV always 0)
static bool aes_cbc_encrypt(const uint8_t key[32], const uint8_t *in, size_t len, uint8_t *out) {
    uint8_t iv[16] = {0};
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    bool ok = (mbedtls_aes_setkey_enc(&ctx, key, 256) == 0 &&
               mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_ENCRYPT, len, iv, in, out) == 0);
    mbedtls_aes_free(&ctx);
    return ok;
}

// Encrypt credential plain text → credential ID (CRED_ID_LEN bytes)
static bool cred_encrypt(const uint8_t master_key[32],
                         const cred_plain_t *plain, uint8_t cred_id[CRED_ID_LEN]) {
    uint8_t nonce[12];
    esp_fill_random(nonce, 12);
    memcpy(cred_id, nonce, 12);

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    int ret = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, master_key, 256);
    if (ret == 0) {
        ret = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT,
            CRED_PLAIN_LEN, nonce, 12, NULL, 0,
            (const uint8_t *)plain, cred_id + 12, 16, cred_id + 12 + CRED_PLAIN_LEN);
    }
    mbedtls_gcm_free(&gcm);
    return ret == 0;
}

// Decrypt credential ID → credential plain text; returns false on auth failure
static bool cred_decrypt(const uint8_t master_key[32],
                         const uint8_t cred_id[CRED_ID_LEN], cred_plain_t *plain) {
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    int ret = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, master_key, 256);
    if (ret == 0) {
        ret = mbedtls_gcm_auth_decrypt(&gcm, CRED_PLAIN_LEN,
            cred_id, 12, NULL, 0,
            cred_id + 12 + CRED_PLAIN_LEN, 16,
            cred_id + 12, (uint8_t *)plain);
    }
    mbedtls_gcm_free(&gcm);
    return ret == 0;
}

// Generate ECDSA P-256 keypair
static bool gen_keypair(uint8_t priv[32], uint8_t pub_x[32], uint8_t pub_y[32]) {
    mbedtls_ecp_group grp;
    mbedtls_mpi d;
    mbedtls_ecp_point Q;
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_ecp_point_init(&Q);

    bool ok = false;
    uint8_t pt[65]; size_t pt_len;
    if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
        mbedtls_ecp_gen_keypair(&grp, &d, &Q, rng_func, NULL) == 0 &&
        mbedtls_mpi_write_binary(&d, priv, 32) == 0 &&
        mbedtls_ecp_point_write_binary(&grp, &Q, MBEDTLS_ECP_PF_UNCOMPRESSED,
                                       &pt_len, pt, sizeof(pt)) == 0 &&
        pt_len == 65) {
        memcpy(pub_x, pt + 1,  32);
        memcpy(pub_y, pt + 33, 32);
        ok = true;
    }
    mbedtls_ecp_group_free(&grp);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_point_free(&Q);
    return ok;
}

// ECDSA P-256 sign; returns DER-encoded sig and its length (≤72)
static bool ecdsa_sign(const uint8_t priv[32], const uint8_t hash[32],
                       uint8_t sig[72], size_t *sig_len) {
    mbedtls_ecp_keypair kp;
    mbedtls_ecdsa_context ctx;
    mbedtls_ecp_keypair_init(&kp);
    mbedtls_ecdsa_init(&ctx);
    bool ok = false;
    if (mbedtls_ecp_read_key(MBEDTLS_ECP_DP_SECP256R1, &kp, priv, 32) == 0 &&
        mbedtls_ecdsa_from_keypair(&ctx, &kp) == 0 &&
        mbedtls_ecdsa_write_signature(&ctx, MBEDTLS_MD_SHA256,
                                      hash, 32, sig, 72, sig_len,
                                      rng_func, NULL) == 0) {
        ok = true;
    }
    mbedtls_ecp_keypair_free(&kp);
    mbedtls_ecdsa_free(&ctx);
    return ok;
}

// Write COSE P-256 public key for ECDSA (alg: ES256 = -7); used in credential responses
static void ce_cose_key(cbor_enc_t *e, const uint8_t x[32], const uint8_t y[32]) {
    ce_map(e, 5);
    ce_uint(e, 1);  ce_uint(e, 2);   // kty: EC2
    ce_uint(e, 3);  ce_nint(e, -7);  // alg: ES256 (signing)
    ce_nint(e, -1); ce_uint(e, 1);   // crv: P-256
    ce_nint(e, -2); ce_bstr(e, x, 32); // x
    ce_nint(e, -3); ce_bstr(e, y, 32); // y
}

// Write COSE P-256 key for PIN key agreement (alg: ECDH-ES+HKDF-256 = -25); CTAP2 spec §6.5.4
static void ce_cose_ecdh_key(cbor_enc_t *e, const uint8_t x[32], const uint8_t y[32]) {
    ce_map(e, 5);
    ce_uint(e, 1);  ce_uint(e, 2);    // kty: EC2
    ce_uint(e, 3);  ce_nint(e, -25);  // alg: ECDH-ES+HKDF-256
    ce_nint(e, -1); ce_uint(e, 1);    // crv: P-256
    ce_nint(e, -2); ce_bstr(e, x, 32); // x
    ce_nint(e, -3); ce_bstr(e, y, 32); // y
}

// ── CTAPHID packet I/O ────────────────────────────────────────────────────────

static void ctaphid_send_packet(uint32_t cid, uint8_t cmd,
                                const uint8_t *data, uint16_t len) {
    if (!s_tx_cb) return;
    uint8_t pkt[64] = {0};
    pkt[0] = (cid >> 24) & 0xFF;
    pkt[1] = (cid >> 16) & 0xFF;
    pkt[2] = (cid >>  8) & 0xFF;
    pkt[3] =  cid        & 0xFF;
    pkt[4] = 0x80 | cmd;
    pkt[5] = (len >> 8) & 0xFF;
    pkt[6] =  len       & 0xFF;
    uint16_t first = len < 57 ? len : 57;
    memcpy(pkt + 7, data, first);
    s_tx_cb(pkt);

    uint16_t sent = first;
    uint8_t  seq  = 0;
    while (sent < len) {
        memset(pkt, 0, 64);
        pkt[0] = (cid >> 24) & 0xFF;
        pkt[1] = (cid >> 16) & 0xFF;
        pkt[2] = (cid >>  8) & 0xFF;
        pkt[3] =  cid        & 0xFF;
        pkt[4] = seq++;
        uint16_t chunk = (len - sent) < 59 ? (len - sent) : 59;
        memcpy(pkt + 5, data + sent, chunk);
        s_tx_cb(pkt);
        sent += chunk;
    }
}

static void ctaphid_error(uint32_t cid, uint8_t code) {
    diag_append("CTAPHID_ERR cid=%08lX code=0x%02X\n", (unsigned long)cid, code);
    ctaphid_send_packet(cid, CTAPHID_ERROR, &code, 1);
}

static void ctaphid_keepalive(uint32_t cid, uint8_t status) {
    ctaphid_send_packet(cid, CTAPHID_KEEPALIVE, &status, 1);
}

// Send a CTAP2 response (prepend status byte to cbor_body)
static void ctap2_respond(uint32_t cid, uint8_t status, const uint8_t *body, size_t body_len) {
    diag_append("#%lu %s → %s(0x%02X)\n",
                (unsigned long)++s_diag_seq, s_diag_cmd,
                ctap_status_name(status), status);
    ESP_LOGI(TAG, "%s → 0x%02X %s", s_diag_cmd, status, ctap_status_name(status));

    uint8_t *buf = malloc(1 + body_len);
    if (!buf) { ctaphid_error(cid, 0x27); return; }
    buf[0] = status;
    if (body && body_len) memcpy(buf + 1, body, body_len);
    ctaphid_send_packet(cid, CTAPHID_CBOR, buf, (uint16_t)(1 + body_len));
    free(buf);
}

// ── CTAP2 command: getInfo ────────────────────────────────────────────────────

static void cmd_get_info(uint32_t cid) {
    bool pin_set = storage_fido2_has_pin();

    uint8_t buf[256];
    cbor_enc_t e;
    ce_init(&e, buf, sizeof(buf));

    ce_map(&e, 5);

    ce_uint(&e, 0x01); ce_arr(&e, 1); ce_tstr(&e, "FIDO_2_0"); // versions

    ce_uint(&e, 0x03); ce_bstr(&e, BLUEPASS_AAGUID, 16);        // aaguid

    ce_uint(&e, 0x04);                                           // options
    ce_map(&e, 5);
    ce_tstr(&e, "rk");        ce_bool(&e, true);
    ce_tstr(&e, "up");        ce_bool(&e, true);
    ce_tstr(&e, "uv");        ce_bool(&e, false);    // no biometric UV; PIN uses clientPin
    ce_tstr(&e, "plat");      ce_bool(&e, false);
    ce_tstr(&e, "clientPin"); ce_bool(&e, pin_set);

    ce_uint(&e, 0x05); ce_uint(&e, 2048);                        // maxMsgSize

    ce_uint(&e, 0x06); ce_arr(&e, 1); ce_uint(&e, 1);           // pinUvAuthProtocols: [1]

    ctap2_respond(cid, CTAP2_OK, buf, e.pos);
}

// ── Build authData ────────────────────────────────────────────────────────────

// flags: bit0=UP, bit2=UV, bit6=AT (attested credential data present)
#define AUTHDATA_FLAG_UP  0x01
#define AUTHDATA_FLAG_UV  0x04
#define AUTHDATA_FLAG_AT  0x40

// Build authData for makeCredential: rpIdHash + flags + signCount + AAGUID + credIdLen + credId + coseKey
static size_t build_auth_data_make(uint8_t *out, size_t cap,
                                   const uint8_t rp_id_hash[32],
                                   uint8_t flags, uint32_t sign_count,
                                   const uint8_t cred_id[CRED_ID_LEN],
                                   const uint8_t pub_x[32], const uint8_t pub_y[32]) {
    cbor_enc_t e;
    // Use raw writing for fixed fields, then CBOR for COSE key
    size_t pos = 0;
    if (pos + 32 > cap) return 0;
    memcpy(out + pos, rp_id_hash, 32); pos += 32;
    out[pos++] = flags | AUTHDATA_FLAG_AT;
    out[pos++] = (sign_count >> 24) & 0xFF;
    out[pos++] = (sign_count >> 16) & 0xFF;
    out[pos++] = (sign_count >>  8) & 0xFF;
    out[pos++] =  sign_count        & 0xFF;
    // Attested credential data
    memcpy(out + pos, BLUEPASS_AAGUID, 16); pos += 16;
    out[pos++] = 0;          // credIdLen high byte
    out[pos++] = CRED_ID_LEN; // credIdLen low byte
    memcpy(out + pos, cred_id, CRED_ID_LEN); pos += CRED_ID_LEN;
    // COSE public key
    ce_init(&e, out + pos, cap - pos);
    ce_cose_key(&e, pub_x, pub_y);
    pos += e.pos;
    return pos;
}

// Build authData for getAssertion: rpIdHash + flags + signCount
static size_t build_auth_data_get(uint8_t *out, const uint8_t rp_id_hash[32],
                                  uint8_t flags, uint32_t sign_count) {
    memcpy(out, rp_id_hash, 32);
    out[32] = flags;
    out[33] = (sign_count >> 24) & 0xFF;
    out[34] = (sign_count >> 16) & 0xFF;
    out[35] = (sign_count >>  8) & 0xFF;
    out[36] =  sign_count        & 0xFF;
    return 37;
}

// ── Wait for user presence ────────────────────────────────────────────────────

static bool wait_for_up(uint32_t cid) {
    s_up_pending = true;
    wifi_manager_set_fido2_pending(true);
    bool confirmed = false;
    int64_t deadline = (int64_t)(xTaskGetTickCount()) + pdMS_TO_TICKS(UP_TIMEOUT_MS);
    while (!confirmed) {
        ctaphid_keepalive(cid, CTAPHID_STATUS_UPNEEDED);
        int64_t remaining = deadline - (int64_t)xTaskGetTickCount();
        if (remaining <= 0) break;
        TickType_t wait = remaining > pdMS_TO_TICKS(100) ? pdMS_TO_TICKS(100) : (TickType_t)remaining;
        if (xSemaphoreTake(s_up_sem, wait) == pdTRUE) {
            confirmed = true;
        }
    }
    s_up_pending = false;
    wifi_manager_set_fido2_pending(false);
    return confirmed;
}

// ── CTAP2 command: makeCredential ─────────────────────────────────────────────

static void cmd_make_credential(uint32_t cid, const uint8_t *req, size_t req_len) {
    fido2_config_t cfg = {0};
    storage_get_fido2_config(&cfg);

    // 1. Parse required fields
    cbor_dec_t v;

    // clientDataHash (key 0x01)
    if (!cd_map_uint(req, req_len, 0x01, &v)) {
        ctap2_respond(cid, CTAP2_ERR_MISSING_PARAMETER, NULL, 0); return;
    }
    size_t cdh_len;
    const uint8_t *cdh = cd_bstr(&v, &cdh_len);
    if (!cdh || cdh_len != 32) {
        ctap2_respond(cid, CTAP2_ERR_INVALID_CBOR, NULL, 0); return;
    }
    uint8_t client_data_hash[32];
    memcpy(client_data_hash, cdh, 32);

    // rp.id (key 0x02 → map → key "id")
    if (!cd_map_uint(req, req_len, 0x02, &v)) {
        ctap2_respond(cid, CTAP2_ERR_MISSING_PARAMETER, NULL, 0); return;
    }
    cbor_dec_t rp_v;
    if (!cd_map_tstr(v.buf, v.len, "id", &rp_v)) {
        ctap2_respond(cid, CTAP2_ERR_MISSING_PARAMETER, NULL, 0); return;
    }
    size_t rp_id_len;
    const char *rp_id = cd_tstr(&rp_v, &rp_id_len);
    if (!rp_id || rp_id_len == 0 || rp_id_len > 128) {
        ctap2_respond(cid, CTAP2_ERR_INVALID_CBOR, NULL, 0); return;
    }
    uint8_t rp_id_hash[32];
    sha256((const uint8_t *)rp_id, rp_id_len, rp_id_hash);

    // pubKeyCredParams (key 0x04): must include ES256 (-7)
    bool has_es256 = false;
    if (cd_map_uint(req, req_len, 0x04, &v)) {
        cd_item_t arr = cd_item(&v);
        if (!v.err && arr.type == 4) {
            for (uint64_t i = 0; i < arr.val && !v.err; i++) {
                // Each element is a map; look for "alg" = -7
                cbor_dec_t alg_v;
                // peek start of this map in v
                size_t map_start = v.pos;
                if (cd_map_tstr(v.buf + v.pos, v.len - v.pos, "alg", &alg_v)) {
                    cd_item_t alg = cd_item(&alg_v);
                    if (!alg_v.err && alg.type == 1 && alg.val == 6) { // -7 = nint(6)
                        has_es256 = true;
                    }
                }
                // skip this map element
                cd_skip(&v);
                (void)map_start;
            }
        }
    }
    if (!has_es256) {
        ctap2_respond(cid, CTAP2_ERR_UNSUPPORTED_ALG, NULL, 0); return;
    }

    // options (key 0x07): rk, uv
    bool opt_rk = false, opt_uv = false;
    if (cd_map_uint(req, req_len, 0x07, &v)) {
        cbor_dec_t ov;
        if (cd_map_tstr(v.buf, v.len, "rk", &ov)) {
            cd_item_t bv = cd_item(&ov); opt_rk = (!ov.err && bv.type == 7 && bv.val == 21);
        }
        if (cd_map_tstr(v.buf, v.len, "uv", &ov)) {
            cd_item_t bv = cd_item(&ov); opt_uv = (!ov.err && bv.type == 7 && bv.val == 21);
        }
    }

    // UV via PIN: honour what the relying party requests
    bool uv_flag = false;
    cbor_dec_t pin_auth_v;
    bool has_pin_auth = cd_map_uint(req, req_len, 0x08, &pin_auth_v);

    if (has_pin_auth) {
        if (!s_pin_token_valid) {
            ctap2_respond(cid, CTAP2_ERR_PIN_REQUIRED, NULL, 0); return;
        }
        size_t auth_len;
        const uint8_t *auth_param = cd_bstr(&pin_auth_v, &auth_len);
        if (!auth_param || auth_len < 16) {
            ctap2_respond(cid, CTAP2_ERR_PIN_AUTH_INVALID, NULL, 0); return;
        }
        uint8_t expected[32];
        hmac_sha256(s_pin_token, 16, client_data_hash, 32, expected);
        if (memcmp(expected, auth_param, 16) != 0) {
            ctap2_respond(cid, CTAP2_ERR_PIN_AUTH_INVALID, NULL, 0); return;
        }
        uv_flag = true;
    } else if (opt_uv) {
        if (!storage_fido2_has_pin()) {
            ctap2_respond(cid, CTAP2_ERR_PIN_NOT_SET, NULL, 0); return;
        }
        ctap2_respond(cid, CTAP2_ERR_PIN_REQUIRED, NULL, 0); return;
    }

    // Resident key storage check
    if (opt_rk) {
        if (cfg.rk_count >= FIDO2_RK_MAX) {
            ctap2_respond(cid, CTAP2_ERR_KEY_STORE_FULL, NULL, 0); return;
        }
    }

    // Wait for user presence
    if (!wait_for_up(cid)) {
        ctap2_respond(cid, CTAP2_ERR_ACTION_TIMEOUT, NULL, 0); return;
    }

    // Generate credential keypair
    uint8_t priv[32], pub_x[32], pub_y[32];
    if (!gen_keypair(priv, pub_x, pub_y)) {
        ctap2_respond(cid, CTAP2_ERR_OPERATION_DENIED, NULL, 0); return;
    }

    // Build credential plain and encrypt → credential ID
    cred_plain_t plain = {0};
    memcpy(plain.priv_key, priv, 32);
    memcpy(plain.rp_id_hash, rp_id_hash, 32);
    plain.flags = opt_rk ? 0x01 : 0x00;
    if (uv_flag) plain.flags |= 0x02;

    uint8_t master_key[32];
    storage_get_fido2_master_key(master_key);

    uint8_t cred_id[CRED_ID_LEN];
    if (!cred_encrypt(master_key, &plain, cred_id)) {
        ctap2_respond(cid, CTAP2_ERR_OPERATION_DENIED, NULL, 0); return;
    }

    // Increment global sign counter
    uint32_t sign_count = storage_fido2_inc_sign_counter();

    // Store as resident key if requested
    if (opt_rk) {
        // Parse user info from request
        fido2_rk_t rk = {0};
        rk.active = true;
        memcpy(rk.rp_id_hash, rp_id_hash, 32);
        memcpy(rk.cred_id, cred_id, CRED_ID_LEN);
        rk.sign_count = sign_count;

        if (cd_map_uint(req, req_len, 0x03, &v)) { // user map
            cbor_dec_t uv2;
            if (cd_map_tstr(v.buf, v.len, "id", &uv2)) {
                size_t uid_len;
                const uint8_t *uid = cd_bstr(&uv2, &uid_len);
                if (uid) {
                    rk.user_id_len = uid_len < FIDO2_USER_ID_MAX ? (uint8_t)uid_len : FIDO2_USER_ID_MAX;
                    memcpy(rk.user_id, uid, rk.user_id_len);
                }
            }
            if (cd_map_tstr(v.buf, v.len, "name", &uv2)) {
                size_t uname_len;
                const char *uname = cd_tstr(&uv2, &uname_len);
                if (uname) {
                    size_t cp = uname_len < FIDO2_USER_NAME_MAX-1 ? uname_len : FIDO2_USER_NAME_MAX-1;
                    memcpy(rk.user_name, uname, cp);
                }
            }
            if (cd_map_tstr(v.buf, v.len, "displayName", &uv2)) {
                size_t dn_len;
                const char *dn = cd_tstr(&uv2, &dn_len);
                if (dn) {
                    size_t cp = dn_len < FIDO2_USER_NAME_MAX-1 ? dn_len : FIDO2_USER_NAME_MAX-1;
                    memcpy(rk.display_name, dn, cp);
                }
            }
        }

        // Find free slot or existing slot with same rpIdHash + userId
        int slot = -1;
        for (int i = 0; i < FIDO2_RK_MAX; i++) {
            fido2_rk_t existing = {0};
            if (storage_get_fido2_rk(i, &existing) != ESP_OK || !existing.active) {
                if (slot < 0) slot = i;
                continue;
            }
            // Overwrite if same rp and same user
            if (memcmp(existing.rp_id_hash, rp_id_hash, 32) == 0 &&
                existing.user_id_len == rk.user_id_len &&
                memcmp(existing.user_id, rk.user_id, rk.user_id_len) == 0) {
                slot = i; break;
            }
        }
        if (slot < 0) {
            ctap2_respond(cid, CTAP2_ERR_KEY_STORE_FULL, NULL, 0); return;
        }
        storage_set_fido2_rk(slot, &rk);
    }

    // Build authData
    uint8_t auth_data[300];
    uint8_t flags = AUTHDATA_FLAG_UP | (uv_flag ? AUTHDATA_FLAG_UV : 0);
    size_t auth_len = build_auth_data_make(auth_data, sizeof(auth_data),
                                           rp_id_hash, flags, sign_count,
                                           cred_id, pub_x, pub_y);

    // Self-attestation: sign SHA256(authData || clientDataHash) with credential key
    uint8_t to_sign[64];
    sha256(auth_data, auth_len, to_sign);
    // Hash of (authData_hash || clientDataHash): we need hash of concatenation
    // Correct: sig = ECDSA( privKey, SHA256( authData || clientDataHash ) )
    uint8_t combined_hash[32];
    {
        mbedtls_sha256_context sc;
        mbedtls_sha256_init(&sc);
        mbedtls_sha256_starts(&sc, 0);
        mbedtls_sha256_update(&sc, auth_data, auth_len);
        mbedtls_sha256_update(&sc, client_data_hash, 32);
        mbedtls_sha256_finish(&sc, combined_hash);
        mbedtls_sha256_free(&sc);
    }

    uint8_t sig[72]; size_t sig_len = 0;
    if (!ecdsa_sign(priv, combined_hash, sig, &sig_len)) {
        ctap2_respond(cid, CTAP2_ERR_OPERATION_DENIED, NULL, 0); return;
    }

    // Build response CBOR
    uint8_t resp[512];
    cbor_enc_t e;
    ce_init(&e, resp, sizeof(resp));
    ce_map(&e, 3);
    ce_uint(&e, 1); ce_tstr(&e, "packed");                     // fmt
    ce_uint(&e, 2); ce_bstr(&e, auth_data, auth_len);          // authData
    ce_uint(&e, 3);                                             // attStmt
    ce_map(&e, 2);
    ce_tstr(&e, "alg"); ce_nint(&e, -7);
    ce_tstr(&e, "sig"); ce_bstr(&e, sig, sig_len);

    ctap2_respond(cid, CTAP2_OK, resp, e.pos);
    memset(priv, 0, sizeof(priv)); // zero private key
}

// ── CTAP2 command: getAssertion ───────────────────────────────────────────────

static void cmd_get_assertion(uint32_t cid, const uint8_t *req, size_t req_len) {
    cbor_dec_t v;

    // rpId (key 0x01)
    if (!cd_map_uint(req, req_len, 0x01, &v)) {
        ctap2_respond(cid, CTAP2_ERR_MISSING_PARAMETER, NULL, 0); return;
    }
    size_t rp_id_len;
    const char *rp_id = cd_tstr(&v, &rp_id_len);
    if (!rp_id || rp_id_len == 0) {
        ctap2_respond(cid, CTAP2_ERR_INVALID_CBOR, NULL, 0); return;
    }
    uint8_t rp_id_hash[32];
    sha256((const uint8_t *)rp_id, rp_id_len, rp_id_hash);

    // clientDataHash (key 0x02)
    if (!cd_map_uint(req, req_len, 0x02, &v)) {
        ctap2_respond(cid, CTAP2_ERR_MISSING_PARAMETER, NULL, 0); return;
    }
    size_t cdh_len;
    const uint8_t *cdh = cd_bstr(&v, &cdh_len);
    if (!cdh || cdh_len != 32) {
        ctap2_respond(cid, CTAP2_ERR_INVALID_CBOR, NULL, 0); return;
    }
    uint8_t client_data_hash[32];
    memcpy(client_data_hash, cdh, 32);

    // Find credential: check allowList first, then resident keys
    uint8_t master_key[32];
    storage_get_fido2_master_key(master_key);

    cred_plain_t matched_plain = {0};
    uint8_t      matched_cred_id[CRED_ID_LEN] = {0};
    bool         found = false;

    if (cd_map_uint(req, req_len, 0x03, &v)) { // allowList
        cd_item_t arr = cd_item(&v);
        if (!v.err && arr.type == 4) {
            for (uint64_t i = 0; i < arr.val && !v.err && !found; i++) {
                // Each element: {"type":"public-key","id":bytes}
                cbor_dec_t id_v;
                if (cd_map_tstr(v.buf + v.pos, v.len - v.pos, "id", &id_v)) {
                    size_t cid_len;
                    const uint8_t *cid_bytes = cd_bstr(&id_v, &cid_len);
                    if (cid_bytes && cid_len == CRED_ID_LEN) {
                        cred_plain_t pl = {0};
                        if (cred_decrypt(master_key, cid_bytes, &pl) &&
                            memcmp(pl.rp_id_hash, rp_id_hash, 32) == 0) {
                            matched_plain = pl;
                            memcpy(matched_cred_id, cid_bytes, CRED_ID_LEN);
                            found = true;
                        }
                    }
                }
                cd_skip(&v);
            }
        }
    }

    // No allowList match: try resident keys
    if (!found) {
        for (int i = 0; i < FIDO2_RK_MAX && !found; i++) {
            fido2_rk_t rk = {0};
            if (storage_get_fido2_rk(i, &rk) != ESP_OK || !rk.active) continue;
            if (memcmp(rk.rp_id_hash, rp_id_hash, 32) != 0) continue;
            cred_plain_t pl = {0};
            if (cred_decrypt(master_key, rk.cred_id, &pl) &&
                memcmp(pl.rp_id_hash, rp_id_hash, 32) == 0) {
                matched_plain = pl;
                memcpy(matched_cred_id, rk.cred_id, CRED_ID_LEN);
                found = true;
            }
        }
    }

    if (!found) {
        ctap2_respond(cid, CTAP2_ERR_NO_CREDENTIALS, NULL, 0); return;
    }

    // options (key 0x05): uv
    bool opt_uv = false;
    if (cd_map_uint(req, req_len, 0x05, &v)) {
        cbor_dec_t ov;
        if (cd_map_tstr(v.buf, v.len, "uv", &ov)) {
            cd_item_t bv = cd_item(&ov); opt_uv = (!ov.err && bv.type == 7 && bv.val == 21);
        }
    }

    // UV via PIN: honour what the relying party requests
    bool uv_flag = false;
    cbor_dec_t pin_auth_v;
    bool has_pin_auth = cd_map_uint(req, req_len, 0x06, &pin_auth_v);

    if (has_pin_auth) {
        if (!s_pin_token_valid) {
            ctap2_respond(cid, CTAP2_ERR_PIN_REQUIRED, NULL, 0); return;
        }
        size_t auth_len;
        const uint8_t *auth_param = cd_bstr(&pin_auth_v, &auth_len);
        if (!auth_param || auth_len < 16) {
            ctap2_respond(cid, CTAP2_ERR_PIN_AUTH_INVALID, NULL, 0); return;
        }
        uint8_t expected[32];
        hmac_sha256(s_pin_token, 16, client_data_hash, 32, expected);
        if (memcmp(expected, auth_param, 16) != 0) {
            ctap2_respond(cid, CTAP2_ERR_PIN_AUTH_INVALID, NULL, 0); return;
        }
        uv_flag = true;
    } else if (opt_uv) {
        if (!storage_fido2_has_pin()) {
            ctap2_respond(cid, CTAP2_ERR_PIN_NOT_SET, NULL, 0); return;
        }
        ctap2_respond(cid, CTAP2_ERR_PIN_REQUIRED, NULL, 0); return;
    }

    // Wait for user presence
    if (!wait_for_up(cid)) {
        ctap2_respond(cid, CTAP2_ERR_ACTION_TIMEOUT, NULL, 0); return;
    }

    uint32_t sign_count = storage_fido2_inc_sign_counter();

    uint8_t flags = AUTHDATA_FLAG_UP | (uv_flag ? AUTHDATA_FLAG_UV : 0);
    uint8_t auth_data[37];
    build_auth_data_get(auth_data, rp_id_hash, flags, sign_count);

    // Sign SHA256(authData || clientDataHash)
    uint8_t combined_hash[32];
    {
        mbedtls_sha256_context sc;
        mbedtls_sha256_init(&sc);
        mbedtls_sha256_starts(&sc, 0);
        mbedtls_sha256_update(&sc, auth_data, 37);
        mbedtls_sha256_update(&sc, client_data_hash, 32);
        mbedtls_sha256_finish(&sc, combined_hash);
        mbedtls_sha256_free(&sc);
    }

    uint8_t sig[72]; size_t sig_len = 0;
    if (!ecdsa_sign(matched_plain.priv_key, combined_hash, sig, &sig_len)) {
        ctap2_respond(cid, CTAP2_ERR_OPERATION_DENIED, NULL, 0); return;
    }

    uint8_t resp[256];
    cbor_enc_t e;
    ce_init(&e, resp, sizeof(resp));
    ce_map(&e, 3);
    ce_uint(&e, 2); ce_bstr(&e, auth_data, 37);              // authData
    ce_uint(&e, 3); ce_bstr(&e, sig, sig_len);               // signature
    ce_uint(&e, 1);                                           // credential descriptor
    ce_map(&e, 2);
    ce_tstr(&e, "type"); ce_tstr(&e, "public-key");
    ce_tstr(&e, "id");   ce_bstr(&e, matched_cred_id, CRED_ID_LEN);

    ctap2_respond(cid, CTAP2_OK, resp, e.pos);
    memset(matched_plain.priv_key, 0, 32);
}

// ── CTAP2 command: clientPIN ──────────────────────────────────────────────────

static void cmd_client_pin(uint32_t cid, const uint8_t *req, size_t req_len) {
    cbor_dec_t v;

    // subCommand (key 0x02)
    if (!cd_map_uint(req, req_len, 0x02, &v)) {
        ctap2_respond(cid, CTAP2_ERR_MISSING_PARAMETER, NULL, 0); return;
    }
    cd_item_t subcmd_item = cd_item(&v);
    if (v.err || subcmd_item.type != 0) {
        ctap2_respond(cid, CTAP2_ERR_INVALID_CBOR, NULL, 0); return;
    }
    uint8_t subcmd = (uint8_t)subcmd_item.val;
    static char s_pin_cmd_name[32];
    switch (subcmd) {
    case CTAP2_SUBCMD_GET_RETRIES:   snprintf(s_pin_cmd_name, sizeof(s_pin_cmd_name), "clientPIN.getRetries");       break;
    case CTAP2_SUBCMD_GET_KEY_AGREE: snprintf(s_pin_cmd_name, sizeof(s_pin_cmd_name), "clientPIN.getKeyAgreement");  break;
    case CTAP2_SUBCMD_SET_PIN:       snprintf(s_pin_cmd_name, sizeof(s_pin_cmd_name), "clientPIN.setPIN");           break;
    case CTAP2_SUBCMD_CHANGE_PIN:    snprintf(s_pin_cmd_name, sizeof(s_pin_cmd_name), "clientPIN.changePIN");        break;
    case CTAP2_SUBCMD_GET_PIN_TOKEN: snprintf(s_pin_cmd_name, sizeof(s_pin_cmd_name), "clientPIN.getPINToken");      break;
    default:                         snprintf(s_pin_cmd_name, sizeof(s_pin_cmd_name), "clientPIN.sub0x%02X", subcmd); break;
    }
    s_diag_cmd = s_pin_cmd_name;

    fido2_config_t cfg = {0};
    storage_get_fido2_config(&cfg);

    if (subcmd == CTAP2_SUBCMD_GET_RETRIES) {
        // Return remaining PIN retries
        uint8_t buf[8];
        cbor_enc_t e; ce_init(&e, buf, sizeof(buf));
        ce_map(&e, 1);
        ce_uint(&e, 3); ce_uint(&e, cfg.pin_retries);
        ctap2_respond(cid, CTAP2_OK, buf, e.pos);
        return;
    }

    if (subcmd == CTAP2_SUBCMD_GET_KEY_AGREE) {
        // Generate fresh ECDH keypair for PIN protocol session
        if (s_pin_key_valid) {
            mbedtls_mpi_free(&s_pin_d);
            mbedtls_ecp_point_free(&s_pin_Q);
        }
        mbedtls_ecp_group_free(&s_pin_grp);
        mbedtls_ecp_group_init(&s_pin_grp);
        mbedtls_mpi_init(&s_pin_d);
        mbedtls_ecp_point_init(&s_pin_Q);
        s_pin_key_valid = false;

        if (mbedtls_ecp_group_load(&s_pin_grp, MBEDTLS_ECP_DP_SECP256R1) != 0 ||
            mbedtls_ecp_gen_keypair(&s_pin_grp, &s_pin_d, &s_pin_Q, rng_func, NULL) != 0) {
            ctap2_respond(cid, CTAP2_ERR_OPERATION_DENIED, NULL, 0); return;
        }
        s_pin_key_valid = true;

        uint8_t pt[65]; size_t pt_len = 0;
        mbedtls_ecp_point_write_binary(&s_pin_grp, &s_pin_Q,
            MBEDTLS_ECP_PF_UNCOMPRESSED, &pt_len, pt, sizeof(pt));
        uint8_t x[32] = {0}, y[32] = {0};
        if (pt_len == 65) { memcpy(x, pt + 1, 32); memcpy(y, pt + 33, 32); }

        uint8_t buf[128]; cbor_enc_t e; ce_init(&e, buf, sizeof(buf));
        ce_map(&e, 1);
        ce_uint(&e, 1);
        ce_cose_ecdh_key(&e, x, y);
        ctap2_respond(cid, CTAP2_OK, buf, e.pos);
        return;
    }

    // For remaining sub-commands we need the shared secret from ECDH
    if (!s_pin_key_valid) {
        ctap2_respond(cid, CTAP2_ERR_PIN_AUTH_INVALID, NULL, 0); return;
    }

    // keyAgreement (key 0x03): client public key in COSE format
    if (!cd_map_uint(req, req_len, 0x03, &v)) {
        ctap2_respond(cid, CTAP2_ERR_MISSING_PARAMETER, NULL, 0); return;
    }

    // Extract x and y from COSE key map — find nint keys -2 (val=1) and -3 (val=2)
    uint8_t peer_x[32] = {0}, peer_y[32] = {0};
    {
        cbor_dec_t dv; cd_init(&dv, v.buf, v.len);
        cd_item_t map = cd_item(&dv);
        if (!dv.err && map.type == 5) {
            for (uint64_t i = 0; i < map.val && !dv.err; i++) {
                cd_item_t k = cd_item(&dv);
                if (dv.err) break;
                // Check for nint keys: -2 (val=1), -3 (val=2)
                if (k.type == 1 && k.val == 1) { // -2 → x
                    size_t bl; const uint8_t *bp = cd_bstr(&dv, &bl);
                    if (bp && bl == 32) memcpy(peer_x, bp, 32); else cd_skip(&dv);
                } else if (k.type == 1 && k.val == 2) { // -3 → y
                    size_t bl; const uint8_t *bp = cd_bstr(&dv, &bl);
                    if (bp && bl == 32) memcpy(peer_y, bp, 32); else cd_skip(&dv);
                } else {
                    cd_skip(&dv);
                }
            }
        }
    }

    // Compute ECDH shared secret — build peer point from uncompressed format
    uint8_t peer_buf[65] = {0x04};
    memcpy(peer_buf + 1, peer_x, 32);
    memcpy(peer_buf + 33, peer_y, 32);

    mbedtls_ecp_point peer_Q;
    mbedtls_ecp_point_init(&peer_Q);
    mbedtls_mpi shared;
    mbedtls_mpi_init(&shared);
    uint8_t shared_secret[32] = {0};
    bool ecdh_ok = false;

    if (mbedtls_ecp_point_read_binary(&s_pin_grp, &peer_Q, peer_buf, 65) == 0 &&
        mbedtls_ecdh_compute_shared(&s_pin_grp, &shared, &peer_Q, &s_pin_d, rng_func, NULL) == 0) {
        uint8_t shared_bytes[32] = {0};
        mbedtls_mpi_write_binary(&shared, shared_bytes, 32);
        sha256(shared_bytes, 32, shared_secret);
        ecdh_ok = true;
    }
    mbedtls_ecp_point_free(&peer_Q);
    mbedtls_mpi_free(&shared);

    if (!ecdh_ok) {
        ctap2_respond(cid, CTAP2_ERR_OPERATION_DENIED, NULL, 0); return;
    }

    if (subcmd == CTAP2_SUBCMD_SET_PIN || subcmd == CTAP2_SUBCMD_CHANGE_PIN) {
        crash_mark("sP:0\n");
        if (subcmd == CTAP2_SUBCMD_SET_PIN && storage_fido2_has_pin()) {
            ctap2_respond(cid, CTAP2_ERR_NOT_ALLOWED, NULL, 0); goto done_pin;
        }

        // newPinEnc (key 0x05): AES256-CBC(sharedSecret, IV=0, newPin) padded to 64 bytes
        if (!cd_map_uint(req, req_len, 0x05, &v)) {
            ctap2_respond(cid, CTAP2_ERR_MISSING_PARAMETER, NULL, 0); goto done_pin;
        }
        size_t npe_len; const uint8_t *npe = cd_bstr(&v, &npe_len);
        if (!npe || npe_len < 64 || npe_len > 256 || npe_len % 16 != 0) {
            ctap2_respond(cid, CTAP2_ERR_INVALID_CBOR, NULL, 0); goto done_pin;
        }
        crash_mark("sP:1\n");

        // For changePIN: verify old PIN via pinHashEnc (key 0x06)
        const uint8_t *phe_for_hmac = NULL;
        size_t phe_hmac_len = 0;
        if (subcmd == CTAP2_SUBCMD_CHANGE_PIN) {
            crash_mark("cP:0\n");
            if (cfg.pin_retries == 0) { ctap2_respond(cid, CTAP2_ERR_PIN_BLOCKED, NULL, 0); goto done_pin; }
            if (!cd_map_uint(req, req_len, 0x06, &v)) {
                ctap2_respond(cid, CTAP2_ERR_MISSING_PARAMETER, NULL, 0); goto done_pin;
            }
            const uint8_t *phe = cd_bstr(&v, &phe_hmac_len);
            if (!phe || phe_hmac_len != 16) { ctap2_respond(cid, CTAP2_ERR_INVALID_CBOR, NULL, 0); goto done_pin; }
            crash_mark("cP:1\n");
            uint8_t dec_hash[16] = {0};
            aes_cbc_decrypt(shared_secret, phe, 16, dec_hash);
            crash_mark("cP:2\n");
            uint8_t stored_hash[16] = {0};
            storage_get_fido2_pin_hash(stored_hash);
            crash_mark("cP:3\n");
            if (memcmp(dec_hash, stored_hash, 16) != 0) {
                cfg.pin_retries--;
                storage_set_fido2_config(&cfg);
                ctap2_respond(cid, cfg.pin_retries == 0 ? CTAP2_ERR_PIN_BLOCKED : CTAP2_ERR_PIN_INVALID, NULL, 0);
                goto done_pin;
            }
            crash_mark("cP:4\n");
            cfg.pin_retries = PIN_RETRIES_MAX;
            phe_for_hmac = phe; // valid for HMAC — points into s_hid.buf which lives for this call
        }

        // pinAuth (key 0x04) = LEFT(HMAC-SHA-256(sharedSecret, newPinEnc [|| pinHashEnc]), 16)
        if (!cd_map_uint(req, req_len, 0x04, &v)) {
            ctap2_respond(cid, CTAP2_ERR_MISSING_PARAMETER, NULL, 0); goto done_pin;
        }
        size_t pap_len; const uint8_t *pap = cd_bstr(&v, &pap_len);
        if (!pap || pap_len < 16) {
            ctap2_respond(cid, CTAP2_ERR_PIN_AUTH_INVALID, NULL, 0); goto done_pin;
        }
        crash_mark("sP:2\n");
        uint8_t expected_hmac[32];
        if (phe_for_hmac) {
            // changePIN: HMAC over newPinEnc || pinHashEnc
            mbedtls_md_context_t md;
            mbedtls_md_init(&md);
            int rc = mbedtls_md_setup(&md, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
            if (rc != 0) {
                mbedtls_md_free(&md);
                ctap2_respond(cid, CTAP2_ERR_OPERATION_DENIED, NULL, 0); goto done_pin;
            }
            mbedtls_md_hmac_starts(&md, shared_secret, 32);
            mbedtls_md_hmac_update(&md, npe, npe_len);
            mbedtls_md_hmac_update(&md, phe_for_hmac, phe_hmac_len);
            mbedtls_md_hmac_finish(&md, expected_hmac);
            mbedtls_md_free(&md);
        } else {
            hmac_sha256(shared_secret, 32, npe, npe_len, expected_hmac);
        }
        if (memcmp(expected_hmac, pap, 16) != 0) {
            ctap2_respond(cid, CTAP2_ERR_PIN_AUTH_INVALID, NULL, 0); goto done_pin;
        }
        crash_mark("sP:3\n");

        // Decrypt new PIN
        uint8_t *pin_plain = malloc(npe_len);
        if (!pin_plain) { ctap2_respond(cid, CTAP2_ERR_OPERATION_DENIED, NULL, 0); goto done_pin; }
        aes_cbc_decrypt(shared_secret, npe, npe_len, pin_plain);
        size_t pin_len = strnlen((char *)pin_plain, npe_len);
        if (pin_len < 4) {
            free(pin_plain);
            ctap2_respond(cid, CTAP2_ERR_PIN_POLICY_VIOLATION, NULL, 0); goto done_pin;
        }
        crash_mark("sP:4\n");

        // Store pinHash = LEFT(SHA-256(pin), 16) per CTAP2 §6.5.4
        uint8_t h1[32];
        sha256(pin_plain, pin_len, h1);
        free(pin_plain);
        storage_set_fido2_pin_hash(h1);
        crash_mark("sP:5\n");
        cfg.pin_retries = PIN_RETRIES_MAX;
        storage_set_fido2_config(&cfg);
        s_pin_token_valid = false;
        ctap2_respond(cid, CTAP2_OK, NULL, 0);
        crash_mark("sP:done\n");
        goto done_pin;
    }

    if (subcmd == CTAP2_SUBCMD_GET_PIN_TOKEN) {
        if (!storage_fido2_has_pin()) {
            ctap2_respond(cid, CTAP2_ERR_PIN_NOT_SET, NULL, 0); goto done_pin;
        }
        if (cfg.pin_retries == 0) {
            ctap2_respond(cid, CTAP2_ERR_PIN_BLOCKED, NULL, 0); goto done_pin;
        }

        // pinHashEnc (key 0x06)
        if (!cd_map_uint(req, req_len, 0x06, &v)) {
            ctap2_respond(cid, CTAP2_ERR_MISSING_PARAMETER, NULL, 0); goto done_pin;
        }
        size_t phe_len; const uint8_t *phe = cd_bstr(&v, &phe_len);
        if (!phe || phe_len < 16) { ctap2_respond(cid, CTAP2_ERR_INVALID_CBOR, NULL, 0); goto done_pin; }

        uint8_t dec_hash[16] = {0};
        aes_cbc_decrypt(shared_secret, phe, 16, dec_hash);

        uint8_t stored_hash[16] = {0};
        storage_get_fido2_pin_hash(stored_hash);

        if (memcmp(dec_hash, stored_hash, 16) != 0) {
            cfg.pin_retries--;
            storage_set_fido2_config(&cfg);
            ctap2_respond(cid, cfg.pin_retries == 0 ? CTAP2_ERR_PIN_BLOCKED : CTAP2_ERR_PIN_INVALID, NULL, 0);
            goto done_pin;
        }

        // PIN correct — reset retries, generate PIN token
        cfg.pin_retries = PIN_RETRIES_MAX;
        storage_set_fido2_config(&cfg);
        esp_fill_random(s_pin_token, 16);
        s_pin_token_valid = true;

        // Return encrypted PIN token
        uint8_t enc_token[16];
        aes_cbc_encrypt(shared_secret, s_pin_token, 16, enc_token);

        uint8_t buf[32]; cbor_enc_t e; ce_init(&e, buf, sizeof(buf));
        ce_map(&e, 1);
        ce_uint(&e, 2); ce_bstr(&e, enc_token, 16);
        ctap2_respond(cid, CTAP2_OK, buf, e.pos);
        goto done_pin;
    }

    ctap2_respond(cid, CTAP2_ERR_INVALID_CBOR, NULL, 0);

done_pin:
    memset(shared_secret, 0, sizeof(shared_secret));
    return;
}

// ── CTAP2 command: reset ──────────────────────────────────────────────────────

static void cmd_reset(uint32_t cid) {
    // Reset requires UP
    if (!wait_for_up(cid)) {
        ctap2_respond(cid, CTAP2_ERR_ACTION_TIMEOUT, NULL, 0); return;
    }
    fido2_factory_reset();
    s_pin_token_valid = false;
    ctap2_respond(cid, CTAP2_OK, NULL, 0);
}

// ── CTAPHID packet processing ─────────────────────────────────────────────────

#define CTAPHID_BUF_LEN 2048

typedef struct {
    uint32_t cid;
    uint8_t  cmd;
    uint16_t total_len;
    uint16_t recv_len;
    uint8_t  next_seq;
    uint8_t  buf[CTAPHID_BUF_LEN];
    bool     active;
} ctaphid_state_t;

static ctaphid_state_t s_hid;

static void ctaphid_dispatch(uint32_t cid, uint8_t cmd,
                              const uint8_t *data, uint16_t len) {
    fido2_config_t cfg = {0};
    storage_get_fido2_config(&cfg);

    diag_append("HID cmd=0x%02X cid=%08lX len=%u\n",
                cmd, (unsigned long)cid, (unsigned)len);

    if (cmd == CTAPHID_INIT) {
        // Handle INIT regardless of enabled state
        if (len < 8) { ctaphid_error(cid, 0x03); return; }
        uint32_t new_cid;
        if (cid == CTAPHID_CID_BROADCAST) {
            esp_fill_random(&new_cid, 4);
            if (new_cid == 0 || new_cid == CTAPHID_CID_BROADCAST) new_cid = 1;
        } else {
            new_cid = cid;
        }
        uint8_t resp[17];
        memcpy(resp, data, 8);            // echo nonce
        resp[8]  = (new_cid >> 24) & 0xFF;
        resp[9]  = (new_cid >> 16) & 0xFF;
        resp[10] = (new_cid >>  8) & 0xFF;
        resp[11] =  new_cid        & 0xFF;
        resp[12] = 2;   // CTAPHID protocol version
        resp[13] = 1;   // major version (Bluepass 1.x)
        resp[14] = 0;   // minor version
        resp[15] = 0;   // build
        resp[16] = 0x04 | 0x08; // CAPABILITY_CBOR | CAPABILITY_NMSG
        // Response goes on the same CID as the request (broadcast→broadcast per CTAP2.0 §8.1.5.4)
        // new_cid is carried in the response payload (bytes 8-11), not in the packet header
        uint32_t resp_cid = (cid == CTAPHID_CID_BROADCAST) ? CTAPHID_CID_BROADCAST : new_cid;
        ctaphid_send_packet(resp_cid, CTAPHID_INIT, resp, 17);
        diag_append("  INIT ok new_cid=%08lX resp_on=%08lX\n",
                    (unsigned long)new_cid, (unsigned long)resp_cid);
        return;
    }

    if (!cfg.enabled) {
        diag_append("  disabled → operationDenied\n");
        ctaphid_error(cid, CTAP2_ERR_OPERATION_DENIED);
        return;
    }

    if (cmd == CTAPHID_CANCEL) {
        return; // no response; UP timeout handles pending operations
    }

    if (cmd == CTAPHID_PING) {
        ctaphid_send_packet(cid, CTAPHID_PING, data, len);
        return;
    }

    if (cmd == CTAPHID_CBOR && len > 0) {
        uint8_t ctap_cmd = data[0];
        const uint8_t *cbor = data + 1;
        size_t cbor_len = len - 1;

        diag_append("  CBOR ctap=0x%02X cbor_len=%u\n", ctap_cmd, (unsigned)cbor_len);
        switch (ctap_cmd) {
        case CTAP2_CMD_GET_INFO:
            s_diag_cmd = "getInfo";
            cmd_get_info(cid);
            break;
        case CTAP2_CMD_MAKE_CREDENTIAL:
            s_diag_cmd = "makeCredential";
            cmd_make_credential(cid, cbor, cbor_len);
            break;
        case CTAP2_CMD_GET_ASSERTION:
            s_diag_cmd = "getAssertion";
            cmd_get_assertion(cid, cbor, cbor_len);
            break;
        case CTAP2_CMD_CLIENT_PIN:
            s_diag_cmd = "clientPIN";  // refined inside cmd_client_pin
            cmd_client_pin(cid, cbor, cbor_len);
            crash_mark("disp:ret\n");
            break;
        case CTAP2_CMD_RESET:
            s_diag_cmd = "reset";
            cmd_reset(cid);
            break;
        default:
            s_diag_cmd = "unknownCmd";
            diag_append("  unknown CTAP cmd=0x%02X\n", ctap_cmd);
            ctap2_respond(cid, CTAP1_ERR_INVALID_COMMAND, NULL, 0);
            break;
        }
        return;
    }

    ctaphid_error(cid, 0x01); // CTAP1_ERR_INVALID_COMMAND
}

static void ctaphid_process_packet(const uint8_t *pkt) {
    uint32_t cid = ((uint32_t)pkt[0] << 24) | ((uint32_t)pkt[1] << 16) |
                   ((uint32_t)pkt[2] << 8)  |  pkt[3];
    bool is_init = (pkt[4] & 0x80) != 0;

    if (is_init) {
        // Initialization packet
        uint8_t  cmd = pkt[4] & 0x7F;
        uint16_t total = ((uint16_t)pkt[5] << 8) | pkt[6];

        if (total > CTAPHID_BUF_LEN) {
            ctaphid_error(cid, 0x07); // INVALID_LENGTH
            s_hid.active = false;
            return;
        }

        s_hid.cid       = cid;
        s_hid.cmd       = cmd;
        s_hid.total_len = total;
        s_hid.recv_len  = 0;
        s_hid.next_seq  = 0;
        s_hid.active    = true;

        uint16_t chunk = total < 57 ? total : 57;
        memcpy(s_hid.buf, pkt + 7, chunk);
        s_hid.recv_len = chunk;

        if (s_hid.recv_len >= s_hid.total_len) {
            ctaphid_dispatch(cid, cmd, s_hid.buf, s_hid.total_len);
            crash_mark("proc:ret\n");
            s_hid.active = false;
        }
    } else {
        // Continuation packet
        if (!s_hid.active || s_hid.cid != cid) {
            ctaphid_error(cid, 0x05); // INVALID_SEQ
            return;
        }
        uint8_t seq = pkt[4] & 0x7F;
        if (seq != s_hid.next_seq++) {
            ctaphid_error(cid, 0x05);
            s_hid.active = false;
            return;
        }
        uint16_t remaining = s_hid.total_len - s_hid.recv_len;
        uint16_t chunk = remaining < 59 ? remaining : 59;
        memcpy(s_hid.buf + s_hid.recv_len, pkt + 5, chunk);
        s_hid.recv_len += chunk;

        if (s_hid.recv_len >= s_hid.total_len) {
            ctaphid_dispatch(cid, s_hid.cmd, s_hid.buf, s_hid.total_len);
            crash_mark("proc:ret\n");
            s_hid.active = false;
        }
    }
}

// ── FreeRTOS task ─────────────────────────────────────────────────────────────

static void fido2_task(void *arg)
{
    uint8_t pkt[64];
    while (1) {
        if (xQueueReceive(s_rx_queue, pkt, portMAX_DELAY) == pdTRUE) {
            ctaphid_process_packet(pkt);
            crash_mark("task:done\n");
            UBaseType_t hwm = uxTaskGetStackHighWaterMark(NULL);
            diag_append("stack_hwm=%u\n", (unsigned)hwm);
            char hwm_str[10];
            snprintf(hwm_str, sizeof(hwm_str), "h%u\n", (unsigned)hwm);
            crash_mark(hwm_str);
        }
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

void fido2_register_tx(void (*tx_cb)(const uint8_t *buf))
{
    s_tx_cb = tx_cb;
}

void fido2_on_hid_rx(const uint8_t *buf)
{
    if (!s_rx_queue) {
        diag_append("RX: no queue\n");
        return;
    }
    diag_append("RX %02X%02X%02X%02X cmd=%02X\n",
                buf[0], buf[1], buf[2], buf[3], buf[4]);
    xQueueSend(s_rx_queue, buf, 0);
}

bool fido2_is_enabled(void)
{
    return s_enabled;
}

bool fido2_pending_up(void)
{
    return s_up_pending;
}

void fido2_confirm_up(void)
{
    if (s_up_pending)
        xSemaphoreGive(s_up_sem);
}

bool fido2_intercept_key(uint8_t modifiers, uint8_t keycode)
{
    if (!s_up_pending || keycode == 0) return false;
    fido2_config_t cfg = {0};
    storage_get_fido2_config(&cfg);
    if (cfg.confirm_keycode == 0) return false;
    if (cfg.confirm_keycode != keycode) return false;
    if (cfg.confirm_modifiers != 0 && cfg.confirm_modifiers != modifiers) return false;
    fido2_confirm_up();
    return true;
}

esp_err_t fido2_regen_master_key(void)
{
    uint8_t new_key[32];
    esp_fill_random(new_key, 32);
    esp_err_t err = storage_set_fido2_master_key(new_key);
    memset(new_key, 0, 32);
    s_pin_token_valid = false;
    return err;
}

esp_err_t fido2_factory_reset(void)
{
    // Clear all resident keys
    for (int i = 0; i < FIDO2_RK_MAX; i++)
        storage_delete_fido2_rk(i);

    // Reset config
    fido2_config_t cfg = {0};
    storage_get_fido2_config(&cfg); // keep enabled/confirm key
    cfg.pin_retries = PIN_RETRIES_MAX;
    cfg.rk_count    = 0;
    storage_set_fido2_config(&cfg);

    storage_clear_fido2_pin_hash();
    storage_fido2_reset_sign_counter();
    s_pin_token_valid = false;

    return fido2_regen_master_key();
}

esp_err_t fido2_init(void)
{
    s_rx_queue = xQueueCreate(8, 64);
    s_up_sem   = xSemaphoreCreateBinary();
    if (!s_rx_queue || !s_up_sem) return ESP_ERR_NO_MEM;

    // Recover crash log from RTC memory if this is a software-reset recovery
    if (s_crash_magic == CRASH_MAGIC && s_crash_len > 0) {
        diag_append("[CRASH] last steps: %.*s\n", (int)s_crash_len, s_crash_buf);
    }
    s_crash_magic = CRASH_MAGIC;
    s_crash_buf[0] = '\0';
    s_crash_len = 0;

    mbedtls_ecp_group_init(&s_pin_grp);
    mbedtls_mpi_init(&s_pin_d);
    mbedtls_ecp_point_init(&s_pin_Q);
    s_pin_key_valid   = false;
    s_pin_token_valid = false;
    s_up_pending      = false;

    fido2_config_t cfg = {0};
    if (storage_get_fido2_config(&cfg) != ESP_OK) {
        cfg.pin_retries = PIN_RETRIES_MAX;
        storage_set_fido2_config(&cfg);
    }
    s_enabled = cfg.enabled;

    // Generate master key if not present
    uint8_t mkey[32] = {0};
    if (storage_get_fido2_master_key(mkey) != ESP_OK) {
        fido2_regen_master_key();
    }
    memset(mkey, 0, 32);

    xTaskCreate(fido2_task, "fido2", 16384, NULL, 4, NULL);
    ESP_LOGI(TAG, "init (enabled=%d)", cfg.enabled);
    return ESP_OK;
}
