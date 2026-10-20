#pragma once
// Formatting for the Health page. Header-only and hardware-free so the host
// tests can pin the shapes the card draws (the card itself is firmware-only).
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>

namespace health_fmt {

// "3d 04h" / "04h 12m" / "42m" — uptime is read at a glance, so the units are
// dropped the same way fmtReset() drops them on the Usage page.
inline void formatUptime(char* buf, size_t n, uint32_t sec) {
  uint32_t d = sec / 86400;
  uint32_t h = (sec % 86400) / 3600;
  uint32_t m = (sec % 3600) / 60;
  if (d > 0)      snprintf(buf, n, "%ud %02uh", (unsigned)d, (unsigned)h);
  else if (h > 0) snprintf(buf, n, "%uh %02um", (unsigned)h, (unsigned)m);
  else            snprintf(buf, n, "%um", (unsigned)m);
}

// "142K" / "1.4M" / "512B" — free heap, to three significant-ish digits. The
// exact byte count is never what the reader wants; the order of magnitude is.
inline void formatBytes(char* buf, size_t n, uint32_t bytes) {
  if (bytes >= 1024u * 1024u)
    snprintf(buf, n, "%u.%uM", (unsigned)(bytes / (1024u * 1024u)),
             (unsigned)((bytes % (1024u * 1024u)) * 10u / (1024u * 1024u)));
  else if (bytes >= 1024u)
    snprintf(buf, n, "%uK", (unsigned)(bytes / 1024u));
  else
    snprintf(buf, n, "%uB", (unsigned)bytes);
}

// "820ms" / "1.8s" / "12.3s" — one HTTP phase (DNS or TLS), from milliseconds.
inline void formatMs(char* buf, size_t n, uint32_t ms) {
  if (ms < 1000u) snprintf(buf, n, "%ums", (unsigned)ms);
  else            snprintf(buf, n, "%u.%us", (unsigned)(ms / 1000u),
                           (unsigned)((ms % 1000u) / 100u));
}

// "42s" / "3m" / "2h" / "1d" — how long ago the last fetch finished.
inline void formatAge(char* buf, size_t n, uint32_t sec) {
  if (sec >= 86400u)     snprintf(buf, n, "%ud", (unsigned)(sec / 86400u));
  else if (sec >= 3600u) snprintf(buf, n, "%uh", (unsigned)(sec / 3600u));
  else if (sec >= 60u)   snprintf(buf, n, "%um", (unsigned)(sec / 60u));
  else                   snprintf(buf, n, "%us", (unsigned)sec);
}

}  // namespace health_fmt
