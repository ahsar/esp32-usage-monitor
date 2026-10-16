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
// interrupted. Ownership is deliberately one-sided: the context is a static,
// the semaphore is created once and never deleted, and dnsTask is the only
// code that releases the slot (it clears the busy flag last, after its final
// write to the context). The waiting task never has to free anything, so the
// fetch watchdog killing it mid-lookup — the exact point where the old
// refcounted design leaked the context plus its semaphore — is harmless: the
// resolver still finishes, writes the result into a slot nobody reads, and
// hands the context back for the next lookup.
static const uint32_t DNS_TIMEOUT_MS = 8000;
// A previous lookup that timed out (or whose waiter was killed) still owns the
// static context until its resolver finishes. lwIP's retry schedule is the
// only bound on that, so the handover wait is generous but finite; past it the
// lookup fails as a transport failure rather than reading a half-written ip.
static const uint32_t DNS_HANDOVER_MS = 60000;
// TLS handshake cap — the setter takes SECONDS (see the note in httpRequest()).
static const uint32_t HANDSHAKE_TIMEOUT_S = 30;

// See http_client.h: "the path carried nothing" flag for the last request.
static bool s_transportFailed = false;

bool httpTransportFailed() { return s_transportFailed; }

struct DnsCtx {
  char          host[80];
  IPAddress     ip;
  volatile bool ok;
};

static DnsCtx s_dns;
static SemaphoreHandle_t s_dnsSem;            // created lazily, never deleted
static volatile bool     s_dnsBusy = false;   // cleared last, by the resolver

static void dnsTask(void* arg) {
  DnsCtx* c = (DnsCtx*)arg;
  IPAddress ip;
  bool ok = WiFi.hostByName(c->host, ip);
  if (ok) c->ip = ip;
  c->ok = ok;
  // The waiter reads the context only after taking this semaphore, so the
  // results are published before the slot can be reused; clearing the busy
  // flag last is what gives the next lookup its green light.
  xSemaphoreGive(s_dnsSem);
  s_dnsBusy = false;
  vTaskDelete(nullptr);
}

static bool resolveHost(const char* host, IPAddress& out, uint32_t timeoutMs) {
  if (out.fromString(host)) return true;      // already a literal address

  if (!s_dnsSem) {
    s_dnsSem = xSemaphoreCreateBinary();
    if (!s_dnsSem) return false;
  }

  // A previous lookup may still own the context (it timed out, or its waiter
  // was deleted). Take the resolver's completion before even the new name is
  // copied in, so the two tasks never write the same result at once.
  if (s_dnsBusy) {
    if (xSemaphoreTake(s_dnsSem, pdMS_TO_TICKS(DNS_HANDOVER_MS)) != pdTRUE) {
      Serial.println("http: dns context still held by an earlier lookup");
      return false;
    }
    s_dnsBusy = false;
  }

  strlcpy(s_dns.host, host, sizeof(s_dns.host));
  s_dns.ok = false;
  s_dnsBusy = true;
  xSemaphoreTake(s_dnsSem, 0);                // drain a stale give, just in case
  if (xTaskCreate(dnsTask, "dns", 4096, &s_dns, 1, nullptr) != pdPASS) {
    s_dnsBusy = false;
    return false;
  }

  bool ok = (xSemaphoreTake(s_dnsSem, pdMS_TO_TICKS(timeoutMs)) == pdTRUE) && s_dns.ok;
  if (ok) out = s_dns.ip;
  // On a timeout the resolver keeps the slot; it clears busy itself when the
  // lookup finally lands (see dnsTask), and the caller just reads false.
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
  s_transportFailed = false;

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
    s_transportFailed = true;
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
    s_transportFailed = true;
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
    s_transportFailed = true;
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
