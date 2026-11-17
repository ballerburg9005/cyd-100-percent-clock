# 100% Clock — a day, displayed as a percentage

An offline-first **landscape (320×240)** percentage clock for the **Sunton ESP32-2432S022C**
(ESP32-WROOM-32, native 240×320 ST7789 i80 LCD, CST816S capacitive touch).
One civil day runs from **00.00% to 99.99%**, with **50.00% at 12:00**.
The percentage dominates every clock/alarms/timer screen; the conventional
HH:MM and ISO 8601 date are small and secondary. Big numerals use embedded
rasterized typographic glyphs, not seven-segment blocks.
No phone app, subscription or cloud account is required.

> The display, touch, TF mounting and original clock firmware have been
> flashed and exercised on an ESP32-2432S022C. **The new DAC audio playback
> still requires listening and interaction testing on the physical board.**
> S022C is *not* the 2.8-inch SPI CYD.

## Experimental composite TV output (this branch only)

**Unverified on hardware. This is deliberately NOT merged into `main`.**
This is a direct GPIO25 DAC1 monochrome composite experiment using the
GPL-3.0-or-later `aquaticus/esp32_composite_video_lib` source. That dependency
has copyleft redistribution requirements; do not distribute resulting firmware
without satisfying its GPL obligations. The audio GPIO26 DAC2 is left active.

**Connection, only after flashing:** RCA composite center/tip (yellow video)
to **GPIO25**, RCA outer shield to **any board GND**. Verify the PCB pad is
actually GPIO25, not an LCD connector pin number. Power off for soldering.
Connect **only** to a real composite video input, not HDMI, aerial/RF,
mains, or an audio amplifier output. This is a test output and a TV's
75-ohm input loads GPIO25, which is also hardwired to the LCD data bus.

**Behavior:** TF FILES > select MP3/WAV/RTTTL > PLAY starts audio on GPIO26,
shows a frozen STOP button on the built-in LCD, then releases the parallel
ST7789/I2S0 controller and produces an animated monochrome test/player
screen on composite TV. Tap the same STOP button position on the LCD to
end audio and TV; LCD restarts. Selecting a BMP in TF FILES automatically
starts TV mode with a dithered thumbnail: press the frozen LCD BACK control
(top-right) to restore the LCD. Text files are unchanged. Existing clock,
alarm and contact functions are unchanged when composite is inactive.

The display RAM retains a frozen picture during composite; this version
does not yet interrupt TV to refresh the clock every 8.64 seconds, because
a TV losing synchronization would defeat a first test. A full LCD driver
reinitialize restores the display when TV mode ends.

For an existing clone with an old ignored `sdkconfig`, use:

```sh
git fetch origin
git switch experiment/composite-tv-tf-media
git pull --ff-only
python3 tools/enable_audio_partition.py
python3 tools/enable_composite_preview.py
. ~/esp/esp-idf/export.sh
idf.py reconfigure build
idf.py -p /dev/ttyUSB0 flash monitor
```

For a pure default build, PAL 320x256 is selected (change
`Clock board adjustments > Experimental composite television uses NTSC`
in `idf.py menuconfig` if your TV accepts NTSC only). The third-party
DAC library requires **-O2** optimization; the helper script sets this
without discarding locally customized LCD/touch configuration.

**Risk/limitations:** untested I2S0 transfer-controller reinitialization,
possible TV sync problems from a GPIO25 LCD-data load, possible GPIO26 DAC2
interaction with the upstream DAC driver, and potentially degraded MP3
decoding if composite scanline ISRs use too much CPU. If the display
stays blank or an assert occurs, flash the previous main branch again.
The GitHub CI build proves compilation only, not electrical signal quality.

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
  Both alarm and timer invert the whole display's colors for 300 ms
  approximately every 1.1 seconds while ringing, so they are noticeable even
  without a speaker. Any touchscreen tap silences the alert and restores the
  selected color theme immediately; both flashing and sound stop after
  **0.10% of a day = 86.4 seconds** (1 min 26.4 sec).
  Idle backlight dimming is suspended during alerts.
  Both pages have a **SOUND** button for selecting one of four firmware
  chiptunes or an external TF-card WAV/MP3/RTTTL file. The default is a short
  ascending three-note chime. Sounds repeat until the alarm/timer is dismissed
  or reaches its automatic 86.4-second cutoff.
- **Kitchen timer:** `X.XX%` duration (0.01%–9.99% of one day), adjustable
  in ±0.01% or ±0.10% steps. **RESET** restores exactly five minutes
  (300 seconds), shown as approximately `0.35%`. The underlying timer stays
  exact. The display is decimal only: no redundant MM:SS or explanatory row.
- **Set Time:** `[X] USE NTP` checkbox defaults ON; uncheck for the
  symmetrically spaced offline date/time editor. Year is shown as `26`
  rather than `2026` but the full year is retained internally; the
  enlarged **SAVE TIME** button sits at bottom-left. The six adjustment
  controls sit in three symmetrical, inset columns with about 20px bezel
  margin, matching the keyboard edge spacing. DS3231, when fitted,
  backs up UTC independently of display time zone.
- **Wi-Fi:** touchscreen keyboard uses inset 38×25-pixel keys, a visible
  plaintext input and character count, DELETE on the left, SHIFT in the
  middle and SAVE on the right. Supports 8–63-character WPA2 passphrases
  or **exactly 64 hexadecimal characters** (the WPA2 raw PSK format);
  invalid keys are rejected without deleting the typed text.
  Enabling NTP or saving changed Wi-Fi credentials requests an immediate
  network sync when a saved SSID is available. **SET TIME → SYNC NOW** retries
  on demand, with status for connecting, waiting for NTP, missing SSID and
  Wi-Fi authentication failures. Every boot attempts NTP when enabled.
  **WIFI → SCAN** lists nearby SSIDs with their exact case, so users need not
  guess capitalization; manual typing is still available. The phone/PC
  setup portal at `http://192.168.4.1/` is much faster for long keys:
  connect to the displayed temporary 10-minute WPA2 access point and paste
  the full key there. This uses AP-only mode on 2.4 GHz channel 6 to keep the
  setup network visible; the clock temporarily disconnects from home Wi-Fi
  until the portal closes or you save. Touching STOP WIFI PORTAL turns the setup access point and its web server off immediately; touching START WIFI PORTAL starts it again with
  the same lowercase 8-character password. Use only on a trusted local device.
- **Contact:** shows **100% CLOCK MADE BY**, XMPP, email and SMS
  information, plus a **140×180-pixel borderless portrait** (right margin
  12px, bottom margin 10px). The image is compiled into firmware at build time
  from the repository's `face.bmp`, center-cropped without distortion to
  a 128×165 RGB565 thumbnail. **No SD card is needed for CONTACT.**
  The RELOAD SD button and SD status were removed from CONTACT; the separate
  TF FILES browser still supports SD cards. CONTACT has compact PLAY/STOP and
  NEXT controls for previewing five original built-in chiptunes without any
  TF card. XMPP, email and SMS are no
  longer truncated to 24 characters: long values continue over the photo,
  with a thin black text outline where more than three characters overlap.
  Values longer than one full 320px row wrap onto the next line.
  Build defaults come from root `contact_details.txt`, containing **public
  fake/example information**. For your own local build, create
  `contact_details.private.txt` in the repository root; the generator uses
  it instead, and Git ignores that private override. Use these lines:
  ```text
  XMPP: example@domain.test
  Email: example@domain.test
  SMS: +00 123 456 789
  ```
  The script generates headers only under `build/`. The firmware copies new
  defaults into NVS when the contact source changes, while later Wi-Fi portal
  edits remain available. **Do not commit `contact_details.private.txt` or
  upload your locally compiled firmware if it contains private information:**
  contact values are embedded in the binary and can be extracted by someone
  who obtains it. The tracked `contact_details.txt` stays public, and Git
  cannot hide files that were committed previously merely via `.gitignore`.
- **Look:** reworked muted, high-contrast palettes **SLATE**, **MOSS**,
  **PAPER** and **BLACK**. BLACK uses literal RGB565 zero as background,
  with cool-white ink and matching subdued controls. LCD colour inversion
  is disabled so black does not display white. The DARK button selects BLACK
  immediately, while NEXT COLOR cycles all four. Backlight dims
  after 40 seconds of idle time; the LCD stays on.
- **Files:** TF/microSD root browser with **RESCAN** in the top bar and
  a visible mount status/error so hot insertion can be tested without rebooting.
  Reads plain `.txt`, displays uncompressed 24/32-bit `.bmp`, and can
  **PLAY** `.wav`, `.mp3` and `.rtttl` sound files. On an audio file's
  detail screen, PLAY/STOP previews it, while ALARM/TIMER sets that file
  as the corresponding alert. MP3 playback uses the CC0 minimp3 decoder.
  JPEG/PNG, EPUB, folders and other binary decoders remain unavailable.
  A missing TF card cannot prevent boot; selecting a missing TF alert sound
  falls back to the built-in chime. FAT filenames are preserved as UTF-8
  byte sequences rather than chopped at 63 bytes. Overlong names are skipped
  instead of opening a damaged path.

### Audio playback and available flash

Five original melodies are built into firmware: CLASSIC CHIME, PIXEL SUNRISE,
8-BIT RUSH, NIGHT CIRCUIT and 1999 RADIO CLOCK. The first four retain their
previous sound IDs (existing alarm settings remain valid). The new tune is
an original two-tone/triple-beep 1990s clock-radio alarm, evoking the
digital alarm heard in Neo's apartment in *The Matrix* without copying
its film soundtrack. CONTACT -> PLAY and NEXT work without a card.
ALARM -> SOUND and TIMER -> SOUND choose independently from those melodies
or open TF FILES to select a removable audio file. TF FILES also allows
independent preview and one-tap assignment as an alarm or timer.

Optional original RTTTL files are available under [chiptunes/](chiptunes/).
Copy these `.rtttl` files to the **root** of a FAT32 TF card to play them.
Tracker modules (`.xm`, `.mod`, `.s3m`, `.it`) are richer chiptune formats,
but are **not directly supported** by the firmware. Convert them on a PC to
MP3 or PCM WAV before copying to TF. `libxmp-lite` is a possible future
embedded decoder, though its RAM usage needs evaluation for this no-PSRAM
board. Other supported files: uncompressed RIFF PCM `.wav` (8- or 16-bit,
mono/stereo, 8–48 kHz) and standard MPEG Layer III `.mp3` (including ID3v2
tags). Audio is rendered as **10 kHz, mono, 8-bit DAC**, via GPIO26's
onboard speaker amplifier; expect lo-fi quality and reduced loudness.
This is new code, and actual timing/volume must be tested on the board.

**Why no I2S DAC DMA?** On ESP32, the i80 parallel LCD and internal
I2S DAC DMA share I2S0. Audio instead uses the DAC *oneshot* mode with
a 10 kHz **task-dispatched esp_timer callback**, avoiding the high-frequency
GPTimer interrupt that panicked on a physical S022C when TF playback started.
The MP3 decoder runs in a separate FreeRTOS task. Earlier firmware only
allocated **8 KiB** of task stack, but minimp3's temporary decoding structure
alone uses more than that; this could corrupt memory and trigger crashes in
unrelated timer code. The decoder task now has **32 KiB**, and logs stack
headroom while reading MP3 frames. This is a firmware correction; replay
large MP3s on the actual board to verify it.

**Flash:** The tested S022C has 4 MiB flash; the old default firmware had
only a 1 MiB app partition. `partitions.csv` configures a **3 MiB factory
app** starting at the same 0x10000 offset, with NVS/PHY offsets unchanged.
New clones select this automatically. If you already built this project,
your ignored local `sdkconfig` may still select the old 1 MiB partition.
After pulling, run the one-time migration below; it preserves all other
settings and backs up your previous file:

```sh
python3 tools/enable_audio_partition.py
. ~/esp/esp-idf/export.sh
idf.py reconfigure build
idf.py -p /dev/ttyUSB0 flash
```

The previous app and existing NVS are not erased by this partition change.
Do not use this 3 MiB partition table on a device with less than 4 MiB flash.
GitHub Actions builds the public **fake-contact** variant. Locally generated
firmware containing private contact details should not be uploaded publicly.

**Hardware notes:** The LCD is rotated to landscape and, by default, a
further **180° flip** places the physical button and TF-card slot at the TOP.
The same 180° transform is applied to the CST816S capacitive touch mapping,
so the on-screen controls remain aligned. To restore the older layout, disable
**Flip display and touch 180 degrees** in `idf.py menuconfig` →
**100 Percent Clock board adjustments**, then rebuild/flash.
A configurable **Touch edge expansion** (112% default) stretches coordinates
slightly away from the center and clamps them to the screen; this is not a
measured five-point calibration. The separate touch mirror/swap options
remain available if required on other board revisions. The touch dot
indicates what coordinates the firmware registered.

**Not implemented:** streaming radio, XMPP messaging, RSS/news, weather,
tracker-module XM/MOD/S3M/IT decoding, or battery percentage. These require separate network/audio
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
