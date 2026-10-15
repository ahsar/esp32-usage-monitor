// Wireless firmware update — see ota.h for the contract and the threading rules.
//
// The shape of this module is dictated by ArduinoOTAClass::handle(): once a
// transfer is accepted it runs the *whole* receive + flash-write loop inside
// handle() and only returns when the image is in or has failed. loop() is
// therefore frozen for the duration of a 1.6 MB push, which is why
//
//   * the panel is painted from the progress callback,
//   * the pollers are paused for the whole transfer (the fetch worker lives on
//     its own FreeRTOS task and would otherwise keep polling straight through
//     it, competing for the radio), and
//   * pending state is flushed from the stop callback before the reboot.
#include "ota.h"
#include "diag.h"
#include "hw/net.h"     // APP_WIFI_PS (the measured MIN_MODEM policy)
#include "poller.h"
#include <Arduino.h>
#include <ArduinoOTA.h>
#include <WiFi.h>
#include <string.h>

// Every poller stays parked for the transfer + slack for a host that stalls
// mid-push. pollerSelect() reads the same cooldown as a backoff would set, so
// this also covers the forced paths (pollerForceAll() on reconnect,
// screenWake()'s pollerForceUsageIfStale()).
static const uint32_t OTA_POLL_PAUSE_MS = 15 * 60 * 1000;

// The status server owns port 80; ArduinoOTA defaults to 3232 (and espota
// must be told the same, which is what `upload_protocol = espota` does).
static const uint16_t OTA_PORT = 3232;

// Silence tolerated before the receive loop gives up, per wait. The default is
// 1 s (4 tries ≈ 4 s of silence), which a weak link blows through: espota acks
// every 1 KB chunk, so one lost segment sends TCP into retransmit backoff
// (200 ms doubling) and the host is then blocked inside sendall — the device
// sees no data and aborts an otherwise healthy transfer. espota's own per-chunk
// socket timeout is 10 s, so matching it here rides out the backoff without
// letting a genuinely dead host hang the update (the pollers are paused for it).
static const int OTA_SILENCE_TIMEOUT_MS = 10000;

// An unauthenticated firmware-write endpoint on the LAN is not acceptable, so
// the server does not start without a password (fail closed — the device keeps
// working normally, it just is not updatable over the air). This is a floor, not
// a strength meter: `tools/ota.sh --gen-password` writes a 32-character random
// secret. It is deliberately *not* a silent fallback to "no auth".
static const size_t OTA_PASSWORD_MIN_CHARS = 12;

static bool     s_active = false;
static uint32_t s_pct    = 0;
static uint32_t s_drawnPct = 0xFFFFFFFFu;   // last percentage we repainted
static void (*s_progressCb)(uint32_t pct) = nullptr;
static void (*s_stopCb)(bool ok)          = nullptr;

bool     otaActive()      { return s_active; }
void otaSetProgressCallback(void (*cb)(uint32_t pct)) { s_progressCb = cb; }
void otaSetStopCallback(void (*cb)(bool ok))          { s_stopCb = cb; }

static const char* otaErrorName(ota_error_t err) {
  switch (err) {
    case OTA_AUTH_ERROR:    return "auth";
    case OTA_BEGIN_ERROR:   return "begin (no free OTA slot, or image too big)";
    case OTA_CONNECT_ERROR: return "connect";
    case OTA_RECEIVE_ERROR: return "receive";
    case OTA_END_ERROR:     return "end (write/verify)";
    default:                return "unknown";
  }
}

// Hand the radio back. Called on every non-success path; a successful transfer
// reboots from inside onEnd() instead.
static void finishTransfer() {
  // Only the transfer raises power save to WIFI_PS_NONE, so the measured
  // MIN_MODEM default (hw/net.h) comes straight back.
  WiFi.setSleep(APP_WIFI_PS);
  pollerPauseAll(millis(), 0);
  s_active = false;
}

static void otaOnStart() {
  // Reached only after Update.begin() succeeded, i.e. the other app slot is
  // reserved and the announced size fits in it.
  s_active    = true;
  s_pct       = 0;
  s_drawnPct  = 0xFFFFFFFFu;
  pollerPauseAll(millis(), OTA_POLL_PAUSE_MS);
  // Keep the link fully awake for the push: a 1.6 MB transfer at MIN_MODEM
  // is workable but pays a beacon-timed wake-up per burst. Transfer-scoped
  // only — see APP_WIFI_PS for why MIN_MODEM is the resting policy.
  WiFi.setSleep(WIFI_PS_NONE);
  diagMark(DIAG_OTA_START, 0);
  Serial.println("ota: receiving — pollers paused");
}

static void otaOnProgress(unsigned int done, unsigned int total) {
  // The first call is always (0, size): log what is landing, since the host-side
  // progress bar is the only other place the size shows up.
  if (done == 0 && total) Serial.printf("ota: image %u bytes\n", total);
  s_pct = total ? (uint32_t)((uint64_t)done * 100u / total) : 0;
  // ArduinoOTA calls this once per ~1 KB chunk — ~1600 times for this image — and
  // each repaint is a full-frame QSPI push (pushMax measures 61-75 ms on the
  // normal pages) with loop() blocked and the radio idle. Redrawing every chunk
  // is pure waste; one repaint per percent is plenty and bounds it to ~100
  // frames. (It also shortens the window in which the receive loop is not
  // reading, which is what espota's 10 s per-chunk ack deadline cares about.)
  if (s_pct == s_drawnPct) return;
  s_drawnPct = s_pct;
  if (s_progressCb) s_progressCb(s_pct);
}

static void otaOnEnd() {
  Serial.printf("ota: image written (%u%%) — flushing state, rebooting\n",
                (unsigned)s_pct);
  diagMark(DIAG_OTA_DONE, s_pct);
  // Last chance to persist anything that lives outside the app slot. The
  // caller's flush is the point of this callback: a refresh token that rotated
  // during the transfer and is still in the fetch task's hand-off slot would
  // otherwise die with the reboot (the device would need re-authorising).
  if (s_stopCb) s_stopCb(true);
  WiFi.setSleep(APP_WIFI_PS);
  s_active = false;
}

static void otaOnError(ota_error_t err) {
  Serial.printf("ota: FAILED (%s) — staying up, pollers resumed\n",
                otaErrorName(err));
  diagMark(DIAG_OTA_ERROR, (uint32_t)err);
  // Update.abort() has already run for a receive error, and the boot partition
  // only moves in Update.end(true) — so the running app is untouched and the
  // next reboot lands right back in it.
  finishTransfer();
  if (s_stopCb) s_stopCb(false);
}

void otaBegin(const char* password) {
  size_t len = password ? strlen(password) : 0;
  if (len < OTA_PASSWORD_MIN_CHARS) {
    // Refuse rather than expose an unauthenticated update endpoint: an empty or
    // short password is a configuration mistake, not a request for "no auth".
    Serial.printf("ota: DISABLED — ota_password must be at least %u characters "
                  "(have %u); set one in config.json and re-run uploadfs\n",
                  (unsigned)OTA_PASSWORD_MIN_CHARS, (unsigned)len);
    return;
  }

  ArduinoOTA.setHostname("usage-monitor");   // same name as the mDNS http service
  ArduinoOTA.setPort(OTA_PORT);
  ArduinoOTA.setTimeout(OTA_SILENCE_TIMEOUT_MS);
  ArduinoOTA.setPassword(password);
  // main.cpp owns mDNS (it registers the http service the plugin uses), and
  // ArduinoOTA::begin() would MDNS.begin() a second time. espota is given an
  // address explicitly, so the IDE auto-discovery service is not needed.
  ArduinoOTA.setMdnsEnabled(false);
  ArduinoOTA.onStart(otaOnStart);
  ArduinoOTA.onProgress(otaOnProgress);
  ArduinoOTA.onEnd(otaOnEnd);
  ArduinoOTA.onError(otaOnError);
  ArduinoOTA.begin();

  Serial.printf("ota: ready on port %u (password, %u chars) — "
                "tools/ota.sh <ip>  (or pio run -e esp32-ota -t upload "
                "--upload-port <ip> with OTA_AUTH set)\n",
                (unsigned)OTA_PORT, (unsigned)len);
}

void otaUpdate() { ArduinoOTA.handle(); }
