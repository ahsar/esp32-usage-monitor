#include "config.h"
#include "storage.h"
#include "api_client.h"
#include "http_client.h"
#include "token_store.h"
#include "weather_client.h"
#include "usage_display.h"
#include "ui_compose.h"
#include "frame_dump.h"
#include "wifi_setup.h"
#include "pager.h"
#include "status_server.h"
#include "clock_sync.h"
#include "easter_egg.h"
#include "util/interval.h"
#include "hw/hw.h"
#include "hw/net.h"
#include "voice/i2s_audio.h"
#include "version.h"
#include <WiFi.h>

static UsageData s_usage;
static WeatherData s_weather;
static MoonData s_moon;
static AirData s_air;
static AppConfig s_cfg;
static TokenSet s_tokens;

// Periodic network pollers. loop() runs at most ONE of these per iteration, so
// a burst of due timers (e.g. right after WiFi connects) can never stall input
// and rendering behind several back-to-back blocking fetches.
static Interval s_ivUsage(60000);
static Interval s_ivWeather(900000);
static Interval s_ivMoon(21600000);
static Interval s_ivAir(1800000);
static Interval s_ivEnv(1000);

static WifiState s_wifi;

// ---- display power (dirty-frame push + idle dim) --------------------------
static bool     s_dirty = true;          // canvas changed → panel needs a push
static bool     s_screenDim = false;
static int      s_brightLevel = 2;       // BRIGHT_LUT index; 2 = 150/255
static uint32_t s_lastActivity = 0;
static bool     s_swallowKey   = false;  // first key only wakes, doesn't act
static uint32_t s_lastDraw = 0;
// Two distinct dirty stages: `s_redrawNow` means input/pager changed so we must
// re-render this iteration; `s_dirty` means the canvas differs from the panel
// and needs a push. Keeping them separate is what lets idle frames skip the
// render+push entirely (idle drops from ~60/s to ~1/s).
static bool     s_redrawNow = true;      // render this iteration (pager/key/wake)

static const uint32_t IDLE_DIM_MS = 30000;   // 30 s idle → dim
static const uint8_t  DIM_LEVEL   = 0;       // BRIGHT_LUT[0] = 50/255

static void screenWake() {
    if (s_screenDim) {
        hwDisplayBrightness((uint8_t)s_brightLevel);
        s_screenDim = false;
    }
    s_lastDraw = 0;          // force an immediate re-render on wake
    s_redrawNow = true;
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

static void refreshEnv(uint32_t now) {
    if (!s_ivEnv.due(now)) return;
    s_ivEnv.ran(now);

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

static UiData currentUiData() {
    UiData d;
    d.usage   = &s_usage;
    d.weather = &s_weather;
    d.moon    = &s_moon;
    d.air     = &s_air;
    d.hasAir  = s_cfg.waqi_token[0] != 0;
    return d;
}

// ---- boot / loop ----------------------------------------------------------

// Bring up the I2S bus + ES8311 codec at boot. No startup sound; this is what
// the shake-to-play 555 easter egg relies on for playback later.
static void audioBootInit() {
    if (!audioInit()) Serial.println("audio: init failed (no easter-egg sound)");
}

// ---- UI service frame -----------------------------------------------------

// One UI frame: activity/idle bookkeeping, Key1 handling, pager, render and
// panel push. Input must already be up to date for this iteration. Factored
// out of loop() so the blocking fetch path can pump the same code (uiPump)
// and never freeze the screen mid-scroll.
static void uiFrame(uint32_t now) {
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

    if (s_wifi.open) {
        wifiSetupDraw(s_wifi);
        s_dirty = true;
        return;
    }

    screenIdleUpdate();

    bool thinking = statusServerThinking();
    if (hwBtnA().wasPressed && !s_swallowKey) {
        if (easterEggActive()) {
            easterEggStop();
        }
        if (thinking) {
            statusServerClearThinking();
            thinking = false;
        } else {
            pagerAdvance();
            s_redrawNow = true;
        }
    }

    // Always advance the gesture machine, even while the easter egg / thinking
    // animation owns the screen — otherwise scrolling is ignored and looks
    // stuck. The render below still shows the animation instead of the pager.
    if (pagerUpdate()) s_redrawNow = true;

    bool animated = easterEggActive() || thinking;

#ifdef FRAME_DUMP
    frameDumpTick(now, currentUiData());
#else
    bool fast = animated || s_redrawNow;
    if (fast || now - s_lastDraw >= 1000) {
        s_lastDraw = now;
        if (easterEggActive()) {
            usageDisplayDrawEaster555(easterEggElapsedMs());
        } else if (thinking) {
            usageDisplayDrawThinking(now - statusServerThinkingSince());
        } else {
            uiRender(currentUiData());
        }
        s_redrawNow = false;
        s_dirty = true;
    }
#endif

    if (s_dirty) {
        hwDisplayPush();
        s_dirty = false;
    }
}

// Pumped from inside a blocking fetch (http_client) so input/pager/animation
// keep running while the radio is busy. Must not touch the network stack.
static void uiPump() {
    uint32_t now = millis();
    hwInputUpdate();
    refreshEnv(now);
    clockSyncUpdate(now, false);
    easterEggUpdate(now);
    uiFrame(now);
}

void setup() {
    Serial.begin(115200);
    delay(100);
    Serial.printf("ESP32 Usage Monitor fw %s\n", FW_VERSION);

    hwInit();

    spr.fillScreen(0x0000);
    spr.drawRoundRect(SAFE_L, SAFE_T, SAFE_W, SAFE_H, 8, 0xFFFF);
    spr.drawRect(SAFE_L + 4, SAFE_T + 4, SAFE_W - 8, SAFE_H - 8, 0x8410);
    hwDisplayPush();

    storageMount();
    loadConfig(s_cfg);
    tokenStoreInit(s_tokens, s_cfg.refresh_token,
                   s_cfg.org_id[0] ? s_cfg.org_id : s_cfg.workspace_id);
    if (!s_tokens.valid) {
        Serial.println("No refresh token; run tools/authorize.py to authorise");
    }
    if (!s_cfg.waqi_token[0]) {
        Serial.println("No waqi_token; air quality card will be empty");
    }

    pagerInit();
    httpSetUiPump(uiPump);
    statusServerSetWakeCallback(screenWake);
    easterEggSetWakeCallback(screenWake);

    audioBootInit();

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
    refreshEnv(now);

    if (s_wifi.open && WiFi.status() == WL_CONNECTED) {
        WiFi.setSleep(WIFI_PS_MIN_MODEM);
        WiFi.setTxPower(WIFI_POWER_11dBm);
        clockSyncUpdate(now, true);
        statusServerBegin();
        s_wifi.open = false;
        // Pull fresh data on the next few iterations (one fetch per iteration).
        s_ivUsage.forcePending();
        s_ivWeather.forcePending();
        s_ivMoon.forcePending();
        s_ivAir.forcePending();
        s_lastActivity = millis();   // don't idle-out the instant setup closes
        Serial.println("WiFi connected (auto)");
    }

    if (!s_wifi.open) {
        // ---- network polling (at most one fetch per iteration) ----
        // Skip the whole block while offline: a due timer stays pending, so the
        // first fetch fires as soon as the link is back instead of each timer
        // attempt blocking the loop on a connect() timeout.
        // The fetch is blocking, but http_client pumps uiPump() every ~16 ms so
        // input and page animations keep running while it is in flight.
        if (WiFi.status() == WL_CONNECTED) {
            if (s_tokens.valid && s_ivUsage.due(now)) {
                s_ivUsage.ran(now);
                apiFetchUsage(s_usage, s_tokens);
                if (s_tokens.dirty) {
                    tokenStoreSave(s_tokens);
                    s_tokens.dirty = false;
                }
            } else if (s_ivWeather.due(now)) {
                s_ivWeather.ran(now);
                if (apiFetchWeather(s_weather) && clockSyncValid()) {
                    snprintf(s_weather.obsTime, sizeof(s_weather.obsTime), "%02d:%02d",
                             s_env.hh, s_env.mm);
                }
            } else if (s_ivMoon.due(now)) {
                s_ivMoon.ran(now);
                apiFetchMoon(s_moon);
            } else if (s_cfg.waqi_token[0] && s_ivAir.due(now)) {
                s_ivAir.ran(now);
                apiFetchAir(s_air, s_cfg.waqi_token);
            }
        }

        clockSyncUpdate(now, false);
        easterEggUpdate(now);
    }

    // Input is already up to date (top of loop); render/push + pager live here
    // so the same frame can be pumped from inside a blocking fetch.
    uiFrame(now);

    s_swallowKey = false;
    delay(16);
}
