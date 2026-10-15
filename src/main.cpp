#include "config.h"
#include "storage.h"
#include "diag.h"
#include "fetch_task.h"
#include "token_store.h"
#include "usage_display.h"
#include "ui/lvgl_text.h"
#include "ui_compose.h"
#include "frame_dump.h"
#include "wifi_setup.h"
#include "pager.h"
#include "status_server.h"
#include "clock_sync.h"
#include "usage_history.h"
#include "poller.h"
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

// The poll schedule and failure policy (periods, exponential backoff, the wedge
// ladder) live in poller.cpp — pure, host-tested logic. loop() only *schedules*
// at most one fetch job per iteration and hands it to the background task
// (fetch_task.cpp), so a burst of due timers (e.g. right after WiFi connects)
// can never stall input and rendering behind several back-to-back blocking
// fetches. pollerInit() in setup() applies the periods from config.
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

// ---- fetch scheduling safety nets ------------------------------------------
// A worker that stops making progress must never stop *every* poller again
// (that is exactly what a 4 h handshake timeout once did: the usage meter just
// froze). The policy lives in poller.cpp; only the *actor* side — restarting
// the fetch task — stays here, because this is the only file that talks to
// fetchTask*. The per-phase bounds in http_client.cpp are what actually keep
// the worker moving; JOB_DEADLINE_MS is only the last resort for an SDK call
// that ignores them (a DNS lookup is the known one), so it is set above the
// slowest *legitimate* job — a usage poll that refreshes the token twice — to
// avoid false positives that would pause the meter.
static const uint32_t JOB_DEADLINE_MS  = 240000;    // wedged worker → restart
static const uint32_t BACKOFF_BASE_MS  = 60000;     // first failure: 1 min
static const uint32_t BACKOFF_USAGE_MS = 120000;    // the meter matters most
static const uint32_t BACKOFF_OTHER_MS = 1800000;   // weather/moon/air: 30 min
static const uint32_t WEDGE_PAUSE_MS   = 300000;    // 5 min after a wedge
static const uint32_t WEDGE_PAUSE_MAX_MS = 1800000; // …30 min if it recurs

// The wedge recovery ladder: recreate the worker, then keep that host out of
// rotation for a while. Repeated wedges (the restarted worker gets stuck too)
// stretch the pause, so this can never become a restart loop.
static void fetchWatchdog(uint32_t now) {
    if (!fetchTaskBusy()) return;
    uint32_t age = fetchTaskJobAgeMs(now);
    if (!pollerJobWedged(age)) return;

    FetchJob job = fetchTaskCurrentJob();
    uint32_t pause = pollerWedgePauseMs();
    Serial.printf("fetch: %s wedged for %lums (bc=%s host=%s) — restarting "
                  "worker, %s paused %lus\n",
                  fetchJobName(job), (unsigned long)age,
                  diagCodeName(diagFetchCodeNow()), diagHostNow(),
                  fetchJobName(job), (unsigned long)(pause / 1000));
    pollerPauseFor(job, now, pause);
    fetchTaskRestart();
    s_forceRender = true;
}

static void screenWake() {
    if (s_screenDim) {
        hwDisplayBrightness((uint8_t)s_brightLevel);
        s_screenDim = false;
    }
    s_forceRender  = true;
    s_lastActivity = millis();
    // If the usage meter has gone stale while idle, pull it once on wake — but
    // rate-limit it, so a chatty status-server callback can't spam fetches.
    // Only the schedule happens here: the fetch runs on the background task,
    // so waking the panel never costs the UI a blocking network round trip.
    pollerForceUsageIfStale(millis(), 30000u);
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
// the completion chimes play through later.
static void audioBootInit() {
    if (!audioInit()) Serial.println("audio: init failed (no chimes)");
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

    screenIdleUpdate();

    bool thinking = statusServerThinking();
    if (hwBtnA().wasPressed && !s_swallowKey) {
        if (thinking) {
            statusServerClearThinking();
            thinking = false;
        } else {
            pagerAdvance();
            s_forceRender = true;
        }
    }

    // Always advance the gesture machine, even while the thinking animation
    // owns the screen — otherwise scrolling is ignored and looks stuck. The
    // render below still shows the animation instead of the pager.
    if (pagerUpdate()) s_forceRender = true;

    // Re-read after Key1 may have dismissed it: a manual dismiss must not leave
    // the waiting animation on screen.
    bool waiting = statusServerWaiting();

    bool animated = thinking;

#ifdef FRAME_DUMP
    frameDumpTick(now, currentUiData());
#else
    if (animated) {
        // Blink animations only change every ~100 ms; a full-frame push takes
        // tens of ms, so cap the cadence instead of re-pushing identical frames.
        if (now - s_lastPush >= ANIM_FRAME_MS) {
            if (waiting) {
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
    // (the 555 artwork is read from here, and LittleFS is not thread-safe).
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
    // Back the host off instead of retrying it every cycle: a blocked or
    // erroring endpoint must not keep occupying the single job slot.
    PollerOutcome oc = pollerResult(res.job, res.ok, millis());
    if (!res.ok) {
        Serial.printf("fetch %s failed x%u; retry in %lus\n",
                      fetchJobName(res.job), (unsigned)oc.streak,
                      (unsigned long)(oc.cooldownMs / 1000));
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
    // Token store + the background fetch task. Every HTTPS fetch — including
    // the blocking TLS connect and any token refresh — runs there, so loop()
    // never waits on the network (see fetch_task.h for the threading rules).
    fetchTaskBegin(s_cfg);
    if (!s_cfg.waqi_token[0]) {
        Serial.println("No waqi_token; air quality card will be empty");
    }

    // Poll periods + failure policy from config (loadConfig() already filled
    // sane defaults). The constants at the top of this file cover the
    // retry/wedge behaviour, which is not exposed in config.json.
    pollerInit(PollerConfig{
        (uint32_t)s_cfg.poll_usage_s      * 1000u,
        (uint32_t)s_cfg.poll_usage_idle_s * 1000u,
        (uint32_t)s_cfg.poll_weather_s    * 1000u,
        (uint32_t)s_cfg.poll_moon_s       * 1000u,
        (uint32_t)s_cfg.poll_air_s        * 1000u,
        BACKOFF_BASE_MS, BACKOFF_USAGE_MS, BACKOFF_OTHER_MS,
        JOB_DEADLINE_MS, WEDGE_PAUSE_MS, WEDGE_PAUSE_MAX_MS,
    });
    Serial.printf("poll: usage %us (idle %us) weather %us air %us moon %us\n",
                  (unsigned)s_cfg.poll_usage_s, (unsigned)s_cfg.poll_usage_idle_s,
                  (unsigned)s_cfg.poll_weather_s, (unsigned)s_cfg.poll_air_s,
                  (unsigned)s_cfg.poll_moon_s);

    pagerInit();
    trendReset();
    statusServerSetKey(s_cfg.status_key);
    if (statusServerAuthEnabled()) {
        Serial.println("status auth: HMAC-SHA256 enabled");
    } else {
        Serial.println("status auth: DISABLED (set status_key in config.json)");
    }
    statusServerSetWakeCallback(screenWake);

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
    // The middle key IS the AXP PWRON button, so the very press that powered the
    // board on also reads as a PWRON hold: the AXP latches its long-press IRQ
    // while we are still booting, and honouring it would switch the device
    // straight back off ("the screen flashes, then goes black"). Drop whatever
    // the boot press already latched, then arm the shutdown only once the key is
    // up — and keep draining the IRQ while it is still held, so a hold that
    // crosses the 2.5 s threshold during boot cannot fire later.
    static bool s_pwrArmed = false;
    static bool s_pwrInit  = false;
    if (!s_pwrInit) {
        s_pwrInit = true;
        hwAxpPekeyLongPress();                     // consume the boot-press IRQ
        s_pwrArmed = !hwBtnA().isPressed;
    }
    if (hwBtnA().wasReleased) s_pwrArmed = true;
    bool pwrLong = hwAxpPekeyLongPress() && s_pwrArmed;
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
    // main task so the audio shares the loop with everything else.
    if (s_chimeEnabled) {
        switch (statusServerTakeEvent()) {
            case STATUS_EV_DONE:    chimePlay(CHIME_DONE);    break;
            case STATUS_EV_FAILED:  chimePlay(CHIME_FAILED);  break;
            case STATUS_EV_WAITING: chimePlay(CHIME_WAITING); break;
            default: break;
        }
    } else {
        statusServerTakeEvent();   // drop it rather than fire it late
    }
    chimeUpdate(now);

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
        pollerForceAll();
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
            pollerSetUsageIdle((uint32_t)(now - s_lastActivity) >= IDLE_NET_MS);

            // The most overdue poller, unless it is cooling down. Only the
            // schedule happens here; the fetch itself runs on the background
            // task, so this never blocks the UI.
            FetchJob job;
            if (pollerSelect(now, fetchTaskTokensValid(), s_cfg.waqi_token[0] != 0, &job)) {
                // Seed with what is on screen so a failed poll keeps the last
                // values and just flags them stale (weather/moon/air).
                FetchSeed seed;
                seed.usage   = s_usage;
                seed.weather = s_weather;
                seed.moon    = s_moon;
                seed.air     = s_air;
                // Only arm the timer when the job was actually accepted, so a
                // busy worker simply retries on a later iteration.
                if (fetchTaskStart(job, seed)) pollerStarted(job, now);
            }
        }

        clockSyncUpdate(now, false);
    }

    // Input is already up to date (top of loop); render/push + pager live here
    // and run every iteration — nothing blocks on the network any more.
    uiFrame(now);

    s_swallowKey = false;
    delay(16);
}
