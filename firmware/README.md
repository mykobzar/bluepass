# bluepass — pre-built firmware

This folder contains ready-to-flash binaries for **ESP32-S3 SuperMini**.  
No toolchain installation required — only `esptool.py` (Python package).

## Versions

| Version | Date | Notes |
|---|---|---|
| **0.9.11** | 2026-05-23 | Fix "Failed to fetch": move GitHub version check to device side (/api/ota/check) |
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

## Files — v0.9.11

| File | Flash address | Description |
|---|---|---|
| `bootloader-0.9.11.bin` | `0x0` | Second-stage bootloader |
| `partition-table-0.9.11.bin` | `0x8000` | Partition layout (NVS + dual OTA slots) |
| `ota_data_initial-0.9.11.bin` | `0x10000` | OTA slot selector (initial state) |
| `bluepass-0.9.11.bin` | `0x20000` | Main application |

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

Open a terminal in this `firmware/` folder and run (replace the port and version as needed):

```bash
esptool.py \
  --chip esp32s3 \
  --port /dev/ttyUSB0 \
  --baud 460800 \
  write_flash \
  --flash_mode dio \
  --flash_freq 80m \
  --flash_size 4MB \
  0x0     bootloader-0.9.11.bin \
  0x8000  partition-table-0.9.11.bin \
  0x10000 ota_data_initial-0.9.11.bin \
  0x20000 bluepass-0.9.11.bin
```

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
  0x0     bootloader-0.9.11.bin ^
  0x8000  partition-table-0.9.11.bin ^
  0x10000 ota_data_initial-0.9.11.bin ^
  0x20000 bluepass-0.9.11.bin
```

Replace `COM3` with your actual port number.

---

## Flash — Windows (GUI, no Python required)

1. Download the [Espressif Flash Download Tool](https://www.espressif.com/en/support/download/other-tools) and unzip it.
2. Run `flash_download_tool_x.x.x.exe`.
3. Select **Chip Type: ESP32-S3**, **WorkMode: Develop**, click OK.
4. In the file list add all four files with their addresses:

   | File | Address |
   |---|---|
   | `bootloader-0.9.11.bin` | `0x0` |
   | `partition-table-0.9.11.bin` | `0x8000` |
   | `ota_data_initial-0.9.11.bin` | `0x10000` |
   | `bluepass-0.9.11.bin` | `0x20000` |

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
