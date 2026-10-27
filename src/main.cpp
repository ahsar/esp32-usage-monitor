#include "config.h"
#include "settings_store.h"
#include "storage.h"
#include "diag.h"
#include "fetch_task.h"
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
#include "status_channel.h"
#include "ble_status.h"
#include "ota.h"
#include "clock_sync.h"
#include "usage_history.h"
#include "trend_store.h"
#include "poller.h"
#include "util/interval.h"
#include "util/usage_alert.h"
#include "util/charge_anim.h"
#include "hw/hw.h"
#include "hw/net.h"
#include "voice/i2s_audio.h"
#include "voice/chime.h"
#include "version.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <esp_heap_caps.h>
#include <time.h>

static UsageData s_usage;
static WeatherData s_weather;
static MoonData s_moon;
static AirData s_air;
static ForecastData s_forecast;
static NoticeData s_notice;
// Bumped by the channel on every accepted notice and on expiry, so the frame key
// can see a repaint without hashing the text.
static uint32_t s_noticeRev = 0;
// The device's own state (Health page). Sampling is cheap and lives in
// refreshEnv(); what the last *fetch* did is recorded in serviceFetch(), the one
// place that sees a finished job.
static HealthData s_health;
static int8_t   s_lastFetchJob    = 0;      // FetchJob of the last finished job
static bool     s_lastFetchOk     = false;
static uint32_t s_lastFetchDoneMs = 0;      // millis() of that finish, 0 = never
static AppConfig s_cfg;
// config.json as loaded, before /settings.json is overlaid. Every settings change
// re-derives s_cfg from this, so clearing an override restores the configured
// value instead of leaving the override behind.
static AppConfig s_cfgBase;
static bool s_chimeEnabled = true;

// The poll schedule and failure policy (periods, exponential backoff, the wedge
// ladder) live in poller.cpp — pure, host-tested logic. loop() only *schedules*
// at most one fetch job per iteration and hands it to the background task
// (fetch_task.cpp), so a burst of due timers (e.g. right after WiFi connects)
// can never stall input and rendering behind several back-to-back blocking
// fetches. pollerInit() in setup() applies the periods from config.
static Interval s_ivEnv(1000);

// The Usage page's data age. `s_lastUsageOkMs` is the last *successful* usage
// poll (a failed one leaves the old numbers on screen, which is exactly the case
// the age exists to expose). 0 = never.
static uint32_t s_lastUsageOkMs = 0;
// Trend persistence: the ring is a cache, so a write every five minutes is
// plenty, and the write is on this task only (LittleFS, like the token flush).
static const uint32_t TREND_SAVE_MS = 300000;
static uint32_t s_lastTrendSaveMs = 0;
// A manual refresh (tap on the title bar) is rate-limited: one tap per five
// seconds, so a hand on the panel cannot become a poll storm.
static uint32_t s_lastManualRefreshMs = 0;

static WifiState s_wifi;

// ---- WiFi credentials from the device channel ------------------------------
// status_channel/ble_status hand these over on the loop task, and the handler
// must not touch the radio: associating blocks (WiFi.begin() waits up to 1 s to
// tear down the current link) and, over HTTP, the reply that authorised the
// request is travelling over the very link the change would drop. So the
// handler only records it, and loop() applies it after their replies are out.
static bool     s_wifiCredPending = false;
static WifiCred s_wifiCred;

// The attempt in flight: which credentials we tried and when. Kept so the
// passphrase is only written to NVS once the link actually comes up — the same
// rule netConnect() (the on-device keyboard path) applies, and the reason a typo
// cannot overwrite a working network with a useless one.
static bool      s_wifiTrying     = false;
static WifiCred  s_wifiTry;
static uint32_t  s_wifiTryStart   = 0;
static const uint32_t WIFI_TRY_MS = 45000;   // connect grace (30 s) + slack

// ---- display power (state-driven render + idle dim) ------------------------
static bool     s_screenDim = false;
// Awake brightness: an index into BRIGHT_LUT (see hw/display.h). The factory
// default is named because the settings channel can *clear* its override, and a
// cleared override has to land back here — 0 would mean the dim level, which is
// what BRIGHT_LUT[0] is.
static const uint8_t DEFAULT_BRIGHT_LEVEL = 2;   // BRIGHT_LUT[2] = 150/255
static int      s_brightLevel = DEFAULT_BRIGHT_LEVEL;
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
// The one-time half of "the link came up" (HTTP listener, mDNS, OTA endpoint).
// Separate from s_wifi.open because the credentials channel can associate a
// second time, and none of those may be started twice.
static bool     s_linkBroughtUp = false;

static const uint32_t IDLE_DIM_MS   = 30000;   // 30 s idle → dim
static const uint8_t  DIM_LEVEL     = 0;       // BRIGHT_LUT[0] = 50/255
static const uint32_t IDLE_NET_MS   = 30000;   // idle > this → slow the pollers
static const uint32_t ANIM_FRAME_MS = 60;      // blink anims: cap the push rate
// The Clock page's charging animation (the comet sweeping the battery pill).
// ~12 fps: the sweep moves ~5 logical px per frame at the 2.4 s cycle, which
// reads as motion, while a full-frame push is 16-70 ms — pushing at the agent
// screens' 60 ms cadence would spend most of the loop inside the QSPI blit.
// Slow it to 120 if the panel feels sluggish to the touch while on USB.
static const uint32_t CHARGE_ANIM_MS = 80;

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
// AP re-selection (roaming): after this many consecutive fetches that died on
// the path with no success in between, scan and re-associate. Three is ~90 s of
// a dead path at the observed 30 s handshake cap, which is fast enough to matter
// and slow enough not to thrash a link that is merely having a bad minute; the
// cooldown then bounds how often even a hopeless re-scan can happen.
static const uint32_t ROAM_FAILS       = 3;
static const uint32_t ROAM_COOLDOWN_MS = 180000;

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

// The Usage page's "re-authorise" state is an edge, not a level: the cue is
// played once when a working meter stops working, not on every screen refresh.
static bool s_authSeen  = false;   // the boot state has been sampled
static bool s_authWasOk = true;
static bool s_authCue   = false;   // to be played by loop()

// Spend thresholds (80% / 100% of the rolling meter) are edges too, and much
// more likely to repeat than a token death, so the crossing logic lives in its
// own host-tested state machine (util/usage_alert.h). Here the events only arm a
// one-shot flag; loop()'s chime dispatch consumes it on the task that owns audio.
static usage_alert::State s_usageAlert;
static bool s_usageCueWarn = false;
static bool s_usageCueFull = false;

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
    s_env.city     = s_cfg.place;
    s_env.airLabel = s_cfg.air_label[0] ? s_cfg.air_label : s_cfg.place;
    // Packed, not formatted: the renderer formats it, and the frame key needs
    // something cheap to compare so a DHCP change repaints the clock page.
    IPAddress ip = WiFi.localIP();
    s_env.ipv4 = ((uint32_t)ip[0] << 24) | ((uint32_t)ip[1] << 16) |
                 ((uint32_t)ip[2] << 8)  |  (uint32_t)ip[3];
    s_env.bleUp = (bleStatusState() == BLE_STATE_UP);
    // Terminal, not transient: only a refusal from the token endpoint clears the
    // worker's token — a fetch that failed on the network leaves it alone — so a
    // flaky link cannot raise this.
    bool authOk = fetchTaskTokensValid();
    s_env.authNeeded = !authOk;
    if (!s_authSeen) {
        // Seed the edge detector from the state we actually booted into, so an
        // unconfigured device does not play an alarm at every power-on — only a
        // meter that had been working and just stopped does.
        s_authSeen = true;
        s_authWasOk = authOk;
    } else if (s_authWasOk && !authOk) {
        s_authCue = true;
    }
    s_authWasOk = authOk;

    // How old the meter is — the honest answer to "is this still current?".
    // The threshold is the idle poll period plus a minute of slack: while idle
    // the device legitimately goes three minutes between polls, and calling that
    // stale would train the user to ignore the note.
    s_env.usageAgeSec = s_lastUsageOkMs ? (now - s_lastUsageOkMs) / 1000u : 0;
    s_env.usageStale  = s_usage.valid && s_lastUsageOkMs &&
                        s_env.usageAgeSec > (uint32_t)s_cfg.poll_usage_idle_s + 60u;

    // The Notice page. The countdown is "time until the event", not "time until
    // the notice expires", and it is computed here at 1 Hz rather than stored
    // so a notice that arrives while the page is showing just works.
    const NoticeState& nt = statusChannelNotice();
    s_noticeRev = nt.rev;
    s_notice.valid = nt.valid;
    s_notice.hasCountdown = nt.hasLeft;
    s_notice.countdownSec = 0;
    if (nt.valid) {
      strlcpy(s_notice.kind, nt.kind, sizeof(s_notice.kind));
      strlcpy(s_notice.text, nt.text, sizeof(s_notice.text));
      if (nt.hasLeft) {
        uint32_t elapsed = (now - nt.receivedMs) / 1000u;
        s_notice.countdownSec = nt.leftSec > elapsed ? nt.leftSec - elapsed : 0;
      }
    } else {
      s_notice.kind[0] = '\0';
      s_notice.text[0] = '\0';
    }

    diagSetNet(s_env.online, (int)WiFi.status(), (int)WiFi.RSSI());

    // The Health page's snapshot. Deliberately at the same 1 Hz cadence as the
    // rest of the env: it is read-only, and the frame key quantises it further.
    s_health.valid        = true;
    s_health.online       = s_env.online;
    s_health.rssi         = (int)WiFi.RSSI();
    s_health.uptimeSec    = now / 1000u;
    s_health.heapInternal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    s_health.heapPsram    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    s_health.tokensValid  = authOk;
    s_health.bleUp        = s_env.bleUp;
    s_health.lastJob      = s_lastFetchJob;
    s_health.lastOk       = s_lastFetchOk;
    s_health.lastFetchAgeSec = s_lastFetchDoneMs
                                   ? (now - s_lastFetchDoneMs) / 1000u : 0;
    httpLastTimings(s_health.lastDnsMs, s_health.lastTlsMs, s_health.lastReadMs);
    strlcpy(s_health.fwVersion, FW_VERSION, sizeof(s_health.fwVersion));

    uiSetEnv(s_env);
}

// The settings channel's actor (status_channel.cpp calls this with a validated,
// range-checked set of overrides and the fields the request touched): store them
// first, then apply them live. Storing first means the device never claims a
// change it cannot reproduce after a reboot.
static bool applyDeviceSettings(const DeviceSettings& st, uint16_t changed) {
    if (!settingsSave(st)) return false;

    s_cfg = s_cfgBase;
    settingsOverlay(st, s_cfg);
    s_chimeEnabled = s_cfg.chime;
    chimeSetVolume(s_cfg.chime_volume);

    if (changed & SET_BRIGHTNESS) {
        // A cleared override goes back to the factory level, not to 0 (the dim
        // level), which would leave the "awake" panel as dark as the idle one.
        s_brightLevel = (st.set & SET_BRIGHTNESS) ? st.brightness : DEFAULT_BRIGHT_LEVEL;
        // Leave the panel dimmed if it is dimmed; the wake path pushes the new
        // level, so a settings write cannot un-dim someone's quiet room.
        if (!s_screenDim) hwDisplayBrightness((uint8_t)s_brightLevel);
    }

    const uint16_t LOCATION = SET_PLACE | SET_COORDS | SET_TZ | SET_STATION | SET_AIR_LABEL;
    if (changed & LOCATION) {
        weatherSetLocation(s_cfg.place, s_cfg.lat, s_cfg.lon, s_cfg.tz_offset_min,
                           s_cfg.waqi_station);
        if (changed & SET_TZ) {
            // Re-sync NTP (and so the RTC) in the new zone: the RTC holds local
            // wall time, so leaving it alone would keep the old offset's time
            // until the next hourly sync.
            clockSyncSetOffset(s_cfg.tz_offset_min);
            clockSyncUpdate(millis(), true);
        }
        // The panel is showing another place now; pull everything again instead
        // of waiting out the weather/moon/air periods. Force every poller (the
        // same thing a fresh WiFi association does) so the usage meter is not
        // left on a half-updated screen.
        pollerForceAll();
    }

    s_ivEnv.forcePending();     // refreshEnv() re-reads city/airLabel next loop
    s_forceRender = true;
    return true;
}

static UiData currentUiData() {
    UiData d;
    d.usage    = &s_usage;
    d.weather  = &s_weather;
    d.moon     = &s_moon;
    d.air      = &s_air;
    d.forecast = &s_forecast;
    d.notice   = &s_notice;
    d.health   = &s_health;
    d.hasAir   = s_cfg.waqi_token[0] != 0;
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
//
// `now` is only needed for the Clock page's charging animation: while that is
// running the key has to move on its own or the sweep would freeze on its first
// frame. The tick is gated on the animation being *visible* (see
// charge_anim::active), so no other page and no dimmed panel pays for it.
static uint32_t uiFrameKey(uint32_t now) {
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
    // The charging animation is the one thing on the Clock page that moves by
    // itself. Only while it is actually running: awake, charging, and that page
    // on screen — otherwise a plugged-in device would repaint every 80 ms while
    // the user reads the Usage meter, and a dimmed panel would never settle.
    if (charge_anim::active(s_env.charging, s_screenDim, pagerPage()))
        h = hashMix(h, now / CHARGE_ANIM_MS);
    // The clock page shows these, and a DHCP lease can change both without any
    // other input, so they have to move the key or the page would keep rendering
    // the old address.
    h = hashMix(h, s_env.ipv4);
    h = hashMix(h, s_env.bleUp ? 1u : 0u);
    // The Usage page swaps its whole body for the "re-authorise" message, so a
    // token dying has to move the key or the stale bars would stay on screen.
    h = hashMix(h, s_env.authNeeded ? 1u : 0u);
    // Health is the one page whose content moves on its own (rssi, uptime, heap,
    // the age of the last fetch), so it has to be mixed in or the page would
    // freeze at whatever it showed first. Only while it is the visible page:
    // including it unconditionally would repaint every other page on every tick.
    //
    // Quantised on purpose. Raw values change every second, and every change is
    // a full-frame push (16-70 ms) on a page nobody is looking at — the "a
    // static minute costs zero renders" property is what pays for the animations
    // elsewhere. Uptime is to the minute, rssi to 5 dBm, heap to 4 KB, the fetch
    // age to 10 s; each is far below what the reader can act on.
    if (pagerPage() == UI_PAGE_HEALTH) {
        h = hashMix(h, (uint32_t)(s_health.rssi / 5));
        h = hashMix(h, s_health.uptimeSec / 60u);
        h = hashMix(h, s_health.heapInternal / 4096u);
        h = hashMix(h, s_health.heapPsram / 65536u);
        h = hashMix(h, s_health.lastFetchAgeSec / 10u);
        h = hashMix(h, (uint32_t)s_health.lastJob |
                       (s_health.lastOk ? 0x100u : 0) |
                       (s_health.online ? 0x200u : 0) |
                       (s_health.tokensValid ? 0x400u : 0) |
                       (s_health.bleUp ? 0x800u : 0));
        h = hashMix(h, s_health.lastDnsMs / 100u);
        h = hashMix(h, s_health.lastTlsMs / 100u);
    }
    // The Usage page's staleness note changes only when it crosses into stale
    // (the age itself is quantised to five minutes), so a healthy page still
    // costs zero renders.
    if (pagerPage() == UI_PAGE_USAGE && s_env.usageStale)
        h = hashMix(h, s_env.usageAgeSec / 300u + 1u);
    // Notice: repaint on a new/expired notice, and once a minute while its
    // countdown runs.
    if (pagerPage() == UI_PAGE_NOTICE) {
        h = hashMix(h, s_noticeRev);
        if (s_notice.hasCountdown) h = hashMix(h, s_notice.countdownSec / 60u);
    }
    h = hashMix(h, s_dataRev);
    return h;
}

// ---- boot / loop ----------------------------------------------------------

// Bring up the I2S bus + ES8311 codec at boot. No startup sound; this is what
// the completion chimes play through later.
static void audioBootInit() {
    if (!audioInit()) Serial.println("audio: init failed (no chimes)");
}

// Manual refresh: a tap on the title bar (the top strip, above the divider)
// polls the page's own data now instead of waiting out its interval. That strip
// is the one place a tap does not mean "next page", and it is also where the
// Usage page's data age is drawn, so the cause and the effect are adjacent.
// `pollerForce()` clears the job's backoff as well as its timer, because the
// person asking has already decided the screen is stale.
static void forceRefreshVisible(uint32_t now) {
    if (now - s_lastManualRefreshMs < 5000) return;
    s_lastManualRefreshMs = now;

    switch (pagerPage()) {
        case UI_PAGE_WEATHER:
            pollerForce(FETCH_WEATHER, now);
            pollerForce(FETCH_FORECAST, now);
            break;
        case UI_PAGE_NOTICE:
            return;                     // pushed by the PC, nothing to fetch
        default:
            pollerForce(FETCH_USAGE, now);
            break;
    }
    Serial.printf("refresh: page=%d\n", pagerPage());
}

// ---- UI service frame -----------------------------------------------------

// One UI frame: activity/idle bookkeeping, key handling, pager, render and
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

    // A tap on the title bar is a manual refresh, not a page turn — the strip
    // above the divider is the one place a tap means "now, not next" (see
    // forceRefreshVisible()). Swallowing it here keeps the pager's gesture
    // machine from also advancing the page on release. Not while the WiFi setup
    // screen owns the panel: its keyboard has buttons up there.
    if (!s_wifi.open && tp.justPressed && tp.y < 33) {
        pagerSwallowNextTap();
        forceRefreshVisible(now);
    }

#ifdef FRAME_DUMP
    // The setup screens are what the page walk cannot reach (loop() returns here
    // before it), and the one screen family whose layout cannot be read off the
    // code. The walk seeds its own WifiState (frame_dump.cpp), so it runs whether
    // or not this device is on the air — which matters because "associated" is
    // the normal state, and gating it on `s_wifi.open` meant the screens could
    // only be dumped on a device that had been left without credentials.
    if (frameDumpWifiTick(now)) return;
#endif

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

    bool thinking = statusChannelThinking();
    if (hwBtnA().wasPressed && !s_swallowKey) {
        if (thinking) {
            statusChannelClearThinking();
            thinking = false;
        } else {
            pagerAdvance();
            s_forceRender = true;
        }
    }

    // Side keys: each one pushes the content the same way the key itself sits,
    // so left = content slides left (the page to the right comes in) and right =
    // content slides right (the page to the left comes in). Always on the
    // horizontal axis (even on Weather), and the axis is an endless carousel —
    // the button equivalent of the matching left/right swipe. A press that only
    // woke the panel is swallowed by s_swallowKey above, exactly like Key1.
    // Deliberately does not dismiss the thinking animation (Key1 owns that); it
    // just pages underneath.
#if BOARD_HAS_KEY2
    if (hwBtnB().wasPressed && !s_swallowKey) {      // IO18 (left) → next page
        pagerPageStep(+1);
        s_forceRender = true;
    }
#endif
#if BOARD_BTN_THIRD
    if (hwBtnBoot().wasPressed && !s_swallowKey) {   // BOOT (right) → previous page
        pagerPageStep(-1);
        s_forceRender = true;
    }
#endif

    // Always advance the gesture machine, even while the thinking animation
    // owns the screen — otherwise scrolling is ignored and looks stuck. The
    // render below still shows the animation instead of the pager.
    if (pagerUpdate()) s_forceRender = true;

    // Re-read after Key1 may have dismissed it: a manual dismiss must not leave
    // the waiting animation on screen.
    bool waiting = statusChannelWaiting();

    bool animated = thinking;

#ifdef FRAME_DUMP
    frameDumpTick(now, currentUiData());
#else
    if (animated) {
        // Blink animations only change every ~100 ms; a full-frame push takes
        // tens of ms, so cap the cadence instead of re-pushing identical frames.
        if (now - s_lastPush >= ANIM_FRAME_MS) {
            // The context comes from the last `?agent=` request (empty when the
            // client sent none), the elapsed time from the state machine — the
            // device measures the run itself rather than trusting a timestamp
            // from a host whose clock it cannot check.
            const AgentInfo& agent = statusChannelAgentInfo();
            uint32_t elapsed = now - statusChannelThinkingSince();
            if (waiting) {
                usageDisplayDrawWaiting(elapsed, agent.note, agent.repo);
            } else {
                usageDisplayDrawThinking(elapsed, agent.repo, agent.tool,
                                         statusChannelAgentCount());
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
        uint32_t key = uiFrameKey(now);
        if (s_forceRender || !s_frameKeyValid || key != s_frameKey) {
            uint32_t t0 = millis();
            // Animation clock for the cards. 0 means "static frame": it is what
            // the dimmed panel and the FRAME_DUMP walk get, and it is why the
            // card can draw a settled pill without knowing about idle state.
            uiSetAnimMs(charge_anim::active(s_env.charging, s_screenDim, pagerPage())
                            ? now : 0u);
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

// Persist a rotated token the worker handed over. Flash writes stay on this
// task: the fetch task must not touch LittleFS (loop() owns it — config,
// tokens, settings — and it is not thread-safe).
//
// Every path that can end the app's life early has to call this, because a
// refresh token is single-use: if a rotation is left in the hand-off slot while
// the device reboots (OTA, or the power key) the next boot replays the token it
// already spent, and *that* is what revokes the whole family — the failure the
// token store and tools/uploadfs_guard.py exist to prevent.
//
// Loop, because one pass can miss the second half of a double rotation: a worker
// blocked in handOffTokens() re-publishes as soon as fetchTaskSaveDone() clears
// the slot (the usage poll is explicitly allowed to refresh twice), and at
// shutdown there is no next iteration to pick that up. The pause after a write
// gives that blocked worker the tick it needs; the cap is what keeps this from
// becoming a spin.
static void flushPendingTokens() {
    for (int i = 0; i < 8; i++) {
        const TokenSet* pending = fetchTaskPendingSave();
        if (!pending) return;
        if (!tokenStoreSave(*pending)) {
            Serial.println("tokens: flush failed; the rotated token is lost on reboot");
        }
        fetchTaskSaveDone();
        delay(10);
    }
}

// Persist the trend ring on the way into a reboot/power-off. The periodic save
// covers the five-minute window; this is what makes the last few samples survive
// an OTA or the power key instead of vanishing with the RAM ring.
static void saveTrendNow() {
    if (!clockSyncValid()) return;
    if (trendStoreSave((uint32_t)time(nullptr), millis() / 1000u))
        s_lastTrendSaveMs = millis();
}

// Publish whatever the background fetch task finished: persist rotated tokens,
// copy the payload into the UI's data holders and move the frame key. Runs
// every iteration (also while the WiFi setup screen owns the display) and
// never blocks.
static void serviceFetch() {
    flushPendingTokens();

    FetchResult res;
    if (!fetchTaskHarvest(res)) return;

    // A failed poll still lands: it carries the previous payload back plus the
    // stale flags the fetcher set, exactly like the old in-place mutation.
    switch (res.job) {
        case FETCH_USAGE:    s_usage    = res.usage;    break;
        case FETCH_WEATHER:  s_weather  = res.weather;  break;
        case FETCH_MOON:     s_moon     = res.moon;     break;
        case FETCH_AIR:      s_air      = res.air;      break;
        case FETCH_FORECAST: s_forecast = res.forecast; break;
    }
    // Back the host off instead of retrying it every cycle: a blocked or
    // erroring endpoint must not keep occupying the single job slot.
    PollerOutcome oc = pollerResult(res.job, res.ok, !httpTransportFailed(), millis());

    // What the Health page reports as "Last". Recorded here rather than in
    // refreshEnv() so a failed poll counts too — the interesting case is often
    // the one that did not succeed.
    s_lastFetchJob    = (int8_t)res.job;
    s_lastFetchOk     = res.ok;
    s_lastFetchDoneMs = millis();
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
        // spend is what the trend page plots. The timestamp comes from
        // trend_store, whose virtual clock keeps the spacing of samples that
        // survived a reboot.
        if (res.job == FETCH_USAGE) {
            const uint32_t nowMono = millis() / 1000u;
            trendPush(trendNowSec(nowMono), s_usage.rollingUsedMicroCents,
                      s_usage.rollingPercent);
            s_lastUsageOkMs = millis();

            // Persist at most every five minutes: this is a cache of the last
            // two hours, not a log, and every write is flash on the loop task.
            if (clockSyncValid() && millis() - s_lastTrendSaveMs >= TREND_SAVE_MS) {
                if (trendStoreSave((uint32_t)time(nullptr), nowMono))
                    s_lastTrendSaveMs = millis();
            }

            // Threshold cues. Only a successful poll feeds this: a stale cached
            // meter must not re-arm a threshold that is already behind us.
            switch (usage_alert::tick(s_usageAlert, s_usage.rollingPercent,
                                      s_usage.rollingUsedMicroCents)) {
                case usage_alert::EV_WARN: s_usageCueWarn = true; break;
                case usage_alert::EV_FULL: s_usageCueFull = true; break;
                default: break;
            }
        }
        s_dataRev++;
    }
}

// The WiFi credentials channel's actor (status_channel.cpp calls this with a
// validated request). Nothing is stored here: the request is only recorded, and
// loop() acts on it after both transports have sent their replies — see the note
// on s_wifiCredPending. The passphrase itself is written to NVS once the link
// comes up, so a wrong one never replaces credentials that worked.
static bool applyWifiCred(const WifiCred& cred) {
    s_wifiCred = cred;
    s_wifiCredPending = true;
    return true;
}

// ---- OTA (wireless firmware update; see ota.h) ------------------------------
// Both callbacks run from inside ArduinoOTA's blocking receive loop, which owns
// loop() for the whole transfer — so this is the only code that can touch the
// panel (or flush anything) while an update is in flight.
static void otaDrawProgress(uint32_t pct) {
    usageDisplayDrawOta(pct);
    diagMark(DIAG_UI_PUSH, 0xFFFFFFFCu);
    hwDisplayPush();
}

static void otaStopped(bool ok) {
    if (ok) {
        // The reboot follows immediately. flushPendingTokens() is the point: a
        // refresh token that rotated during the transfer would otherwise die
        // with the reboot and the next use of the stale one revokes the whole
        // family (see AGENTS.md).
        flushPendingTokens();
        saveTrendNow();
        return;
    }
    // Nothing was written and the device keeps running, so drop the progress
    // screen: force a repaint of the page underneath on the next iteration.
    s_frameKeyValid = false;
    s_forceRender   = true;
}

void setup() {
    Serial.begin(115200);
    delay(100);
    diagBoot();
    Serial.printf("ESP32 Usage Monitor fw %s\n", FW_VERSION);

    hwInit();

    spr.fillScreen(0x0000);
    spr.drawRoundRect(SAFE_L, SAFE_T, SAFE_W, SAFE_H, 8, 0xFFFF);
    spr.drawRect(SAFE_L + 4, SAFE_T + 4, SAFE_W - 8, SAFE_H - 8, 0x8410);
    hwDisplayPush();

    storageMount();
    loadConfig(s_cfg);
    s_cfgBase = s_cfg;
    // /settings.json (written by the HTTP settings channel) overrides config.json
    // field by field; a corrupt or missing file leaves the configured values.
    DeviceSettings st;
    settingsClear(st);
    settingsLoad(st);
    settingsOverlay(st, s_cfg);
    if (st.set & SET_BRIGHTNESS) {
        s_brightLevel = st.brightness;
        hwDisplayBrightness((uint8_t)s_brightLevel);   // hwDisplayInit() left 150
    }
    // Location and timezone come from config, so they have to land before the
    // fetch task starts (the request paths) and before the RTC seeds the system
    // clock (the UTC offset the RTC's wall time is interpreted with).
    weatherSetLocation(s_cfg.place, s_cfg.lat, s_cfg.lon, s_cfg.tz_offset_min,
                       s_cfg.waqi_station);
    clockSyncSetOffset(s_cfg.tz_offset_min);
    // Seed the system clock from the PCF85063 before anything does TLS: the
    // certificate-date check needs a plausible wall clock, and NTP may be
    // blocked on this network.
    clockSyncRestoreFromRtc();

    // Trend history from the last boot, now that the clock is plausible: the
    // file is pruned to samples that could still belong to the current rolling
    // window, and trend_store rebases them onto this boot's timeline. Must run
    // before the first fetch can push a sample.
    trendReset();
    trendStoreRestore(clockSyncValid() ? (uint32_t)time(nullptr) : 0, millis() / 1000u);

    s_chimeEnabled = s_cfg.chime;
    chimeSetVolume(s_cfg.chime_volume);
    // Token store + the background fetch task. Every HTTPS fetch — including
    // the blocking TLS connect and any token refresh — runs there, so loop()
    // never waits on the network (see fetch_task.h for the threading rules).
    fetchTaskBegin(s_cfg);
    if (!s_cfg.waqi_token[0]) {
        Serial.println("No waqi_token; air quality card will be empty");
    }
    Serial.printf("loc: %s %.4f,%.4f tz=%+dmin station=%s air=%s\n",
                  s_cfg.place, (double)s_cfg.lat, (double)s_cfg.lon,
                  s_cfg.tz_offset_min, s_cfg.waqi_station,
                  s_cfg.air_label[0] ? s_cfg.air_label : s_cfg.place);

    // Poll periods + failure policy from config (loadConfig() already filled
    // sane defaults). The constants at the top of this file cover the
    // retry/wedge behaviour, which is not exposed in config.json.
    pollerInit(PollerConfig{
        (uint32_t)s_cfg.poll_usage_s      * 1000u,
        (uint32_t)s_cfg.poll_usage_idle_s * 1000u,
        (uint32_t)s_cfg.poll_weather_s    * 1000u,
        (uint32_t)s_cfg.poll_forecast_s   * 1000u,
        (uint32_t)s_cfg.poll_moon_s       * 1000u,
        (uint32_t)s_cfg.poll_air_s        * 1000u,
        BACKOFF_BASE_MS, BACKOFF_USAGE_MS, BACKOFF_OTHER_MS,
        JOB_DEADLINE_MS, WEDGE_PAUSE_MS, WEDGE_PAUSE_MAX_MS,
        ROAM_FAILS, ROAM_COOLDOWN_MS,
    });
    Serial.printf("poll: usage %us (idle %us) weather %us forecast %us air %us moon %us\n",
                  (unsigned)s_cfg.poll_usage_s, (unsigned)s_cfg.poll_usage_idle_s,
                  (unsigned)s_cfg.poll_weather_s, (unsigned)s_cfg.poll_forecast_s,
                  (unsigned)s_cfg.poll_air_s, (unsigned)s_cfg.poll_moon_s);

    pagerInit();
    statusChannelSetKey(s_cfg.status_key);
    statusChannelSetSettings(st);
    statusChannelSetSettingsCallback(applyDeviceSettings);
    // The credentials channel (docs/pc-bridge.md): how a PC gives the device its
    // WiFi passphrase instead of the setup screen's character grid. The actor only
    // records the request; loop() applies it once the reply is out.
    statusChannelSetWifiCallback(applyWifiCred);
    if (statusChannelAuthEnabled()) {
        Serial.println("status auth: HMAC-SHA256 enabled");
    } else {
        Serial.println("status auth: DISABLED (set status_key in config.json)");
    }
    statusChannelSetWakeCallback(screenWake);

    // The same channel over BLE, for when the PC cannot reach us over the LAN
    // (ble_status.h). Started here — after the key and the settings callback —
    // because it serves that channel, and it only ever runs on this task.
    bleStatusBegin(s_cfg.ble_status);

    // Wireless update path (ArduinoOTA). Wired up in setup() so the endpoints
    // exist before WiFi associates; the server itself starts on connect below.
    otaSetProgressCallback(otaDrawProgress);
    otaSetStopCallback(otaStopped);

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
    // radio heat vs. the ~19.5 dBm default (~89 mW). Bump if signal is weak —
    // APP_WIFI_TX_DBM in hw/net.h says how, and why "weak" is the uplink.
    WiFi.setTxPower(APP_WIFI_TX_DBM);
    delay(300);
    if (hasCreds) {
        WiFi.begin(savedSSID, savedPass);
    } else {
        Serial.println("No saved WiFi; opening scan/setup UI");
    }

    wifiSetupInit(s_wifi, hasCreds ? savedSSID : nullptr, hasCreds ? savedPass : nullptr);
    diagMark(DIAG_SETUP_DONE, hasCreds ? 1 : 0);
}

void loop() {
    static uint32_t s_iter = 0;
    diagLoopTick(millis(), ++s_iter);
    hwInputUpdate();
    handleHttpStatus();
    // The same channel's other transport: never fetches anything itself, so it
    // sits next to the HTTP server rather than near the pollers.
    bleStatusUpdate();
    // Expire a notice whose TTL ran out, and any agent session whose host has
    // gone quiet (AGENT_SESSION_TTL_MS). Cheap, loop task only.
    statusChannelTick(millis());
    // Wireless firmware update. Blocks for the whole transfer (see ota.h) —
    // the OTA module paints the panel and pauses the pollers for its duration.
    otaUpdate();
    serviceFetch();               // publish finished fetch jobs (never blocks)
    if (!otaActive()) fetchWatchdog(millis());  // restart the worker if a job stopped moving

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
        spr.setNoWrap(false);   // the setup screen may have turned wrapping off
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
        // Last chance to persist a token the worker rotated, and it has to be
        // *here*: the flush waits for nothing else, so every millisecond before
        // it is a millisecond the device was still polling and could publish a
        // rotation. Draining before the banner instead would spend the whole
        // 400 ms unwritten — which is the one thing this is here to prevent.
        // Unlike OTA there is no reboot after this that could still rescue the
        // token: hwPowerOff() cuts the rails and the device comes back holding
        // the one it already used.
        flushPendingTokens();
        saveTrendNow();
        hwPowerOff();
    }

    uint32_t now = millis();

    // One-shot completion chimes from the status server. This only *arms* the
    // cue: the PCM streams on the chime task (voice/chime.cpp) because pushing
    // one chunk per loop() iteration cannot keep the I2S DMA fed.
    if (s_chimeEnabled) {
        if (s_authCue) { s_authCue = false; chimePlay(CHIME_FAILED); }
        // Spend thresholds reuse the attention cues deliberately: 80% is "look at
        // me" (the two soft taps) and 100% is "something went wrong" (the low
        // note). A dedicated pair of notes can follow if the shared ones turn out
        // to be ambiguous in daily use.
        if (s_usageCueWarn) { s_usageCueWarn = false; chimePlay(CHIME_WAITING); }
        if (s_usageCueFull) { s_usageCueFull = false; chimePlay(CHIME_FAILED); }
        switch (statusChannelTakeEvent()) {
            case STATUS_EV_DONE:    chimePlay(CHIME_DONE);    break;
            case STATUS_EV_FAILED:  chimePlay(CHIME_FAILED);  break;
            case STATUS_EV_WAITING: chimePlay(CHIME_WAITING); break;
            default: break;
        }
    } else {
        s_authCue = false;
        s_usageCueWarn = false;
        s_usageCueFull = false;
        statusChannelTakeEvent();   // drop it rather than fire it late
    }

    refreshEnv(now);

    if (s_wifi.open && WiFi.status() == WL_CONNECTED) {
        WiFi.setSleep(APP_WIFI_PS);
        WiFi.setTxPower(APP_WIFI_TX_DBM);

        // One-time platform bring-up. A second association — credentials sent
        // over the device channel (docs/pc-bridge.md) — re-runs the block below
        // but must not repeat these two: MDNS.begin() *errors* on a second call
        // (mdns_init() rejects it) rather than no-op'ing, and ArduinoOTA would
        // bind its UDP port again.
        if (!s_linkBroughtUp) {
            s_linkBroughtUp = true;
            statusServerBegin();
            // mDNS so the host plugin can use a stable name instead of a DHCP IP.
            if (MDNS.begin("usage-monitor")) {
                MDNS.addService("http", "tcp", 80);
                Serial.println("mDNS: http://usage-monitor.local/");
            } else {
                Serial.println("mDNS: begin failed");
            }
            // Wireless OTA, once the link is up. After the mDNS block on purpose:
            // ArduinoOTA is told not to touch mDNS (main.cpp owns it here).
            otaBegin(s_cfg.ota_password);
            diagMark(DIAG_WIFI_CONNECTED, 0);
        }

        clockSyncUpdate(now, true);
        s_wifi.open = false;
        // Pull fresh data right away (one job in flight at a time).
        pollerForceAll();
        s_lastActivity = millis();   // don't idle-out the instant setup closes
        // BSSID printed on every association, not just the first: it is the one
        // datum that says whether the device picked a different AP since last
        // time (roaming / band steering), which a changing rssi alone cannot
        // distinguish from a fading link — and after a re-provision it is how the
        // new address is announced.
        Serial.printf("diag: ip=%s rssi=%d bssid=%s\n",
                      WiFi.localIP().toString().c_str(), (int)WiFi.RSSI(),
                      WiFi.BSSIDstr().c_str());
        Serial.println("WiFi connected (auto)");

        // Credentials sent over the device channel are stored here — the moment
        // the passphrase is known to be right — so a typo can never replace a
        // working network with a useless one. Same rule as netConnect(), the
        // on-device keyboard path.
        if (s_wifiTrying) {
            s_wifiTrying = false;
            if (!strcmp(WiFi.SSID().c_str(), s_wifiTry.ssid)) {
                if (netSaveCred(s_wifiTry.ssid, s_wifiTry.pass)) {
                    Serial.printf("wifi: credentials for \"%s\" stored\n", s_wifiTry.ssid);
                } else {
                    Serial.println("wifi: connected, but the credentials did not store");
                }
            }
        }
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
        // An AP that associates and then carries nothing cannot be fixed from
        // here, and the ESP32 has no 802.11k/v/r to fall back on: after
        // ROAM_FAILS consecutive dead-path fetches, re-run AP selection. Not
        // while a fetch is in flight — the scan would disrupt it — and
        // pollerRoamAttempted() is called whether or not the station moved, so a
        // hopeless scan cannot become one scan per loop iteration.
        if (WiFi.status() == WL_CONNECTED && !fetchTaskBusy() && !otaActive() &&
            pollerRoamDue(now)) {
            netRoam();
            pollerRoamAttempted(now);
        }
        if (WiFi.status() == WL_CONNECTED && clockSyncValid() && !fetchTaskBusy() && !otaActive()) {
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
                seed.usage    = s_usage;
                seed.weather  = s_weather;
                seed.moon     = s_moon;
                seed.air      = s_air;
                seed.forecast = s_forecast;
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

    // ---- WiFi credentials from the device channel --------------------------
    // Applied at the very bottom of the iteration on purpose: the transports at
    // the top wrote their replies during this same pass, and the reply that
    // authorised the request must be on the wire before the association carrying
    // it is torn down. (Over BLE there is no race — BLE is not the link being
    // changed — but one path for both is what keeps this honest.)
    if (s_wifiCredPending) {
        s_wifiCredPending = false;
        if (s_wifiCred.action == WIFI_CRED_FORGET) {
            netForgetCred();                    // our NVS, and the driver's own copy
            s_wifiTrying = false;
            wifiSetupInit(s_wifi, nullptr, nullptr);   // back to scan/select
            Serial.println("wifi: credentials cleared; back to the setup screen");
        } else {
            // WiFi.begin() tears the current link down itself (up to 1 s of it) —
            // which is exactly why this cannot run inside the request handler.
            WiFi.begin(s_wifiCred.ssid, s_wifiCred.pass);
            s_wifiTry      = s_wifiCred;
            s_wifiTryStart = now;
            s_wifiTrying   = true;
            // Reuse the setup screen's auto-connect step: it already grants the
            // association a grace period, closes itself on WL_CONNECTED and falls
            // back to the network list when the credentials do not work.
            wifiSetupInit(s_wifi, s_wifiCred.ssid, s_wifiCred.pass);
            Serial.printf("wifi: connecting to \"%s\" (credentials from the device channel)\n",
                          s_wifiCred.ssid);
        }
        s_forceRender = true;
    } else if (s_wifiTrying && now - s_wifiTryStart > WIFI_TRY_MS) {
        // The setup screen's grace has expired by now and it is showing the
        // network list again; say so in the log, and leave the old credentials
        // (which are still in NVS) in place.
        s_wifiTrying = false;
        Serial.printf("wifi: could not connect to \"%s\"; credentials not stored\n",
                      s_wifiTry.ssid);
    }
}
