# 100% Clock — a day, displayed as a percentage

An offline-first **landscape (320×240)** percentage clock for the **Sunton ESP32-2432S022C**
(ESP32-WROOM-32, native 240×320 ST7789 i80 LCD, CST816S capacitive touch).
One civil day runs from **00.00% to 99.99%**, with **50.00% at 12:00**.
The percentage dominates every clock/alarms/timer screen; the conventional
HH:MM and ISO 8601 date are small and secondary. Big numerals use embedded
rasterized typographic glyphs, not seven-segment blocks.
No phone app, subscription or cloud account is required.

> **Prototype, not yet tested on the author's physical board.** Check the
> printed PCB model before flashing. S022C is *not* the 2.8-inch SPI CYD.

## Quick flash (ESP-IDF v5.4)

```sh
git clone https://github.com/ballerburg9005/cyd-100-percent-clock
cd cyd-100-percent-clock
. ~/esp/esp-idf/export.sh
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash
```

The flash command intentionally does **not** enter the serial monitor, so
it returns control to your shell immediately. If you want debug output:

```sh
idf.py -p /dev/ttyUSB0 monitor
```

Exit monitor with **Ctrl+]**, or **Ctrl+T**, release, then **X**. On German
keyboards Ctrl+] can be difficult to type. Ctrl+C is **not** the normal
monitor exit shortcut. If the terminal consumes both shortcuts, close the
monitor from another terminal or simply use flash without monitor.

No Wi-Fi password is compiled into the firmware. On first boot, it starts a
10-minute setup AP automatically and displays the setup screen without requiring
navigation. You can reopen or restart it via **MENU → WIFI → PORTAL**. The screen
stays bright during setup, with an AP countdown, SSID, device-specific eight-letter
**lowercase** WPA2 password, and URL. The generated password is saved so it stays
the same when the portal is restarted, including after reboot. The AP name is
`100PERCENT-XXXX-SETUP` (where XXXX identifies the device), avoiding the old AP
name cached with a different password on your phone. Connect your phone to it
and browse **http://192.168.4.1/**. Enter home Wi-Fi credentials, POSIX time-zone
and a short gift message. The device restarts when you save.
Alternatively use **WIFI → SSID / PASS** for the on-screen touch keyboard;
the portal is generally easier for long mixed-case passwords.

Set the time without Wi-Fi using **MENU → SET TIME**; the clock also reads
an optional DS3231 RTC at startup. It uses SNTP when online and writes the
UTC clock to DS3231 after synchronization or manual time changes.

### Time convention

```text
percent_basis_points = floor((local_hour*3600 + minute*60 + second) * 10000 / 86400)
display = basis_points / 100, with exactly two decimals
00:00 -> 00.00%; 06:00 -> 25.00%; 12:00 -> 50.00%; 18:00 -> 75.00%.
```

Two decimals here mean **0.01% of one day = 8.64 seconds**, not a minute.
Civil local time is used even across daylight-saving transitions (noon is
always 50%); the percentage can repeat or skip a small interval on a DST
change. The display never prematurely rolls over to 100.00%.

### Controls in the landscape firmware

- **Clock:** balanced vertically, with a four-pixel gap between the large
  `XX.XX%` characters. The alarm's decimal time and a bell appear in the
  top row when enabled, between the small status and MENU. Date, HH:MM
  and weekday share one line (e.g. `2026-11-17 01:24 Tuesday`).
  The progress bar remains 11px.
- **Alarm:** local daily time shown as `XX.XX%` with smaller digits
  to clear the local-time caption. Tap ±0.01% or ±1.00%; holding a key
  accelerates repeat after 0.7s, increasing further after 2.3s. Flash
  writes are deferred until the adjustment ends. 0.01% of a day is 8.64s.
  It uses a gentle ascending three-note tone on the onboard speaker.
- **Kitchen timer:** `X.XX%` duration (0.01%–9.99% of one day), adjustable
  in ±0.01% or ±0.10% steps. **RESET** restores exactly five minutes
  (300 seconds), shown as approximately `0.35%`. The underlying timer stays
  exact. The display is decimal only: no redundant MM:SS or explanatory row.
- **Set Time:** `[X] USE NTP` checkbox defaults ON; uncheck for the
  symmetrically spaced offline date/time editor. Year is shown as `26`
  rather than `2026` but the full year is retained internally; the
  enlarged **SAVE TIME** button sits at bottom-left. DS3231, when fitted,
  backs up UTC independently of display time zone.
- **Wi-Fi:** touchscreen keyboard uses inset 38×25-pixel keys, a visible
  plaintext input and character count, DELETE on the left, SHIFT in the
  middle and SAVE on the right. Supports 8–63-character WPA2 passphrases
  or **exactly 64 hexadecimal characters** (the WPA2 raw PSK format);
  invalid keys are rejected without deleting the typed text.
  **WIFI → SCAN** lists nearby SSIDs with their exact case, so users need not
  guess capitalization; manual typing is still available. The phone/PC
  setup portal at `http://192.168.4.1/` is much faster for long keys:
  connect to the displayed temporary 10-minute WPA2 access point and paste
  the full key there. This uses AP-only mode on 2.4 GHz channel 6 to keep the
  setup network visible; the clock temporarily disconnects from home Wi-Fi
  until the portal closes or you save. Touching PORTAL again restarts it with
  the same lowercase 8-character password. Use only on a trusted local device.
- **Contact:** XMPP JID and email address configured through portal. Place a
  **24-bit uncompressed BMP named `face.bmp`** in the microSD root to show
  a local thumbnail. No personal picture/details are bundled in firmware.
- **Look:** reworked muted, high-contrast palettes **SLATE**, **MOSS**,
  **PAPER** and **BLACK**. BLACK uses literal RGB565 zero as background,
  with cool-white ink and matching subdued controls. LCD colour inversion
  is disabled so black does not display white. The DARK button selects BLACK
  immediately, while NEXT COLOR cycles all four. Backlight dims
  after 40 seconds of idle time; the LCD stays on.
- **Files:** optional TF/microSD root browser. Reads plain `.txt`, displays
  uncompressed 24-bit `.bmp`, and **lists** `.mp3` filenames. MP3 playback,
  folders, JPEG/PNG, EPUB and other binary decoders are deliberately absent.
  Inserting a card is optional: missing cards must not prevent boot.

**Hardware notes:** The LCD is rotated to landscape; default touch mapping
is native CST816S portrait (x: 0..239, y: 0..319) → landscape
`x=raw_y, y=239-raw_x` on the currently tested orientation. After the
rotation/mirror mapping, a configurable **Touch edge expansion** (112% by
default) stretches coordinates slightly away from the center and clamps
them at the screen boundary. This is a provisional correction for the
reported edge-shortfall, **not** a physical calibration. Change it under
`idf.py menuconfig` → **100 Percent Clock board adjustments** or set it to
100 to disable the correction. If taps are mirrored, use the separate
touch mirror/swap options. The visible touch dot helps diagnose this.

**Not implemented:** streaming radio, XMPP messaging, RSS/news, weather,
audio decoding, or battery percentage. These require separate network/audio
engines, security, and board-specific testing; no fake UI features are added.

### Pins for ESP32-2432S022C

| Function | GPIO / I2C |
| --- | --- |
| ST7789 i80 D0..D7 | 15,13,12,14,27,25,33,32 |
| WR/DC/CS/RD | 4/16/17/2 |
| Backlight | GPIO0 (PWM; ESP32 boot strap) |
| Touch CST816S | I2C 0x15, SDA21/SCL22 |
| Optional DS3231 RTC | I2C 0x68, same SDA21/SCL22 |
| Mono amplifier | GPIO26 |
| microSD (reserved) | CS5, CLK18, MISO19, MOSI23 |

The touch and RTC are different I2C addresses; wire them **in parallel**,
not in series. The RTC needs **3.3 V, GND, SDA, SCL**. GPIO21/22 may
require soldering to labelled pads because this board has few free headers.
Do not wire the RTC to TFT pins. Verify exact board revision before soldering.

**DS3231 backup-cell safety:** some cheap ZS-042 modules contain a
trickle-charge circuit intended for rechargeable LIR2032 cells. Disable
that charger before inserting a non-rechargeable CR2032. Do not apply 5 V
to the shared 3.3 V touch I2C bus.

### Battery / power

A **3.7 V 103450 LiPo** is a *single-cell battery*, not a regulated 3.3 V
supply. Connect only through a **verified battery/charger connector** with
correct polarity and charge protection. Do not attach the raw cell to ESP32
3V3, a GPIO, or an unknown JST socket. Check the exact PCB charger circuit
before using the battery, and confirm cell protection and charging-current
compatibility. The DS3231's button-cell backup is **separate**.

The LCD backlight + Wi-Fi consume substantially more power than the RTC.
The firmware dims the backlight after inactivity and **turns Wi-Fi fully off**
between five-minute NTP synchronizations, with a 35-second maximum active
attempt if the router/time server cannot be reached. Before the first
successful sync, two quick retries are scheduled after 20 seconds, then
the radio falls back to five-minute intervals to preserve battery.
The clock counts time locally between polls. Wi-Fi scan and setup AP
are explicit exceptions.
An always-visible LCD on a ~2500 mAh cell is generally an
**hours-to-day** project rather than weeks. Actual runtime must be measured.
USB power is the realistic mode for permanent full-brightness operation.

### Case concept

Landscape desk clock with 3D-printed wedge stand at ~15° tilt, front
window for 2.2-inch display, recessed USB access and ventilation, a
removable rear battery/RTC compartment, and optional 35 mm photo holder.
Leave clearance from LiPo to heat sources and avoid compression or puncture.

### Known limitations / test plan

1. Verify LCD rotation/color and CST816S touch coordinates on actual S022C.
2. Check NTP, reboot without Wi-Fi, and optional DS3231 retention.
3. Test AP portal and visible-password keyboard with real WPA2 credentials.
4. Validate that idle dim / speaker alarm work and do not cause boot issues.
5. Measure battery current at bright/dim/dark settings before building an
   enclosed rechargeable gift device.
6. If the exact board differs, use its schematic instead of these pins.

Firmware is ESP-IDF (C) and uses i80 stripe transfers to avoid allocating
a full RGB565 frame buffer on the no-PSRAM ESP32.
