#pragma once

#include <stdint.h>

// Text helpers shared by the USB and BLE HID output paths.
//
// Stored payloads (passwords, text substitutions) are UTF-8. HID transmits scan
// codes, not characters, so anything outside printable ASCII has to be entered
// through a host-side Unicode mechanism — on Windows that is Alt + numpad with a
// leading zero, which feeds a CP1252 byte and is independent of the active
// keyboard layout.

// How a payload is turned into keystrokes.
//
// Scan codes are *positions* on the host's active layout, so on anything but a
// US layout even plain ASCII comes out wrong (@ " # \ | ~ move, QWERTZ swaps
// Y/Z, a Cyrillic layout turns every letter Cyrillic).
//
// Alt+numpad decimal (ALT) is immune to key positions, but the number is
// interpreted through the ANSI code page of the host's active input language —
// not always CP1252. Under a Cyrillic layout that is CP1251, where 0xFC is 'ь'
// rather than 'ü' and 'ü' has no representation at all. ASCII is safe because
// every ANSI code page shares it.
//
// Alt+numpad hex (ALT_HEX) sends the Unicode code point itself and is free of
// the code-page problem, but the host needs EnableHexNumpad set in the registry
// and the A–F digits travel as ordinary letter keys, so they still need a Latin
// layout active. Code points whose hex form is all digits work anywhere.
typedef enum {
    HID_TEXT_MODE_AUTO    = 0,  // ASCII as scan codes, everything else via Alt+numpad
    HID_TEXT_MODE_ALT     = 1,  // everything printable via Alt + decimal CP1252 code
    HID_TEXT_MODE_SCAN    = 2,  // scan codes only; characters outside ASCII are skipped
    HID_TEXT_MODE_ALT_HEX = 3,  // everything printable via Alt + '+' + hex code point
} hid_text_mode_t;

// Decode the next UTF-8 code point from *p and advance *p past it.
// Returns 0 at end of string. Malformed bytes yield 0xFFFD and consume one byte.
uint32_t hid_utf8_next(const uint8_t **p);

// Map a Unicode code point to its CP1252 (Windows ANSI) byte.
// Returns -1 if the code point has no CP1252 representation.
int hid_cp1252_from_codepoint(uint32_t cp);
