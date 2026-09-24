# VisualDay

Today's Google Calendar and weather on the M5Stack Paper Color, so a glance tells
you what's on, where you are in the day, and what's next.

Top to bottom:

- **Header** - the day and date.
- **All-day events** - yellow chips.
- **Now** - green bar with what's on and when it ends.
- **Next** - large card with the next event, how long until it starts, time and
  location. After midday, once today is done, it shows tomorrow's first event.
- **Timeline** - today's events. Finished ones are outlined black, the current one
  is green, the next one blue, later ones outlined blue. Overlapping events sit
  side by side. Two or more empty hours in a row collapse into a short zig-zag
  gap marked "...", leaving the space to busy hours. The red line is the time of
  the last refresh.
- **Weather** - icon, max and min temperature, chance of rain, and when rain is
  likely to start ([Open-Meteo](https://open-meteo.com), no key needed).
- **Bins** - bottom right, a wheelie bin for each bin due today, from Google Tasks
  titled `Green Bin`, `Brown Bin` or `Black Bin`. They disappear when ticked off,
  and don't show anywhere else.

## Refreshing and sleep

Each e-paper refresh is a full-screen flash of about 16 seconds, so the screen
updates every quarter hour and whenever an event starts or ends. Between 22:00 and
06:00 it only updates once, at 00:05, for the new day, plus once when any event
still running at 22:00 ends. If the calendar can't be
reached it keeps showing what it has, marked "Offline since HH:MM", and retries
every 5 minutes.

Between refreshes the device is in deep sleep with WiFi off; the e-paper keeps the
image with no power. A scheduled wake takes about 25 seconds (WiFi, clock sync,
fetch, 16 s refresh) and goes straight back to sleep. The last calendar and
weather are kept in RTC memory so an offline wake still has something to show.

**Top button:** press to wake and refresh now. Hold for 3 seconds (while waking
it, or while switching on) to open the setup portal. After a button wake or
power-on the device stays awake for 60 seconds before sleeping.

## Setup

1. Set up the calendar feed: see [apps-script/README.md](apps-script/README.md).
2. Flash the firmware (below), then hold the **top button** while switching on.
   Join the **VisualDay** hotspot, choose **Configure WiFi**, pick your network
   and paste the feed URL. The weather location defaults to London; change the
   latitude and longitude on the same page.

With no feed URL set, the device shows demo data.

## Building

Requires [PlatformIO](https://platformio.org/) (`brew install platformio`):

```
pio run -t upload
```

The USB port disappears while the device sleeps, so flash it within 60 seconds of
pressing the top button (or of switching it on).

For development, copy `.env.example` to `.env` and fill it in, to bake WiFi, feed
URL and location into local builds so no setup portal is needed. Each build turns
it into `include/secrets.h`. Both files are gitignored; edit `.env`, not the header.
Environment variables of the same names override `.env`.

## Development tools

Both need the PlatformIO Python (`~/.platformio/penv/bin/python`). On macOS,
opening the serial port restarts the device, so each run starts from boot. Press
the top button first if the device is asleep.

- `tools/serial_capture.py 90` - watch the log for 90 seconds.
- `tools/screenshot.py shots/x.png "d;t1430" --reset` - boot, switch to demo data,
  render as if it were 14:30, and save the screen as a PNG. No panel refresh is
  needed, so layout changes can be checked in seconds.

Serial commands: `r` refresh, `s` screenshot, `d` toggle demo data, `t1430` pretend
it's 14:30 (`t` alone to undo), `w` setup portal.

## Hardware notes

Learned on [readerpedia](../readerpedia), all still apply:

- No PlatformIO board exists for the Paper Color; `esp32-s3-devkitc1-n16r8` with
  OPI PSRAM matches it, and M5GFX refuses to drive the panel without PSRAM.
- No partial refresh: every `display()` is a ~16 second full refresh, so
  auto-display is off and the screen is drawn into a PSRAM canvas and pushed once.
- Six inks (black, white, yellow, red, blue, green), no greys. Everything is drawn
  in exactly those colours with `epd_fastest` (nearest ink, no dithering). Brown
  bins are a red and black checkerboard.
- Fonts are ASCII only, so weather icons, bins and degree signs are drawn as
  shapes, and the Apps Script strips emoji and accents from event text.
- Log with `log_printf`, not `Serial`, which drops output on this board.
- Google Apps Script URLs copied from a browser may contain `/u/2/` (an account
  index); that version returns a sign-in page, so the device strips it.
- `M5.begin()` clears the screen by default: two full refreshes, ~33 seconds.
  `cfg.clear_display = false` skips that, so waking from sleep takes ~0.3 s and
  the old image stays up while the new one is prepared.
- The top button (BtnC) is GPIO1, an RTC GPIO, so it can wake the S3 from deep
  sleep via `ext1`. The sleep timer drifts a few seconds per quarter hour; the
  clock is resynced over NTP on every wake. `sntp_get_sync_status()` reports
  `COMPLETED` only once, then resets.
- If the saved WiFi network is unreachable, call `WiFi.disconnect()` before trying
  another, or the ESP32 refuses ("sta is connecting, cannot set config").
