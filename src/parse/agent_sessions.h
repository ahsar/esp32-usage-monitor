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
inline bool agentSessionsApply(AgentTable& t, const char* id, const char* state,
                               const AgentInfo& info, uint32_t now) {
  if (!state || !state[0]) return false;
  const char* sid = id ? id : "";

  // done/failed: that one session is over. A report with no matching session is
  // the old quiet case (a completion we never saw start) and changes nothing.
  if (!strcmp(state, "done") || !strcmp(state, "failed")) {
    int i = agentSessionFind(t, sid);
    if (i < 0) return false;
    t.lastEndedFailed = !strcmp(state, "failed");
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

// The session that owns the screen: the one blocked longest when anything is
// waiting, else the most recently heard from. nullptr when idle.
inline const AgentSession* agentSessionsOwner(const AgentTable& t) {
  const AgentSession* best = nullptr;
  for (int i = 0; i < t.n; i++) {
    if (strcmp(t.s[i].state, "waiting")) continue;
    if (!best || (int32_t)(t.s[i].since - best->since) < 0) best = &t.s[i];
  }
  if (best) return best;
  for (int i = 0; i < t.n; i++) {
    if (strcmp(t.s[i].state, "running")) continue;
    if (!best || (int32_t)(t.s[i].seen - best->seen) > 0) best = &t.s[i];
  }
  return best;
}

inline const AgentInfo* agentSessionsInfo(const AgentTable& t) {
  const AgentSession* o = agentSessionsOwner(t);
  return o ? &o->info : nullptr;
}

// How long the *screen's* current state has lasted (the owning session's since).
inline uint32_t agentSessionsSince(const AgentTable& t, uint32_t now) {
  const AgentSession* o = agentSessionsOwner(t);
  return o ? o->since : now;
}

// Active sessions (running or waiting), for "2 running" on the animation.
inline int agentSessionsActive(const AgentTable& t) { return t.n; }
