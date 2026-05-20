#include "totp.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "mbedtls/md.h"
#include <string.h>
#include <time.h>

#define TAG "totp"
#define TOTP_STEP   30      // 30-second window
#define TOTP_DIGITS 1000000 // 10^6 → 6-digit code

static const char s_base32_alpha[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

static int base32_decode(const char *src, uint8_t *dst, size_t dst_len)
{
    int bits = 0, val = 0, out = 0;
    for (const char *p = src; *p && *p != '='; p++) {
        char c = *p >= 'a' ? *p - 32 : *p; // uppercase
        const char *pos = strchr(s_base32_alpha, c);
        if (!pos) {
            // skip whitespace and common separators silently; reject true garbage
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '-') continue;
            return -1;
        }
        val = (val << 5) | (int)(pos - s_base32_alpha);
        bits += 5;
        if (bits >= 8) {
            if ((size_t)out >= dst_len) return -1;
            dst[out++] = (uint8_t)(val >> (bits - 8));
            bits -= 8;
        }
    }
    return out;
}

static esp_err_t hmac_sha1(const uint8_t *key, size_t key_len,
                            const uint8_t *msg, size_t msg_len,
                            uint8_t *out20)
{
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
    int rc = mbedtls_md_setup(&ctx, info, 1);
    if (rc == 0) rc = mbedtls_md_hmac_starts(&ctx, key, key_len);
    if (rc == 0) rc = mbedtls_md_hmac_update(&ctx, msg, msg_len);
    if (rc == 0) rc = mbedtls_md_hmac_finish(&ctx, out20);
    mbedtls_md_free(&ctx);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t totp_generate_at(const char *base32_secret, int64_t unix_time, uint32_t *code_out)
{
    uint8_t secret[64];
    int secret_len = base32_decode(base32_secret, secret, sizeof(secret));
    if (secret_len <= 0) {
        ESP_LOGE(TAG, "invalid base32 secret");
        return ESP_ERR_INVALID_ARG;
    }

    uint64_t counter = (uint64_t)(unix_time / TOTP_STEP);
    uint8_t msg[8] = {
        (counter >> 56) & 0xFF, (counter >> 48) & 0xFF,
        (counter >> 40) & 0xFF, (counter >> 32) & 0xFF,
        (counter >> 24) & 0xFF, (counter >> 16) & 0xFF,
        (counter >>  8) & 0xFF, (counter      ) & 0xFF,
    };

    uint8_t hash[20];
    ESP_RETURN_ON_ERROR(hmac_sha1(secret, secret_len, msg, 8, hash), TAG, "hmac failed");

    // Dynamic truncation (RFC 4226 §5.4)
    int offset = hash[19] & 0x0F;
    uint32_t code = ((hash[offset]   & 0x7F) << 24)
                  | ((hash[offset+1] & 0xFF) << 16)
                  | ((hash[offset+2] & 0xFF) <<  8)
                  | ((hash[offset+3] & 0xFF));
    *code_out = code % TOTP_DIGITS;
    return ESP_OK;
}

esp_err_t totp_generate(const char *base32_secret, uint32_t *code_out)
{
    time_t now;
    time(&now);
    if (now < 1000000000) {
        // Clock not synchronized — time(NULL) returns seconds since boot or garbage
        ESP_LOGE(TAG, "system time not synchronized");
        return ESP_ERR_INVALID_STATE;
    }
    return totp_generate_at(base32_secret, (int64_t)now, code_out);
}

uint8_t totp_seconds_remaining(void)
{
    time_t now;
    time(&now);
    return (uint8_t)(TOTP_STEP - (now % TOTP_STEP));
}
