#pragma once

// Pull in TinyUSB's canonical HID types:
//   hid_keyboard_report_t  { uint8_t modifier; uint8_t reserved; uint8_t keycode[6]; }
//   HID_KEY_* keycode constants
#include "class/hid/hid.h"

// Unified report that covers both keyboard and Consumer Control (HID page 0x0C).
// consumer_code != 0 → Consumer Control event; keyboard fields are zero.
// consumer_code == 0 → Keyboard event.
typedef struct {
    hid_keyboard_report_t keyboard;   // modifier + reserved + keycode[6]
    uint16_t              consumer_code; // HID Consumer Usage ID, 0 = keyboard event
} bluepass_hid_report_t;

// Modifier-bit aliases — shorter names used throughout bluepass code.
// These map to TinyUSB's KEYBOARD_MODIFIER_* enum values.
#define HID_MOD_L_CTRL   KEYBOARD_MODIFIER_LEFTCTRL
#define HID_MOD_L_SHIFT  KEYBOARD_MODIFIER_LEFTSHIFT
#define HID_MOD_L_ALT    KEYBOARD_MODIFIER_LEFTALT
#define HID_MOD_L_GUI    KEYBOARD_MODIFIER_LEFTGUI
#define HID_MOD_R_CTRL   KEYBOARD_MODIFIER_RIGHTCTRL
#define HID_MOD_R_SHIFT  KEYBOARD_MODIFIER_RIGHTSHIFT
#define HID_MOD_R_ALT    KEYBOARD_MODIFIER_RIGHTALT
#define HID_MOD_R_GUI    KEYBOARD_MODIFIER_RIGHTGUI
