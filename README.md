# bluepass

> Hardware Bluetooth → USB HID bridge with password injection, TOTP codes and jiggler — firmware for ESP32-S3.

![Version](https://img.shields.io/badge/version-1.0.0-blue)
![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.2%2B-blue)
![Target](https://img.shields.io/badge/target-ESP32--S3-informational)
![License](https://img.shields.io/badge/license-MIT-green)

**[⚡ Install in browser](https://mykobzar.github.io/bluepass/)** — no toolchain needed, Chrome/Edge only

---

## What is bluepass?

bluepass is a small hardware device that sits between a Bluetooth keyboard and a laptop. It forwards all keystrokes transparently — the laptop sees a standard USB HID keyboard and never knows there is anything in between.

The interesting part is what happens to *certain* key combinations. You assign hotkeys to stored secrets: a password, a TOTP code, or a block of text. When you press the hotkey on your Bluetooth keyboard, bluepass intercepts it and types the secret on the laptop character by character, over USB, without ever exposing it to software on the host.

There is no driver, no browser extension, and no software to install. The secrets live in the device flash, encrypted at rest. The web management interface is hosted locally on the device and is **never reachable without a deliberate button press** — it activates on demand and shuts itself off after five minutes of inactivity.

### What problem does it solve?

Password managers and TOTP apps require software on the host. On locked-down corporate machines, kiosks, remote-desktop sessions, or shared workstations you often cannot install anything. bluepass requires nothing from the host — it looks like a USB keyboard.

It also means your secrets are never typed by software on the host. A keylogger on the host sees keystrokes but has no way to distinguish a real keystroke from a substituted one, and the source of the secret never touches host memory.

---

## What it does

```
Bluetooth keyboard  ──BLE──▶  [ESP32-S3 board]  ──USB HID──▶  laptop
```

All keystrokes pass through transparently. Configured hotkey combinations are intercepted and replaced with:

| Substitution | Description |
|---|---|
| **Password** | Stored secret typed as keystrokes; never exposed over the API |
| **Text** | Arbitrary string, including characters the physical keyboard cannot produce |
| **TOTP code** | Live 6-digit code (RFC 6238 / Google Authenticator) |
| **Jiggler** | Toggles periodic keypresses to prevent the laptop from sleeping |

The web interface is available over WiFi and **only after an explicit button press** — it is never exposed on boot.

---

## Compatible hardware

bluepass has two hard requirements on the microcontroller:

- **BLE host mode** — to receive HID reports from a Bluetooth keyboard
- **Native USB OTG** — to present itself as a USB HID keyboard to the host (a USB-UART bridge is not sufficient; the port must be wired to the chip's USB OTG peripheral)

Among the current ESP32 family, **only the ESP32-S3** satisfies both requirements simultaneously. The table below shows why:

| Chip | BLE | Native USB OTG | WiFi | Compatible |
|---|---|---|---|---|
| **ESP32-S3** | BLE 5.0 ✓ | USB OTG ✓ | 802.11 b/g/n ✓ | ✅ **Full support** |
| ESP32-S2 | None ✗ | USB OTG ✓ | 802.11 b/g/n ✓ | ❌ No BLE |
| ESP32-C3 | BLE 5.0 ✓ | Serial/JTAG only ✗ | 802.11 b/g/n ✓ | ❌ No USB HID |
| ESP32-C6 | BLE 5.3 ✓ | None ✗ | WiFi 6 ✓ | ❌ No USB OTG |
| ESP32-H2 | BLE 5.3 ✓ | None ✗ | None ✗ | ❌ No USB, no WiFi |
| ESP32-C2 | BLE 5.0 ✓ | None ✗ | 802.11 b/g/n ✓ | ❌ No USB OTG |
| ESP32 (classic) | BT 4.2 (classic) ✓ | None ✗ | 802.11 b/g/n ✓ | ❌ No USB OTG |
| ESP32-P4 | None (external) ✗ | USB OTG ✓ | None (external) ✗ | ❌ No BLE or WiFi |

### Tested and supported boards

The **Board** tab in the web interface lets you configure which GPIO pins the button and LED are on, so bluepass adapts to different ESP32-S3 boards without reflashing.

| Board | USB-C wired to OTG? | Built-in button | Built-in LED | Default config |
|---|---|---|---|---|
| **ESP32-S3 SuperMini** ⭐ | Yes | GPIO0 | WS2812 RGB on GPIO48 | Ready to use |
| Seeed Studio XIAO ESP32-S3 | Yes | GPIO0 (boot pad) | None / GPIO2 on Sense variant | Set LED GPIO to 2 or None |
| Adafruit QT Py ESP32-S3 | Yes | GPIO0 (boot pad) | NeoPixel on GPIO39 | Set LED GPIO to 39 |
| Adafruit Feather ESP32-S3 | Yes | GPIO0 (boot pad) | NeoPixel on GPIO33 | Set LED GPIO to 33 |
| ESP32-S3-DevKitC-1 | Yes (USB port, not UART) | GPIO0 | RGB LED on GPIO38 or GPIO48 | Set LED GPIO accordingly |
| M5Stack StampS3 | Yes | GPIO0 | RGB LED on GPIO21 | Set LED GPIO to 21, type RGB |

> **Note for boards without a button:** GPIO0 is the ROM bootloader entry pin and is pulled up by default. A momentary switch to GND on GPIO0 works as the bluepass button on any board.

> **Note for boards with a simple LED (not WS2812):** Set LED type to *Simple* in the Board tab and configure the GPIO and active level. The same state machine (web UI active, WiFi error, jiggler, substitution flash) works with both LED types; the simple LED shows on/off instead of colour.

### Flash and RAM requirements

| Resource | Minimum | Recommended |
|---|---|---|
| Flash | 4 MB | 8 MB |
| RAM | 512 KB internal | 512 KB + PSRAM |

The partition layout uses dual OTA slots (each 1.75 MB) plus NVS. 4 MB flash is the minimum for OTA updates. 8 MB flash allows larger future firmware and leaves more room for NVS.

---

## Hardware

| Part | Notes |
|---|---|
| ESP32-S3 SuperMini | Main board (or any compatible board above) |
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
  0x0     firmware/bootloader-0.9.14.bin \
  0x8000  firmware/partition-table-0.9.14.bin \
  0x10000 firmware/ota_data_initial-0.9.14.bin \
  0x20000 firmware/bluepass-0.9.14.bin

# Windows — use COM3, COM4, etc.
esptool.py --chip esp32s3 --port COM3 --baud 460800 write_flash ^
  --flash_mode dio --flash_freq 80m --flash_size 4MB ^
  0x0     firmware\bootloader-0.9.14.bin ^
  0x8000  firmware\partition-table-0.9.14.bin ^
  0x10000 firmware\ota_data_initial-0.9.14.bin ^
  0x20000 firmware\bluepass-0.9.14.bin
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

Connect the ESP32-S3 board to your development machine via USB-C. Find the port:

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

On the **Settings → WiFi** tab enter your network SSID and password, then click **Save & Reboot**.  
The device reboots and joins your network.

### Step 4 — activate the web interface

The web UI is **not started automatically** after connecting to WiFi.  
**Short-press the button** (GPIO0 by default) on the board to toggle it on.

Find the device IP in your router's client list (hostname: `bluepass`) and open it in a browser.

### Step 5 — pair the Bluetooth keyboard

1. Put your keyboard into pairing mode.
2. Open the **Bluetooth** tab → **Scan (4 s)**.
3. Click **Connect** next to your keyboard.
4. If a PIN prompt appears, type it on the keyboard and press Enter.

The paired address is saved to NVS and reconnected automatically on the next boot.

---

## Button

| Press duration | Action |
|---|---|
| Short (< 10 s) | Toggle the web interface on / off |
| Long (≥ 10 s) | Erase WiFi credentials → start access point + web interface |

The button GPIO can be changed in **Settings → Board**. Default: GPIO0.

**LED behaviour:**

| State | WS2812 RGB LED | Simple LED |
|---|---|---|
| Web interface active | Solid **blue** | On |
| WiFi not connected / no credentials | **Red** fast blink (5 Hz) | Fast blink |
| Jiggler running | **Green** slow blink (1 Hz) | Slow blink |
| Password / text / TOTP substituted | Single **white** 150 ms flash | 150 ms pulse |
| Connected, idle | Off | Off |

LED type (RGB / Simple / None), GPIO pin, and brightness are configurable in **Settings → Board**.  
The web interface **turns off automatically after 5 minutes of inactivity** (no browser requests).

---

## Web interface tabs

| Tab | Purpose |
|---|---|
| **Info** | WiFi and Bluetooth connection status with signal strength; live key log |
| **Text** | Hotkey → arbitrary text substitution |
| **Passwords** | Hotkey → password (masked in the UI, never returned by the API) |
| **TOTP** | Hotkey → TOTP code; shows device clock sync status |
| **Jiggler** | Jiggler interval, key code, enable/disable hotkeys |
| **List** | Full HID keycode reference (keyboard + Consumer Control) |
| **Bluetooth** | Scan, pair, disconnect; BLE diagnostic log |
| **Settings → WiFi** | Change WiFi network (triggers reboot) |
| **Settings → Firmware** | OTA firmware update |
| **Settings → Security** | Flash encryption status; switch from Development to Release mode |
| **Settings → Board** | Button GPIO; LED type (RGB / Simple / None), GPIO and brightness |

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

**Toggle mode:** leave the Disable hotkey field empty and the same key acts as a toggle — one press enables, next press disables.

### Match and replace modes

Each hotkey slot has two independent mode selectors:

**Match mode** — how the incoming key event is compared against the stored trigger:

| Mode | Behaviour |
|---|---|
| Exact (default) | Keycode *and* modifier mask must both match |
| Keycode only | Only the keycode is compared; any modifier state triggers the slot |

**Replace mode** — how the outgoing substitution is sent:

| Mode | Behaviour |
|---|---|
| Replace all (default) | Substitution is sent with no modifiers held |
| Keep modifiers | Modifiers active at trigger time are preserved and re-sent after typing |

These modes are set per slot in the Add / Edit form on the Text, Passwords, and TOTP tabs.

### Editing existing slots

All three substitution tabs (Text, Passwords, TOTP) have an **Edit** button next to each slot.  
Clicking Edit copies the slot's settings into the form — label, hotkey, match/replace mode — ready to be modified and saved.

Passwords and TOTP secrets are never shown in the UI. To change a secret, type a new value in the payload field while editing; leave it blank to keep the current value unchanged.

### Secure deletion

When a Password or TOTP slot is deleted, the stored payload is overwritten twice (first with `0x00`, then with `0xFF`) before the NVS key is erased. This ensures the secret does not persist in flash as a recoverable stale NVS page.

### Board configuration

The **Settings → Board** tab configures the physical GPIO assignments without reflashing:

| Setting | Default | Description |
|---|---|---|
| Button GPIO | 0 | GPIO pin for the control button |
| LED type | RGB | WS2812 RGB (via RMT) or simple GPIO output or None |
| RGB GPIO | 48 | Data pin for the WS2812 LED |
| Brightness | 4% | RGB LED brightness (1–100%) |
| Simple GPIO | — | GPIO pin for a plain (non-RGB) LED |
| Active high | Yes | Whether the simple LED is on when GPIO is high |

Changes take effect after reboot.

### Flash encryption

bluepass supports ESP32 hardware AES-XTS flash encryption. The **Settings → Security** tab shows the current encryption mode and explains the consequences of each state:

| Mode | Flash content | UART flash | JTAG |
|---|---|---|---|
| Disabled | Plaintext | Allowed | Allowed |
| Development | Encrypted | Allowed | Allowed |
| Release | Encrypted | **Disabled permanently** | **Disabled permanently** |

Firmware ships with encryption in **Development mode** — data is encrypted, but UART reflashing is still possible. Switching to Release mode is irreversible: it permanently burns eFuse bits that disable UART download and JTAG. Only OTA updates remain possible after that.

> Verify that OTA firmware updates work correctly before switching to Release mode.

### OTA firmware update

The **Firmware** tab lets you update the firmware without a USB cable.

**Update from GitHub (recommended)**

1. Open the **Firmware** tab → click **Check for update**.
2. The device queries the GitHub repository for the latest git tag and compares it with the installed version.
3. If a newer version is available, click **Update from GitHub** — the device downloads and flashes it automatically over WiFi, then reboots.

The device must be connected to WiFi for this to work.

**Manual upload**

Expand **Manual upload** and select a `.bin` file from your computer. The file is streamed directly from the browser to the device flash.

**Erase settings option**

Before updating, you can check **Erase all settings after update** to wipe the NVS partition on reboot. This removes WiFi credentials, hotkeys, passwords, TOTP secrets, and the paired keyboard — the device starts from factory defaults after flashing.

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
| `board` | Button GPIO, LED type, LED GPIO, brightness |

Up to **16 hotkey slots** total across passwords, text and TOTP.

---

## Full factory reset

Erases all flash including NVS — removes WiFi, hotkeys, TOTP secrets, jiggler config, paired keyboard, and board config:

```bash
idf.py -p /dev/ttyUSB0 erase-flash
```

Then flash the firmware again and run through first-time setup.

**WiFi-only reset:** hold the button for ≥ 10 seconds. Hotkeys, TOTP secrets, jiggler settings, and board config are preserved.

---

## License

MIT — see [LICENSE](LICENSE).
