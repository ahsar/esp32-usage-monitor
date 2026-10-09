#define spr (*hwCanvas())

#include "config.h"
#include "storage.h"
#include "api_client.h"
#include "token_store.h"
#include "weather_client.h"
#include "usage_display.h"
#include "wifi_setup.h"
#include "pager.h"
#include "status_server.h"
#include "clock_sync.h"
#include "easter_egg.h"
#include "hw/hw.h"
#include "hw/net.h"
#include "voice/i2s_audio.h"
#include <math.h>
#include <WiFi.h>

static UsageData s_usage;
static WeatherData s_weather;
static MoonData s_moon;
static AirData s_air;
static AppConfig s_cfg;
static TokenSet s_tokens;

static uint32_t s_lastFetch = 0;
static uint32_t s_lastWeather = 0;
static uint32_t s_lastMoon = 0;
static uint32_t s_lastAir = 0;
static WifiState s_wifi;

// ---- display power (dirty-frame push + idle dim) --------------------------
static bool     s_dirty = true;          // canvas changed → panel needs a push
static bool     s_screenDim = false;
static int      s_brightLevel = 2;       // BRIGHT_LUT index; 2 = 150/255
static uint32_t s_lastActivity = 0;
static bool     s_swallowKey   = false;  // first key only wakes, doesn't act
static uint32_t s_lastDraw = 0;
static bool     s_uiDirty = true;        // pager state changed → redraw now

static const uint32_t IDLE_DIM_MS = 30000;   // 30 s idle → dim
static const uint8_t  DIM_LEVEL   = 0;       // BRIGHT_LUT[0] = 50/255

static void screenWake() {
    if (s_screenDim) {
        hwDisplayBrightness((uint8_t)s_brightLevel);
        s_screenDim = false;
    }
    s_lastDraw = 0;          // force an immediate re-render on wake
    s_uiDirty  = true;
    s_lastActivity = millis();
}

static void screenIdleUpdate() {
    // Dim only — never blank the panel. It stays readable at DIM_LEVEL so the
    // clock/usage is still glanceable without having to touch the screen.
    if (!s_screenDim && millis() - s_lastActivity >= IDLE_DIM_MS) {
        hwDisplayBrightness(DIM_LEVEL);
        s_screenDim = true;
    }
}

// ---- UI environment -------------------------------------------------------

static UiEnv s_env;
static uint32_t s_envNext = 0;

static void refreshEnv() {
    uint32_t now = millis();
    if (now < s_envNext) return;
    s_envNext = now + 1000;

    s_env.online = (WiFi.status() == WL_CONNECTED);
    s_env.timeValid = clockSyncValid();

    HwTime t;
    if (hwRtcRead(&t)) {
        s_env.hh = t.H; s_env.mm = t.M; s_env.ss = t.S;
        s_env.year = t.Y; s_env.month = t.Mo; s_env.day = t.D; s_env.weekday = t.dow;
    }
    HwBattery bat = hwBattery();
    s_env.batteryPct = bat.pct;
    s_env.charging = bat.charging;
    s_env.usbPresent = bat.usbPresent;
    s_env.city = "Beijing";
    uiSetEnv(s_env);
}

// ---- rendering ------------------------------------------------------------

static const int PAGE_W = HW_W;
static const int PAGE_H = HW_H;

#ifdef FRAME_DUMP
static void renderUi();

// Debug-only: walk every screen/card and stream the raw RGB565 canvas over USB
// CDC so a host script can rebuild PNGs. Build with -DFRAME_DUMP.
static uint32_t s_dumpIdx = 0;
static uint32_t s_dumpNext = 0;
static void frameDump() {
    if (s_dumpIdx >= 7) return;
    if (millis() < s_dumpNext) return;
    // Wait for real data (or give up after 45s) so the cards aren't placeholders.
    if (s_dumpIdx == 0 && !(s_usage.valid && s_weather.valid) && millis() < 45000) return;
    s_dumpNext = millis() + 1500;

    uint32_t idx = s_dumpIdx;
    if (idx < UI_PAGE_COUNT) {
        pagerSetPage((int)idx);
    } else {
        pagerSetPage(UI_PAGE_WEATHER);
        pagerSetCard((int)(idx - UI_PAGE_COUNT + 1));
    }
    pagerCancelAnim();

    renderUi();
    hwDisplayPush();

    uint16_t* fb = (uint16_t*)hwCanvas()->getFramebuffer();
    Serial.printf("FRAME %u %d %d\n", idx, HW_CANVAS_W, HW_CANVAS_H);
    delay(30);
    Serial.write((uint8_t*)fb, HW_CANVAS_W * HW_CANVAS_H * 2);
    Serial.write('\n');
    Serial.flush();
    s_dumpIdx++;
}
#endif

static void drawPage(int page, int card, int ox, int oy) {
    switch (page) {
        case UI_PAGE_USAGE: uiDrawUsage(s_usage, ox, oy); break;
        case UI_PAGE_CLOCK: uiDrawClock(ox, oy); break;
        default:
            uiDrawWeatherCard(card, s_weather, s_moon, s_air,
                              s_cfg.waqi_token[0] != 0, ox, oy);
            break;
    }
}

static void renderUi() {
    uiBeginFrame();

    bool vaxis = pagerVerticalAxis();
    int off = (int)lroundf(pagerOffset());
    int page = pagerPage();
    int card = pagerCard();
    bool hasAir = s_cfg.waqi_token[0] != 0;

    if (vaxis && page == UI_PAGE_WEATHER) {
        uiDrawWeatherCard(card, s_weather, s_moon, s_air, hasAir, 0, off);
        if (off < 0 && card + 1 < UI_WC_COUNT)
            uiDrawWeatherCard(card + 1, s_weather, s_moon, s_air, hasAir, 0, off + PAGE_H);
        else if (off > 0 && card - 1 >= 0)
            uiDrawWeatherCard(card - 1, s_weather, s_moon, s_air, hasAir, 0, off - PAGE_H);
    } else {
        drawPage(page, card, off, 0);
        if (off < 0 && page + 1 < UI_PAGE_COUNT) drawPage(page + 1, card, off + PAGE_W, 0);
        else if (off > 0 && page - 1 >= 0) drawPage(page - 1, card, off - PAGE_W, 0);
    }

    uiDrawPageDots(page);
    if (page == UI_PAGE_WEATHER) uiDrawCardDots(card, UI_WC_COUNT);
}

// ---- boot / loop ----------------------------------------------------------

static void playBootTone() {
    if (!audioInit()) return;
    uint8_t toneBuf[512];
    float phase = 0;
    uint32_t total = 0;
    const uint32_t target = 32000;
    if (!audioTxStart()) return;
    while (total < target) {
        size_t n = 512;
        if (total + n > target) n = target - total;
        for (size_t i = 0; i < n; i += 2) {
            int16_t s = (int16_t)(8000 * sin(phase));
            toneBuf[i] = s & 0xFF;
            toneBuf[i + 1] = (s >> 8) & 0xFF;
            phase += 2 * PI * 440.0 / 16000.0;
            if (phase > 2 * PI) phase -= 2 * PI;
        }
        audioTxWrite(toneBuf, n);
        total += n;
    }
    audioTxStop();
    Serial.println("boot tone done");
}

void setup() {
    Serial.begin(115200);
    delay(100);
    Serial.println("ESP32 Usage Monitor");

    hwInit();

    spr.fillScreen(0x0000);
    spr.drawRoundRect(SAFE_L, SAFE_T, SAFE_W, SAFE_H, 8, 0xFFFF);
    spr.drawRect(SAFE_L + 4, SAFE_T + 4, SAFE_W - 8, SAFE_H - 8, 0x8410);
    hwDisplayPush();

    storageMount();
    loadConfig(s_cfg);
    tokenStoreLoad(s_tokens);
    if (!s_tokens.refreshToken[0] && s_cfg.refresh_token[0]) {
        strlcpy(s_tokens.refreshToken, s_cfg.refresh_token, sizeof(s_tokens.refreshToken));
        s_tokens.valid = true;
        s_tokens.dirty = true;
    }
    if (!s_tokens.orgId[0]) {
        strlcpy(s_tokens.orgId,
                s_cfg.org_id[0] ? s_cfg.org_id : s_cfg.workspace_id,
                sizeof(s_tokens.orgId));
    }
    if (!s_tokens.valid) {
        Serial.println("No refresh token; run tools/authorize.py to authorise");
    }
    if (!s_cfg.waqi_token[0]) {
        Serial.println("No waqi_token; air quality card will be empty");
    }

    pagerInit();
    statusServerSetWakeCallback(screenWake);
    easterEggSetWakeCallback(screenWake);

    playBootTone();

    char savedSSID[33], savedPass[65];
    bool hasCreds = netLoadCred(savedSSID, 33, savedPass, 65);

    WiFi.mode(WIFI_STA);
    // Radio sleeps between DTIM beacons — big heat/power win vs. the old
    // WiFi.setSleep(false). Costs up to ~a DTIM interval of latency on the
    // status-server webhook, which is fine for a desk gadget.
    WiFi.setSleep(WIFI_PS_MIN_MODEM);
    // Desk device sits close to the AP; 11 dBm (~13 mW) is plenty and cuts
    // radio heat vs. the ~19.5 dBm default (~89 mW). Bump if signal is weak.
    WiFi.setTxPower(WIFI_POWER_11dBm);
    delay(300);
    if (hasCreds) {
        WiFi.begin(savedSSID, savedPass);
    } else {
        Serial.println("No saved WiFi; opening scan/setup UI");
    }

    wifiSetupInit(s_wifi, hasCreds ? savedSSID : nullptr);
}

void loop() {
    hwInputUpdate();
    handleHttpStatus();

#if BOARD_HAS_KEY2
    bool pwrLong = hwAxpPekeyLongPress();
#else
    bool pwrLong = hwAxpBtnEvent() & 0x04;
#endif
    if (pwrLong) {
        Serial.println("PWR long-press: shutting down");
        spr.fillScreen(0x0000);
#if BOARD_DISPLAY_UI_SCALE > 1
        spr.setFont(u8g2_font_helvB24_tf);
#else
        spr.setFont(u8g2_font_helvB12_tf);
#endif
        spr.setTextColor(0xFFFF);
        spr.setCursor(SAFE_L, SAFE_T + SAFE_H / 2);
        spr.print("Shutting down...");
        hwDisplayPush();
        delay(400);
        hwPowerOff();
    }

    uint32_t now = millis();
    refreshEnv();

    if (s_wifi.open && WiFi.status() == WL_CONNECTED) {
        WiFi.setSleep(WIFI_PS_MIN_MODEM);
        WiFi.setTxPower(WIFI_POWER_11dBm);
        clockSyncUpdate(now, true);
        statusServerBegin();
        s_wifi.open = false;
        s_lastFetch = 0;
        s_lastWeather = 0;
        s_lastMoon = 0;
        s_lastAir = 0;
        s_lastActivity = millis();   // don't idle-out the instant setup closes
        Serial.println("WiFi connected (auto)");
    }

    // Any touch/key activity resets the idle timer (and wakes a dimmed panel).
    const HwTouch& tp = hwTouch();
    bool keyHit = hwBtnA().wasPressed
#if BOARD_HAS_KEY2
                || hwBtnB().wasPressed
#endif
#if BOARD_BTN_THIRD
                || hwBtnBoot().wasPressed
#endif
                ;
    bool activity = tp.justPressed || tp.down || keyHit;
    if (activity) {
        if (s_screenDim) {
            screenWake();
            if (tp.justPressed) pagerSwallowNextTap();
            if (keyHit)         s_swallowKey   = true;
        } else {
            s_lastActivity = now;
        }
    }
    if (!s_wifi.open) screenIdleUpdate();

    if (!s_wifi.open) {
        // ---- network polling ----
        if (s_tokens.valid && (now - s_lastFetch >= 60000 || s_lastFetch == 0)) {
            s_lastFetch = now;
            apiFetchUsage(s_usage, s_tokens);
            if (s_tokens.dirty) {
                tokenStoreSave(s_tokens);
                s_tokens.dirty = false;
            }
        }

        if (now - s_lastWeather >= 900000 || s_lastWeather == 0) {
            s_lastWeather = now;
            if (apiFetchWeather(s_weather) && clockSyncValid()) {
                snprintf(s_weather.obsTime, sizeof(s_weather.obsTime), "%02d:%02d",
                         s_env.hh, s_env.mm);
            }
        }

        if (now - s_lastMoon >= 21600000 || s_lastMoon == 0) {
            s_lastMoon = now;
            apiFetchMoon(s_moon);
        }

        if (s_cfg.waqi_token[0] && (now - s_lastAir >= 1800000 || s_lastAir == 0)) {
            s_lastAir = now;
            apiFetchAir(s_air, s_cfg.waqi_token);
        }

        clockSyncUpdate(now, false);

        // ---- easter egg (shake) ----
        easterEggUpdate(now);

        bool thinking = statusServerThinking();
        bool animated = easterEggActive() || thinking;

        if (hwBtnA().wasPressed && !s_swallowKey) {
            if (easterEggActive()) {
                easterEggStop();
            }
            if (thinking) {
                statusServerClearThinking();
            } else {
                pagerAdvance();
                s_uiDirty = true;
            }
        }

        if (!animated && pagerUpdate()) s_uiDirty = true;

#ifdef FRAME_DUMP
        frameDump();
#else
        // Redraw immediately on any pager state change (drag, animation step,
        // commit, key) so the final frame is never left to the 1 s idle tick —
        // otherwise the screen freezes on a mid-swipe frame for up to a second.
        bool fast = animated || s_uiDirty;
        if (fast || now - s_lastDraw >= 1000) {
            s_lastDraw = now;
            if (easterEggActive()) {
                usageDisplayDrawEaster555(easterEggElapsedMs());
            } else if (thinking) {
                usageDisplayDrawThinking(now - statusServerThinkingSince());
            } else {
                renderUi();
            }
            s_uiDirty = false;
            s_dirty = true;
        }
#endif
    } else {
        wifiSetupDraw(s_wifi);
        s_dirty = true;
    }

    // Push only when the canvas actually changed: idle frames drop from ~60/s
    // to ~1/s, and nothing is pushed while the panel is off.
    // ApiFetch* keep the status server alive; nothing else to do here.
    if (s_dirty) {
        hwDisplayPush();
        s_dirty = false;
    }
    s_swallowKey = false;
    delay(16);
}
