# Ideas — considered, not built

A backlog of things that were investigated properly but deliberately left unbuilt.
Each entry records what the idea is, why it might be worth doing, and what it costs
or breaks — so the analysis does not have to be repeated.

Nothing here is scheduled.

---

## Typing arbitrary text on a host with an unknown layout

The whole group exists because HID transmits **key positions**, not characters.
What a position produces is decided by the host's active layout, which the device
cannot read — the only host-to-device channel is the LED output report (Caps / Num /
Scroll), and it says nothing about the layout.

v2.1.12 covers part of the problem with Alt+numpad modes. What remains is below.

### 1. Per-layout scan-code tables ⭐ the main one

Ship a table per target layout (US, DE, FR, RU, UA, …) mapping a Unicode code point
to `(scan code, modifier)`, and let the user pick which layout the target computer
uses. This is what everyone else does — Ducky Script language files, Mooltipass,
OnlyKey.

The insight that makes it worth building: **when a Cyrillic layout is active,
Cyrillic is trivial to type** — `ю` is simply the position of the US `.` key. It is
Latin that becomes impossible. So a ЙЦУКЕН table turns the hardest case (Cyrillic
text) into the cheapest one: plain scan codes, full speed, no Alt, no Num Lock, no
registry changes on the host.

- Size: roughly 33 letters × 2 cases + punctuation per layout, on the order of a
  kilobyte of flash each.
- The modifier field must carry **AltGr** (right Alt): on European layouts `@ { } \`
  are unreachable without it.
- UI: a "host layout" select next to the existing typing mode.
- Limitation: the user has to state the layout. The device cannot detect it, ever.

### 2. Deterministic layout switching (prefix / suffix keys per slot)

Blind switching (`Alt+Shift`, `Win+Space`) is a lottery — it cycles an unknown list
from an unknown starting point. But Windows can bind a hotkey to a **specific**
input language: *Advanced keyboard settings → Input language hot keys*, where
`Ctrl+Shift+0…9` switches straight to one language instead of cycling.

Given that, a slot could carry two optional key sequences: one sent before the
payload, one after. "Switch to English, type, switch back to Russian" becomes
deterministic with no guessing.

- Cheap to implement: two optional fields on the slot, reused hotkey parser.
- Needs a one-time, entirely standard configuration step on the host.
- This is the only mechanism that closes the last gap in the matrix below.

### 3. Layout-invariant alphabet (ModHex)

YubiKey types OTPs using only `cbdefghijklnrtuv` — letters whose positions coincide
across virtually all Latin layouts, so no layout knowledge is needed at all.

Elegant, but it only applies when **we choose the payload**. Arbitrary text and
other people's passwords have their alphabet fixed for us, and our TOTP codes are
digits, which are already near-invariant. Recorded for completeness; no use here.

### 4. Per-device layout on Linux hosts

X11 and Wayland can assign a layout to one specific input device (`xinput`, or a
udev rule), which makes scan codes from bluepass deterministic regardless of what
the user has selected globally. Host-side configuration, but legitimate and
one-time — worth a paragraph in the README rather than firmware work.

### 5. Alt+numpad speed

Each character costs five keystrokes in decimal mode and up to seven in hex, at
`TYPE_INTER_KEY_MS = 10` per transition. Long strings type visibly slowly. Worth
measuring whether the numpad digits tolerate a shorter gap than ordinary keys
before touching it — the v2.1.12 corruption showed this path is timing-sensitive.

### Where each case lands today

| Text | Host layout | Mechanism | Status |
|---|---|---|---|
| ASCII | any | Alt decimal | works (v2.1.11) |
| Cyrillic | Cyrillic | ЙЦУКЕН scan-code table | **idea 1** |
| Cyrillic | Latin | Alt hex | works (v2.1.12) |
| `ü` and Latin-1 | Latin | Alt decimal or hex | works |
| `ü` | Cyrillic | layout switch | **idea 2** |

---

## Transport and connectivity

### 6. Web interface over USB Ethernet (NCM / RNDIS)

Present the board to the connected computer as a network adapter and serve the
config UI over the same cable. NCM needs no driver on Windows 11, macOS or Linux;
RNDIS covers older Windows. Both classes are already vendored in TinyUSB
(`class/net/ncm_device.c`, `ecm_rndis_device.c`).

Why it is attractive: it removes WiFi from the critical path entirely. The
2026-08-16 session lost hours to a link at RSSI −90 that could not carry a
200-byte packet, which made the web UI unusable through no fault of the firmware.
Over USB there is no signal strength, no router, and no dependency on the network
the board happens to sit on.

Two caveats:

- **Endpoint budget.** EP1 IN (keyboard), EP2 IN and EP2 OUT (FIDO2) are taken;
  NCM wants a notification IN plus a bulk IN/OUT pair. Whether the composite fits
  the ESP32-S3 OTG core needs checking before anything else.
- **Security model.** Today the config surface is reachable only over WiFi and only
  after a button press. Over USB it would become reachable from the very machine
  the device types passwords into. Keeping the button gate mandatory preserves the
  principle, but the change deserves thought rather than a shrug.

### 7. Lower TCP MSS as a weak-link fallback

`CONFIG_LWIP_TCP_MSS` is 1440. On a marginal link the measured loss was strongly
size-dependent (0% at 56 B, 50% at 150 B, 100% at 200 B and above), so cutting the
MSS to a few hundred bytes would let data through where full-size segments cannot.
A crutch, not a fix — the real answer is moving the board or the access point — but
worth keeping in the drawer.

### 8. Cache headers for the web page

The page is gzipped to ~24 KB but is re-sent in full on every reload. An `ETag`
derived from the build, or a short `Cache-Control: max-age`, would make reloads a
304. Small win now that the page is small; more interesting if the page grows.

---

## Known defects left unfixed

### 9. The `ENC_CHANGE` dead end in the BLE host

`ble_hid_host.c` handles a failed encryption change by logging *"keyboard rejected
our LTK"* and doing nothing else. The link stays up, discovery never starts, no
keys arrive — the device looks connected but is deaf, and only a manual re-pair
recovers it.

The correct behaviour is next to it already: the `REPEAT_PAIRING` handler deletes
the stale bond and retries. The failure branch should do the same.

### 10. `s_ws_clients` has no lock

The WebSocket broadcast list is touched from three contexts (the httpd thread on
handshake and on socket close, the hotkey task on broadcast). The v2.1.10 fd
validation makes the race benign — the worst case is a dropped key-log frame rather
than a frame written into a stranger's socket — but it is still a race. Doing it
properly means copying the list under a spinlock and sending outside it, since
socket I/O must not happen inside a critical section.

### 11. The NVS partition is small

24 KB total (`nvs, data, nvs, 0x9000, 0x6000`), now shared by WiFi credentials,
32 hotkey slots, TOTP secrets, FIDO2 resident keys, board config, webhooks, MQTT
and — since v2.1.10 — BLE bonds. Nothing is near the limit as far as anyone has
checked, but nothing checks. If it ever fills, pairing is likely to break first.

---

## Device behaviour

### 12. Local time instead of UTC

`TZ` is never set anywhere in the firmware, so the device runs on UTC and the Info
tab labels it as such. A timezone setting (fixed offset, or a POSIX TZ string with
DST rules) would make the clock and the key log read naturally. TOTP is unaffected
either way — it works off the epoch.

### 13. Idle timeout measured from the button press

The web UI currently shuts down five minutes after the last *request*, and any
background poll refreshes that. Measuring from the button press instead would give
a hard upper bound on how long the config surface can stay exposed, which is the
stricter reading of what the button is for. The cost is that the interface can
close while someone is still typing a long password into it.
