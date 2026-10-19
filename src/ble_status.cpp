#include "ble_status.h"
#include "status_channel.h"
#include "status_server.h"
#include "version.h"

#include <Arduino.h>
#include <BLE2902.h>
#include <BLEAdvertising.h>
#include <BLECharacteristic.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEService.h>
#include <WiFi.h>
#include <esp32-hal-bt.h>   // btStarted(): the only honest "did the controller come up?"
#include <esp_bt.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <string.h>

// One UUID family: the first field's last hex digit picks the member, so a
// service and its characteristics are obviously related when read off a scanner.
// Private (not registered) — nothing else needs to know these.
#define BLE_RX_UUID   "6f4b1e01-9a3c-4d5e-8f21-7c0d9e5a4b30"   // write: requests
#define BLE_TX_UUID   "6f4b1e02-9a3c-4d5e-8f21-7c0d9e5a4b30"   // notify: replies
#define BLE_INFO_UUID "6f4b1e03-9a3c-4d5e-8f21-7c0d9e5a4b30"   // read: what is this

#define BLE_DEVICE_NAME "usage-monitor"

// A manufacturer-specific field carrying a marker the client can match on bytes.
//
// It exists because the advertised service UUID and the name are not dependable
// discovery keys on macOS: an unfiltered scan there reported `service_uuids` empty
// for 40 of 41 nearby devices (the one that did report them was using 16-bit
// UUIDs) and reported no names at all. Manufacturer data is a first-class field in
// CoreBluetooth, so this is what tools/ble-bridge.py falls back to.
//
//   [0xFF 0xFF] company id — unassigned; nothing here pretends to be a product
//   [0xE5 0x1B] magic
//   [0x01]      layout revision
#define BLE_MFR_MAGIC0 0xE5
#define BLE_MFR_MAGIC1 0x1B
#define BLE_MFR_REV    0x01

// The device's ATT MTU. macOS negotiates what it can, and a settings request plus
// its 64-hex signature is more than the 20-byte default would carry, so ask for
// the stack's maximum and let the peer pick. A write longer than the negotiated
// MTU arrives as a prepared (long) write, which the BLE library reassembles into
// one onWrite() — the frame format below assumes the whole frame at once.
#define BLE_MTU 517

// A request frame is "<ts> <sig> <query>": at most 10 + 1 + 64 + 1 plus the
// longest query a transport accepts — 383 bytes for a WiFi credentials request
// with every byte percent-encoded (status_server.cpp sizes the same string).
// A reply is "<len4><code3><body>" and is kept to a single notification
// (see notifyReply) rather than chunked, so the client needs no reassembly.
#define BLE_FRAME_MAX 480
#define BLE_BODY_MAX  512

// Backstop only: the PC-side bridge drops its own link well before this, and this
// bounds how long a *vanished* central can keep the device out of the air — a
// peripheral does not advertise while connected, so a stale link is not just wasted
// radio, it makes the device undiscoverable. Kept modest for that reason.
#define BLE_IDLE_DISCONNECT_MS 90000UL

// Connection parameters to *request* once a peer connects. Units are 1.25 ms for
// the intervals (so 56/80 = 70/100 ms) and 10 ms for the timeout, per the BLE
// spec. The default macOS interval is ~30 ms; asking for a slower one is the
// coexistence mitigation — BLE then occupies far less airtime while the pollers
// are running. The peer may refuse (macOS clamps to what its own policy allows),
// and the numbers are not measured yet: they are a first cut at "slow enough to
// not fight WiFi, fast enough that a state ping still lands immediately". Re-run
// tools/link_probe.py for the `http: … tls=` numbers before believing them.
#define BLE_CONN_MIN_INT 56
#define BLE_CONN_MAX_INT 80
#define BLE_CONN_LATENCY 4
#define BLE_CONN_TIMEOUT 600

static BLEServer* s_server = nullptr;
static BLECharacteristic* s_tx = nullptr;
static BLECharacteristic* s_info = nullptr;
static BleState s_state = BLE_STATE_OFF;
// What the bring-up cost, kept for the info characteristic so the answer does not
// depend on catching the boot log (see refreshInfo()).
static int    s_initDelta = 0;
static volatile bool s_connected = false;
static uint16_t s_connId = 0;
static uint32_t s_lastRx = 0;

// Written on the BTC task, read on the loop task. `s_frameLen` is set last, and
// the client is synchronous (it waits for the reply notification before writing
// again), so a second write cannot overtake one still being handled — see
// bleStatusUpdate(). The RX characteristic enforces the same thing at the
// protocol level by offering PROPERTY_WRITE only (no write-without-response).
// Kept out of a queue on purpose: one small buffer is the whole contract, and
// it is the one the protocol already implies.
static char s_frame[BLE_FRAME_MAX];
static volatile size_t s_frameLen = 0;
static volatile uint8_t s_peerBda[6] = {0};

static void peerString(char* out, size_t cap) {
  const uint8_t* b = (const uint8_t*)s_peerBda;
  snprintf(out, cap, "%02x:%02x:%02x:%02x:%02x:%02x", b[0], b[1], b[2], b[3], b[4], b[5]);
}

// "<ts> <sig> <query>" -> a pointer to the query (which may itself contain
// spaces, '&' and '=' freed), or nullptr when the frame is not shaped like one.
// `buf` is modified in place; ts/sig are copied out first.
static const char* parseFrame(char* buf, char* ts, size_t tsCap, char* sig, size_t sigCap) {
  char* sp1 = strchr(buf, ' ');
  if (!sp1) return nullptr;
  *sp1 = '\0';
  char* sp2 = strchr(sp1 + 1, ' ');
  if (!sp2) return nullptr;
  *sp2 = '\0';

  size_t nTs = strlen(buf), nSig = strlen(sp1 + 1);
  if (nTs == 0 || nTs >= tsCap || nSig == 0 || nSig >= sigCap) return nullptr;
  memcpy(ts, buf, nTs + 1);
  memcpy(sig, sp1 + 1, nSig + 1);

  const char* q = sp2 + 1;
  return q[0] ? q : nullptr;
}

// "<len4><code3><body>", in one notification. The length is what the client
// checks a truncated read against — it is not needed for reassembly, because
// there is nothing to reassemble: the caller sizes the body from this same MTU.
static void notifyReply(const char* status, const char* body) {
  if (!s_connected || !s_tx || !s_server) return;

  uint16_t mtu = s_server->getPeerMTU(s_connId);
  if (mtu < 23) mtu = 23;                       // the floor every stack must honour
  size_t room = (size_t)mtu - 3;                // ATT payload
  if (room > BLE_FRAME_MAX) room = BLE_FRAME_MAX;

  size_t bodyLen = strlen(body);
  if (7 + bodyLen > room) bodyLen = (room > 7) ? room - 7 : 0;

  char frame[BLE_FRAME_MAX + 8];
  size_t total = 7 + bodyLen;
  snprintf(frame, 5, "%04x", (unsigned)total);  // 4 hex digits + NUL
  memcpy(frame + 4, status, 3);                 // "200 OK" -> "200"
  memcpy(frame + 7, body, bodyLen);

  s_tx->setValue((uint8_t*)frame, total);
  s_tx->notify();
}

// The value behind a read of the info characteristic: enough for a client to see
// it found the right device, and enough for a human with nRF Connect to tell
// whether the LAN path is even up. Refreshed from the loop, since only it knows
// the current IP.
//
// It also carries the three numbers this feature is judged on, so one
// `tools/ble-bridge.py --scan --info` answers everything without a serial console
// (and without racing a boot capture):
//   iheap = internal free heap, the pool Bluedroid and mbedTLS actually share
//           (`heap=` on the diag line is PSRAM-dominated and says nothing here)
//   bt    = internal bytes BLEDevice::init() took, measured at bring-up
static void refreshInfo() {
  if (!s_info) return;
  IPAddress ip = WiFi.localIP();
  char line[128];
  snprintf(line, sizeof(line), "fw=%s ip=%u.%u.%u.%u auth=%s ble=%s iheap=%u bt=%d",
           FW_VERSION, ip[0], ip[1], ip[2], ip[3],
           statusServerAuthEnabled() ? "hmac" : "off",
           bleStatusStateName(),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (int)s_initDelta);
  s_info->setValue((uint8_t*)line, strlen(line));
}

class StatusServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer, esp_ble_gatts_cb_param_t* param) override {
    s_connId = param->connect.conn_id;
    memcpy((void*)s_peerBda, param->connect.remote_bda, 6);
    s_connected = true;
    s_lastRx = millis();
    pServer->updateConnParams(param->connect.remote_bda,
                              BLE_CONN_MIN_INT, BLE_CONN_MAX_INT,
                              BLE_CONN_LATENCY, BLE_CONN_TIMEOUT);
    Serial.println("ble: connected (no pairing requested)");
  }

  void onDisconnect(BLEServer* pServer) override {
    s_connected = false;
    // A connect stops advertising; put it back so the next request can find us.
    // The client reconnects on demand, so there is no reason to keep a slot.
    pServer->startAdvertising();
    Serial.println("ble: disconnected, advertising again");
  }

  void onMtuChanged(BLEServer* pServer, esp_ble_gatts_cb_param_t* param) override {
    Serial.printf("ble: mtu=%u\n", param->mtu.mtu);
  }
};

class StatusWriteCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    // BTC task: copy only — no flash, no Serial-heavy work, no channel state.
    size_t n = c->getLength();
    if (n == 0 || n > sizeof(s_frame)) return;
    memcpy(s_frame, c->getData(), n);
    s_frameLen = n;   // set last: this is what publishes the frame
  }
};

void bleStatusBegin(bool enable) {
  if (!enable) {
    Serial.println("ble: DISABLED (ble_status off in config)");
    return;
  }

  // Why the stack's own diagnosis is invisible by default, and what turns it on.
  //
  // The messages we would most want ("esp_ble_gap_config_adv_data: rc=…",
  // "esp_ble_gap_start_advertising: rc=…") come from the *Arduino* BLE library,
  // which PlatformIO compiles with this project's flags: at CORE_DEBUG_LEVEL=0
  // every log_e/log_i/log_v in it is compiled out. IDF's own prebuilt libraries
  // (libbt.a, the controller) are different — they were built with
  // CONFIG_LOG_DEFAULT_LEVEL=1, so their ESP_LOGE output does reach the console.
  // The upshot is that a controller-level failure is visible and an
  // advertising-level one is not.
  //
  //   PLATFORMIO_BUILD_FLAGS='-DBLE_DEBUG_LOG -DCORE_DEBUG_LEVEL=4' \
  //     pio run -e esp32-usage-monitor -t upload
  //
  // compiles those messages in; the runtime level is raised too, because the
  // default (ERROR) would still filter out the log_d/log_v lines that describe
  // what start() was actually asked to advertise.
#ifdef BLE_DEBUG_LOG
  esp_log_level_set("*", ESP_LOG_VERBOSE);
  Serial.println("ble: BLE_DEBUG_LOG on — the stack's own messages follow");
#endif

  // The number that decides whether BLE is viable on this board at all, printed
  // around the init so it is a measurement rather than an argument.
  //
  // Bluedroid allocates from *internal* RAM and 8 MiB of PSRAM buys almost none
  // of it back: CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096 forces every allocation
  // of 4 KB or less internal, and Bluedroid is built out of those. The internal
  // pool is already shared with WiFi, the task stacks, and mbedTLS — whose
  // buffers are internal too, since CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC is off. All
  // of that lives in the *prebuilt* platform libraries, so these are the facts we
  // work with, not knobs. Hence: measure.
  size_t heapBefore = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  BLEDevice::init(BLE_DEVICE_NAME);
  size_t heapAfter = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  s_initDelta = (int)heapBefore - (int)heapAfter;
  Serial.printf("ble: internal heap before=%u after-init=%u (init took %d)\n",
                (unsigned)heapBefore, (unsigned)heapAfter, s_initDelta);

  // Ask the *controller*, not the library. BLEDevice::init() sets its own
  // `initialized` flag as its very first statement (BLEDevice.cpp:313) and then
  // returns early on any failure, so BLEDevice::getInitialized() is true even when
  // btStart() bailed — useless as a health check.
  if (!btStarted()) {
    Serial.println("ble: FAILED — BT controller did not start (internal RAM?); no BLE fallback");
    s_state = BLE_STATE_FAILED;
    return;
  }
  BLEDevice::setMTU(BLE_MTU);   // after init(): the stack must exist
  // +6 dBm, not the +9 dBm ceiling: the same "close to the host, do not heat the
  // board" trim the WiFi side makes (APP_WIFI_TX_DBM, hw/net.h). Raise it if the
  // device ends up far from the PC.
  BLEDevice::setPower(ESP_PWR_LVL_P6, ESP_BLE_PWR_TYPE_DEFAULT);

  s_server = BLEDevice::createServer();
  if (!s_server) {
    Serial.println("ble: FAILED — no server (internal RAM?); no BLE fallback");
    s_state = BLE_STATE_FAILED;
    return;
  }
  s_server->setCallbacks(new StatusServerCallbacks());

  BLEService* svc = s_server->createService(BLE_STATUS_SERVICE_UUID);
  if (!svc) {
    Serial.println("ble: FAILED — no GATT service (internal RAM?); no BLE fallback");
    s_state = BLE_STATE_FAILED;
    return;
  }

  // PROPERTY_WRITE only, no write-without-response. The shared s_frame
  // contract below is synchronous — the client waits for the reply
  // notification before writing again — and a write-without-response would
  // let a second frame overwrite s_frame while bleStatusUpdate() was still
  // copying the previous one (a torn query that surfaces as an HMAC reject
  // with no diagnostic). Declining the property makes the protocol itself
  // stay in sync: a client that tries a write-NR gets an ATT error instead
  // of a silently corrupted frame. tools/ble-bridge.py already writes with
  // response.
  BLECharacteristic* rx = svc->createCharacteristic(
      BLE_RX_UUID, BLECharacteristic::PROPERTY_WRITE);
  rx->setCallbacks(new StatusWriteCallbacks());

  s_tx = svc->createCharacteristic(BLE_TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  s_tx->addDescriptor(new BLE2902());   // the notification enable bit macOS writes

  s_info = svc->createCharacteristic(BLE_INFO_UUID, BLECharacteristic::PROPERTY_READ);
  refreshInfo();

  svc->start();

  BLEAdvertising* adv = BLEDevice::getAdvertising();
  // 200-300 ms between advertisements. This started at ~1 s ("slow on purpose"),
  // which turned out to be too slow to be *reliably* discovered: a 6 s macOS scan
  // hears only a handful of packets and would intermittently match nothing at all,
  // even back to back on the same device. Only the advertisement is at stake, no
  // connection is held, and the client scans exactly once per message, so the
  // extra airtime is the cheap side of this trade.
  adv->setMinInterval(320);    // 200 ms  (units of 0.625 ms)
  adv->setMaxInterval(480);    // 300 ms

  // Both packets are built by hand, which makes the library skip its own
  // structured configuration entirely (setAdvertisementData /
  // setScanResponseData set m_custom*Data). That is not a style choice — the
  // library's implicit path cannot express this combination:
  //
  //   start() copies m_advData into m_scanRespData *including* service_uuid_len,
  //   so with a 128-bit service UUID the scan response comes out as
  //   UUID(18) + name(15) + txpower(3) = 36 bytes, over the 31-byte limit. IDF
  //   rejects that, and start() then returns *before* esp_ble_gap_start_advertising()
  //   (BLEAdvertising.cpp:266) — leaving a live GATT service that never
  //   advertises, with the only explanation on a log_e that CORE_DEBUG_LEVEL=0
  //   compiles out. That is precisely the state `ble == up` + "not found" was.
  //
  // Sized here so the trap cannot come back silently:
  //   advertisement : flags(3) + 128-bit service UUID(18)   = 21
  //   scan response : the full name                          = 15
  BLEAdvertisementData advData;
  advData.setFlags(ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT);
  advData.setCompleteServices(BLEUUID(BLE_STATUS_SERVICE_UUID));
  // setManufacturerData() adds the length and the 0xFF type itself, so the
  // company id is ours to include.
  const char mfr[5] = { (char)0xFF, (char)0xFF, (char)BLE_MFR_MAGIC0,
                        (char)BLE_MFR_MAGIC1, (char)BLE_MFR_REV };
  advData.setManufacturerData(String(mfr, sizeof(mfr)));

  BLEAdvertisementData scanResp;
  scanResp.setName(BLE_DEVICE_NAME);

  size_t advLen = advData.getPayload().length();
  size_t rspLen = scanResp.getPayload().length();
  Serial.printf("ble: advertisement %u bytes, scan response %u bytes (limit 31 each)\n",
                (unsigned)advLen, (unsigned)rspLen);
  if (advLen > 31 || rspLen > 31) {
    Serial.println("ble: FAILED — advertisement over the 31-byte limit; would "
                   "silently never start, so refusing to advertise");
    s_state = BLE_STATE_FAILED;
    return;
  }

  adv->setAdvertisementData(advData);
  adv->setScanResponseData(scanResp);
  BLEDevice::startAdvertising();

  // Be precise about what this proves, because it is not much: the library exposes
  // no advertising state (its GAP handlers are empty and track nothing), so all
  // this says is that the controller is running and the service is registered.
  // A scan from the PC is the real test — tools/ble-bridge.py --scan.
  s_state = BLE_STATE_UP;
  refreshInfo();   // so an immediate read shows "up" rather than "off"
  Serial.printf("ble: service up as '%s'; advertising requested (verify with "
                "tools/ble-bridge.py --scan)\n", BLE_DEVICE_NAME);
  Serial.printf("ble: internal heap with BT resident=%u (this is what the pollers "
                "and mbedTLS have left to work with)\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

BleState    bleStatusState()     { return s_state; }
const char* bleStatusStateName() {
  switch (s_state) {
    case BLE_STATE_UP:     return "up";
    case BLE_STATE_FAILED: return "failed";
    default:               return "off";
  }
}

void bleStatusUpdate() {
  if (s_state != BLE_STATE_UP) return;
  uint32_t now = millis();

  // The info line carries the IP, which changes with DHCP, so it is refreshed
  // rather than set once.
  static uint32_t s_lastInfo = 0;
  if (now - s_lastInfo >= 5000) {
    s_lastInfo = now;
    refreshInfo();
  }

  size_t n = s_frameLen;
  if (n) {
    char frame[BLE_FRAME_MAX];
    if (n > sizeof(frame)) n = sizeof(frame);
    memcpy(frame, s_frame, n);
    frame[n] = '\0';
    s_frameLen = 0;   // taken: the client is waiting for the reply, not writing
    s_lastRx = now;

    char ts[24] = {0}, sig[80] = {0};
    const char* q = parseFrame(frame, ts, sizeof(ts), sig, sizeof(sig));

    if (!q) {
      Serial.println("ble: malformed frame (want \"<ts> <sig> <query>\")");
      notifyReply("400 Bad Request", "malformed frame\n");
    } else {
      char peer[24];
      peerString(peer, sizeof(peer));

      // Size the body from the negotiated MTU so the reply is always a single
      // notification; a longer settings summary is truncated by the same
      // settingsDescribe() cap the HTTP path already applies.
      uint16_t mtu = s_server ? s_server->getPeerMTU(s_connId) : 23;
      if (mtu < 23) mtu = 23;
      size_t bodyCap = (size_t)mtu - 10;   // 4 length + 3 code
      if (bodyCap > BLE_BODY_MAX) bodyCap = BLE_BODY_MAX;

      char body[BLE_BODY_MAX];
      StatusReq r{q, ts, sig, peer};
      const char* status = statusChannelHandle(r, body, bodyCap);
      // Redacted for the one request kind that carries a secret: the shared
      // handler decides what a query means, this line just prints the frame.
      char safe[64];
      Serial.printf("ble: %s -> %s\n", wifiQueryForLog(q, safe, sizeof(safe)), status);
      notifyReply(status, body);
    }
  }

  if (s_connected && (now - s_lastRx) > BLE_IDLE_DISCONNECT_MS) {
    s_lastRx = now;
    Serial.println("ble: idle link, disconnecting");
    s_server->disconnect(s_connId);
  }
}
