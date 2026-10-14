#include "config.h"
#include "storage.h"
#include "api_client.h"
#include "http_client.h"
#include "token_store.h"
#include "weather_client.h"
#include "usage_display.h"
#include "ui/lvgl_text.h"
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
// and rendering behind several back-to-back blocking fetches. The periods are
// overwritten from config in setup(); the literals here are only the fallback.
static Interval s_ivUsage(60000);
static Interval s_ivWeather(1800000);
static Interval s_ivMoon(43200000);
static Interval s_ivAir(3600000);
static Interval s_ivEnv(1000);

static WifiState s_wifi;

// ---- display power (state-driven render + idle dim) ------------------------
static bool     s_screenDim = false;
static int      s_brightLevel = 2;       // BRIGHT_LUT index; 2 = 150/255
static uint32_t s_lastActivity = 0;
static bool     s_swallowKey   = false;  // first key only wakes, doesn't act

// Rendering is state-driven. uiFrame() re-renders — and full-frame pushes —
// only when the visible state changes. `s_frameKey` is a hash of everything the
// frame depends on; `s_forceRender` covers one-shot events (wake/key/drag/data)
// that must not wait for the hash to differ.
static uint32_t s_lastPush = 0;          // throttle for animated/setup frames
static uint32_t s_frameKey = 0;
static bool     s_frameKeyValid = false;
static bool     s_forceRender   = false;
static uint32_t s_dataRev = 0;           // bumped on every successful fetch

static const uint32_t IDLE_DIM_MS   = 30000;   // 30 s idle → dim
static const uint8_t  DIM_LEVEL     = 0;       // BRIGHT_LUT[0] = 50/255
static const uint32_t IDLE_NET_MS   = 30000;   // idle > this → slow the pollers
static const uint32_t ANIM_FRAME_MS = 60;      // blink anims: cap the push rate

static void screenWake() {
    if (s_screenDim) {
        hwDisplayBrightness((uint8_t)s_brightLevel);
        s_screenDim = false;
    }
    s_forceRender  = true;
    s_lastActivity = millis();
    // If the usage meter has gone stale while idle, pull it once on wake — but
    // rate-limit it, so a chatty status-server callback can't spam fetches.
    if (!s_ivUsage.pending && (uint32_t)(millis() - s_ivUsage.lastMs) >= 30000u) {
        s_ivUsage.forcePending();
    }
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

// ---- state-driven render key ----------------------------------------------

static uint32_t hashMix(uint32_t h, uint32_t v) {
    h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
    return h;
}

// Hash of everything the rendered frame depends on. When it is unchanged the
// canvas would come out pixel-identical, so uiFrame() skips the render and the
// full-frame push entirely — this is what keeps idle cost near zero without
// diffing pixels.
static uint32_t uiFrameKey() {
    uint32_t flags = (s_env.timeValid ? 1u : 0u) | (s_env.online ? 2u : 0u) |
                     (s_env.charging  ? 4u : 0u) | (s_env.usbPresent ? 8u : 0u);
    uint32_t h = 2166136261u;
    h = hashMix(h, (uint32_t)pagerPage());
    h = hashMix(h, (uint32_t)pagerCard());
    h = hashMix(h, (uint32_t)(int)(pagerOffset() * 4.0f));   // quantised drag offset
    h = hashMix(h, flags);
    h = hashMix(h, (uint32_t)s_env.hh);
    h = hashMix(h, (uint32_t)s_env.mm);
    h = hashMix(h, (uint32_t)s_env.batteryPct);
    h = hashMix(h, s_dataRev);
    return h;
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
        // The setup screen has a blink + elapsed counters but no heavy motion,
        // and every frame is a full-panel push, so cap it at ~10 fps. Touch and
        // key handling still run inside wifiSetupDraw() at this cadence.
        if (now - s_lastPush >= 100) {
            wifiSetupDraw(s_wifi);
            hwDisplayPush();
            s_lastPush = now;
        }
        // The setup canvas never participates in the frame key, so force a
        // re-render as soon as it closes.
        s_frameKeyValid = false;
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
            s_forceRender = true;
        }
    }

    // Always advance the gesture machine, even while the easter egg / thinking
    // animation owns the screen — otherwise scrolling is ignored and looks
    // stuck. The render below still shows the animation instead of the pager.
    if (pagerUpdate()) s_forceRender = true;

    bool animated = easterEggActive() || thinking;

#ifdef FRAME_DUMP
    frameDumpTick(now, currentUiData());
#else
    if (animated) {
        // Blink animations only change every ~100 ms; a full-frame push takes
        // tens of ms, so cap the cadence instead of re-pushing identical frames.
        if (now - s_lastPush >= ANIM_FRAME_MS) {
            if (easterEggActive()) {
                usageDisplayDrawEaster555(easterEggElapsedMs());
            } else {
                usageDisplayDrawThinking(now - statusServerThinkingSince());
            }
            hwDisplayPush();
            s_lastPush = now;
        }
        // The animation canvas never participates in the frame key, so force a
        // re-render of the normal page as soon as the animation ends.
        s_frameKeyValid = false;
    } else {
        // Re-render only when something visible actually changed: a static
        // minute costs zero renders and zero panel traffic.
        uint32_t key = uiFrameKey();
        if (s_forceRender || !s_frameKeyValid || key != s_frameKey) {
            uiRender(currentUiData());
            hwDisplayPush();
            s_frameKey      = key;
            s_frameKeyValid = true;
            s_forceRender   = false;
            s_lastPush      = now;
        }
    }
#endif
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

    // Poll periods from config (loadConfig() already filled sane defaults).
    s_ivUsage.periodMs   = (uint32_t)s_cfg.poll_usage_s   * 1000u;
    s_ivWeather.periodMs = (uint32_t)s_cfg.poll_weather_s * 1000u;
    s_ivAir.periodMs     = (uint32_t)s_cfg.poll_air_s     * 1000u;
    s_ivMoon.periodMs    = (uint32_t)s_cfg.poll_moon_s    * 1000u;
    Serial.printf("poll: usage %us (idle %us) weather %us air %us moon %us\n",
                  (unsigned)s_cfg.poll_usage_s, (unsigned)s_cfg.poll_usage_idle_s,
                  (unsigned)s_cfg.poll_weather_s, (unsigned)s_cfg.poll_air_s,
                  (unsigned)s_cfg.poll_moon_s);

    pagerInit();
    httpSetUiPump(uiPump);
    statusServerSetWakeCallback(screenWake);
    easterEggSetWakeCallback(screenWake);

    audioBootInit();

    char savedSSID[33], savedPass[65];
    bool hasCreds = netLoadCred(savedSSID, 33, savedPass, 65);

    WiFi.mode(WIFI_STA);
    // Keep the radio responsive until we are associated (see APP_WIFI_PS_* in
    // hw/net.h): MAX_MODEM during association can miss the AP's handshake frames
    // and blow past the setup UI's connect grace. loop() / netConnect() switch to
    // MAX_MODEM once connected.
    WiFi.setSleep(APP_WIFI_PS_BOOT);
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
        spr.setFont(&lv_font_ui_24);
#else
        spr.setFont(&lv_font_ui_12);
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
        WiFi.setSleep(APP_WIFI_PS);
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
            // Idle backoff: the usage meter only matters while someone is
            // looking, so poll it slowly once the device has been idle a while.
            // Any activity refreshes s_lastActivity and restores the fast rate.
            bool idle = (uint32_t)(now - s_lastActivity) >= IDLE_NET_MS;
            s_ivUsage.periodMs = (uint32_t)(idle ? s_cfg.poll_usage_idle_s
                                                 : s_cfg.poll_usage_s) * 1000u;

            if (s_tokens.valid && s_ivUsage.due(now)) {
                s_ivUsage.ran(now);
                if (apiFetchUsage(s_usage, s_tokens)) s_dataRev++;
                if (s_tokens.dirty) {
                    tokenStoreSave(s_tokens);
                    s_tokens.dirty = false;
                }
            } else if (s_ivWeather.due(now)) {
                s_ivWeather.ran(now);
                if (apiFetchWeather(s_weather)) {
                    s_dataRev++;
                    if (clockSyncValid()) {
                        snprintf(s_weather.obsTime, sizeof(s_weather.obsTime), "%02d:%02d",
                                 s_env.hh, s_env.mm);
                    }
                }
            } else if (s_ivMoon.due(now)) {
                s_ivMoon.ran(now);
                if (apiFetchMoon(s_moon)) s_dataRev++;
            } else if (s_cfg.waqi_token[0] && s_ivAir.due(now)) {
                s_ivAir.ran(now);
                if (apiFetchAir(s_air, s_cfg.waqi_token)) s_dataRev++;
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
