#include "http_client.h"
#include "diag.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Bounded DNS
// ---------------------------------------------------------------------------
// TLS connect does not exist on its own: NetworkClientSecure::connect(host, …)
// resolves the name internally via lwip_getaddrinfo(), which takes no timeout.
// A DNS server that stops answering is therefore bounded only by lwIP's own
// retry schedule (seconds to tens of seconds), and the delay is invisible in
// the logs. Resolving here instead gives DNS a real deadline and its own phase
// in the timings below. The TLS connect still uses the *hostname* (SNI +
// hostname verification), which now costs nothing — lwIP's cache answers it.
//
// The resolver runs in a throwaway task because the lwIP call cannot be
// interrupted. The context is refcounted (caller + task), so a lookup that
// outlives its deadline cannot be freed under the still-running task.
static const uint32_t DNS_TIMEOUT_MS = 8000;
// TLS handshake cap — the setter takes SECONDS (see the note in httpRequest()).
static const uint32_t HANDSHAKE_TIMEOUT_S = 30;

struct DnsCtx {
  char              host[80];
  IPAddress         ip;
  volatile bool     ok;
  SemaphoreHandle_t done;
  volatile int      refs;
};

static void dnsTask(void* arg) {
  DnsCtx* c = (DnsCtx*)arg;
  IPAddress ip;
  bool ok = WiFi.hostByName(c->host, ip);
  if (ok) c->ip = ip;
  c->ok = ok;
  xSemaphoreGive(c->done);
  if (__atomic_sub_fetch(&c->refs, 1, __ATOMIC_SEQ_CST) == 0) {
    vSemaphoreDelete(c->done);
    free(c);
  }
  vTaskDelete(nullptr);
}

static bool resolveHost(const char* host, IPAddress& out, uint32_t timeoutMs) {
  if (out.fromString(host)) return true;      // already a literal address

  DnsCtx* c = (DnsCtx*)calloc(1, sizeof(DnsCtx));
  if (!c) return false;
  strlcpy(c->host, host, sizeof(c->host));
  c->done = xSemaphoreCreateBinary();
  c->refs = 2;                                 // caller + resolver task
  if (!c->done || xTaskCreate(dnsTask, "dns", 4096, c, 1, nullptr) != pdPASS) {
    if (c->done) vSemaphoreDelete(c->done);
    free(c);
    return false;
  }
  bool ok = (xSemaphoreTake(c->done, pdMS_TO_TICKS(timeoutMs)) == pdTRUE) && c->ok;
  if (ok) out = c->ip;
  if (__atomic_sub_fetch(&c->refs, 1, __ATOMIC_SEQ_CST) == 0) {
    vSemaphoreDelete(c->done);
    free(c);
  }
  return ok;
}

// The ESP-IDF CA bundle (~130 public roots, compiled into libmbedtls). Using it
// instead of setInsecure() means the TLS chain is actually verified, so an
// active MitM on the LAN cannot impersonate opencode.ai with a self-signed
// certificate and lift the OAuth tokens. The bundle is the IDF default, so
// there is nothing to embed or rotate here.
//
// Verification also checks the certificate's validity dates, so the system
// clock must be plausible before the first fetch — see clockSyncRestoreFromRtc()
// (called from setup()) and the clockSyncValid() gate on the poll schedule.
extern "C" const uint8_t x509_crt_imported_bundle_bin_start[] asm("_binary_x509_crt_bundle_start");
extern "C" const uint8_t x509_crt_imported_bundle_bin_end[] asm("_binary_x509_crt_bundle_end");

bool httpRequest(const char* host, const char* method, const char* path,
                 const char* extraHeaders, const char* body,
                 HttpResponse& res, bool http10, size_t maxBody, uint32_t timeoutMs,
                 const char* userAgent) {
  res.status = 0;
  res.body   = "";
  res.date   = "";

  WiFiClientSecure client;
  client.setCACertBundle(x509_crt_imported_bundle_bin_start,
                         (size_t)(x509_crt_imported_bundle_bin_end -
                                  x509_crt_imported_bundle_bin_start));
  // Bound the TLS handshake: without this the default is 120 s, so a failing
  // handshake (bad chain, blocked port) wedges the fetch task for two minutes.
  //
  // NOTE the unit: NetworkClientSecure::setHandshakeTimeout() takes SECONDS and
  // multiplies by 1000 internally (the field is compared against millis()).
  // Passing 15000 here asked for 4 h 10 m, not 15 s, so a silently-dropped
  // ClientHello (blocked host) froze the whole fetch task — and with it every
  // poller — for hours.
  //
  // 30 s (not 15): a lossy link makes the handshake run into TCP retransmit
  // backoff (1+2+4+8…), which lands real handshakes in the 15-30 s band. The
  // cap only has to be a bound — a dead peer is abandoned long before this —
  // and a cap that is too tight turns "slow" into "always fails".
  client.setHandshakeTimeout(HANDSHAKE_TIMEOUT_S);
  // Stream::setTimeout() is milliseconds (the socket timeouts derive from it).
  client.setTimeout(10000);

  // Phase timings: each blocking step is logged so a slow/blocked host is
  // obvious at a glance instead of looking like a dead worker.
  uint32_t tDns = millis();
  IPAddress ip;
  if (!resolveHost(host, ip, DNS_TIMEOUT_MS)) {
    Serial.printf("http: %s dns failed/timed out after %lums\n", host,
                  (unsigned long)(millis() - tDns));
    diagMark(DIAG_HTTP_CONNECT, 0xFFFF);
    return false;
  }
  uint32_t dnsMs = millis() - tDns;

  diagMark(DIAG_HTTP_CONNECT, 0);
  diagMarkHost(host);
  uint32_t tConn = millis();
  if (!client.connect(host, 443, 10000)) {
    char err[128] = {0};
    client.lastError(err, sizeof(err));
    Serial.printf("http: connect failed %s ip=%s dns=%lums tls=%lums (%s)\n",
                  host, ip.toString().c_str(), (unsigned long)dnsMs,
                  (unsigned long)(millis() - tConn), err);
    diagMark(DIAG_HTTP_CONNECTED, 0xFFFF);
    return false;
  }
  uint32_t connMs = millis() - tConn;
  diagMark(DIAG_HTTP_CONNECTED, millis());

  size_t bodyLen = body ? strlen(body) : 0;

  String head;
  head.reserve(512);
  head += method; head += " "; head += path; head += " HTTP/1.";
  head += http10 ? "0" : "1";
  head += "\r\n";
  head += "Host: "; head += host; head += "\r\n";
  head += "User-Agent: "; head += userAgent; head += "\r\n";
  head += "Accept: */*\r\n";
  head += "Connection: close\r\n";
  if (extraHeaders && extraHeaders[0]) head += extraHeaders;
  if (body) {
    head += "Content-Type: application/json\r\n";
    head += "Content-Length: "; head += (unsigned)bodyLen; head += "\r\n";
  }
  head += "\r\n";

  client.print(head);
  if (body && bodyLen) client.print(body);

  uint32_t start = millis();
  uint32_t lastData = millis();
  String raw;
  raw.reserve(maxBody < 4096 ? 4096 : (maxBody > 16384 ? 16384 : maxBody));
  uint8_t chunk[512];
  while (millis() - start < timeoutMs) {
    diagMark(DIAG_HTTP_READ, raw.length());
    int avail = client.available();
    if (avail > 0) {
      // Drain what is currently buffered. Re-querying available() per chunk is
      // expensive on WiFiClientSecure and badly hurts throughput.
      while (avail > 0) {
        int want = avail > (int)sizeof(chunk) ? (int)sizeof(chunk) : avail;
        int r = client.read(chunk, want);
        if (r <= 0) break;
        raw.concat((const char*)chunk, (unsigned)r);
        lastData = millis();
        avail -= r;
        if (raw.length() > maxBody) break;
      }
      if (raw.length() > maxBody) break;
      continue;
    }

    // After the socket closes the TLS layer may still hold decrypted bytes, so
    // keep draining briefly before giving up.
    if (!client.connected() && (millis() - lastData) > 500) break;
    delay(1);
  }
  client.stop();
  diagMark(DIAG_HTTP_DONE, raw.length());

  uint32_t readMs = millis() - start;
  int hdrEnd = raw.indexOf("\r\n\r\n");
  if (hdrEnd < 0) {
    Serial.printf("http: %s no response headers ip=%s dns=%lums tls=%lums "
                  "read=%lums rx=%u\n",
                  host, ip.toString().c_str(), (unsigned long)dnsMs,
                  (unsigned long)connMs, (unsigned long)readMs,
                  (unsigned)raw.length());
    return false;
  }

  String headers = raw.substring(0, hdrEnd);
  res.body = raw.substring(hdrEnd + 4);

  // Only trust a well-formed status line ("HTTP/1.x <code> ...").
  if (headers.startsWith("HTTP/")) {
    int sp = headers.indexOf(' ');
    if (sp > 0) res.status = headers.substring(sp + 1).toInt();  // stops at the space
  }

  String lower = headers;
  lower.toLowerCase();
  int d = lower.indexOf("\r\ndate:");
  if (d >= 0) {
    int e = lower.indexOf("\r\n", d + 2);
    res.date = headers.substring(d + 2, e < 0 ? headers.length() : e);
    res.date.trim();
  }
  Serial.printf("http: %s -> %d ip=%s dns=%lums tls=%lums read=%lums rx=%u\n",
                host, res.status, ip.toString().c_str(), (unsigned long)dnsMs,
                (unsigned long)connMs, (unsigned long)readMs,
                (unsigned)raw.length());
  return true;
}
