#include "http_client.h"
#include "diag.h"
#include <WiFiClientSecure.h>
#include <string.h>

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
  client.setHandshakeTimeout(15000);
  client.setTimeout(10000);
  diagMark(DIAG_HTTP_CONNECT, 0);
  diagMarkHost(host);
  if (!client.connect(host, 443, 10000)) {
    char err[128] = {0};
    client.lastError(err, sizeof(err));
    Serial.printf("http: connect failed %s (%s)\n", host, err);
    diagMark(DIAG_HTTP_CONNECTED, 0xFFFF);
    return false;
  }
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

  int hdrEnd = raw.indexOf("\r\n\r\n");
  if (hdrEnd < 0) return false;

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
  return true;
}
