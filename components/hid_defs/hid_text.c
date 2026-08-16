#include "hid_text.h"

uint32_t hid_utf8_next(const uint8_t **p)
{
    const uint8_t *s = *p;
    uint8_t c = *s;
    if (c == 0) return 0;

    uint32_t cp;
    int extra;
    if (c < 0x80)        { *p = s + 1; return c; }
    else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
    else                 { *p = s + 1; return 0xFFFD; }  // stray continuation byte

    for (int i = 1; i <= extra; i++) {
        if ((s[i] & 0xC0) != 0x80) { *p = s + 1; return 0xFFFD; }  // truncated sequence
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    *p = s + extra + 1;
    return cp;
}

// CP1252 0x80–0x9F — the range where Windows ANSI differs from Latin-1.
// Index = byte - 0x80; 0 marks the five unassigned slots (0x81, 0x8D, 0x8F, 0x90, 0x9D).
static const uint16_t s_cp1252_high[32] = {
    0x20AC, 0x0000, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x0000, 0x017D, 0x0000,
    0x0000, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x0000, 0x017E, 0x0178,
};

int hid_cp1252_from_codepoint(uint32_t cp)
{
    // 0x00–0x7F and 0xA0–0xFF are identical to Latin-1.
    if (cp < 0x80) return (int)cp;
    if (cp >= 0xA0 && cp <= 0xFF) return (int)cp;

    for (int i = 0; i < 32; i++)
        if (s_cp1252_high[i] && s_cp1252_high[i] == cp) return 0x80 + i;

    return -1;
}
