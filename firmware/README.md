# bluepass — pre-built firmware

This folder contains ready-to-flash binaries for **ESP32-S3** boards.  
No toolchain installation required — only `esptool.py` (Python package).

Compatible boards: ESP32-S3 SuperMini, Seeed XIAO ESP32-S3, Adafruit QT Py ESP32-S3, Adafruit Feather ESP32-S3, ESP32-S3-DevKitC-1, and any other ESP32-S3 board with native USB OTG.  
After flashing, use **Settings → Board** to configure the correct GPIO pins for your board's button and LED.

## Versions

| Version | Date | Notes |
|---|---|---|
| **2.0.15-beta** | 2026-05-28 | FIDO2: add pin:0/pin:v/hwm:0/hwm:1 crash markers — ecdh:0 absent in v2.0.14 log means crash is before ECDH or in uxTaskGetStackHighWaterMark itself |
| **2.0.14-beta** | 2026-05-28 | FIDO2: add ecdh:0/1 crash markers — 32 KB fixed getKeyAgreement; crash moved to mbedtls_ecdh_compute_shared in changePIN path (before sP:0 marker) |
| **2.0.13-beta** | 2026-05-28 | FIDO2: stack 16→32 KB — software ECC (no hardware P-256 on ESP32-S3) overflows canary during getKeyAgreement; add gKA:0/1/2 markers + reorder hwm before diag_append |
| **2.0.12-beta** | 2026-05-28 | FIDO2: expand crash buffer 128→512 bytes + add HWM per packet — previous buffer filled before changePIN markers were written |
| **2.0.11-beta** | 2026-05-27 | FIDO2: add caller-chain crash markers (disp:ret, proc:ret, task:done), stack HWM logging, stack 12→16 KB — pinpoint changePIN crash location |
| **2.0.10-beta** | 2026-05-27 | FIDO2: fix changePIN crash — diag_append race on dual-core ESP32-S3 corrupted BSS (portMUX spinlock) |
| **2.0.10-beta** | 2026-05-27 | FIDO2: fix changePIN crash (stack 8→12 KB, PIN hash LEFT(SHA-256,16), mbedtls_md_setup error check, RTC crash log) |
| **2.0.10-beta** | 2026-05-27 | FIDO2: fix CTAPHID_INIT response CID — send on broadcast channel per CTAP2.0 §8.1.5.4 |
| **2.0.10-beta** | 2026-05-27 | FIDO2: extend diagnostic log to CTAPHID layer (packet RX, INIT, errors) |
| **2.0.10-beta** | 2026-05-27 | FIDO2: add diagnostic log to Passkey settings tab (GET/DELETE /api/passkey/diag) |
| **2.0.5-beta** | 2026-05-27 | FIDO2: fix getKeyAgreement COSE key alg=-25 (ECDH-ES+HKDF-256, was -7/ES256); add CBOR command logging |
| **2.0.4-beta** | 2026-05-27 | FIDO2: fix clientPIN key numbers for setPIN/changePIN (CTAP2 spec: pinAuth=0x04, newPinEnc=0x05, pinHashEnc=0x06); fix changePIN HMAC to cover newPinEnc\|\|pinHashEnc |
| **2.0.3-beta** | 2026-05-27 | FIDO2: fix `uv: false` in getInfo (no biometric UV), fix unknown command error code to CTAP1_ERR_INVALID_COMMAND |
| **2.0.2-beta** | 2026-05-27 | FIDO2: fix getInfo CBOR keys, CANCEL handling, UV/PIN follows relying party request; confirm key via keyboard hotkey; Passkey page in root nav; orange LED during UP wait |
| **2.0.1-beta** | 2026-05-26 | FIDO2/Passkey authenticator: CTAP2 over USB HID, clientPIN, resident keys, user-presence confirm via button; dual USB HID interfaces (keyboard + FIDO2); Passkey tab in web UI |
| **2.0.0-beta** | 2026-05-26 | v2.0 foundation: separate installer page, multi-role connection modes (BT-USB, BT-BT, USB-BT) |
| **1.1.2** | 2026-05-25 | UI polish: Hotkeys/Flash Encryption rename, mode labels, tab descriptions, key code reference page, status on WiFi/MQTT tabs, BLE log spoiler; LED gamma correction (brightness default 4→20%) |
| **1.0.0** | 2026-05-25 | First stable release; all 0.9.x features promoted to 1.0: BT→USB bridge, passwords, TOTP, jiggler, OTA, Info/Security/Board tabs |
| 0.9.14 | 2026-05-23 | Board tab: configurable button GPIO, RGB LED with brightness, simple LED support; expanded README with compatible boards |
| 0.9.13 | 2026-05-23 | Info tab: rename Log→Info, add WiFi and BLE status cards with RSSI |
| 0.9.12 | 2026-05-23 | Security tab: explain Disabled state, show UART flash instructions to enable encryption |
| 0.9.11 | 2026-05-23 | Fix "Failed to fetch": move GitHub version check to device side (/api/ota/check) |
| 0.9.10 | 2026-05-23 | Fix OTA rollback: disable abort-on-no-encryption check that blocked OTA on unencrypted devices |
| 0.9.9 | 2026-05-23 | Match/replace modes per slot; Edit slots; secure deletion; flash encryption; Settings sub-menu |
| 0.9.8 | 2026-05-20 | Docs: update CLAUDE.md and README for WS2812 RGB LED |
| 0.9.7 | 2026-05-20 | RGB WS2812 LED on GPIO48: blue=web UI, red blink=no WiFi, green blink=jiggler |
| 0.9.6 | 2026-05-20 | Fix LED GPIO pin: use GPIO21 (correct pin for ESP32-S3 SuperMini) |
| 0.9.5 | 2026-05-20 | Fix LED polarity (active-LOW): solid ON when web UI active, fast blink when WiFi disconnected |
| 0.9.4 | 2026-05-20 | Logout button; fix LED race condition; fix OTA (rollback state + upload stack overflow) |
| 0.9.3 | 2026-05-20 | LED rework; web UI auto-off (5 min idle); fix USB HID character skip |
| 0.9.2 | 2025-05-20 | Initial release |

---

## Files — v2.0.15-beta

Two variants are available.  Use the **standard** variant for a normal install.  Use the **encrypted** variant if you want hardware-level AES-XTS flash encryption.

### Standard (no encryption)

| File | Flash address | Description |
|---|---|---|
| `bootloader-2.0.15-beta.bin` | `0x0` | Second-stage bootloader |
| `partition-table-2.0.15-beta.bin` | `0x8000` | Partition layout (NVS + dual OTA slots) |
| `ota_data_initial-2.0.15-beta.bin` | `0x10000` | OTA slot selector (initial state) |
| `bluepass-2.0.15-beta.bin` | `0x20000` | Main application |

### With flash encryption (recommended)

| File | Flash address | Description |
|---|---|---|
| `bootloader-2.0.15-beta-enc.bin` | `0x0` | Bootloader with encryption support |
| `partition-table-2.0.15-beta-enc.bin` | `0x8000` | Partition layout |
| `ota_data_initial-2.0.15-beta-enc.bin` | `0x10000` | OTA slot selector |
| `bluepass-2.0.15-beta-enc.bin` | `0x20000` | Main application (encryption-enabled build) |

> **Note:** v2.x is a beta branch — FIDO2/Passkey support is in active development.  For stable production use, see v1.1.2 below.

---

## Files — v1.1.2

Two variants are available.  Use the **standard** variant for a normal install.  Use the **encrypted** variant if you want hardware-level AES-XTS flash encryption (recommended for security-sensitive deployments).

### Standard (no encryption)

| File | Flash address | Description |
|---|---|---|
| `bootloader-1.1.2.bin` | `0x0` | Second-stage bootloader |
| `partition-table-1.1.2.bin` | `0x8000` | Partition layout (NVS + dual OTA slots) |
| `ota_data_initial-1.1.2.bin` | `0x10000` | OTA slot selector (initial state) |
| `bluepass-1.1.2.bin` | `0x20000` | Main application |

### With flash encryption (recommended)

> **Irreversible** — once flashed, encryption cannot be disabled. UART flashing remains possible in Development mode but is permanently blocked if you later switch to Release mode from **Settings → Flash Encryption**.

| File | Flash address | Description |
|---|---|---|
| `bootloader-1.1.2-enc.bin` | `0x0` | Bootloader with encryption support |
| `partition-table-1.1.2-enc.bin` | `0x8000` | Partition layout |
| `ota_data_initial-1.1.2-enc.bin` | `0x10000` | OTA slot selector |
| `bluepass-1.1.2-enc.bin` | `0x20000` | Main application (encryption-enabled build) |

On first boot the bootloader generates a unique AES-XTS key, burns it into eFuse, encrypts the entire flash, then reboots into Development mode automatically.  All subsequent OTA updates are encrypted transparently.

**Easiest way to install the encrypted variant** — use the browser web flasher (no Python required):  
→ [mykobzar.github.io/bluepass](https://mykobzar.github.io/bluepass/) → click **Install with flash encryption**

---

## Files — v1.0.0

| File | Flash address | Description |
|---|---|---|
| `bootloader-1.0.0.bin` | `0x0` | Second-stage bootloader |
| `partition-table-1.0.0.bin` | `0x8000` | Partition layout (NVS + dual OTA slots) |
| `ota_data_initial-1.0.0.bin` | `0x10000` | OTA slot selector (initial state) |
| `bluepass-1.0.0.bin` | `0x20000` | Main application |

All four files must be flashed together on a **blank or previously erased** device.

---

## Install esptool

```bash
pip install esptool
```

Works on Linux, macOS and Windows (Python 3.7+ required).  
Windows users who prefer a GUI can use the [Espressif Flash Download Tool](https://www.espressif.com/en/support/download/other-tools) — see the Windows section below.

---

## Find the serial port

Connect the ESP32-S3 SuperMini via USB-C.

> **Device not detected?**  
> Hold the **BOOT (GPIO0)** button on the board, plug in USB, then release.  
> This forces the ROM bootloader and makes the port appear.

### Linux

```bash
ls /dev/ttyUSB* /dev/ttyACM*
# typically:  /dev/ttyUSB0  or  /dev/ttyACM0
```

If you get a permission error when flashing:

```bash
sudo usermod -aG dialout $USER
# log out and log back in, then retry
```

### macOS

```bash
ls /dev/cu.usbserial* /dev/cu.SLAB* /dev/cu.usbmodem*
# typically:  /dev/cu.usbserial-0001  or  /dev/cu.SLAB_USBtoUART
```

### Windows

Open **Device Manager → Ports (COM & LPT)**.  
The device appears as `Silicon Labs CP210x` or `USB Serial Device` — note the `COMx` number.

---

## Flash — Linux and macOS

### Standard (no encryption)

Open a terminal in this `firmware/` folder and run (replace the port as needed):

```bash
esptool.py \
  --chip esp32s3 \
  --port /dev/ttyUSB0 \
  --baud 460800 \
  write_flash \
  --flash_mode dio \
  --flash_freq 80m \
  --flash_size 4MB \
  0x0     bootloader-1.1.2.bin \
  0x8000  partition-table-1.1.2.bin \
  0x10000 ota_data_initial-1.1.2.bin \
  0x20000 bluepass-1.1.2.bin
```

### With flash encryption (recommended, advanced)

> **Prefer the [browser web flasher](https://mykobzar.github.io/bluepass/)** — it requires no Python and handles the encryption-enabled binary automatically.

If you need to flash manually, use the `-enc` variant instead:

```bash
esptool.py \
  --chip esp32s3 \
  --port /dev/ttyUSB0 \
  --baud 460800 \
  write_flash \
  --flash_mode dio \
  --flash_freq 80m \
  --flash_size 4MB \
  0x0     bootloader-1.1.2-enc.bin \
  0x8000  partition-table-1.1.2-enc.bin \
  0x10000 ota_data_initial-1.1.2-enc.bin \
  0x20000 bluepass-1.1.2-enc.bin
```

After flashing, the bootloader generates an AES-XTS key, burns it into eFuse, and reboots into Development mode automatically.

Expected output:

```
esptool.py v4.x
Serial port /dev/ttyUSB0
Connecting....
Chip is ESP32-S3
...
Writing at 0x00020000... (100 %)
Hash of data verified.
Leaving...
Hard resetting via RTS pin...
```

After flashing the device reboots automatically.

---

## Flash — Windows (command line)

Open **Command Prompt** or **PowerShell** in this folder:

```bat
esptool.py ^
  --chip esp32s3 ^
  --port COM3 ^
  --baud 460800 ^
  write_flash ^
  --flash_mode dio ^
  --flash_freq 80m ^
  --flash_size 4MB ^
  0x0     bootloader-1.1.2.bin ^
  0x8000  partition-table-1.1.2.bin ^
  0x10000 ota_data_initial-1.1.2.bin ^
  0x20000 bluepass-1.1.2.bin
```

Replace `COM3` with your actual port number.  
For the encrypted variant use the `-enc` filenames (see Linux/macOS section above), or use the [browser web flasher](https://mykobzar.github.io/bluepass/) — no Python or drivers required.

---

## Flash — Windows (GUI, no Python required)

1. Download the [Espressif Flash Download Tool](https://www.espressif.com/en/support/download/other-tools) and unzip it.
2. Run `flash_download_tool_x.x.x.exe`.
3. Select **Chip Type: ESP32-S3**, **WorkMode: Develop**, click OK.
4. In the file list add all four files with their addresses:

   | File | Address |
   |---|---|
   | `bootloader-1.1.2.bin` | `0x0` |
   | `partition-table-1.1.2.bin` | `0x8000` |
   | `ota_data_initial-1.1.2.bin` | `0x10000` |
   | `bluepass-1.1.2.bin` | `0x20000` |

5. Set **COM** to your port, **BAUD** to `460800`.
6. Set **SPI SPEED: 80 MHz**, **SPI MODE: DIO**, **FLASH SIZE: 4MB**.
7. Click **START**.

---

## Erase before reflashing (recommended when upgrading from a different version)

```bash
esptool.py --chip esp32s3 --port /dev/ttyUSB0 erase_flash
```

> **Warning:** this erases all settings — WiFi credentials, hotkeys, TOTP secrets, paired keyboard.  
> For upgrades within the same major version (e.g. 0.9.x → 0.9.y) erase is usually not required.

Then run the full flash command above.

---

## After flashing

1. The device boots and starts a WiFi access point named **`bluepass`** (open network).
2. Connect to it and open `http://192.168.4.1`.
3. Enter your WiFi credentials on the **WiFi** tab → **Save & Reboot**.
4. After reboot, find the device IP in your router and **short-press the GPIO0 button** to activate the web interface.

Full setup guide: [README.md](../README.md)
