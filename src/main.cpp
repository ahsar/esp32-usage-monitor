#define spr (*hwCanvas())

#include "config.h"
#include "api_client.h"
#include "token_store.h"
#include "weather_client.h"
#include "usage_display.h"
#include "wifi_setup.h"
#include "hw/hw.h"
#include "hw/net.h"
#include "voice/i2s_audio.h"
#include "hw/imu.h"
#include <math.h>
#include <WiFi.h>
#include <time.h>
#include <LittleFS.h>

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
static bool s_ntpStarted = false;
static bool s_timeValid = false;
static uint32_t s_lastRtcSync = 0;
static WiFiServer s_server(80);
static bool s_thinking = false;
static uint32_t s_thinkingSince = 0;
static WifiState s_wifi;

static int s_easterState = 0;
static uint32_t s_easterStart = 0;
static File s_easterFile;
static uint32_t s_easterTotal = 0;
static uint32_t s_easterWritten = 0;

// ---- pager state ----------------------------------------------------------

static const int PAGE_W = HW_W;
static const int PAGE_H = HW_H;

static int  s_page = UI_PAGE_WEATHER;
static int  s_wcard = UI_WC_NOW;

static bool s_touchActive = false;
static int  s_tx0 = 0, s_ty0 = 0;
static int  s_dragAxis = 0;          // 0 none, 1 horizontal, 2 vertical
static float s_off = 0.0f;           // current drag/animation offset in px
static uint32_t s_touchDownMs = 0;

static bool s_animating = false;
static bool s_animAxisV = false;static int  s_animDir = 0;           // +1 next, -1 prev, 0 snap back
static float s_animFrom = 0.0f, s_animTo = 0.0f;
static uint32_t s_animStart = 0;
static const uint32_t ANIM_MS = 170;

static UiEnv s_env;
static uint32_t s_envNext = 0;
static uint32_t s_lastDraw = 0;

extern "C" void handleHttpStatus() {
    WiFiClient client = s_server.accept();
    if (!client) return;

    client.setTimeout(50);

    uint32_t t0 = millis();
    while (!client.available() && (millis() - t0) < 100) {
        delay(1);
    }

    String req;
    for (int i = 0; i < 15; i++) {
        if (!client.connected() && !client.available()) break;
        if (!client.available()) break;
        String line = client.readStringUntil('\n');
        req += line;
        if (line == "\r") break;
    }

    if (req.indexOf("state=running") >= 0) {
        s_thinking = true;
        s_thinkingSince = millis();
        Serial.println("state=running");
    } else if (req.indexOf("state=done") >= 0 || req.indexOf("state=failed") >= 0) {
        s_thinking = false;
        Serial.println("state=done/failed");
    }

    client.println("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nOK");
    delay(10);
    client.stop();
}

static void syncNtp() {
    if (!s_ntpStarted) {
        configTime(8 * 3600, 0, "ntp.aliyun.com", "pool.ntp.org");
        s_ntpStarted = true;
    }
    time_t ts = time(nullptr);
    if (ts > 100000) {
        struct tm lt;
        localtime_r(&ts, &lt);
        HwTime rt = {
            (uint8_t)lt.tm_hour, (uint8_t)lt.tm_min, (uint8_t)lt.tm_sec,
            (uint16_t)(lt.tm_year + 1900),
            (uint8_t)(lt.tm_mon + 1), (uint8_t)lt.tm_mday, (uint8_t)lt.tm_wday
        };
        hwRtcWrite(rt);
        s_timeValid = true;
    }
}

// ---- UI environment -------------------------------------------------------

static void refreshEnv() {
    uint32_t now = millis();
    if (now < s_envNext) return;
    s_envNext = now + 1000;

    s_env.online = (WiFi.status() == WL_CONNECTED);
    s_env.timeValid = s_timeValid;

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
        s_page = (int)idx;
    } else {
        s_page = UI_PAGE_WEATHER;
        s_wcard = (int)(idx - UI_PAGE_COUNT + 1);
    }
    s_off = 0;
    s_animating = false;

    renderUi();
    hwDisplayPush();

    uint16_t* fb = (uint16_t*)hwCanvas()->getFramebuffer();
    Serial.printf("FRAME %u %d %d\n", idx, HW_W, HW_H);
    delay(30);
    Serial.write((uint8_t*)fb, HW_W * HW_H * 2);
    Serial.write('\n');
    Serial.flush();
    s_dumpIdx++;
}
#endif

static void drawPage(int page, int ox, int oy) {
    switch (page) {
        case UI_PAGE_USAGE: uiDrawUsage(s_usage, ox, oy); break;
        case UI_PAGE_CLOCK: uiDrawClock(ox, oy); break;
        default:
            uiDrawWeatherCard(s_wcard, s_weather, s_moon, s_air, s_cfg.waqi_token[0] != 0, ox, oy);
            break;
    }
}

static void renderUi() {
    uiBeginFrame();

    bool vaxis = s_animating ? s_animAxisV : (s_dragAxis == 2);
    int off = (int)lroundf(s_off);

    if (vaxis && s_page == UI_PAGE_WEATHER) {
        uiDrawWeatherCard(s_wcard, s_weather, s_moon, s_air, s_cfg.waqi_token[0] != 0, 0, off);
        if (off < 0 && s_wcard + 1 < UI_WC_COUNT)
            uiDrawWeatherCard(s_wcard + 1, s_weather, s_moon, s_air, s_cfg.waqi_token[0] != 0, 0, off + PAGE_H);
        else if (off > 0 && s_wcard - 1 >= 0)
            uiDrawWeatherCard(s_wcard - 1, s_weather, s_moon, s_air, s_cfg.waqi_token[0] != 0, 0, off - PAGE_H);
    } else {
        drawPage(s_page, off, 0);
        if (off < 0 && s_page + 1 < UI_PAGE_COUNT) drawPage(s_page + 1, off + PAGE_W, 0);
        else if (off > 0 && s_page - 1 >= 0) drawPage(s_page - 1, off - PAGE_W, 0);
    }

    uiDrawPageDots(s_page);
    if (s_page == UI_PAGE_WEATHER) uiDrawCardDots(s_wcard, UI_WC_COUNT);
}

// ---- pager interaction ----------------------------------------------------

static void startAnim(bool vertical, float from, float to, int dir) {
    s_animating = true;
    s_animAxisV = vertical;
    s_animDir = dir;
    s_animFrom = from;
    s_animTo = to;
    s_animStart = millis();
}

static void commitAnim() {
    if (s_animDir > 0) {
        if (s_animAxisV) { if (s_wcard + 1 < UI_WC_COUNT) s_wcard++; }
        else             { if (s_page + 1 < UI_PAGE_COUNT) s_page++; }
    } else if (s_animDir < 0) {
        if (s_animAxisV) { if (s_wcard > 0) s_wcard--; }
        else             { if (s_page > 0) s_page--; }
    }
    s_off = 0;
    s_animating = false;
    s_animDir = 0;
}

static void advance() {
    if (s_page == UI_PAGE_WEATHER) s_wcard = (s_wcard + 1) % UI_WC_COUNT;
    else s_page = (s_page + 1) % UI_PAGE_COUNT;
}

static void pagerUpdate() {
    const HwTouch& tp = hwTouch();
    uint32_t now = millis();

    if (tp.justPressed) {
        s_touchActive = true;
        s_tx0 = tp.x; s_ty0 = tp.y;
        s_dragAxis = 0;
        s_touchDownMs = now;
        s_off = 0;
        s_animating = false;
    }

    if (s_touchActive && tp.down) {
        int dx = tp.x - s_tx0;
        int dy = tp.y - s_ty0;
        if (s_dragAxis == 0 && (abs(dx) > 8 || abs(dy) > 8))
            s_dragAxis = (abs(dy) > abs(dx)) ? 2 : 1;

        if (s_dragAxis == 1) {
            s_off = dx;
            bool canPrev = s_page > 0, canNext = s_page + 1 < UI_PAGE_COUNT;
            if ((s_off > 0 && !canPrev) || (s_off < 0 && !canNext)) s_off /= 3.0f;
        } else if (s_dragAxis == 2 && s_page == UI_PAGE_WEATHER) {
            s_off = dy;
            bool canPrev = s_wcard > 0, canNext = s_wcard + 1 < UI_WC_COUNT;
            if ((s_off > 0 && !canPrev) || (s_off < 0 && !canNext)) s_off /= 3.0f;
        } else {
            s_off = 0;
        }
    }

    if (s_touchActive && tp.justReleased) {
        s_touchActive = false;
        bool vertical = (s_dragAxis == 2) && (s_page == UI_PAGE_WEATHER);
        int swipeThresh = 38;
        int dir = 0;

        if (s_dragAxis == 0 || (abs((int)s_off) < 8 && now - s_touchDownMs < 350)) {
            advance();                     // treat as a tap
            s_off = 0;
        } else if (s_dragAxis == 2 && !vertical) {
            s_off = 0;                     // vertical on non-weather pages: nothing
        } else if (s_off <= -swipeThresh) {
            dir = 1;
        } else if (s_off >= swipeThresh) {
            dir = -1;
        }

        if (dir != 0) {
            bool can = vertical ? (dir > 0 ? s_wcard + 1 < UI_WC_COUNT : s_wcard > 0)
                                : (dir > 0 ? s_page + 1 < UI_PAGE_COUNT : s_page > 0);
            if (!can) dir = 0;
        }

        float size = vertical ? PAGE_H : PAGE_W;
        float target = (dir > 0) ? -size : (dir < 0 ? size : 0.0f);
        startAnim(vertical, s_off, target, dir);
        s_dragAxis = 0;
    }

    if (s_animating) {
        float t = (float)(now - s_animStart) / (float)ANIM_MS;
        if (t >= 1.0f) {
            commitAnim();
        } else {
            if (t < 0) t = 0;
            t = t * t * (3.0f - 2.0f * t);     // smoothstep
            s_off = s_animFrom + (s_animTo - s_animFrom) * t;
        }
    }
}

// ---- boot / loop ----------------------------------------------------------

static void stopEaster() {
    if (s_easterState == 1) {
        s_easterFile.close();
        audioTxStop();
        s_easterState = 0;
    }
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

    bool audioOk = audioInit();
    if (audioOk) {
        uint8_t toneBuf[512];
        float phase = 0;
        uint32_t total = 0;
        const uint32_t target = 32000;
        if (audioTxStart()) {
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
    }

    char savedSSID[33], savedPass[65];
    bool hasCreds = netLoadCred(savedSSID, 33, savedPass, 65);

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);   // modem sleep adds ~100ms+ latency per round trip
    delay(300);
    if (hasCreds) {
        WiFi.begin(savedSSID, savedPass);
    } else {
        Serial.println("No saved WiFi; opening scan/setup UI");
    }

    wifiSetupInit(s_wifi);
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
        spr.setFont(u8g2_font_helvB12_tf);
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
        WiFi.setSleep(false);
        syncNtp();
        s_server.begin();
        s_wifi.open = false;
        s_lastFetch = 0;
        s_lastWeather = 0;
        s_lastMoon = 0;
        s_lastAir = 0;
        Serial.println("WiFi connected (auto)");
    }

    if (!s_wifi.open) {
        // Desktop mode
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
            if (apiFetchWeather(s_weather) && s_env.timeValid) {
                snprintf(s_weather.obsTime, sizeof(s_weather.obsTime), "%02d:%02d", s_env.hh, s_env.mm);
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

        uint32_t ntpInterval = s_timeValid ? 3600000 : 5000;
        if (s_ntpStarted && now - s_lastRtcSync >= ntpInterval) {
            s_lastRtcSync = now;
            syncNtp();
        }

        // Shake detection
        if (s_easterState == 0 && !s_thinking) {
            float ax, ay, az;
            hwImuAccel(&ax, &ay, &az);
            float mag2 = ax*ax + ay*ay + az*az;
            static uint8_t shakeCount = 0;
            if (mag2 > 4.0f) shakeCount++;
            else shakeCount = 0;
            if (shakeCount >= 3) {
                shakeCount = 0;
                Serial.println("Easter: shake detected!");
                stopEaster();
                s_thinking = false;
                s_easterFile = LittleFS.open("/555_audio.raw", "r");
                if (s_easterFile) {
                    s_easterTotal = s_easterFile.size();
                    s_easterWritten = 0;
                    s_easterStart = now;
                    s_easterState = 1;
                    audioTxStart();
                    Serial.printf("Easter: playing %u bytes\n", s_easterTotal);
                }
            }
        }

        if (s_easterState == 1) {
            uint8_t buf[512];
            size_t toRead = sizeof(buf);
            if (s_easterTotal - s_easterWritten < toRead)
                toRead = s_easterTotal - s_easterWritten;
            if (toRead > 0) {
                size_t n = s_easterFile.read(buf, toRead);
                audioTxWrite(buf, n);
                s_easterWritten += n;
            }
            if (s_easterWritten >= s_easterTotal) {
                s_easterFile.close();
                audioTxStop();
                s_easterState = 0;
                Serial.println("Easter: done");
            }
        }

        bool animated = (s_easterState == 1) || s_thinking;

        if (hwBtnA().wasPressed) {
            if (s_easterState == 1) {
                stopEaster();
            }
            if (s_thinking) {
                s_thinking = false;
            } else {
                advance();
            }
        }

        if (!animated) {
            pagerUpdate();
        }

#ifdef FRAME_DUMP
        frameDump();
#else
        bool fast = animated || s_touchActive || s_animating;
        if (fast || now - s_lastDraw >= 1000) {
            s_lastDraw = now;
            if (s_easterState == 1) {
                usageDisplayDrawEaster555(now - s_easterStart);
            } else if (s_thinking) {
                usageDisplayDrawThinking(now - s_thinkingSince);
            } else {
                renderUi();
            }
        }
#endif
    } else {
        wifiSetupDraw(s_wifi);
    }

    // ApiFetch* keep the status server alive; nothing else to do here.
    hwDisplayPush();
    delay(16);
}
