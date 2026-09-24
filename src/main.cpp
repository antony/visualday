// VisualDay - today's Google Calendar and weather on the M5Stack Paper Color.
//
// Screen, top to bottom: date, all-day events, what's on now, a large card for
// the next event, a timeline of the day with a red line at the current time,
// and the day's weather.
//
// Buttons:  top = refresh now (hold at power-on: setup portal for WiFi,
//           calendar URL and weather location).
// Physically, M5Unified's BtnC is the top button, BtnA the middle, BtnB the bottom.
//
// Hardware notes that shape this code (learned on ../readerpedia):
//  - The panel (ED2208, 400x600 colour e-paper, portrait) has no partial
//    refresh. Every display() is a full refresh taking ~16 seconds, so we
//    refresh on a schedule (quarter hours plus event boundaries, none at night)
//    rather than continuously.
//  - Six inks (black, white, yellow, red, blue, green), no greys, so the
//    screen is drawn only in those six colours with no anti-aliasing.
//  - Everything is drawn into a PSRAM canvas first and pushed to the panel in
//    one go. The canvas can also be dumped over serial as a screenshot.
//  - The bundled fonts are ASCII only: no emoji, no degree sign. Weather icons
//    and degree signs are drawn as shapes; the calendar feed strips the rest.

#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <esp_sleep.h>
#include <esp_sntp.h>
#include <driver/rtc_io.h>
#include <time.h>
#include <math.h>
#include <vector>
#include <algorithm>

// Log via the ROM console rather than Serial: the S3's HWCDC "Serial" drops
// output unless it believes a host is attached, and that check misfires here.
#define LOG(...) log_printf(__VA_ARGS__)

// Optional development settings, generated from .env by tools/gen_secrets.py.
#if __has_include("secrets.h")
#include "secrets.h"
#endif

#define BtnTop    (M5.BtnC)
static const gpio_num_t TOP_BUTTON_PIN = GPIO_NUM_1;   // BtnC, active low; can wake from deep sleep

static const char *SETUP_AP_NAME = "VisualDay";
static const int SETUP_PORTAL_TIMEOUT_S = 300;
static const char *TZ_INFO = "GMT0BST,M3.5.0/1,M10.5.0";   // UK
static const float DEFAULT_LAT = 51.5074f, DEFAULT_LON = -0.1278f;   // London
static const char *USER_AGENT = "VisualDay/1.0 (M5Stack Paper Color)";

// Refresh schedule, in minutes of the day.
static const int REFRESH_EVERY = 15;
static const int QUIET_FROM = 22 * 60;     // last refresh of the evening
static const int QUIET_UNTIL = 6 * 60;     // first refresh of the morning
static const int NEW_DAY_AT = 5;           // one refresh just after midnight
static const int RETRY_AFTER_S = 5 * 60;   // after a failed fetch
static const int WEATHER_MAX_AGE_S = 60 * 60;
static const int AWAKE_WINDOW_S = 60;       // after power-on or a button wake, for serial commands and flashing
static const int SETUP_HOLD_MS = 3000;      // hold the top button this long to open setup
static const int EARLY_WAKE_SLACK_S = 120;  // the sleep timer drifts; wakes this early just wait

// The six inks.
static const uint16_t INK_BLACK = TFT_BLACK, INK_WHITE = TFT_WHITE, INK_RED = TFT_RED,
                      INK_GREEN = TFT_GREEN, INK_BLUE = TFT_BLUE, INK_YELLOW = TFT_YELLOW;

// Layout
static const int MARGIN = 12;
static const int HEADER_H = 48;
static const int WEATHER_H = 84;
static const int GUTTER = 52;          // hour labels left of the timeline
static const int MIN_BLOCK_H = 20;     // shortest drawn event block

struct Event {
    String title, location;
    time_t start = 0, end = 0;
    bool allDay = false;
};

struct Weather {
    bool ok = false;
    int code = 0;
    float tmax = 0, tmin = 0;
    int rainMax = 0;
    int hourly[24] = {0};
    time_t fetchedAt = 0;
    time_t day = 0;
};

static M5Canvas canvas;
static int W = 0, H = 0;

static String cfgCalendarUrl;
static float cfgLat = DEFAULT_LAT, cfgLon = DEFAULT_LON;

static std::vector<Event> events;   // today and tomorrow, sorted by start
static bool calendarOk = false;
static long fakeOffset = 0;         // serial "t1430" pretends it is 14:30, for testing
static unsigned long awakeUntil = 0;   // millis(); 0 = sleep as soon as the refresh is done

// Kept in RTC memory through deep sleep, so a wake with no network still has
// something to show. Cleared on power-on.
RTC_DATA_ATTR static time_t eventsDay = 0;       // local midnight of the day events were fetched for
RTC_DATA_ATTR static bool demoMode = false;
RTC_DATA_ATTR static Weather weather;
RTC_DATA_ATTR static time_t lastGoodFetch = 0;
RTC_DATA_ATTR static time_t nextRefreshAt = 0;

struct CachedEvent {
    char title[64], location[40];
    int64_t start, end;
    bool allDay;
};
static const int CACHE_MAX = 24;
RTC_DATA_ATTR static CachedEvent rtcEvents[CACHE_MAX] = {};
RTC_DATA_ATTR static int rtcEventCount = 0;

static void saveEventsToRtc() {
    rtcEventCount = min((int)events.size(), CACHE_MAX);
    for (int i = 0; i < rtcEventCount; i++) {
        CachedEvent &c = rtcEvents[i];
        strlcpy(c.title, events[i].title.c_str(), sizeof c.title);
        strlcpy(c.location, events[i].location.c_str(), sizeof c.location);
        c.start = events[i].start;
        c.end = events[i].end;
        c.allDay = events[i].allDay;
    }
}

static void loadEventsFromRtc() {
    events.clear();
    for (int i = 0; i < rtcEventCount && i < CACHE_MAX; i++) {
        Event e;
        e.title = rtcEvents[i].title;
        e.location = rtcEvents[i].location;
        e.start = rtcEvents[i].start;
        e.end = rtcEvents[i].end;
        e.allDay = rtcEvents[i].allDay;
        events.push_back(e);
    }
}

// ---------------------------------------------------------------- time

static time_t nowT() { return time(nullptr) + fakeOffset; }
static bool timeValid() { return time(nullptr) > 1700000000; }

static time_t localMidnight(time_t t) {
    struct tm tm;
    localtime_r(&t, &tm);
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
    tm.tm_isdst = -1;
    return mktime(&tm);
}

// Local time on the day starting at `midnight`, `m` minutes in (may exceed a day).
static time_t atMinute(time_t midnight, int m) {
    struct tm tm;
    localtime_r(&midnight, &tm);
    tm.tm_hour = 0;
    tm.tm_min = m;
    tm.tm_sec = 0;
    tm.tm_isdst = -1;
    return mktime(&tm);
}

static int minuteOfDay(time_t t) {
    struct tm tm;
    localtime_r(&t, &tm);
    return tm.tm_hour * 60 + tm.tm_min;
}

// Wall-clock minutes of `t` on the day starting at `day`, clamped to 0..1440.
static int dayMinute(time_t t, time_t day) {
    if (t <= day) return 0;
    if (t >= atMinute(day, 1440)) return 1440;
    return minuteOfDay(t);
}

static String hhmm(time_t t) {
    char b[8];
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(b, sizeof b, "%H:%M", &tm);
    return b;
}

static String untilText(time_t from, time_t to) {
    long mins = (to - from + 59) / 60;
    if (mins <= 0) return "now";
    if (mins < 60) return "in " + String(mins) + " min";
    long h = mins / 60, m = mins % 60;
    return "in " + String(h) + "h" + (m ? " " + String(m) + "m" : "");
}

// ---------------------------------------------------------------- config

// Browser URLs can carry an account index ("/macros/u/2/s/..."), which makes
// Google answer with a sign-in page instead of the feed.
static String normaliseUrl(String url) {
    url.trim();
    int u = url.indexOf("/macros/u/");
    if (u >= 0) {
        int s = url.indexOf("/s/", u);
        if (s > 0) url = url.substring(0, u) + "/macros" + url.substring(s);
    }
    return url;
}

static void loadConfig() {
    Preferences p;
    if (p.begin("visualday", true)) {
        cfgCalendarUrl = p.getString("cal", "");
        cfgLat = p.getFloat("lat", DEFAULT_LAT);
        cfgLon = p.getFloat("lon", DEFAULT_LON);
        p.end();
    }
#ifdef CALENDAR_URL
    if (cfgCalendarUrl.isEmpty()) cfgCalendarUrl = CALENDAR_URL;
#endif
#if defined(WEATHER_LAT) && defined(WEATHER_LON)
    if (cfgLat == DEFAULT_LAT && cfgLon == DEFAULT_LON) { cfgLat = WEATHER_LAT; cfgLon = WEATHER_LON; }
#endif
    cfgCalendarUrl = normaliseUrl(cfgCalendarUrl);
    LOG("[config] calendar %s, weather at %.4f,%.4f\n",
        cfgCalendarUrl.isEmpty() ? "not set" : "set", cfgLat, cfgLon);
}

static void saveConfig() {
    Preferences p;
    p.begin("visualday", false);
    p.putString("cal", cfgCalendarUrl);
    p.putFloat("lat", cfgLat);
    p.putFloat("lon", cfgLon);
    p.end();
}

// ---------------------------------------------------------------- drawing helpers

static String fitToWidth(const String &s, int maxW) {
    if (canvas.textWidth(s) <= maxW) return s;
    String out = s;
    while (out.length() > 1 && canvas.textWidth(out + "...") > maxW) out.remove(out.length() - 1);
    out.trim();
    return out + "...";
}

// Greedy word wrap into at most `maxLines`; the last line is ellipsised.
static int wrapText(const String &s, int maxW, String *out, int maxLines) {
    int n = 0;
    String line;
    int i = 0, len = s.length();
    while (i < len && n < maxLines) {
        int j = s.indexOf(' ', i);
        if (j < 0) j = len;
        String word = s.substring(i, j);
        String trial = line.isEmpty() ? word : line + " " + word;
        if (canvas.textWidth(trial) <= maxW || line.isEmpty()) {
            line = trial;
            i = j + 1;
        } else if (n == maxLines - 1) {
            break;
        } else {
            out[n++] = line;
            line = "";
        }
    }
    if (n < maxLines && !line.isEmpty()) {
        if (i < len) line += " " + s.substring(i);   // leftovers on the last line
        out[n++] = fitToWidth(line, maxW);
    }
    return n;
}

// Degree sign drawn as a ring, since the fonts don't have one.
static void drawDegree(int x, int y, int r, uint16_t c) {
    canvas.fillCircle(x + r, y + r, r, c);
    canvas.fillCircle(x + r, y + r, max(1, r - (r > 3 ? 2 : 1)), INK_WHITE);
}

// Thick line as a filled quad (no anti-aliasing, so it stays on pure inks).
static void thickLine(float x0, float y0, float x1, float y1, float w, uint16_t c) {
    float dx = x1 - x0, dy = y1 - y0, l = sqrtf(dx * dx + dy * dy);
    if (l < 0.01f) return;
    float nx = -dy / l * w / 2, ny = dx / l * w / 2;
    canvas.fillTriangle(x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, c);
    canvas.fillTriangle(x0 + nx, y0 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny, c);
}

static void dottedHLine(int x0, int x1, int y, uint16_t c) {
    for (int x = x0; x <= x1; x += 4) canvas.drawPixel(x, y, c);
}

// ---------------------------------------------------------------- weather icons

static void drawSun(float cx, float cy, float r) {
    for (int i = 0; i < 8; i++) {
        float a = i * PI / 4;
        thickLine(cx + cosf(a) * (r + 4), cy + sinf(a) * (r + 4),
                  cx + cosf(a) * (r * 1.7f), cy + sinf(a) * (r * 1.7f), max(3.0f, r * 0.28f), INK_YELLOW);
    }
    canvas.fillCircle(cx, cy, r + 2, INK_RED);
    canvas.fillCircle(cx, cy, r, INK_YELLOW);
}

// Cloud of width w centred on cx, with its flat base at `base`.
static void drawCloud(float cx, float base, float w) {
    struct { float dx, dy, r; } puffs[] = {
        {-0.26f, -0.16f, 0.20f}, {0.02f, -0.30f, 0.27f}, {0.28f, -0.17f, 0.19f},
    };
    for (int pass = 0; pass < 2; pass++) {
        int grow = pass == 0 ? 2 : 0;
        uint16_t c = pass == 0 ? INK_BLACK : INK_WHITE;
        for (auto &p : puffs) canvas.fillCircle(cx + p.dx * w, base + p.dy * w, p.r * w + grow, c);
        canvas.fillRoundRect(cx - 0.46f * w - grow, base - 0.2f * w - grow,
                             0.92f * w + 2 * grow, 0.2f * w + 2 * grow, 0.1f * w, c);
    }
}

static void drawDrop(float cx, float cy, float s, uint16_t c = INK_BLUE) {
    canvas.fillCircle(cx, cy + s * 0.35f, s * 0.45f, c);
    canvas.fillTriangle(cx, cy - s * 0.6f, cx - s * 0.43f, cy + s * 0.25f, cx + s * 0.43f, cy + s * 0.25f, c);
}

static void drawFlake(float cx, float cy, float s) {
    for (int i = 0; i < 3; i++) {
        float a = i * PI / 3;
        thickLine(cx - cosf(a) * s, cy - sinf(a) * s, cx + cosf(a) * s, cy + sinf(a) * s, 2.5f, INK_BLUE);
    }
}

static void drawBolt(float cx, float cy, float s) {
    for (int pass = 0; pass < 2; pass++) {
        float g = pass == 0 ? 2.0f : 0.0f;
        uint16_t c = pass == 0 ? INK_BLACK : INK_YELLOW;
        canvas.fillTriangle(cx + 0.1f * s, cy - 0.6f * s - g, cx - 0.35f * s - g, cy + 0.1f * s + g,
                            cx + 0.05f * s + g, cy + 0.1f * s + g, c);
        canvas.fillTriangle(cx - 0.05f * s - g, cy - 0.05f * s - g, cx + 0.35f * s + g, cy - 0.05f * s - g,
                            cx - 0.1f * s, cy + 0.65f * s + g, c);
    }
}

// Emoji-style icon for a WMO weather code, in a box of size s at (x, y).
static void drawWeatherIcon(int x, int y, int s, int code) {
    float cx = x + s / 2.0f, cy = y + s / 2.0f;
    bool rainy = (code >= 51 && code <= 67) || (code >= 80 && code <= 82);
    bool snowy = (code >= 71 && code <= 77) || code == 85 || code == 86;
    bool storm = code >= 95;
    bool showers = code >= 80 && code <= 86;

    if (code <= 1) {
        drawSun(cx, cy, s * 0.26f);
        if (code == 1) drawCloud(cx + s * 0.2f, y + s * 0.95f, s * 0.5f);
        return;
    }
    if (code == 2 || showers) drawSun(x + s * 0.34f, y + s * 0.3f, s * 0.17f);

    float base = (rainy || snowy || storm || code == 45 || code == 48) ? y + s * 0.62f : y + s * 0.8f;
    drawCloud(cx + (code == 2 || showers ? s * 0.08f : 0), base, s * 0.9f);

    if (code == 45 || code == 48) {
        for (int i = 0; i < 3; i++)
            thickLine(x + s * (0.12f + 0.08f * i), base + 8 + i * 8, x + s * (0.88f - 0.06f * i), base + 8 + i * 8, 3, INK_BLACK);
    } else if (storm) {
        drawBolt(cx, base + s * 0.2f, s * 0.36f);
        drawDrop(cx - s * 0.3f, base + s * 0.2f, s * 0.13f);
        drawDrop(cx + s * 0.3f, base + s * 0.2f, s * 0.13f);
    } else if (snowy) {
        for (int i = -1; i <= 1; i++) drawFlake(cx + i * s * 0.28f, base + s * (i == 0 ? 0.26f : 0.16f), s * 0.09f);
    } else if (rainy) {
        int drops = (code == 51 || code == 53 || code == 61 || code == 80) ? 2 : (code == 65 || code == 82) ? 4 : 3;
        for (int i = 0; i < drops; i++) {
            float dx = (i - (drops - 1) / 2.0f) * s * 0.22f;
            drawDrop(cx + dx, base + s * ((i % 2) ? 0.26f : 0.16f), s * 0.14f);
        }
    }
}

static const char *weatherText(int code) {
    switch (code) {
        case 0: return "Clear";
        case 1: return "Mostly sunny";
        case 2: return "Partly cloudy";
        case 3: return "Cloudy";
        case 45: case 48: return "Fog";
        case 51: case 53: case 55: return "Drizzle";
        case 56: case 57: return "Freezing drizzle";
        case 61: return "Light rain";
        case 63: return "Rain";
        case 65: return "Heavy rain";
        case 66: case 67: return "Freezing rain";
        case 71: case 73: case 75: case 77: return "Snow";
        case 80: case 81: return "Showers";
        case 82: return "Heavy showers";
        case 85: case 86: return "Snow showers";
        case 95: return "Thunderstorms";
        case 96: case 99: return "Thunder and hail";
        default: return "";
    }
}

// ---------------------------------------------------------------- bins

static const int32_t NOT_A_BIN = -1;
static const int32_t BIN_BROWN = 0x10000;   // not an ink: drawn as a red/black checkerboard

// Bins due today, from Google Tasks named "Green Bin", "Brown Bin" or
// "Black Bin" (see apps-script/Code.gs). Drawn as wheelie bins in the weather strip.
RTC_DATA_ATTR static int32_t binsToday[3] = {};
RTC_DATA_ATTR static int binCount = 0;

static int32_t binColour(const String &name) {
    if (name == "green") return INK_GREEN;
    if (name == "brown") return BIN_BROWN;
    if (name == "black") return INK_BLACK;
    return NOT_A_BIN;
}

static void addBin(int32_t colour) {
    if (colour == NOT_A_BIN || binCount >= 3) return;
    for (int i = 0; i < binCount; i++) if (binsToday[i] == colour) return;
    binsToday[binCount++] = colour;
}

static void binShape(int x, int y, int s, uint16_t c, int g) {
    float w = s * 0.78f;
    int bodyTop = y + s * 0.2f, bodyBot = y + s * 0.92f;
    canvas.fillRect(x - g, y + s * 0.08f - g, w + 2 * g, s * 0.12f + 2 * g, c);          // lid
    canvas.fillRect(x + w * 0.35f - g, y - g, w * 0.3f + 2 * g, s * 0.1f + 2 * g, c);     // handle
    canvas.fillTriangle(x + w * 0.04f - g, bodyTop - g, x + w * 0.96f + g, bodyTop - g, x + w * 0.88f + g, bodyBot + g, c);
    canvas.fillTriangle(x + w * 0.04f - g, bodyTop - g, x + w * 0.88f + g, bodyBot + g, x + w * 0.12f - g, bodyBot + g, c);
    canvas.fillCircle(x + w * 0.2f, bodyBot, s * 0.09f + g, c);                             // wheel
}

// Wheelie bin of height s, with a white outline so it shows on any background,
// and a white letter for what goes in it: R(ecycling) green, W(aste) black,
// G(arden) brown.
static void drawBin(int x, int y, int s, int32_t colour) {
    binShape(x, y, s, INK_WHITE, 2);
    uint16_t c = colour == BIN_BROWN ? INK_RED : (uint16_t)colour;
    binShape(x, y, s, c, 0);
    if (colour == BIN_BROWN) {
        // No brown ink: a 2x2 cell of red, black, yellow, red averages to a sienna brown.
        static const uint16_t cell[2][2] = {{INK_RED, INK_BLACK}, {INK_YELLOW, INK_RED}};
        for (int yy = y - 2; yy < y + s + 4; yy++)
            for (int xx = x - 2; xx < x + s; xx++)
                if (canvas.readPixel(xx, yy) == INK_RED) canvas.drawPixel(xx, yy, cell[yy & 1][xx & 1]);
    }
    float w = s * 0.78f;
    canvas.drawFastHLine(x + w * 0.1f, y + s * 0.2f + 2, w * 0.8f, INK_WHITE);   // lid seam

    const char *letter = colour == INK_GREEN ? "R" : colour == BIN_BROWN ? "G" : "W";
    canvas.setFont(s >= 56 ? &fonts::FreeSansBold18pt7b : s >= 36 ? &fonts::FreeSansBold12pt7b : &fonts::FreeSansBold9pt7b);
    canvas.setTextColor(INK_WHITE);
    canvas.setTextDatum(textdatum_t::middle_center);
    canvas.drawString(letter, x + w * 0.5f, y + s * 0.56f);
}

// ---------------------------------------------------------------- screen sections

static void drawHeader(time_t now) {
    canvas.fillRect(0, 0, W, HEADER_H, INK_BLACK);
    canvas.setTextColor(INK_WHITE);
    char day[16], date[24];
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(day, sizeof day, "%A", &tm);
    strftime(date, sizeof date, "%e %B", &tm);
    String d = date;
    d.trim();
    canvas.setTextDatum(textdatum_t::middle_left);
    canvas.setFont(&fonts::FreeSansBold18pt7b);
    canvas.drawString(day, MARGIN, HEADER_H / 2);
    canvas.setTextDatum(textdatum_t::middle_right);
    canvas.setFont(&fonts::FreeSans12pt7b);
    canvas.drawString(d, W - MARGIN, HEADER_H / 2 + 2);
}

// All-day events as yellow chips. Returns the y below them.
static int drawAllDay(int y, const std::vector<const Event *> &list) {
    if (list.empty()) return y;
    canvas.setFont(&fonts::DejaVu12);
    canvas.setTextDatum(textdatum_t::middle_left);
    const int chipH = 22, gap = 6;
    int x = MARGIN, rows = 1;
    y += gap;
    for (const Event *e : list) {
        String t = fitToWidth(e->title, W - 2 * MARGIN - 16);
        int w = canvas.textWidth(t) + 16;
        if (x > MARGIN && x + w > W - MARGIN) {
            if (rows == 2) break;
            rows++;
            x = MARGIN;
            y += chipH + gap;
        }
        canvas.fillRoundRect(x, y, w, chipH, 6, INK_YELLOW);
        canvas.drawRoundRect(x, y, w, chipH, 6, INK_BLACK);
        canvas.setTextColor(INK_BLACK);
        canvas.drawString(t, x + 8, y + chipH / 2 + 1);
        x += w + gap;
    }
    return y + chipH;
}

// "NOW  <title>  until 11:00" in green. Returns the y below it.
static int drawNow(int y, const Event *cur) {
    if (!cur) return y;
    const int h = 30;
    y += 6;
    canvas.fillRoundRect(MARGIN, y, W - 2 * MARGIN, h, 6, INK_GREEN);
    canvas.setTextColor(INK_WHITE);
    canvas.setTextDatum(textdatum_t::middle_left);
    canvas.setFont(&fonts::FreeSansBold9pt7b);
    canvas.drawString("NOW", MARGIN + 10, y + h / 2);
    String until = "until " + hhmm(cur->end);
    canvas.setFont(&fonts::FreeSans9pt7b);
    int uw = canvas.textWidth(until);
    canvas.setTextDatum(textdatum_t::middle_right);
    canvas.drawString(until, W - MARGIN - 10, y + h / 2);
    canvas.setFont(&fonts::FreeSansBold9pt7b);
    canvas.setTextDatum(textdatum_t::middle_left);
    int tx = MARGIN + 60;
    canvas.drawString(fitToWidth(cur->title, W - MARGIN - 10 - uw - 12 - tx), tx, y + h / 2);
    return y + h;
}

// The big "next up" card. Returns the y below it.
static int drawNext(int y, const Event *next, time_t now, bool nextIsTomorrow, bool anyToday) {
    y += 8;
    const int x = MARGIN, w = W - 2 * MARGIN, textX = x + 18, textW = w - 26;
    int top = y;
    y += 8;

    canvas.setTextDatum(textdatum_t::top_left);
    canvas.setTextColor(INK_BLUE);
    canvas.setFont(&fonts::FreeSansBold9pt7b);
    if (!next) {
        canvas.drawString("NEXT", textX, y);
        y += 26;
        canvas.setTextColor(INK_BLACK);
        canvas.setFont(&fonts::FreeSansBold18pt7b);
        canvas.drawString(anyToday ? "Nothing else today" : "Nothing on today", textX, y);
        y += 40;
    } else {
        canvas.drawString(nextIsTomorrow ? "NEXT - TOMORROW" : "NEXT", textX, y);
        canvas.setTextDatum(textdatum_t::top_right);
        canvas.setTextColor(INK_RED);
        canvas.drawString(nextIsTomorrow ? hhmm(next->start) : untilText(now, next->start), x + w - 10, y);
        y += 24;

        canvas.setTextDatum(textdatum_t::top_left);
        canvas.setTextColor(INK_BLACK);
        String lines[2];
        canvas.setFont(&fonts::FreeSansBold18pt7b);
        if (canvas.textWidth(next->title) <= textW) {
            canvas.drawString(next->title, textX, y);
            y += 38;
        } else {
            canvas.setFont(&fonts::FreeSansBold12pt7b);
            int n = wrapText(next->title, textW, lines, 2);
            for (int i = 0; i < n; i++) { canvas.drawString(lines[i], textX, y); y += 28; }
            y += 4;
        }

        canvas.setFont(&fonts::FreeSans12pt7b);
        String when = hhmm(next->start) + " - " + hhmm(next->end);
        canvas.drawString(when, textX, y);
        if (!next->location.isEmpty()) {
            int lx = textX + canvas.textWidth(when) + 14;
            canvas.setFont(&fonts::FreeSans9pt7b);
            canvas.drawString(fitToWidth(next->location, x + w - 10 - lx), lx, y + 4);
        }
        y += 28;
    }
    y += 4;
    canvas.drawRoundRect(x, top, w, y - top, 8, INK_BLUE);
    canvas.drawRoundRect(x + 1, top + 1, w - 2, y - top - 2, 7, INK_BLUE);
    canvas.fillRoundRect(x, top, 8, y - top, 4, INK_BLUE);
    return y;
}

struct Block {
    const Event *e;
    int top, bottom, lane = 0, lanes = 1;
};

// Timeline of today's timed events between y0 and y1. Runs of two or more
// empty hours collapse into a short gap marked "...", so the hours that have
// something in them get the space.
static void drawTimeline(int y0, int y1, time_t day, time_t now,
                         const std::vector<const Event *> &list, const Event *next) {
    int nowMin = dayMinute(now, day);
    bool daytime = nowMin >= QUIET_UNTIL && nowMin < QUIET_FROM + 60;
    int startH = 24, endH = 0;
    for (const Event *e : list) {
        startH = min(startH, dayMinute(e->start, day) / 60);
        endH = max(endH, (dayMinute(e->end, day) + 59) / 60);
    }
    if (list.empty()) { startH = 8; endH = 18; }
    if (daytime) { startH = min(startH, nowMin / 60); endH = max(endH, nowMin / 60 + 1); }
    endH = min(24, max(endH, startH + 2));   // at least two hours, padded after rather than before
    startH = max(0, min(startH, endH - 2));

    // Which hours are empty, and which of those sit in a collapsible run.
    bool empty[24] = {false}, collapsed[24] = {false};
    for (int h = startH; h < endH; h++) {
        bool used = daytime && nowMin / 60 == h;   // keep room for the now line
        for (const Event *e : list)
            if (dayMinute(e->start, day) < (h + 1) * 60 && dayMinute(e->end, day) > h * 60) used = true;
        empty[h] = !used && !list.empty();
    }
    int gaps = 0, fullHours = 0;
    for (int h = startH; h < endH;) {
        int r = h;
        while (r < endH && empty[r]) r++;
        if (r - h >= 2) { for (int k = h; k < r; k++) collapsed[k] = true; gaps++; h = r; }
        else { fullHours++; h++; }
    }

    y0 += 12;   // room for the first hour label
    y1 -= 8;
    const int GAP_H = 30;
    float hourH = (float)(y1 - y0 - gaps * GAP_H) / max(1, fullHours);
    int hourY[25];
    float acc = y0;
    for (int h = startH; h <= endH; h++) {
        hourY[h] = lroundf(acc);
        if (h == endH) break;
        if (!collapsed[h]) { acc += hourH; continue; }
        int r = h;
        while (r < endH && collapsed[r]) r++;
        for (int k = h + 1; k < r; k++) hourY[k] = lroundf(acc + GAP_H * (k - h) / (float)(r - h));
        acc += GAP_H;
        h = r - 1;
    }
    auto Y = [&](int m) {
        m = constrain(m, startH * 60, endH * 60);
        int h = min(m / 60, endH - 1);
        return hourY[h] + (hourY[h + 1] - hourY[h]) * (m - h * 60) / 60;
    };

    // Hour lines and labels, and "..." where hours were skipped.
    canvas.setFont(&fonts::DejaVu12);
    canvas.setTextColor(INK_BLACK);
    int lastLabelY = -100;
    for (int h = startH; h <= endH; h++) {
        bool inside = h > startH && h < endH && collapsed[h - 1] && collapsed[h];   // skipped boundary
        if (inside) continue;
        int y = hourY[h];
        dottedHLine(GUTTER, W - MARGIN, y, INK_BLACK);
        bool underNowBadge = daytime && abs(y - Y(nowMin)) < 14;
        if (y - lastLabelY >= 14 && !underNowBadge) {
            char b[8];
            snprintf(b, sizeof b, "%02d:00", h % 24);
            canvas.setTextDatum(textdatum_t::middle_left);
            canvas.drawString(b, 6, y);
            lastLabelY = y;
        }
        if (h < endH && collapsed[h] && (h == startH || !collapsed[h - 1])) {
            int r = h;
            while (r < endH && collapsed[r]) r++;
            int mid = (hourY[h] + hourY[r]) / 2;
            for (int i = -1; i <= 1; i++) canvas.fillCircle(22 + i * 7, mid, 2, INK_BLACK);
            for (int x = GUTTER + 4; x < W - MARGIN - 8; x += 12) {   // zig-zag: time skipped
                canvas.drawLine(x, mid + 3, x + 6, mid - 3, INK_BLACK);
                canvas.drawLine(x + 6, mid - 3, x + 12, mid + 3, INK_BLACK);
            }
        }
    }
    if (list.empty()) {
        canvas.setFont(&fonts::FreeSans12pt7b);
        canvas.setTextDatum(textdatum_t::middle_center);
        canvas.drawString("No events today", (GUTTER + W) / 2, (y0 + y1) / 2 - 20);
    }

    // Blocks. Overlaps are judged on real times; a short event is drawn taller
    // for legibility, but never down over the next event.
    std::vector<Block> blocks;
    for (const Event *e : list) {
        Block b{e};
        b.top = Y(dayMinute(e->start, day));
        b.bottom = max(Y(dayMinute(e->end, day)), b.top + 1);
        blocks.push_back(b);
    }
    std::sort(blocks.begin(), blocks.end(), [](const Block &a, const Block &b) { return a.e->start < b.e->start; });
    for (Block &b : blocks) {
        int limit = y1;
        for (const Block &o : blocks)
            if (o.e->start >= b.e->end) limit = min(limit, o.top);
        b.bottom = max(b.bottom, min(b.top + MIN_BLOCK_H, limit));
    }
    size_t clusterStart = 0;
    time_t clusterEnd = 0;
    std::vector<time_t> laneEnds;
    auto closeCluster = [&](size_t endIdx) {
        for (size_t k = clusterStart; k < endIdx; k++) blocks[k].lanes = max<int>(1, laneEnds.size());
    };
    for (size_t i = 0; i < blocks.size(); i++) {
        Block &b = blocks[i];
        if (b.e->start >= clusterEnd) {
            closeCluster(i);
            clusterStart = i;
            laneEnds.clear();
        }
        size_t lane = 0;
        while (lane < laneEnds.size() && laneEnds[lane] > b.e->start) lane++;
        if (lane == laneEnds.size()) laneEnds.push_back(b.e->end);
        else laneEnds[lane] = b.e->end;
        b.lane = lane;
        clusterEnd = max(clusterEnd, b.e->end);
    }
    closeCluster(blocks.size());

    const int areaX = GUTTER + 4, areaW = W - MARGIN - areaX;
    for (const Block &b : blocks) {
        const Event *e = b.e;
        int lw = areaW / b.lanes;
        int x = areaX + b.lane * lw, w = lw - 3, top = b.top + 1, h = b.bottom - b.top - 2;
        if (top + h > y1) h = max(MIN_BLOCK_H - 2, y1 - top);
        bool past = e->end <= now, current = e->start <= now && now < e->end, isNext = e == next;

        uint16_t fg = INK_BLACK;
        if (current) { canvas.fillRoundRect(x, top, w, h, 4, INK_GREEN); fg = INK_WHITE; }
        else if (isNext) { canvas.fillRoundRect(x, top, w, h, 4, INK_BLUE); fg = INK_WHITE; }
        else {
            canvas.fillRoundRect(x, top, w, h, 4, INK_WHITE);
            uint16_t edge = past ? INK_BLACK : INK_BLUE;
            canvas.drawRoundRect(x, top, w, h, 4, edge);
            if (!past) canvas.drawRoundRect(x + 1, top + 1, w - 2, h - 2, 3, edge);
        }

        canvas.setClipRect(x + 2, top + 1, w - 4, h - 2);
        canvas.setTextColor(fg);
        canvas.setTextDatum(textdatum_t::middle_left);
        const int tw = w - 12;
        if (h >= 40) {
            canvas.setFont(&fonts::FreeSansBold9pt7b);
            canvas.drawString(fitToWidth(e->title, tw), x + 6, top + 12);
            canvas.setFont(&fonts::DejaVu12);
            String sub = hhmm(e->start) + " - " + hhmm(e->end);
            if (!e->location.isEmpty()) sub += "  " + e->location;
            canvas.drawString(fitToWidth(sub, tw), x + 6, top + 30);
        } else if (h >= 16) {
            canvas.setFont(&fonts::DejaVu12);
            String t = hhmm(e->start) + " ";
            int tx = canvas.textWidth(t);
            canvas.drawString(t, x + 6, top + h / 2);
            canvas.setFont(&fonts::FreeSansBold9pt7b);
            canvas.drawString(fitToWidth(e->title, tw - tx), x + 6 + tx, top + h / 2);
        } else {
            canvas.setFont(&fonts::DejaVu9);
            canvas.drawString(fitToWidth(hhmm(e->start) + " " + e->title, tw), x + 6, top + h / 2);
        }
        canvas.clearClipRect();
    }

    // The current time.
    if (nowMin >= startH * 60 && nowMin <= endH * 60) {
        int y = Y(nowMin);
        canvas.fillRect(GUTTER - 2, y - 1, W - MARGIN - GUTTER + 2, 3, INK_RED);
        canvas.fillRoundRect(2, y - 10, GUTTER - 4, 20, 4, INK_RED);
        canvas.setFont(&fonts::FreeSansBold9pt7b);
        canvas.setTextColor(INK_WHITE);
        canvas.setTextDatum(textdatum_t::middle_center);
        canvas.drawString(hhmm(now), GUTTER / 2, y);
    }
}

// Weather strip along the bottom.
static void drawWeather(int y0, time_t now) {
    canvas.fillRect(0, y0, W, 2, INK_BLACK);
    if (!weather.ok) {
        canvas.setFont(&fonts::DejaVu12);
        canvas.setTextColor(INK_BLACK);
        canvas.setTextDatum(textdatum_t::middle_left);
        canvas.drawString("Weather unavailable", MARGIN, y0 + WEATHER_H / 2);
        return;
    }
    const int iconS = 64;
    drawWeatherIcon(MARGIN, y0 + (WEATHER_H - iconS) / 2 + 2, iconS, weather.code);

    // Max temperature, big, with the summary underneath.
    int x = MARGIN + iconS + 14;
    canvas.setTextColor(INK_BLACK);
    canvas.setTextDatum(textdatum_t::top_left);
    canvas.setFont(&fonts::FreeSansBold24pt7b);
    String t = String((int)lroundf(weather.tmax));
    canvas.drawString(t, x, y0 + 10);
    drawDegree(x + canvas.textWidth(t) + 2, y0 + 12, 5, INK_BLACK);
    canvas.setFont(&fonts::DejaVu12);
    String lo = String("low ") + String((int)lroundf(weather.tmin));
    canvas.drawString(lo, x, y0 + 58);
    drawDegree(x + canvas.textWidth(lo) + 1, y0 + 58, 2, INK_BLACK);

    // Rain: chance for the rest of the day and when it's likely to start.
    int nowH = dayMinute(now, weather.day) / 60;
    int restMax = 0, firstLikely = -1;
    for (int h = min(nowH, 23); h < 24; h++) {
        restMax = max(restMax, weather.hourly[h]);
        if (firstLikely < 0 && weather.hourly[h] >= 50) firstLikely = h;
    }
    String rainLine;
    if (firstLikely == nowH) rainLine = "Rain now";
    else if (firstLikely >= 0) rainLine = "Rain from " + String(firstLikely) + ":00";
    else if (restMax >= 20) rainLine = "Chance of rain";
    else rainLine = nowH < 7 ? "Dry all day" : "Dry today";

    const int rx = 190;
    canvas.setFont(&fonts::FreeSansBold18pt7b);
    const int binsLeft = rx + 32 + canvas.textWidth("100%") + 8;   // clear of the widest rain figure
    const int textW = (binCount ? binsLeft : W - MARGIN) - rx - 4;
    bool wet = firstLikely >= 0;
    drawDrop(rx + 12, y0 + 30, 22, wet ? INK_BLUE : INK_BLACK);
    if (!wet) canvas.fillCircle(rx + 12, y0 + 30 + 7, 6, INK_WHITE);   // hollow drop when dry
    canvas.setFont(&fonts::FreeSansBold18pt7b);
    canvas.setTextColor(wet ? INK_BLUE : INK_BLACK);
    canvas.setTextDatum(textdatum_t::middle_left);
    canvas.drawString(String(weather.rainMax) + "%", rx + 32, y0 + 30);
    canvas.setFont(&fonts::DejaVu12);
    canvas.setTextColor(INK_BLACK);
    canvas.setTextDatum(textdatum_t::top_left);
    canvas.drawString(fitToWidth(String(weatherText(weather.code)), textW), rx, y0 + 50);
    canvas.drawString(fitToWidth(rainLine, textW), rx, y0 + 64);

    // Bins due today, in the bottom right corner, as large as fits.
    if (binCount) {
        const int gap = 6, avail = W - MARGIN - binsLeft;
        int s = min(72, (int)((avail - (binCount - 1) * gap) / (0.78f * binCount)));
        int bw = (int)(0.78f * s);
        int x = W - MARGIN - binCount * bw - (binCount - 1) * gap;
        int y = y0 + 6 + (72 - s) / 2;
        for (int i = 0; i < binCount; i++, x += bw + gap) drawBin(x, y, s, binsToday[i]);
    }
}

static void drawStatus(time_t now) {
    String s;
    if (demoMode) s = cfgCalendarUrl.isEmpty() ? "Demo data - calendar not set up" : "Demo data";
    else if (!calendarOk) s = lastGoodFetch ? "Offline since " + hhmm(lastGoodFetch) : "Calendar unavailable";
    if (s.isEmpty()) return;
    canvas.setFont(&fonts::DejaVu9);
    canvas.setTextDatum(textdatum_t::bottom_right);
    int w = canvas.textWidth(s) + 8;
    int y = H - WEATHER_H - 3;
    canvas.fillRect(W - MARGIN - w, y - 13, w, 13, INK_RED);
    canvas.setTextColor(INK_WHITE);
    canvas.drawString(s, W - MARGIN - 4, y - 1);
}

static void render(time_t now) {
    time_t day = localMidnight(now), tomorrow = atMinute(day, 1440);
    std::vector<const Event *> allDay, timed;
    const Event *current = nullptr, *next = nullptr;
    for (const Event &e : events) {
        bool today = e.start < tomorrow && e.end > day;
        if (e.allDay) { if (today) allDay.push_back(&e); continue; }
        if (today) timed.push_back(&e);
        if (e.start <= now && now < e.end && (!current || e.start >= current->start)) current = &e;
        if (e.start > now && !next) next = &e;   // events are sorted by start
    }
    bool nextIsTomorrow = next && next->start >= tomorrow;
    if (nextIsTomorrow && minuteOfDay(now) < 12 * 60) next = nullptr;   // morning: tomorrow can wait

    canvas.fillScreen(INK_WHITE);
    drawHeader(now);
    int y = drawAllDay(HEADER_H, allDay);
    y = drawNow(y, current);
    y = drawNext(y, next, now, nextIsTomorrow, !timed.empty());
    bool status = demoMode || !calendarOk;
    drawTimeline(y + 6, H - WEATHER_H - (status ? 12 : 0), day, now, timed, nextIsTomorrow ? nullptr : next);
    drawWeather(H - WEATHER_H, now);
    drawStatus(now);
}

static void pushToPanel() {
    M5.Display.startWrite();
    canvas.pushSprite(&M5.Display, 0, 0);
    M5.Display.endWrite();
    M5.Display.setEpdMode(lgfx::epd_mode_t::epd_fastest);   // pure inks only, no dithering
    unsigned long t0 = millis();
    M5.Display.display();   // full e-paper refresh, blocks
    LOG("[display] refresh took %lu ms\n", millis() - t0);
}

static void showMessage(const char *headline, const String &detail) {
    canvas.fillScreen(INK_WHITE);
    canvas.setTextColor(INK_BLACK);
    canvas.setTextDatum(textdatum_t::top_left);
    canvas.setFont(&fonts::FreeSansBold18pt7b);
    canvas.drawString(headline, MARGIN, H / 2 - 50);
    canvas.setFont(&fonts::DejaVu18);
    String lines[3];
    int n = wrapText(detail, W - 2 * MARGIN, lines, 3);
    for (int i = 0; i < n; i++) canvas.drawString(lines[i], MARGIN, H / 2 + i * 24);
    canvas.setFont(&fonts::DejaVu12);
    canvas.drawString("Press the top button to try again", MARGIN, H - 30);
    pushToPanel();
}

// Dumps the canvas over serial as run-length-encoded ink indices, for
// tools/screenshot.py: "SHOT w h", one "R<y> <ink><count>,..." line per row, "SHOT END".
static void dumpScreenshot() {
    static const uint16_t inks[] = {INK_BLACK, INK_WHITE, INK_YELLOW, INK_RED, INK_BLUE, INK_GREEN};
    LOG("SHOT %d %d\n", W, H);
    String row;
    for (int y = 0; y < H; y++) {
        row = "R" + String(y) + " ";
        int run = 0, prev = -1;
        for (int x = 0; x <= W; x++) {
            int ink = -1;
            if (x < W) {
                uint16_t c = canvas.readPixel(x, y);
                ink = 0;
                for (int k = 0; k < 6; k++) if (c == inks[k]) { ink = k; break; }
            }
            if (ink == prev) { run++; continue; }
            if (prev >= 0) row += String(prev) + String(run) + ",";
            prev = ink;
            run = 1;
        }
        LOG("%s\n", row.c_str());
    }
    LOG("SHOT END\n");
}

// ---------------------------------------------------------------- network

static void showSetupScreen(WiFiManager *) {
    LOG("[wifi] setup portal open: join '%s', browse to 192.168.4.1\n", SETUP_AP_NAME);
    canvas.fillScreen(INK_WHITE);
    canvas.setTextColor(INK_BLACK);
    canvas.setTextDatum(textdatum_t::top_left);
    canvas.setFont(&fonts::FreeSansBold18pt7b);
    canvas.drawString("Setup", MARGIN, 40);
    canvas.setFont(&fonts::DejaVu18);
    static const char *steps[] = {
        "1. On your phone, join the WiFi",
        "   network \"VisualDay\".",
        "",
        "2. A setup page should open. If",
        "   not, browse to 192.168.4.1",
        "",
        "3. Choose Configure WiFi. Pick",
        "   your network, enter its",
        "   password, and paste your",
        "   calendar feed URL.",
        "",
        "This screen closes after five",
        "minutes. Hold the top button",
        "while switching on to open it",
        "again.",
    };
    int y = 100;
    for (const char *s : steps) { canvas.drawString(s, MARGIN, y); y += 24; }
    pushToPanel();
}

static bool waitForWiFi(unsigned long ms) {
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < ms) delay(100);
    return WiFi.status() == WL_CONNECTED;
}

// Joins the saved network (or the development one). No portal.
static bool connectWiFi() {
    if (WiFi.status() == WL_CONNECTED) return true;
    WiFi.mode(WIFI_STA);
    WiFi.begin();
    if (waitForWiFi(20000)) return true;
#ifdef WIFI_SSID
    LOG("[wifi] trying build-time network '%s'\n", WIFI_SSID);
    WiFi.disconnect();   // stop retrying the saved network, or begin() is refused
    delay(200);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    if (waitForWiFi(20000)) return true;
#endif
    LOG("[wifi] not connected\n");
    return false;
}

// Setup portal: WiFi network plus calendar URL and weather location.
static void runSetupPortal() {
    char lat[16], lon[16];
    snprintf(lat, sizeof lat, "%.4f", cfgLat);
    snprintf(lon, sizeof lon, "%.4f", cfgLon);
    WiFiManagerParameter pCal("cal", "Calendar feed URL (from the Apps Script)", cfgCalendarUrl.c_str(), 300);
    WiFiManagerParameter pLat("lat", "Weather latitude", lat, 15);
    WiFiManagerParameter pLon("lon", "Weather longitude", lon, 15);

    WiFiManager wm;
    wm.setDebugOutput(false);
    wm.setAPCallback(showSetupScreen);
    wm.setConfigPortalTimeout(SETUP_PORTAL_TIMEOUT_S);
    wm.setBreakAfterConfig(true);   // keep the other settings even if the WiFi join fails
    wm.addParameter(&pCal);
    wm.addParameter(&pLat);
    wm.addParameter(&pLon);
    wm.startConfigPortal(SETUP_AP_NAME);   // blocks while open

    cfgCalendarUrl = normaliseUrl(pCal.getValue());
    float la = atof(pLat.getValue()), lo = atof(pLon.getValue());
    if (la != 0 || lo != 0) { cfgLat = la; cfgLon = lo; }
    saveConfig();
    LOG("[config] saved: calendar %s, weather at %.4f,%.4f\n",
        cfgCalendarUrl.isEmpty() ? "not set" : "set", cfgLat, cfgLon);
}

static bool httpGet(const String &url, String &body, String &err) {
    WiFiClientSecure client;
    client.setInsecure();   // no CA bundle on the device; fine for a personal display
    HTTPClient http;
    http.setUserAgent(USER_AGENT);
    http.setTimeout(20000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);   // Apps Script answers via a redirect
    if (!http.begin(client, url)) { err = "http.begin failed"; return false; }
    int code = http.GET();
    if (code != 200) { err = "HTTP " + String(code); http.end(); return false; }
    body = http.getString();
    http.end();
    return true;
}

static bool fetchCalendar(time_t day, String &err) {
    char range[64];
    snprintf(range, sizeof range, "start=%lld&end=%lld", (long long)day, (long long)atMinute(day, 2 * 1440));
    String url = cfgCalendarUrl + (cfgCalendarUrl.indexOf('?') >= 0 ? "&" : "?") + range;
    String body;
    unsigned long t0 = millis();
    if (!httpGet(url, body, err)) return false;
    JsonDocument doc;
    DeserializationError e = deserializeJson(doc, body);
    if (e) { err = String("JSON: ") + e.c_str(); return false; }
    if (doc["error"].is<const char *>()) { err = doc["error"].as<String>(); return false; }

    events.clear();
    for (JsonObject o : doc["events"].as<JsonArray>()) {
        Event ev;
        ev.title = o["t"] | "";
        ev.location = o["l"] | "";
        ev.start = o["s"] | 0LL;
        ev.end = o["e"] | 0LL;
        ev.allDay = (o["a"] | 0) != 0;
        if (ev.end <= ev.start) ev.end = ev.start + 60;
        events.push_back(ev);
    }
    std::stable_sort(events.begin(), events.end(), [](const Event &a, const Event &b) { return a.start < b.start; });
    eventsDay = day;

    char today[12];
    struct tm tm;
    localtime_r(&day, &tm);
    strftime(today, sizeof today, "%Y-%m-%d", &tm);
    binCount = 0;
    for (JsonObject b : doc["bins"].as<JsonArray>())
        if (String(b["d"] | "") == today) addBin(binColour(b["c"] | ""));
    LOG("[calendar] %u events, %d bins in %lu ms\n", events.size(), binCount, millis() - t0);
    return true;
}

static bool fetchWeather(time_t day, String &err) {
    char url[320];
    snprintf(url, sizeof url,
             "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max"
             "&hourly=precipitation_probability&timezone=auto&forecast_days=1",
             cfgLat, cfgLon);
    String body;
    if (!httpGet(url, body, err)) return false;
    JsonDocument doc;
    DeserializationError e = deserializeJson(doc, body);
    if (e) { err = String("JSON: ") + e.c_str(); return false; }
    JsonObject d = doc["daily"];
    if (d.isNull()) { err = "no daily forecast"; return false; }
    weather.code = d["weather_code"][0] | 0;
    weather.tmax = d["temperature_2m_max"][0] | 0.0f;
    weather.tmin = d["temperature_2m_min"][0] | 0.0f;
    weather.rainMax = d["precipitation_probability_max"][0] | 0;
    JsonArray hp = doc["hourly"]["precipitation_probability"];
    for (int h = 0; h < 24; h++) weather.hourly[h] = hp[h] | 0;
    weather.ok = true;
    weather.fetchedAt = nowT();
    weather.day = day;
    LOG("[weather] code %d, max %.1f, min %.1f, rain %d%%\n", weather.code, weather.tmax, weather.tmin, weather.rainMax);
    return true;
}

// ---------------------------------------------------------------- demo data

static void loadDemoEvents(time_t day) {
    struct { int s, e; const char *t, *l; bool allDay; int dayOffset; } demo[] = {
        {0, 1440, "Mum's birthday", "", true, 0},
        {8 * 60 + 30, 9 * 60, "School run", "", false, 0},
        {9 * 60 + 30, 9 * 60 + 45, "Standup", "", false, 0},
        {10 * 60, 11 * 60 + 30, "Design review", "Google Meet", false, 0},
        {12 * 60 + 30, 13 * 60 + 30, "Lunch with Alex", "The Old House", false, 0},
        {15 * 60, 15 * 60 + 30, "Dentist", "West St Surgery", false, 0},
        {15 * 60 + 15, 16 * 60, "Call the bank", "", false, 0},
        {18 * 60 + 30, 20 * 60, "Football training", "Town Park", false, 0},
        {9 * 60, 10 * 60, "Car MOT", "Main Road Garage", false, 1},
    };
    events.clear();
    for (auto &d : demo) {
        Event e;
        time_t base = atMinute(day, d.dayOffset * 1440);
        e.title = d.t;
        e.location = d.l;
        e.start = atMinute(base, d.s);
        e.end = atMinute(base, d.e);
        e.allDay = d.allDay;
        events.push_back(e);
    }
    eventsDay = day;
    binCount = 0;
    addBin(INK_GREEN);
    addBin(BIN_BROWN);
}

// ---------------------------------------------------------------- schedule

// Next quarter hour or event start/end, skipping the quiet hours overnight
// apart from one refresh just after midnight.
static time_t nextRefreshAfter(time_t now) {
    time_t day = localMidnight(now);
    int m = minuteOfDay(now);
    time_t cand = atMinute(day, (m / REFRESH_EVERY + 1) * REFRESH_EVERY);
    for (const Event &e : events) {
        if (e.allDay) continue;
        if (e.start > now && e.start < cand) cand = e.start;
        if (e.end > now && e.end < cand) cand = e.end;
    }
    int cm = minuteOfDay(cand);
    time_t cday = localMidnight(cand);
    if (cm > QUIET_FROM) cand = atMinute(cday, 1440 + NEW_DAY_AT);
    else if (cm < QUIET_UNTIL) cand = now < atMinute(cday, NEW_DAY_AT) ? atMinute(cday, NEW_DAY_AT) : atMinute(cday, QUIET_UNTIL);

    // An event still running when the quiet hours start gets one more refresh
    // when it ends, so it doesn't sit there as "now" until morning.
    time_t quietStart = atMinute(day, QUIET_FROM);
    for (const Event &e : events) {
        if (e.allDay || e.start > quietStart || e.end <= quietStart || e.end <= now) continue;
        if (e.end < cand) cand = e.end;
    }
    return cand;
}

// The clock keeps running through deep sleep, but the sleep timer is not
// precise, so resync on every wake. Waits briefly if the time is already
// roughly right, longer after power-on when it isn't.
static bool syncTime() {
    configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com");
    unsigned long t0 = millis(), limit = timeValid() ? 5000 : 15000;
    bool synced = false;   // the status reads COMPLETED once, then resets
    while (!(synced = sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) && millis() - t0 < limit) delay(100);
    LOG("[time] %s after %lu ms: %s\n", synced ? "synced" : "not synced", millis() - t0, hhmm(nowT()).c_str());
    return timeValid();
}

// Fetch what's needed, draw, push to the panel, and schedule the next one.
static void refresh(bool toPanel = true) {
    bool online = connectWiFi();
    if (!timeValid() && !(online && syncTime())) {
        showMessage("No connection", online ? "Couldn't get the time from the internet." : "Couldn't join the WiFi network.");
        nextRefreshAt = 0;
        return;
    }
    time_t now = nowT(), day = localMidnight(now);
    String err;
    demoMode = demoMode || cfgCalendarUrl.isEmpty();

    if (demoMode) {
        loadDemoEvents(day);
        calendarOk = true;
    } else if (online && fetchCalendar(day, err)) {
        calendarOk = true;
        lastGoodFetch = now;
        saveEventsToRtc();
    } else {
        LOG("[calendar] fetch failed: %s\n", online ? err.c_str() : "offline");
        calendarOk = false;
        if (eventsDay != day) { events.clear(); binCount = 0; }   // don't show yesterday's as today's
    }

    if (online && (!weather.ok || weather.day != day || now - weather.fetchedAt > WEATHER_MAX_AGE_S)) {
        if (!fetchWeather(day, err)) LOG("[weather] fetch failed: %s\n", err.c_str());
    }
    if (weather.day != day) weather.ok = false;

    render(now);
    if (toPanel) pushToPanel();
    nextRefreshAt = calendarOk ? nextRefreshAfter(now) : min(nextRefreshAfter(now), now + RETRY_AFTER_S);
    LOG("[schedule] next refresh at %s\n", hhmm(nextRefreshAt).c_str());
}

// ---------------------------------------------------------------- serial commands

// Line commands over USB serial, for development:
//   r        refresh now
//   s        dump a screenshot of the current screen
//   d        toggle demo data
//   t1430    pretend it's 14:30 today, redraw without refreshing the panel, dump
//   t        back to the real time
//   w        open the setup portal
static void handleCommand(String cmd) {
    cmd.trim();
    if (cmd.isEmpty()) return;
    LOG("[cmd] %s\n", cmd.c_str());
    char c = cmd[0];
    if (c == 'r') refresh();
    else if (c == 's') dumpScreenshot();
    else if (c == 'd') { demoMode = !demoMode; refresh(false); dumpScreenshot(); }
    else if (c == 'w') { runSetupPortal(); refresh(); }
    else if (c == 't') {
        fakeOffset = 0;
        if (cmd.length() >= 5) {
            int hm = cmd.substring(1).toInt();
            time_t real = time(nullptr);
            fakeOffset = (long)(atMinute(localMidnight(real), (hm / 100) * 60 + hm % 100) - real);
        }
        render(nowT());
        LOG("[schedule] from %s the next refresh would be %s\n", hhmm(nowT()).c_str(), hhmm(nextRefreshAfter(nowT())).c_str());
        dumpScreenshot();
    }
}

// ---------------------------------------------------------------- sleep

static bool topButtonDown() { return !digitalRead(TOP_BUTTON_PIN); }

// True if the top button stays down for SETUP_HOLD_MS.
static bool topButtonHeld() {
    unsigned long t0 = millis();
    while (topButtonDown()) {
        if (millis() - t0 >= SETUP_HOLD_MS) return true;
        delay(20);
    }
    return false;
}

// Deep sleep until the next scheduled refresh, or until the top button.
// The e-paper keeps showing the last image with no power.
static void goToSleep() {
    fakeOffset = 0;
    time_t now = time(nullptr);
    long secs = nextRefreshAt > now ? (long)(nextRefreshAt - now) : RETRY_AFTER_S;
    secs = max(secs, 5L);
    LOG("[sleep] %ld s (until %s)\n", secs, hhmm(now + secs).c_str());

    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    M5.Display.waitDisplay();
    M5.Display.sleep();

    esp_sleep_enable_timer_wakeup((uint64_t)secs * 1000000ULL);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);   // keeps the pull-up alive
    rtc_gpio_pullup_en(TOP_BUTTON_PIN);
    rtc_gpio_pulldown_dis(TOP_BUTTON_PIN);
    esp_sleep_enable_ext1_wakeup_io(1ULL << TOP_BUTTON_PIN, ESP_EXT1_WAKEUP_ANY_LOW);
    while (topButtonDown()) delay(10);   // or it wakes straight back up
    delay(50);
    esp_deep_sleep_start();
}

// ---------------------------------------------------------------- arduino

void setup() {
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    bool timerWake = cause == ESP_SLEEP_WAKEUP_TIMER;
    bool buttonWake = cause == ESP_SLEEP_WAKEUP_EXT1;
    if (timerWake || buttonWake) rtc_gpio_deinit(TOP_BUTTON_PIN);   // hand the pin back to normal GPIO

    auto cfg = M5.config();
    cfg.clear_display = false;   // each clear is a 16 s refresh, and the old image should stay up while we work
    M5.begin(cfg);
    Serial.begin(115200);   // USB CDC receive side, for the serial commands
    pinMode(TOP_BUTTON_PIN, INPUT_PULLUP);

    M5.Display.setAutoDisplay(false);
    W = M5.Display.width();
    H = M5.Display.height();
    canvas.setPsram(true);
    canvas.setColorDepth(16);
    if (!canvas.createSprite(W, H)) LOG("[boot] canvas allocation failed\n");
    LOG("[boot] %s, display %dx%d, %lu ms\n",
        timerWake ? "timer wake" : buttonWake ? "button wake" : "power-on", W, H, millis());

    setenv("TZ", TZ_INFO, 1);
    tzset();
    loadConfig();
    loadEventsFromRtc();

    // Holding the top button (while switching on, or to wake it) opens setup.
    bool forceSetup = topButtonHeld();
    if (forceSetup) LOG("[boot] top button held: setup requested\n");

    WiFi.mode(WIFI_STA);
    if (forceSetup || (!timerWake && !connectWiFi())) runSetupPortal();
    if (connectWiFi()) syncTime();

    if (timerWake && timeValid() && nextRefreshAt) {
        long early = (long)(nextRefreshAt - time(nullptr));
        if (early > EARLY_WAKE_SLACK_S) { LOG("[boot] woke %ld s early\n", early); goToSleep(); }
        if (early > 0) delay(early * 1000UL);
    }

    refresh();
    if (timerWake) goToSleep();
    awakeUntil = millis() + AWAKE_WINDOW_S * 1000UL;
}

void loop() {
    M5.update();

    static String line;
    while (Serial.available()) {
        char ch = Serial.read();
        if (ch == '\n' || ch == '\r') {
            handleCommand(line);
            line = "";
            awakeUntil = millis() + AWAKE_WINDOW_S * 1000UL;
        } else if (line.length() < 64) {
            line += ch;
        }
    }

    if (BtnTop.wasClicked()) {
        LOG("[btn] top: refresh\n");
        refresh();
    } else if (nextRefreshAt && nowT() >= nextRefreshAt) {
        refresh();
    }
    if ((long)(millis() - awakeUntil) >= 0) goToSleep();
    delay(50);
}
