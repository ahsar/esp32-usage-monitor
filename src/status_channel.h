#pragma once
// The device channel, transport-neutral: the state pings and settings requests
// that used to be HTTP-only, plus WiFi credentials. Two transports carry it and
// this is the seam between them:
//
//   - status_server.cpp — plain HTTP on port 80 (the original path, and still
//     the one the OpenCode plugin prefers);
//   - ble_status.cpp    — a GATT service, for when the PC and the device cannot
//     reach each other over the LAN at all.
//
// Framing is the transport's business; the *meaning* of a request, the HMAC
// check, the settings validation and the state machine are not duplicated. That
// is why an untrusted write over BLE is exactly as safe as one over HTTP: it is
// the same verification, not a second implementation of it. All of that lives in
// status_channel.cpp; a transport only pulls the query and the HMAC pair out of
// its wire format and hands them to statusChannelHandle().
//
// Only the PC→device direction is covered. usage/weather/moon/air are fetches
// the device makes out to the internet and still need WiFi.
//
// Four request kinds travel over both transports:
//
//   - `?state=…`          — the agent-activity animation (main.cpp);
//   - `?agent=…`          — the same animation plus *what* the agent is doing
//                           (repo / tool / note / session), for the thinking and
//                           waiting screens. A separate kind rather than more
//                           fields on `?state=` because a state ping signs only
//                           the state word, while this one signs the whole query —
//                           which is what makes the text safe to draw
//                           (agent_parse.h). The session id is what lets two
//                           OpenCode windows coexist (parse/agent_sessions.h);
//   - `?notice=…`         — a lingering, TTL'd one-liner for the Notice page
//                           (parse/notice_parse.h). The only remote text that
//                           outlives its request, which is why the TTL is
//                           mandatory;
//   - everything else     — the settings channel (settings_parse.h), a signed
//                           set of config overrides;
//   - `?wifi_ssid=…&wifi_pass=…` — WiFi credentials (wifi_cred_parse.h). Its own
//                           kind rather than a settings field: it is a secret, it
//                           is *not* persisted in /settings.json, and it is the
//                           one request that has to work on a device with no IP
//                           address yet — i.e. over BLE, which is why the LAN
//                           transport being absent must not make it fail.
//
// A request is the query string the client wants applied plus the HMAC pair it
// signed that string with. The payload the signature covers depends on the
// request — the state value for `?state=`, the whole query for settings, WiFi
// credentials, `?agent=` and `?notice=` — and that rule lives in
// statusChannelHandle(), not in either transport.
#include <stddef.h>
#include <stdint.h>

#include "parse/agent_parse.h"
#include "parse/agent_sessions.h"
#include "parse/notice_parse.h"
#include "parse/settings_parse.h"
#include "parse/status_machine.h"
#include "parse/wifi_cred_parse.h"

// The longest query either transport accepts. A WiFi credentials request with
// every byte percent-encoded is the worst case: 32-byte SSID + 63-byte
// passphrase, each ×3, plus the key names. Both transports size their buffers
// from these, so the two framings cannot drift apart.
constexpr size_t STATUS_QUERY_MAX = 384;
constexpr size_t STATUS_BODY_MAX  = 512;
constexpr size_t STATUS_TS_MAX    = 24;    // unix seconds; 10 digits is 2286
constexpr size_t STATUS_SIG_MAX   = 80;    // 64 hex chars + slack

struct StatusReq {
  // Raw, undecoded query without the leading '?': "state=running",
  // "chime=0&place=Shanghai", or "settings". For a settings request these bytes
  // are what the signature covers, so they must arrive verbatim (a frame that
  // re-encoded or re-ordered them would simply fail auth).
  const char* query;
  const char* ts;    // X-Ts: client unix seconds (ignored when auth is off)
  const char* sig;   // X-Sig: hex HMAC-SHA256 over "<payload>|<ts>"
  const char* peer;  // IP or BT address; used for the rejection log only
};

// Handle one request, writing the response text into `body`, and return the
// status line ("200 OK", "401 Unauthorized", …) from static storage. A short
// answer is normal: the body is the settings summary for a settings request and
// "OK" otherwise.
//
// Call it on the loop task only. A settings request persists through the
// callback registered with statusChannelSetSettingsCallback(), and that write is
// flash — never the fetch task, never the BT stack's own task.
const char* statusChannelHandle(const StatusReq& req, char* body, size_t bodyCap);

// ---------------------------------------------------------------------------
// Channel state / configuration (implemented in status_channel.cpp)
// ---------------------------------------------------------------------------

// Shared secret for request auth. Empty string (or nullptr) disables auth.
void statusChannelSetKey(const char* key);
bool statusChannelAuthEnabled();

// Called when the panel should be looked at: a session entering running or
// waiting (including a second window starting while another already runs), and a
// session completing (done/failed, which leaves no state of its own). Not on a
// heartbeat, not on a session going silent.
void statusChannelSetWakeCallback(void (*cb)());

// Seed the settings channel with what boot loaded from /settings.json, so
// `?settings` answers with the truth from the first request on.
void statusChannelSetSettings(const DeviceSettings& s);

// Called with the overrides a signed settings request produced and the fields it
// touched. Implemented in main.cpp: stores them (/settings.json) *and* applies
// them live. Returning false means the change was rejected, and the device keeps
// answering with the previous settings.
void statusChannelSetSettingsCallback(bool (*cb)(const DeviceSettings&, uint16_t changed));

// Called with the credentials (or the forget request) a signed `?wifi_ssid=…`
// request produced. Implemented in main.cpp, which stores them in NVS — never in
// /settings.json, and never touched from here. Returning false means the write
// failed, so the request is answered with an error rather than a claim.
//
// The reply can only ever report that the request was *recorded*: the passphrase
// reaches NVS once the association succeeds, which is after this returns (and
// after the reply is out). A callback that returns true and an association that
// then fails is a normal outcome, not an error — the panel and the log are what
// report it (docs/pc-bridge.md).
//
// The callback must not touch the radio: it runs inside the request handler on
// the loop task, and associating (or dropping the link an HTTP reply just
// travelled over) has to happen after that reply is out. main.cpp only records
// the request here and acts on it at the bottom of the loop.
void statusChannelSetWifiCallback(bool (*cb)(const WifiCred&));

// The context the last `?agent=…` request carried, for the animation screens.
// Empty fields when no such request has arrived (or the client sent none) — the
// caller draws only what is there. Each agent request replaces its *own
// session's* context (parse/agent_sessions.h); what is published here is the
// session that owns the screen: the one blocked longest when anything is
// waiting, else the most recently active.
const AgentInfo& statusChannelAgentInfo();

// One session as the agent screens draw it: a card in the stack.
struct AgentCard {
  char     id[AGENT_SESSION_ID_CAP];   // the client's short token (pins a selection)
  bool     waiting;
  AgentInfo info;
  uint32_t sinceMs;                    // millis() when this card's state began
};

// Fill up to `cap` cards in the screen's order (waiting first, blocked longest
// first; then running, most recently heard from first) and return how many were
// filled. Card 0 is the session statusChannelAgentInfo() describes, so a single
// session is exactly the screen the device has always drawn.
int statusChannelAgentCards(AgentCard* out, int cap);

// True while the agent state machine is active (running or waiting). This is the
// *state*, not who owns the panel: main.cpp decides that from this, the running
// hold (AGENT_HOLD_MS) and statusChannelDismissed().
bool     statusChannelThinking();
// True only in the waiting (needs-you) state.
bool     statusChannelWaiting();
// When the aggregate state last changed (running/waiting/idle). This is the
// "it just started working" clock: unlike a card's own `sinceMs`, it does not
// move when one session hands the screen over to another.
uint32_t statusChannelStateSince();
// The newest state change among the live sessions, or 0 when none. main.cpp's
// running hold takes the later of this and statusChannelStateSince(), so a
// second window starting while the first is still running re-arms the 3 s
// "it started" card — the aggregate clock would not move at all.
uint32_t statusChannelAgentStartSince();
// How many sessions are active — the card stack's length.
int      statusChannelAgentCount();
// How many of them are waiting on the user. Read from the table rather than the
// machine on purpose: a Key1 dismissal drops the machine to idle without
// answering anything, and the status bar must still say "needs you".
int      statusChannelAgentWaitingCount();
// The repo of the session that completed within `windowMs` (nullptr when none),
// with `failed` saying which of the two completion chimes it earned. With more
// than one session open this is the only trace a `done` in the middle leaves.
const char* statusChannelAgentJustEnded(uint32_t now, uint32_t windowMs, bool* failed);
// Drop back to idle without producing an event (used by Key1). This dismisses
// the *screen*, not the sessions: a heartbeat or a context update will not bring
// it back, but a state change (a new question) will.
void     statusChannelClearThinking();
// True while the user has hidden the agent screen for the current generation
// (Key1). Kept separate from the state machine on purpose: the machine still has
// to follow the sessions while the screen is hidden, or the completion chime for
// a run that was dismissed would be lost (`statusMachineApply` only queues a
// completion while it is not idle).
bool     statusChannelDismissed();

// One-shot events, consumed by loop() to fire the right chime. There are two
// groups: the aggregate edges (a completion, and a question) which always ring,
// and the per-session ones (a start, a completion that leaves another window
// running) which only play when `chime_edges` is on — main.cpp owns that decision.
// statusChannelClearThinking() never produces one, and neither does a session
// that merely aged out (that is not a failure).
StatusEvent statusChannelTakeEvent();

// The last pushed notice (parse/notice_parse.h), or `valid == false`. `rev`
// changes on every accepted notice and on expiry, which is what lets the frame
// key notice a repaint without comparing the text.
struct NoticeState {
  bool     valid = false;
  char     kind[NOTICE_KIND_CAP] = {0};
  char     text[NOTICE_TEXT_CAP] = {0};
  uint32_t ttlSec = 0;
  uint32_t leftSec = 0;        // as received
  bool     hasLeft = false;
  uint32_t receivedMs = 0;     // millis() at receipt
  uint32_t expiresMs = 0;      // millis() when it drops off the panel
  uint32_t rev = 0;
};
const NoticeState& statusChannelNotice();

// Per-iteration housekeeping on the loop task: expire the notice and any session
// that has gone silent (AGENT_SESSION_TTL_MS). Cheap — no flash, no network.
void statusChannelTick(uint32_t now);
