#pragma once

#include <stdint.h>

// Text helpers shared by the USB and BLE HID output paths.
//
// Stored payloads (passwords, text substitutions) are UTF-8. HID transmits scan
// codes, not characters, so anything outside printable ASCII has to be entered
// through a host-side Unicode mechanism — on Windows that is Alt + numpad with a
// leading zero, which feeds a CP1252 byte and is independent of the active
// keyboard layout.

// Decode the next UTF-8 code point from *p and advance *p past it.
// Returns 0 at end of string. Malformed bytes yield 0xFFFD and consume one byte.
uint32_t hid_utf8_next(const uint8_t **p);

// Map a Unicode code point to its CP1252 (Windows ANSI) byte.
// Returns -1 if the code point has no CP1252 representation.
int hid_cp1252_from_codepoint(uint32_t cp);
