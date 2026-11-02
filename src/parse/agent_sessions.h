#pragma once
// Concurrent agent sessions: the device channel used to keep *one* agent state,
// which is exactly right for one OpenCode window and wrong for two — the second
// window's `done` cleared the first window's `running`, and the panel went quiet
// while an agent was still working.
//
// So the table keys reports by session id and derives the one state the screen
// can show:
//
//   any waiting        → "waiting" (the screen that needs you; the session that
//                        has been blocked longest owns the context)
//   else any running   → "running" (the most recently heard from)
//   else               → idle, with `lastEndedFailed` deciding done vs failed
//
// The same ordering is the *card* order the panel shows (agentSessionsOrder):
// card 0 is exactly the session the single-screen design used to show, so one
// session looks and behaves as it always did and N sessions become a stack the
// user can step through. There is one ordering function, so the owner and the
// first card cannot disagree about who is "the" session.
//
// Pure and host-tested: main-loop liveness (the TTL, the chimes, the screen) is
// the caller's job, which is why `now` is passed in rather than read here.
//
// **The TTL is what keeps a dead host from lying.** A plugin that crashes or a
// PC that sleeps never sends `done`, so a session that has been silent for
// AGENT_SESSION_TTL_MS is treated as gone. The plugin heartbeats while it is
// active (tools/opencode-plugin), so the TTL only ever expires silence.
//
// A request without a session id (an older plugin, `tools/status-ping.sh`) is
// the single empty-id session — the behaviour this table replaced.
#include <stdint.h>
#include <string.h>

#include "agent_parse.h"     // AgentInfo, AGENT_STATE_CAP
#include "status_machine.h"  // StatusState

constexpr int AGENT_MAX_SESSIONS = 8;
constexpr uint32_t AGENT_SESSION_TTL_MS = 45u * 60u * 1000u;
constexpr size_t AGENT_SESSION_ID_CAP = 16;   // 15 characters

struct AgentSession {
  char      id[AGENT_SESSION_ID_CAP];
  char      state[AGENT_STATE_CAP];
  AgentInfo info;
  uint32_t  seen;    // millis() of the last report
  uint32_t  since;   // millis() of the state change that put it here
};

struct AgentTable {
  AgentSession s[AGENT_MAX_SESSIONS];
  int      n = 0;
  bool     lastEndedFailed = false;
  // What the status bar can say about the session that *just* ended. With more
  // than one session open, a `done` in the middle is otherwise completely
  // silent: the aggregate is still running, so neither the chime nor the
  // animation changes, and the card simply disappears. `endedRev` moves on every
  // completion so a caller can tell "the same repo finished again" from "nothing
  // happened". Expiry does not touch any of this — a host that went quiet is not
  // a completion.
  char     endedRepo[AGENT_REPO_CAP] = {0};
  uint32_t endedMs  = 0;    // millis() of that completion
  bool     endedFailed = false;
  uint32_t endedRev = 0;
  // Bumped when the set of sessions or any session's state changes — not when
  // only its context does. A heartbeat or a new tool name must not re-open an
  // animation the user dismissed with Key1; a new question (a state change) must.
  uint32_t generation = 0;
};

inline void agentSessionsClear(AgentTable& t) { t = AgentTable(); }

inline int agentSessionFind(const AgentTable& t, const char* id) {
  for (int i = 0; i < t.n; i++)
    if (!strcmp(t.s[i].id, id ? id : "")) return i;
  return -1;
}

inline void agentSessionRemove(AgentTable& t, int i) {
  if (i < 0 || i >= t.n) return;
  for (int j = i; j + 1 < t.n; j++) t.s[j] = t.s[j + 1];
  t.n--;
  t.generation++;
}

// Apply one authenticated report. Returns true when anything the screen reads
// changed (state, set of sessions, or the owning context), so the caller can
// repaint without hashing the table.
//
// `enteredWaiting` (optional) is set when *this* report moved that session into
// waiting — the per-session cue the aggregate machine cannot produce: once the
// screen is already waiting for one session, a second one asking a question is
// a state change nobody would hear otherwise.
inline bool agentSessionsApply(AgentTable& t, const char* id, const char* state,
                               const AgentInfo& info, uint32_t now,
                               bool* enteredWaiting = nullptr) {
  if (enteredWaiting) *enteredWaiting = false;
  if (!state || !state[0]) return false;
  const char* sid = id ? id : "";

  // done/failed: that one session is over. A report with no matching session is
  // the old quiet case (a completion we never saw start) and changes nothing.
  if (!strcmp(state, "done") || !strcmp(state, "failed")) {
    int i = agentSessionFind(t, sid);
    if (i < 0) return false;
    t.lastEndedFailed = !strcmp(state, "failed");
    snprintf(t.endedRepo, sizeof(t.endedRepo), "%s", t.s[i].info.repo);
    t.endedMs     = now;
    t.endedFailed = t.lastEndedFailed;
    t.endedRev++;
    agentSessionRemove(t, i);
    return true;
  }

  if (strcmp(state, "running") && strcmp(state, "waiting")) return false;

  bool anything = false;
  int i = agentSessionFind(t, sid);
  if (i < 0) {
    if (t.n == AGENT_MAX_SESSIONS) {
      // Evict the quietest: with a plugin heartbeat this can only be a session
      // that is already gone but has not aged out yet.
      int oldest = 0;
      for (int j = 1; j < t.n; j++)
        if ((int32_t)(t.s[j].seen - t.s[oldest].seen) < 0) oldest = j;
      agentSessionRemove(t, oldest);
    }
    i = t.n++;
    memset(&t.s[i], 0, sizeof(t.s[i]));
    snprintf(t.s[i].id, sizeof(t.s[i].id), "%s", sid);
    t.s[i].since = now;
    t.generation++;
    anything = true;
  } else if (strcmp(t.s[i].state, state)) {
    t.s[i].since = now;
    t.generation++;
    anything = true;
  }

  if (strcmp(t.s[i].state, state)) {
    if (!strcmp(state, "waiting") && enteredWaiting) *enteredWaiting = true;
    snprintf(t.s[i].state, sizeof(t.s[i].state), "%s", state);
    anything = true;
  }
  if (memcmp(&t.s[i].info, &info, sizeof(info))) {
    t.s[i].info = info;
    anything = true;
  }
  t.s[i].seen = now;
  return anything;
}

// Drop sessions that have been silent for `ttl` (millis-wrap safe). Returns how
// many were dropped. A timeout is not a failure: it means "the host went away",
// so it must not queue the failure chime.
inline int agentSessionsExpire(AgentTable& t, uint32_t now, uint32_t ttl) {
  int dropped = 0;
  for (int i = 0; i < t.n; ) {
    if ((uint32_t)(now - t.s[i].seen) >= ttl) {
      agentSessionRemove(t, i);
      dropped++;
    } else {
      i++;
    }
  }
  if (dropped) t.lastEndedFailed = false;
  return dropped;
}

inline StatusState agentSessionsState(const AgentTable& t) {
  for (int i = 0; i < t.n; i++)
    if (!strcmp(t.s[i].state, "waiting")) return STATUS_WAITING;
  for (int i = 0; i < t.n; i++)
    if (!strcmp(t.s[i].state, "running")) return STATUS_RUNNING;
  return STATUS_IDLE;
}

// The state name status_machine.h expects, or nullptr when idle.
inline const char* agentSessionsStateName(const AgentTable& t) {
  switch (agentSessionsState(t)) {
    case STATUS_WAITING: return "waiting";
    case STATUS_RUNNING: return "running";
    default:             return nullptr;
  }
}

// The screen's order, as indices into `t.s`: waiting first (the session blocked
// longest first), then running (the most recently heard from first). This is the
// one place that rule lives — `agentSessionsOwner()` is its first entry — so the
// single-session screen and the card stack can never disagree about card 0.
// Fills up to `cap` positions and returns how many.
inline int agentSessionsOrder(const AgentTable& t, int* idx, int cap) {
  if (!idx || cap <= 0) return 0;
  if (cap > AGENT_MAX_SESSIONS) cap = AGENT_MAX_SESSIONS;
  bool used[AGENT_MAX_SESSIONS] = {false};
  int  m = 0;
  while (m < cap) {
    int best = -1;
    for (int i = 0; i < t.n; i++) {
      if (used[i]) continue;
      if (best < 0) { best = i; continue; }
      const AgentSession& a = t.s[i];
      const AgentSession& b = t.s[best];
      bool aw = !strcmp(a.state, "waiting");
      bool bw = !strcmp(b.state, "waiting");
      if (aw != bw) {
        if (aw) best = i;              // anything waiting outranks anything running
        continue;
      }
      if (aw) {
        if ((int32_t)(a.since - b.since) < 0) best = i;   // blocked longest
      } else {
        if ((int32_t)(a.seen - b.seen) > 0) best = i;     // most recently heard
      }
    }
    if (best < 0) break;
    used[best] = true;
    idx[m++] = best;
  }
  return m;
}

// Card `pos` in that order, or nullptr when the table is smaller than that.
inline const AgentSession* agentSessionsAt(const AgentTable& t, int pos) {
  int idx[AGENT_MAX_SESSIONS];
  int m = agentSessionsOrder(t, idx, AGENT_MAX_SESSIONS);
  if (pos < 0 || pos >= m) return nullptr;
  return &t.s[idx[pos]];
}

// The session that owns the screen: the one blocked longest when anything is
// waiting, else the most recently heard from. nullptr when idle.
inline const AgentSession* agentSessionsOwner(const AgentTable& t) {
  return agentSessionsAt(t, 0);
}

inline const AgentInfo* agentSessionsInfo(const AgentTable& t) {
  const AgentSession* o = agentSessionsOwner(t);
  return o ? &o->info : nullptr;
}

// How many sessions are active (running or waiting) — the card stack's length.
inline int agentSessionsActive(const AgentTable& t) { return t.n; }

// How many of them are blocked on the user. The status bar says "2 waiting" when
// the modal screen has been dismissed; the aggregate state alone cannot, because
// a dismissal puts the machine back to idle without answering anything.
inline int agentSessionsWaiting(const AgentTable& t) {
  int w = 0;
  for (int i = 0; i < t.n; i++)
    if (!strcmp(t.s[i].state, "waiting")) w++;
  return w;
}
