#include "config.h"
#include "storage.h"
#include "api_client.h"
#include "diag.h"
#include "fetch_task.h"
#include "token_store.h"
#include "weather_client.h"
#include "usage_display.h"
#include "ui/lvgl_text.h"
#include "ui_compose.h"
#include "frame_dump.h"
#include "wifi_setup.h"
#include "pager.h"
#include "gestures.h"
#include "status_server.h"
#include "clock_sync.h"
#include "easter_egg.h"
#include "usage_history.h"
#include "util/interval.h"
#include "hw/hw.h"
#include "hw/net.h"
#include "voice/i2s_audio.h"
#include "voice/chime.h"
#include "version.h"
#include <WiFi.h>
#include <ESPmDNS.h>

static UsageData s_usage;
static WeatherData s_weather;
static MoonData s_moon;
static AirData s_air;
static AppConfig s_cfg;
static bool s_chimeEnabled = true;

// Periodic network pollers. loop() only *schedules* at most one fetch job per
// iteration and hands it to the background task (fetch_task.cpp), so a burst
// of due timers (e.g. right after WiFi connects) can never stall input and
// rendering behind several back-to-back blocking fetches. The periods are
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
static bool     s_faceDown      = false;  // panel resting face-down (blanked)

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

// ---- fetch scheduling safety nets ------------------------------------------
// A worker that stops making progress must never stop *every* poller again
// (that is exactly what a 4 h handshake timeout once did: the usage meter just
// froze). Two independent guards:
//   * a per-job cooldown, so a host that is blocked/erroring stops taking the
//     single job slot every cycle (exponential, capped per job);
//   * a watchdog on the job's wall-clock age; a job older than JOB_DEADLINE_MS
//     means the worker is stuck in an SDK call no timeout covers, so the
//     worker is recreated (see fetchTaskRestart()).
// The per-phase bounds in http_client.cpp are what actually keeps the worker
// moving; this is only the last resort for an SDK call that ignores them (a
// DNS lookup is the known one), so it is set above the slowest *legitimate*
// job — a usage poll that refreshes the token twice — to avoid false positives
// that would pause the meter.
static const uint32_t JOB_DEADLINE_MS = 240000;  // wedged worker → restart
static const uint32_t BACKOFF_BASE_MS = 60000;   // first failure: 1 min
static const uint32_t BACKOFF_USAGE_MS  = 120000;    // the meter matters most
static const uint32_t BACKOFF_OTHER_MS  = 1800000;   // weather/moon/air: 30 min
static const uint32_t WEDGE_PAUSE_MS   = 300000;     // 5 min after a wedge
static const uint32_t WEDGE_PAUSE_MAX_MS = 1800000;  // …30 min if it recurs

// Indexed by FetchJob (1..FETCH_AIR).
static uint32_t s_failStreak[FETCH_AIR + 1];
static uint32_t s_cooldownUntil[FETCH_AIR + 1];
static uint32_t s_wedgeCount = 0;

static bool jobCoolingDown(FetchJob job, uint32_t now) {
    if (s_cooldownUntil[job] == 0) return false;
    if ((int32_t)(s_cooldownUntil[job] - now) > 0) return true;
    s_cooldownUntil[job] = 0;       // expired
    return false;
}

static void jobCooldown(FetchJob job, uint32_t now, uint32_t ms) {
    s_cooldownUntil[job] = now + ms;
}

// The wedge recovery ladder: recreate the worker, then keep that host out of
// rotation for a while. Repeated wedges (the restarted worker gets stuck too)
// stretch the pause, so this can never become a restart loop.
static void fetchWatchdog(uint32_t now) {
    if (!fetchTaskBusy()) return;
    uint32_t age = fetchTaskJobAgeMs(now);
    if (age < JOB_DEADLINE_MS) return;

    FetchJob job = fetchTaskCurrentJob();
    uint32_t pause = (s_wedgeCount == 0) ? WEDGE_PAUSE_MS : WEDGE_PAUSE_MAX_MS;
    s_wedgeCount++;
    Serial.printf("fetch: %s wedged for %lums (bc=%s host=%s) — restarting "
                  "worker, %s paused %lus\n",
                  fetchJobName(job), (unsigned long)age,
                  diagCodeName(diagFetchCodeNow()), diagHostNow(),
                  fetchJobName(job), (unsigned long)(pause / 1000));
    jobCooldown(job, now, pause);
    fetchTaskRestart();
    s_forceRender = true;
}

static void screenWake() {
    // While face-down the panel stays blank; the forced render is deferred
    // until it is lifted (screenFaceDownUpdate restores the level).
    if (s_screenDim && !s_faceDown) {
        hwDisplayBrightness((uint8_t)s_brightLevel);
        s_screenDim = false;
    }
    s_forceRender  = true;
    s_lastActivity = millis();
    // If the usage meter has gone stale while idle, pull it once on wake — but
    // rate-limit it, so a chatty status-server callback can't spam fetches.
    // Only the schedule happens here: the fetch runs on the background task,
    // so waking the panel never costs the UI a blocking network round trip.
    if (!s_ivUsage.pending && (uint32_t)(millis() - s_ivUsage.lastMs) >= 30000u) {
        s_ivUsage.forcePending();
    }
}

static void screenIdleUpdate() {
    // Dim only — never blank the panel. It stays readable at DIM_LEVEL so the
    // clock/usage is still glanceable without having to touch the screen.
    if (s_faceDown) return;   // face-down owns the backlight
    if (!s_screenDim && millis() - s_lastActivity >= IDLE_DIM_MS) {
        hwDisplayBrightness(DIM_LEVEL);
        s_screenDim = true;
    }
}

// Face-down the panel is physically hidden, so it is the one case where we do
// blank it (level 0) rather than dim — it saves power and avoids burn-in.
// Lifting it back up restores the normal level and counts as activity.
static void screenFaceDownUpdate() {
    bool fd = gesturesFaceDown();
    if (fd == s_faceDown) return;
    s_faceDown = fd;
    if (fd) {
        hwDisplayBrightnessRaw(0);
        s_screenDim = true;
    } else {
        hwDisplayBrightnessRaw((uint8_t)s_brightLevel);
        s_screenDim = false;
        s_lastActivity = millis();
        s_forceRender = true;
    }
}

// Translate this frame's latched IMU gestures into actions. Shake/double-tap/
// rotate are suppressed while an animation owns the screen, matching the old
// behaviour where shake was ignored while the agent was "thinking".
static void handleGestures() {
    uint8_t ev = gesturesTakeEvents();
    if (ev == gesture::EV_NONE) return;

    bool busy = easterEggActive() || statusServerThinking();

    if (ev & gesture::EV_SHAKE) {
        chimeStop();          // audio is single-owner
        easterEggStart();
        return;               // a shake is not also a tap/rotate
    }
    if ((ev & gesture::EV_FACE_DOWN) || (ev & gesture::EV_FACE_UP)) {
        // screenFaceDownUpdate() (next frame) reads the final orientation.
        s_forceRender = true;
    }
    if (ev & gesture::EV_PICKUP) {
        screenWake();
    }
    if (!busy && (ev & gesture::EV_DOUBLE_TAP)) {
        pagerAdvance();
        s_forceRender = true;
    }
    if (!busy && (ev & gesture::EV_ROT_CW)) {
        pagerStep(1);
        s_forceRender = true;
    }
    if (!busy && (ev & gesture::EV_ROT_CCW)) {
        pagerStep(-1);
        s_forceRender = true;
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
    diagSetNet(s_env.online, (int)WiFi.status());
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
// panel push. Input must already be up to date for this iteration. It runs
// once per loop() iteration and never waits on the network any more (fetches
// live on the background task), so a swipe cannot freeze mid-gesture.
static void uiFrame(uint32_t now) {
    const HwTouch& tp = hwTouch();
    // TEMP DIAG: input + view changes so a "frozen screen" can be told apart
    // from a frozen firmware (taps that reach the loop but never appear).
    if (tp.justPressed)
        Serial.printf("diag: touch down %d,%d page=%d card=%d\n",
                      tp.x, tp.y, pagerPage(), pagerCard());
    if (tp.justReleased) Serial.println("diag: touch up");
    {
        static int s_lastPage = -1, s_lastCard = -1;
        int pg = pagerPage(), cd = pagerCard();
        if (pg != s_lastPage || cd != s_lastCard) {
            Serial.printf("diag: view page=%d card=%d\n", pg, cd);
            s_lastPage = pg; s_lastCard = cd;
        }
    }
    bool keyHit = hwBtnA().wasPressed
#if BOARD_HAS_KEY2
                || hwBtnB().wasPressed
#endif
#if BOARD_BTN_THIRD
                || hwBtnBoot().wasPressed
#endif
                ;
    bool activity = tp.justPressed || tp.down || keyHit;
    if (activity && !s_faceDown) {
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
            diagMark(DIAG_UI_RENDER, 0xFFFFFFFFu);
            wifiSetupDraw(s_wifi);
            diagMark(DIAG_UI_PUSH, 0xFFFFFFFFu);
            hwDisplayPush();
            s_lastPush = now;
        }
        // The setup canvas never participates in the frame key, so force a
        // re-render as soon as it closes.
        s_frameKeyValid = false;
        return;
    }

    screenFaceDownUpdate();
    screenIdleUpdate();

    handleGestures();

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

    // Re-read after Key1 may have dismissed it: a manual dismiss must not leave
    // the waiting animation on screen.
    bool waiting = statusServerWaiting();

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
            } else if (waiting) {
                usageDisplayDrawWaiting(now - statusServerThinkingSince());
            } else {
                usageDisplayDrawThinking(now - statusServerThinkingSince());
            }
            diagMark(DIAG_UI_PUSH, 0xFFFFFFFEu);
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
            uint32_t t0 = millis();
            diagMark(DIAG_UI_RENDER, key);
            uiRender(currentUiData());
            diagMark(DIAG_UI_PUSH, key);
            hwDisplayPush();
            diagPushMs(millis() - t0);
            s_frameKey      = key;
            s_frameKeyValid = true;
            s_forceRender   = false;
            s_lastPush      = now;
        }
    }
#endif
}

// Publish whatever the background fetch task finished: persist rotated tokens,
// copy the payload into the UI's data holders and move the frame key. Runs
// every iteration (also while the WiFi setup screen owns the display) and
// never blocks.
static void serviceFetch() {
    // Flash writes stay on this task: the fetch task must not touch LittleFS
    // (the easter egg and the 555 card read files from here, and LittleFS is
    // not thread-safe).
    if (const TokenSet* pending = fetchTaskPendingSave()) {
        tokenStoreSave(*pending);
        fetchTaskSaveDone();
    }

    FetchResult res;
    if (!fetchTaskHarvest(res)) return;

    // A failed poll still lands: it carries the previous payload back plus the
    // stale flags the fetcher set, exactly like the old in-place mutation.
    switch (res.job) {
        case FETCH_USAGE:   s_usage   = res.usage;   break;
        case FETCH_WEATHER: s_weather = res.weather; break;
        case FETCH_MOON:    s_moon    = res.moon;    break;
        case FETCH_AIR:     s_air     = res.air;     break;
    }
    uint32_t now = millis();
    if (!res.ok) {
        // Back the host off instead of retrying it every cycle: a blocked or
        // erroring endpoint must not keep occupying the single job slot.
        uint32_t streak = ++s_failStreak[res.job];
        uint32_t cool = BACKOFF_BASE_MS << (streak > 5 ? 4 : streak - 1);
        uint32_t cap = (res.job == FETCH_USAGE) ? BACKOFF_USAGE_MS : BACKOFF_OTHER_MS;
        if (cool > cap) cool = cap;
        jobCooldown(res.job, now, cool);
        Serial.printf("fetch %s failed x%u; retry in %lus\n",
                      fetchJobName(res.job), (unsigned)streak,
                      (unsigned long)(cool / 1000));
    } else {
        s_failStreak[res.job] = 0;
        s_cooldownUntil[res.job] = 0;
        s_wedgeCount = 0;
    }
    if (res.ok) {
        if (res.job == FETCH_WEATHER && clockSyncValid()) {
            snprintf(s_weather.obsTime, sizeof(s_weather.obsTime), "%02d:%02d",
                     s_env.hh, s_env.mm);
        }
        // One trend sample per successful usage poll: the rolling window's
        // spend is what the trend page plots.
        if (res.job == FETCH_USAGE) {
            trendPush((uint32_t)(millis() / 1000u),
                      s_usage.rollingUsedMicroCents, s_usage.rollingPercent);
        }
        s_dataRev++;
    }
}

void setup() {
    Serial.begin(115200);
    delay(100);
    diagBoot();
    Serial.printf("ESP32 Usage Monitor fw %s\n", FW_VERSION);

    hwInit();
    // Seed the system clock from the PCF85063 before anything does TLS: the
    // certificate-date check needs a plausible wall clock, and NTP may be
    // blocked on this network.
    clockSyncRestoreFromRtc();

    spr.fillScreen(0x0000);
    spr.drawRoundRect(SAFE_L, SAFE_T, SAFE_W, SAFE_H, 8, 0xFFFF);
    spr.drawRect(SAFE_L + 4, SAFE_T + 4, SAFE_W - 8, SAFE_H - 8, 0x8410);
    hwDisplayPush();

    storageMount();
    loadConfig(s_cfg);
    s_chimeEnabled = s_cfg.chime;
    gesturesSetEnabled(s_cfg.imu_gestures);
    // Token store + the background fetch task. Every HTTPS fetch — including
    // the blocking TLS connect and any token refresh — runs there, so loop()
    // never waits on the network (see fetch_task.h for the threading rules).
    fetchTaskBegin(s_cfg);
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
    gesturesInit();
    trendReset();
    statusServerSetKey(s_cfg.status_key);
    if (statusServerAuthEnabled()) {
        Serial.println("status auth: HMAC-SHA256 enabled");
    } else {
        Serial.println("status auth: DISABLED (set status_key in config.json)");
    }
    statusServerSetWakeCallback(screenWake);
    easterEggSetWakeCallback(screenWake);

    audioBootInit();
    chimeInit();

    char savedSSID[33], savedPass[65];
    bool hasCreds = netLoadCred(savedSSID, 33, savedPass, 65);

    WiFi.mode(WIFI_STA);
    WiFi.setHostname("usage-monitor");
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
    diagMark(DIAG_SETUP_DONE, hasCreds ? 1 : 0);
}

void loop() {
    static uint32_t s_iter = 0;
    diagLoopTick(millis(), ++s_iter);
    hwInputUpdate();
    handleHttpStatus();
    serviceFetch();               // publish finished fetch jobs (never blocks)
    fetchWatchdog(millis());      // restart the worker if a job stopped moving

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

    // One-shot completion chimes from the status server. Played here on the
    // main task so the audio shares the loop with everything else. Skipped
    // while the easter egg owns the I2S bus.
    if (s_chimeEnabled && !easterEggActive()) {
        switch (statusServerTakeEvent()) {
            case STATUS_EV_DONE:    chimePlay(CHIME_DONE);    break;
            case STATUS_EV_FAILED:  chimePlay(CHIME_FAILED);  break;
            case STATUS_EV_WAITING: chimePlay(CHIME_WAITING); break;
            default: break;
        }
    } else {
        statusServerTakeEvent();   // drop it rather than fire it late
    }
    if (!easterEggActive()) chimeUpdate(now);

    refreshEnv(now);

    if (s_wifi.open && WiFi.status() == WL_CONNECTED) {
        WiFi.setSleep(APP_WIFI_PS);
        WiFi.setTxPower(WIFI_POWER_11dBm);
        clockSyncUpdate(now, true);
        statusServerBegin();
        // mDNS so the host plugin can use a stable name instead of a DHCP IP.
        if (MDNS.begin("usage-monitor")) {
            MDNS.addService("http", "tcp", 80);
            Serial.println("mDNS: http://usage-monitor.local/");
        } else {
            Serial.println("mDNS: begin failed");
        }
        s_wifi.open = false;
        // Pull fresh data right away (one job in flight at a time).
        s_ivUsage.forcePending();
        s_ivWeather.forcePending();
        s_ivMoon.forcePending();
        s_ivAir.forcePending();
        s_lastActivity = millis();   // don't idle-out the instant setup closes
        diagMark(DIAG_WIFI_CONNECTED, 0);
        Serial.printf("diag: ip=%s rssi=%d\n",
                      WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
        Serial.println("WiFi connected (auto)");
    }

    if (!s_wifi.open) {
        // ---- network polling (schedule at most one job at a time) ----
        // The fetch itself runs on the background task (fetch_task.cpp): DNS +
        // TCP + TLS connect blocks for seconds and has no callback to pump
        // from, so running it here froze input — most visibly on wake, where
        // screenWake() forces a usage fetch as soon as a finger lands (and a
        // long idle leaves all four pollers due back to back). Here we only
        // pick what is due and hand it over; results arrive via serviceFetch().
        //
        // Skip the whole block while offline: a due timer stays pending, so the
        // first fetch fires as soon as the link is back instead of each timer
        // attempt waiting out a connect() timeout.
        if (WiFi.status() == WL_CONNECTED && !clockSyncValid()) {
            // TLS verification checks the certificate's validity dates, so a
            // plausible clock is required. Normally the RTC seeds it at boot;
            // this warning only appears when the RTC is blank AND NTP is
            // unreachable (e.g. a filtered corporate network).
            static uint32_t s_lastClockWarn = 0;
            if (now - s_lastClockWarn > 30000) {
                s_lastClockWarn = now;
                Serial.println("poll: no valid clock yet; holding off TLS fetches");
            }
        }
        if (WiFi.status() == WL_CONNECTED && clockSyncValid() && !fetchTaskBusy()) {
            // Idle backoff: the usage meter only matters while someone is
            // looking, so poll it slowly once the device has been idle a while.
            // Any activity refreshes s_lastActivity and restores the fast rate.
            bool idle = (uint32_t)(now - s_lastActivity) >= IDLE_NET_MS;
            s_ivUsage.periodMs = (uint32_t)(idle ? s_cfg.poll_usage_idle_s
                                                 : s_cfg.poll_usage_s) * 1000u;

            FetchJob job = FETCH_USAGE;   // only read when `due` is set
            Interval* due = nullptr;
            if (fetchTaskTokensValid() && s_ivUsage.due(now) &&
                !jobCoolingDown(FETCH_USAGE, now)) {
                job = FETCH_USAGE; due = &s_ivUsage;
            } else if (s_ivWeather.due(now) && !jobCoolingDown(FETCH_WEATHER, now)) {
                job = FETCH_WEATHER; due = &s_ivWeather;
            } else if (s_ivMoon.due(now) && !jobCoolingDown(FETCH_MOON, now)) {
                job = FETCH_MOON; due = &s_ivMoon;
            } else if (s_cfg.waqi_token[0] && s_ivAir.due(now) &&
                       !jobCoolingDown(FETCH_AIR, now)) {
                job = FETCH_AIR; due = &s_ivAir;
            }

            if (due) {
                // Seed with what is on screen so a failed poll keeps the last
                // values and just flags them stale (weather/moon/air).
                FetchSeed seed;
                seed.usage   = s_usage;
                seed.weather = s_weather;
                seed.moon    = s_moon;
                seed.air     = s_air;
                // Only arm the timer when the job was actually accepted, so a
                // busy worker simply retries on a later iteration.
                if (fetchTaskStart(job, seed)) due->ran(now);
            }
        }

        clockSyncUpdate(now, false);
        easterEggUpdate(now);
        gesturesUpdate(now);
    }

    // Input is already up to date (top of loop); render/push + pager live here
    // and run every iteration — nothing blocks on the network any more.
    uiFrame(now);

    s_swallowKey = false;
    delay(16);
}
