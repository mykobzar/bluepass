# bluepass

> Hardware Bluetooth → USB HID bridge with password injection, TOTP codes, FIDO2/Passkey hardware security key and jiggler — firmware for ESP32-S3.

![Version](https://img.shields.io/badge/version-2.1.0-blue)
![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.2%2B-blue)
![Target](https://img.shields.io/badge/target-ESP32--S3-informational)
![USB](https://img.shields.io/badge/Interface-USB-blue?style=flat-square&logo=usb&logoColor=white)
![Bluetooth](https://img.shields.io/badge/Bluetooth-Enabled-0082FC?style=flat-square&logo=bluetooth&logoColor=white)
![HID](https://img.shields.io/badge/Protocol-HID-green?style=flat-square)
![Smart Home](https://img.shields.io/badge/Focus-Smart_Home-success?style=flat-square&logo=homeassistant)
![MQTT](https://img.shields.io/badge/MQTT-Enabled-660066?style=flat-square&logo=mqtt&logoColor=white)


**[⚡ Install in browser](https://mykobzar.github.io/bluepass/)** — no toolchain needed, Chrome/Edge only

---

## What is bluepass?

bluepass is a small hardware device that connects a Bluetooth keyboard to a laptop over USB. It forwards all keystrokes transparently — the laptop sees a standard USB HID keyboard and never knows there is anything in between.

The interesting part is what happens to *certain* key combinations. You assign hotkeys to stored secrets: a password, a one-time code from your authenticator app, or a block of text. When you press the hotkey on your Bluetooth keyboard, bluepass intercepts it and types the secret on the laptop character by character, over USB, without ever exposing it to software on the host.

Starting with **v2.x**, bluepass also acts as a **FIDO2/Passkey hardware security key** — a second USB HID interface presents itself as an authenticator. Store passkeys (resident credentials) directly on the device and confirm authentication with a button press, with no additional hardware required.

There is no driver, no browser extension, and no software to install. Secrets live in the device flash, encrypted at rest. The web management interface is hosted locally on the device and is **never reachable without a deliberate button press** — it activates on demand and shuts itself off after five minutes of inactivity.

### What problem does it solve?

Password managers and authenticator apps require software on the host. On locked-down corporate machines, kiosks, remote-desktop sessions, or shared workstations you often cannot install anything. bluepass requires nothing from the host — it looks like a USB keyboard plus a security key.

It also means your secrets are never typed by software on the host. A keylogger on the host sees keystrokes but has no way to distinguish a real keystroke from a substituted one, and the source of the secret never touches host memory.

---

## What it does

bluepass supports three **connection modes** (configurable in the web interface):

```
BT-USB (default)
Bluetooth keyboard  ──BLE──▶  [ESP32-S3]  ──USB HID──▶  laptop

BT-BT
Bluetooth keyboard  ──BLE──▶  [ESP32-S3]  ──BLE──▶  laptop / BT host

USB-BT
USB keyboard  ──USB HID──▶  [ESP32-S3]  ──BLE──▶  laptop / BT host
```

In all modes, configured hotkey combinations are intercepted and replaced with:

| Substitution | Description |
|---|---|
| **Password** | Stored secret typed as keystrokes; never exposed over the API |
| **Text** | Arbitrary string, including characters the physical keyboard cannot produce |
| **Authenticator code** | Live 6-digit TOTP one-time code — works with Google Authenticator, Microsoft Authenticator, Authy, Apple Passwords, and any TOTP-compatible app |
| **Jiggler** | Toggles periodic keypresses to prevent the laptop from sleeping |
| **Webhook** | Sends an HTTP GET request to a configured URL when a hotkey is pressed |
| **MQTT Out** | Publishes a message to an MQTT topic when a hotkey is pressed |
| **MQTT In** | Sends a keystroke to the host when an MQTT message is received on a subscribed topic |

In addition, bluepass acts as a **FIDO2/Passkey authenticator** on a separate USB HID interface — available in BT-USB mode simultaneously with the keyboard:

| Feature | Description |
|---|---|
| **Passkeys / resident keys** | Store FIDO2 credentials on the device; no server-side credential storage required |
| **clientPIN** | Protect credentials with a PIN |
| **User presence confirmation** | Physical button press (or a configured keyboard hotkey) confirms each authentication request |

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

> **Note for boards with a simple LED (not WS2812):** Set LED type to *Simple* in the Board tab and configure the GPIO and active level. The same state machine works with both LED types; the simple LED shows on/off instead of colour.

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

## Install

### Recommended — web installer

1. Connect the ESP32-S3 board to your computer via USB-C.
2. Open **[https://mykobzar.github.io/bluepass/](https://mykobzar.github.io/bluepass/)** in Chrome or Edge.
3. Choose one of two install options:
   - **Connect & Install** — standard firmware, no encryption.
   - **Install with flash encryption** — recommended for security-sensitive use; enables hardware AES-XTS encryption (see [Flash encryption](#flash-encryption) below).
4. Select the serial port when prompted.

The firmware is downloaded and flashed automatically — no Python, no toolchain, nothing to install.

> **Browser:** Chrome or Edge required (Web Serial API). Firefox and Safari are not supported.  
> **Device not detected?** Hold the **BOOT (GPIO0)** button on the board, plug in USB, then release — this forces the ROM bootloader and makes the port appear.

After flashing, proceed to [First-time setup](#first-time-setup).

---

### Advanced — esptool.py

Use this if the web installer is not available (corporate firewall, unsupported browser, offline environment).

Pre-built binaries are in the [`firmware/`](firmware/) folder. Install the flash tool:

```bash
pip install esptool
```

Flash (replace the port with yours):

```bash
# Linux / macOS
esptool.py --chip esp32s3 --port /dev/ttyUSB0 --baud 460800 write_flash \
  --flash_mode dio --flash_freq 80m --flash_size 4MB \
  0x0     firmware/bootloader-2.1.0.bin \
  0x8000  firmware/partition-table-2.1.0.bin \
  0x10000 firmware/ota_data_initial-2.1.0.bin \
  0x20000 firmware/bluepass-2.1.0.bin

# Windows — use COM3, COM4, etc.
esptool.py --chip esp32s3 --port COM3 --baud 460800 write_flash ^
  --flash_mode dio --flash_freq 80m --flash_size 4MB ^
  0x0     firmware\bootloader-2.1.0.bin ^
  0x8000  firmware\partition-table-2.1.0.bin ^
  0x10000 firmware\ota_data_initial-2.1.0.bin ^
  0x20000 firmware\bluepass-2.1.0.bin
```

> **Windows GUI option:** see [`firmware/README.md`](firmware/README.md) for step-by-step instructions using the Espressif Flash Download Tool (no Python required).

After flashing, proceed to [First-time setup](#first-time-setup).

---

### Advanced — build from source

Use this if you want to modify the firmware or contribute to the project.

#### 1. Install ESP-IDF

Minimum version: **v5.2**. Full instructions: <https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/>

```bash
mkdir -p ~/esp && cd ~/esp
git clone --recursive https://github.com/espressif/esp-idf.git
cd esp-idf && ./install.sh esp32s3
```

#### 2. Clone this repo

```bash
git clone https://github.com/mykobzar/bluepass.git
cd bluepass
```

#### 3. Activate the ESP-IDF environment

Run this in every new terminal session before using `idf.py`:

```bash
. ~/esp/esp-idf/export.sh
```

#### 4. Set the target (once per checkout)

```bash
idf.py set-target esp32s3
```

#### 5. Build

```bash
idf.py build
```

First build: 3–7 minutes. Incremental builds: seconds.

#### 6. Flash

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

> **Tip:** If the device is not detected, hold the **BOOT (GPIO0)** button, connect USB, then release.

#### 7. Verify (optional)

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
2. Open the **Connection** tab → **Scan (4 s)**.
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

The button also serves as the **user-presence confirmation** for FIDO2 authentication requests — a single press confirms the pending operation.

**LED behaviour:**

| State | WS2812 RGB LED | Simple LED | Priority |
|---|---|---|---|
| Web interface active | Solid **blue** | On | 1 (highest) |
| FIDO2 user-presence wait | Solid **amber** | On | 2 |
| WiFi not connected / no credentials | **Red** fast blink (5 Hz) | Fast blink | 3 |
| Jiggler running | **Green** slow blink (~0.35 Hz) | Slow blink | 4 |
| Password / text / TOTP substituted | Single **white** 150 ms flash | 150 ms pulse | — |
| Connected, idle | Off | Off | — |

LED type (RGB / Simple / None), GPIO pin, and brightness are configurable in **Settings → Board**.  
The web interface **turns off automatically after 5 minutes of inactivity** (no browser requests).

---

## Web interface

The interface is divided into five top-level sections. **Hotkeys** and **Settings** each reveal a submenu when selected.

### Main navigation

| Section | Description |
|---|---|
| **Info** | WiFi and Bluetooth connection status with signal strength; live key log |
| **Connection** | Connection mode selector (BT-USB / BT-BT / USB-BT); Bluetooth keyboard scan and pair; BLE diagnostic log |
| **Hotkeys** | All hotkey-based actions (see below) |
| **Passkey** | Confirm-key assignment for FIDO2 user-presence (shown only when FIDO2 is enabled in Settings) |
| **Settings** | Device configuration (see below) |

### Hotkeys submenu

| Tab | Purpose |
|---|---|
| **Text** | Hotkey → arbitrary text substitution |
| **Passwords** | Hotkey → password (masked in the UI, never returned by the API) |
| **Authenticator** | Hotkey → TOTP one-time code; shows device clock sync status |
| **Jiggler** | Jiggler interval, key code, enable/disable hotkeys |
| **Webhook** | Hotkey → HTTP GET request to a configured URL |
| **MQTT Out** | Hotkey → publish to an MQTT topic *(visible only when MQTT Out is enabled)* |
| **MQTT In** | Subscribe topic → send keystroke *(visible only when MQTT In is enabled)* |

Full HID keycode reference: [`docs/keycodes.md`](docs/keycodes.md) — also linked from each hotkey tab.

### Settings submenu

| Tab | Purpose |
|---|---|
| **WiFi** | Change WiFi network (triggers reboot) |
| **Board** | Button GPIO; LED type (RGB / Simple / None), GPIO and brightness |
| **MQTT** | Broker URL, credentials; enable/disable MQTT Out and MQTT In |
| **Firmware** | OTA firmware update |
| **Flash Encryption** | Flash encryption status; switch from Development to Release mode |
| **Passkey** | Enable/disable FIDO2 authenticator; set or change PIN |

---

## Features

### Connection modes

bluepass can operate in three connection modes, selectable in the **Connection** tab (takes effect after reboot):

| Mode | Keyboard input | Computer output | Use case |
|---|---|---|---|
| **BT-USB** (default) | Bluetooth HID | USB HID | Classic bridge — BT keyboard to USB host |
| **BT-BT** | Bluetooth HID | Bluetooth HID | Relay — forward BT keyboard input to a BT host (laptop, tablet, phone) |
| **USB-BT** | USB HID | Bluetooth HID | Reverse bridge — physical USB keyboard to a BT host |

In all modes the hotkey substitution engine, FIDO2 authenticator, jiggler, and all other features remain active.

### FIDO2 / Passkey authenticator

bluepass implements the **CTAP2 standard** (Client to Authenticator Protocol 2), presenting itself to the host as a USB HID security key — the same protocol used by hardware tokens such as YubiKey.

Features:
- **Resident keys (passkeys)** — FIDO2 credentials are stored directly on the device; no server-side credential storage is required
- **clientPIN** — protect stored credentials with a numeric PIN; set and change from **Settings → Passkey**
- **User presence confirmation** — the amber LED lights up when a website or application requests authentication; press the physical button (or a configured keyboard hotkey on the **Passkey** tab) to confirm
- **Diagnostic log** — live CTAP2 command log visible at the bottom of the **Settings → Passkey** tab; useful for debugging

Enable FIDO2: **Settings → Passkey → Enable FIDO2 / Passkey** → Save (triggers reboot).

The FIDO2 interface and the keyboard interface are independent — both are active simultaneously in BT-USB mode. Passkey authentication does not interrupt keyboard input.

> FIDO2 requires BT-USB mode (dual USB HID — keyboard + authenticator). In BT-BT and USB-BT modes the authenticator is unavailable.

### Passwords

Assign a hotkey to a stored password. When the hotkey is pressed on the Bluetooth keyboard the password is typed on the laptop character by character.

- Passwords are **never returned** by the REST API — the endpoint is write-only.
- In the UI passwords are displayed as `***`.

### Text substitutions

Same as passwords but the stored text is visible in the UI. Supports any printable ASCII character and newline. Useful for text snippets, signatures, or characters the physical keyboard layout cannot produce.

### Authenticator app codes

bluepass generates standard TOTP codes (RFC 6238) — the same algorithm used by Google Authenticator, Microsoft Authenticator, Authy, Apple Passwords (iOS 17+ / macOS Sonoma), Bitwarden, 1Password, and any other TOTP-compatible app. If a service offers a QR code for "Google Authenticator", it works with bluepass.

Assign a hotkey to a service secret (base32 string). Pressing the hotkey types the current 6-digit code directly as keystrokes, without any software on the host.

**Requirements:**
- The device must be connected to WiFi.
- The system clock must be synchronised with NTP. The **Authenticator** tab shows the sync status (`Device: synced Δ±Xs`). Codes are not generated if the clock is not synced.

**Import from Google Authenticator:**  
The **Authenticator** tab includes an importer for `otpauth-migration://offline?data=…` links exported by the Google Authenticator app. Secrets from other apps can be entered manually as a base32 string.

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

### Webhooks

Assign a hotkey to an HTTP endpoint. When the hotkey is pressed, bluepass sends an HTTP GET request to the configured URL. An optional `value` parameter can be appended as a query string (`?value=…`).

Use cases: trigger home automation scenes, press a button in a web app, ping a monitoring endpoint.

Up to **4 webhook slots**. Webhooks require the device to be connected to WiFi. TLS (HTTPS) is supported.

### MQTT

bluepass can act as both an MQTT publisher and subscriber. Configuration is shared — a single broker URL, username, and password are used for both directions. MQTT Out and MQTT In can be enabled independently in **Settings → MQTT**.

**MQTT Out** — publish on hotkey:  
Assign a hotkey to a topic + value pair. Pressing the hotkey publishes the message to the MQTT broker.

**MQTT In** — subscribe and send keystrokes:  
Assign a topic (and optional match value) to a keystroke. When a message arrives on the topic (and the value matches, if set), bluepass sends the configured keystroke to the host over USB HID.

Up to **4 MQTT Out slots** and **4 MQTT In slots**.

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

Each slot row in the Text, Passwords, and Authenticator tables shows the current mode as compact labels (`M:exact / M:any`, `R:send / R:keep`). Modes can be changed directly in the Add / Edit form without touching the slot content.

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
| Brightness | 20% | RGB LED brightness (1–100%) |
| Simple GPIO | — | GPIO pin for a plain (non-RGB) LED |
| Active high | Yes | Whether the simple LED is on when GPIO is high |

Changes take effect after reboot.

### Flash encryption

bluepass supports ESP32 hardware AES-XTS flash encryption. The **Settings → Flash Encryption** tab shows the current encryption mode and explains the consequences of each state:

| Mode | Flash content | UART flash | JTAG |
|---|---|---|---|
| Disabled | Plaintext | Allowed | Allowed |
| Development | Encrypted | Allowed | Allowed |
| Release | Encrypted | **Disabled permanently** | **Disabled permanently** |

**Enabling encryption — recommended method (web flasher):**  
Open **[https://mykobzar.github.io/bluepass/](https://mykobzar.github.io/bluepass/)** and click **Install with flash encryption**. No Python, no drivers. The browser flashes the encryption-enabled firmware directly; on first boot the bootloader generates an AES-XTS key, burns it into eFuse, encrypts the flash, and reboots into Development mode automatically.

> **This is irreversible.** Once encryption is enabled it cannot be turned off.

**Advanced — manual esptool.py method:**  
Use the `-enc` firmware variant from [`firmware/`](firmware/) — see [`firmware/README.md`](firmware/README.md) for the full flash command.

**Switching to Release mode:**  
After enabling encryption, use **Settings → Flash Encryption → Switch to Release Mode** to permanently disable UART flashing and JTAG. This burns additional eFuse bits that cannot be undone.

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

Before updating, you can check **Erase all settings after update** to wipe the NVS partition on reboot. This removes WiFi credentials, hotkeys, passwords, TOTP secrets, paired keyboard, and stored passkeys — the device starts from factory defaults after flashing.

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

Full list: [`docs/keycodes.md`](docs/keycodes.md).

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
| `hotkeys` | Up to 32 slots: type, trigger combo, payload/secret, label, match/replace mode |
| `jiggler` | Interval, key, on/off hotkeys, enabled state |
| `ble` | Paired keyboard address + name |
| `board` | Button GPIO, LED type, LED GPIO, brightness |
| `webhook` | Up to 4 webhook slots (URL, value, trigger) |
| `mqtt` | Broker URL/credentials, enabled flags + up to 4 Out slots + up to 4 In slots |
| `conn` | Connection mode (BT-USB / BT-BT / USB-BT) |
| `fido2` | FIDO2 enabled flag, PIN hash, up to 8 resident credentials (passkeys) |

**Slot limits:** 32 hotkey slots (passwords + text + TOTP combined) · 4 webhook slots · 4 MQTT Out slots · 4 MQTT In slots · 8 passkey resident credentials.

---

## Full factory reset

Erases all flash including NVS — removes WiFi, hotkeys, TOTP secrets, jiggler config, paired keyboard, board config, and stored passkeys:

```bash
idf.py -p /dev/ttyUSB0 erase-flash
```

Then flash the firmware again and run through first-time setup.

**WiFi-only reset:** hold the button for ≥ 10 seconds. Hotkeys, TOTP secrets, jiggler settings, board config, and FIDO2 credentials are preserved.

---

## License

MIT — see [LICENSE](LICENSE).
