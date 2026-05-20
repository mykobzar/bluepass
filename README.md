# bluepass

> Bluetooth → USB HID bridge with password injection, TOTP codes and jiggler — firmware for ESP32-S3 SuperMini.

![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.2%2B-blue)
![Target](https://img.shields.io/badge/target-ESP32--S3-informational)
![License](https://img.shields.io/badge/license-MIT-green)

---

## What it does

bluepass sits between a Bluetooth keyboard and a laptop:

```
Bluetooth keyboard  ──BLE──▶  [ESP32-S3 SuperMini]  ──USB HID──▶  laptop
```

All keystrokes are forwarded transparently. Configured hotkey combinations are intercepted and replaced with:

| Substitution | Description |
|---|---|
| **Password** | Stored secret typed as keystrokes; never exposed over the API |
| **Text** | Arbitrary string, including characters the physical keyboard cannot produce |
| **TOTP code** | Live 6-digit code (RFC 6238 / Google Authenticator) |
| **Jiggler** | Toggles periodic keypress to prevent the laptop from sleeping |

The web interface is only available over WiFi and **only after an explicit button press** — it is never exposed on boot.

---

## Hardware

| Part | Notes |
|---|---|
| ESP32-S3 SuperMini | Main board |
| USB-C cable (data) | Connects to the laptop — powers the device and acts as the USB HID keyboard |
| Bluetooth keyboard | Any BLE HID keyboard |

The ESP32-S3 native USB OTG port is used as the HID output. The same port is used for flashing before the USB stack initialises.

---

## Quick start — flash pre-built firmware

A ready-to-flash binary is included in the [`firmware/`](firmware/) folder.  
No toolchain needed — only `esptool.py`:

```bash
pip install esptool
```

Then flash (replace the port with yours):

```bash
# Linux / macOS
esptool.py --chip esp32s3 --port /dev/ttyUSB0 --baud 460800 write_flash \
  --flash_mode dio --flash_freq 80m --flash_size 4MB \
  0x0     firmware/bootloader-0.9.2.bin \
  0x8000  firmware/partition-table-0.9.2.bin \
  0x10000 firmware/ota_data_initial-0.9.2.bin \
  0x20000 firmware/bluepass-0.9.2.bin

# Windows — use COM3, COM4, etc.
esptool.py --chip esp32s3 --port COM3 --baud 460800 write_flash ^
  --flash_mode dio --flash_freq 80m --flash_size 4MB ^
  0x0     firmware\bootloader-0.9.2.bin ^
  0x8000  firmware\partition-table-0.9.2.bin ^
  0x10000 firmware\ota_data_initial-0.9.2.bin ^
  0x20000 firmware\bluepass-0.9.2.bin
```

> **Windows GUI option:** see [`firmware/README.md`](firmware/README.md) for step-by-step instructions using the Espressif Flash Download Tool (no Python required).

After flashing, proceed to [First-time setup](#first-time-setup).

---

## Building and flashing

### 1. Install ESP-IDF

Minimum version: **v5.2**. Full instructions: <https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/>

```bash
mkdir -p ~/esp && cd ~/esp
git clone --recursive https://github.com/espressif/esp-idf.git
cd esp-idf && ./install.sh esp32s3
```

### 2. Clone this repo

```bash
git clone https://github.com/<your-username>/bluepass.git
cd bluepass
```

### 3. Activate the ESP-IDF environment

Run this in every new terminal session before using `idf.py`:

```bash
. ~/esp/esp-idf/export.sh
```

### 4. Set the target (once per checkout)

```bash
idf.py set-target esp32s3
```

### 5. Build

```bash
idf.py build
```

First build: 3–7 minutes. Incremental builds: seconds.

### 6. Flash

Connect the ESP32-S3 SuperMini to your development machine via USB-C. Find the port:

```bash
# Linux
ls /dev/ttyUSB* /dev/ttyACM*

# macOS
ls /dev/cu.usbserial* /dev/cu.SLAB*
```

Flash (replace the port as needed):

```bash
idf.py -p /dev/ttyUSB0 flash
```

> **Tip:** If the device is not detected, hold the **BOOT (GPIO0)** button, connect USB, then release — this forces the ROM bootloader.

### 7. Verify (optional)

```bash
idf.py -p /dev/ttyUSB0 monitor
```

A successful first boot with no stored WiFi credentials prints:

```
I (352) bluepass: no WiFi credentials — starting setup AP
I (358) wifi_mgr: AP started: SSID=bluepass
I (364) web_ui: web UI started
```

Exit the monitor with **Ctrl+]**.

> Once TinyUSB initialises, the serial port becomes unavailable — this is expected. The monitor is only needed during development.

---

## First-time setup

### Step 1 — connect to the access point

After flashing with no stored WiFi credentials the device starts as an open access point:

```
SSID:     bluepass
Password: (none)
```

### Step 2 — open the web interface

```
http://192.168.4.1
```

### Step 3 — set your WiFi credentials

On the **WiFi** tab enter your network SSID and password, then click **Save & Reboot**.  
The device reboots and joins your network.

### Step 4 — activate the web interface

The web UI is **not started automatically** after connecting to WiFi.  
**Short-press the GPIO0 button** on the board to toggle it on.

Find the device IP in your router's client list (hostname: `bluepass`) and open it in a browser.

### Step 5 — pair the Bluetooth keyboard

1. Put your keyboard into pairing mode.
2. Open the **Bluetooth** tab → **Scan (4 s)**.
3. Click **Connect** next to your keyboard.
4. If a PIN prompt appears, type it on the keyboard and press Enter.

The paired address is saved to NVS and reconnected automatically on the next boot.

---

## GPIO0 button

| Press duration | Action |
|---|---|
| Short (< 10 s) | Toggle the web interface on / off |
| Long (≥ 10 s) | Erase WiFi credentials → start access point + web interface |

**LED behaviour:**

| State | LED |
|---|---|
| Connecting to WiFi / no connection | Blinking (500 ms) |
| Connected to WiFi | Solid on |
| AP mode | Solid on |

---

## Web interface tabs

| Tab | Purpose |
|---|---|
| **Log** | Live stream of all received and forwarded keycodes |
| **Text** | Hotkey → arbitrary text substitution |
| **Passwords** | Hotkey → password (masked in the UI, never returned by the API) |
| **TOTP** | Hotkey → TOTP code; shows device clock sync status |
| **Jiggler** | Jiggler interval, key code, enable/disable hotkeys |
| **List** | Full HID keycode reference (keyboard + Consumer Control) |
| **Bluetooth** | Scan, pair, disconnect; BLE diagnostic log |
| **WiFi** | Change WiFi network (triggers reboot) |
| **Firmware** | OTA firmware update |

---

## Features

### Passwords

Assign a hotkey to a stored password. When the hotkey is pressed on the Bluetooth keyboard the password is typed on the laptop character by character.

- Passwords are **never returned** by the REST API — the endpoint is write-only.
- In the UI passwords are displayed as `***`.

### Text substitutions

Same as passwords but the stored text is visible in the UI. Supports any printable ASCII character and newline. Useful for text snippets, signatures, or characters the physical keyboard layout cannot produce.

### TOTP codes

Assign a hotkey to a TOTP service secret (base32). Pressing the hotkey types the current 6-digit code.

**Requirements:**
- The device must be connected to WiFi.
- The system clock must be synchronised with NTP. The **TOTP** tab shows the sync status (`Device: synced Δ±Xs`). Codes are not generated if the clock is not synced.

**Import from Google Authenticator:**  
The TOTP tab includes an importer for `otpauth-migration://offline?data=…` links exported by the Google Authenticator app.

### Jiggler

Periodically sends a harmless key (`F15` by default) to prevent the laptop from going to sleep.

| Setting | Description |
|---|---|
| **Interval** | How often to send the key, in seconds |
| **Key code** | HID keycode to send (default `0x6A` = F15) |
| **Enable hotkey** | Keyboard combination to turn the jiggler on |
| **Disable hotkey** | Keyboard combination to turn the jiggler off |

The enabled/disabled state and all settings are persisted in NVS and restored after reboot.  
If enable/disable hotkeys are configured the jiggler can be controlled directly from the Bluetooth keyboard without opening the web interface.

---

## Hotkey syntax

Hotkeys are entered in the format `<Modifier>+<Key>` in any Hotkey field.

### Modifiers

| Token | Key |
|---|---|
| `<Ctrl>` | Left Control |
| `<Shift>` | Left Shift |
| `<Alt>` | Left Alt |
| `<Win>` | Left Win / Cmd |
| `<RCtrl>` | Right Control |
| `<RShift>` | Right Shift |
| `<RAlt>` | Right Alt / AltGr |
| `<RWin>` | Right Win / Cmd |

### Keys by name

`<Enter>` `<Esc>` `<Tab>` `<Space>` `<Backspace>` `<Delete>` `<Insert>` `<Home>` `<End>` `<PgUp>` `<PgDn>` `<F1>`–`<F24>` `<↑>` `<↓>` `<←>` `<→>` `<A>`–`<Z>` `<0>`–`<9>` and more.

Full list available on the **List** tab in the web interface.

### Keys by hex code

```
<0x15>      keyboard keycode 0x15  (R)
<0x00CD>    Consumer Control: Play/Pause  (3–4 hex digits → Consumer Control)
```

### Examples

```
<Ctrl>+<Shift>+<P>     Ctrl + Shift + P
<RAlt>+<F2>            Right Alt + F2
<Win>+<0x15>           Win + R  (via hex code)
<0x00CD>               Media Play/Pause
```

---

## Storage layout (NVS)

| Namespace | Contents |
|---|---|
| `wifi` | SSID + password |
| `hotkeys` | Slot array: type, trigger combo, payload/secret, label |
| `jiggler` | Interval, key, on/off hotkeys, enabled state |
| `ble` | Paired keyboard address + name |

Up to **16 hotkey slots** total across passwords, text and TOTP.

---

## Full factory reset

Erases all flash including NVS — removes WiFi, hotkeys, TOTP secrets, jiggler config, and paired keyboard:

```bash
idf.py -p /dev/ttyUSB0 erase-flash
```

Then flash the firmware again and run through first-time setup.

**WiFi-only reset:** hold GPIO0 for ≥ 10 seconds. Hotkeys, TOTP secrets, and jiggler settings are preserved.

---

## License

MIT — see [LICENSE](LICENSE).
