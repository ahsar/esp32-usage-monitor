#pragma once
// The BLE transport's request framing: one write is "<ts> <sig> <query>".
// Header-only and dependency-free, so the framing (and its rejection cases) is
// host-tested while ble_status.cpp keeps the GATT lifecycle.
//
// The query may itself contain spaces, '&' and '='; only the first two spaces
// are separators. `buf` is modified in place (the two separators become NULs),
// and the returned pointer aliases it.
#include <stddef.h>
#include <string.h>

// Parse a frame, copying the timestamp and signature out first. Returns a
// pointer to the query, or nullptr when the frame is not shaped like one.
inline const char* bleFrameParse(char* buf, char* ts, size_t tsCap,
                                 char* sig, size_t sigCap) {
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
