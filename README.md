# 100% Clock — a day, displayed as a percentage

An intentionally simple gift clock for the **Sunton ESP32-2432S022C**
(ESP32-WROOM-32, 2.2-inch 240×320 ST7789 i80 display, CST816S touch).
One day is **00.00% → 99.99%**. At **12:00:00 it reads 50.00%**.
The large percentage is always the visual centerpiece; ordinary time/date
stay secondary. No phone app, subscription or cloud account required.

> **Prototype, not yet tested on the author's physical board.** Check the
> printed PCB model before flashing. S022C is *not* the 2.8-inch SPI CYD.

## Quick flash (ESP-IDF v5.4)

```sh
git clone https://github.com/ballerburg9005/cyd-100-percent-clock
cd cyd-100-percent-clock
. ~/esp/esp-idf/export.sh
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

No Wi-Fi password is compiled into the firmware. Start the clock, tap **MENU →
WIFI → PORTAL** to enable a temporary setup AP. The screen shows the SSID,
device-specific eight-character password, and URL. Connect your phone to the
AP and browse **http://192.168.4.1/**. Enter home Wi-Fi credentials, POSIX
time-zone and a short gift message. The device restarts when you save.
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

### First-version controls

- **Main**: very large percent, small clock/date, progress bar; tap MENU.
- **Alarm**: daily local HH:MM alarm with simple onboard-speaker beep, on/off.
- **Timer**: kitchen timer with preset adjustments and start/stop.
- **Set time**: offline year/month/day/hour/minute editor; saves RTC if found.
- **Wi-Fi**: on-device SSID/password keyboard or short-lived setup access point.
- **Gift**: customizable short text via setup portal; physical photo slot in
  the proposed case. No owner's personal details or pictures are embedded.
- **Look**: three color themes, manual backlight level. Idle dim retains an
  always-visible clock instead of turning the display off.

**Not yet implemented:** photos from microSD, RSS, XMPP, weather, streaming
radio, firmware OTA and battery percentage. They are intentionally omitted
rather than represented by dead UI buttons. Speaker **beeps** are supported;
radio requires a reliable audio pipeline, feed selection and hardware
validation.

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
The firmware dims the backlight after inactivity and enables ESP32 Wi-Fi
modem sleep, but an always-visible LCD on a ~2500 mAh cell is generally an
**hours-to-day** project rather than weeks. Actual runtime must be measured.
USB power is the realistic mode for permanent full-brightness operation.

### Case concept

Portrait desk clock with 3D-printed wedge stand at ~15° tilt, front
window for 2.2-inch display, recessed USB access and ventilation, a
removable rear battery/RTC compartment, and optional 35 mm photo holder.
Leave clearance from LiPo to heat sources and avoid compression or puncture.

### Known limitations / test plan

1. Verify LCD rotation/color and CST816S touch coordinates on actual S022C.
2. Check NTP, reboot without Wi-Fi, and optional DS3231 retention.
3. Test AP portal and touchscreen keyboard with real WPA2 credentials.
4. Validate that idle dim / speaker alarm work and do not cause boot issues.
5. Measure battery current at bright/dim settings before building an
   enclosed rechargeable gift device.
6. If the exact board differs, use its schematic instead of these pins.

Firmware is ESP-IDF (C) and uses i80 stripe transfers to avoid allocating
a full RGB565 frame buffer on the no-PSRAM ESP32.
