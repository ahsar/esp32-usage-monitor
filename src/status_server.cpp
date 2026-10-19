// The HTTP transport for the device channel — see status_server.h. Everything
// about *what a request means* (auth, the state machine, settings/WiFi parsing)
// lives in status_channel.cpp; this file only accepts the connection, reads a
// bounded request head and frames the query + HMAC pair for it.
#include "status_server.h"
#include "status_channel.h"
#include "parse/http_request.h"
#include <Arduino.h>
#include <WiFi.h>
#include <stdio.h>
#include <string.h>

static WiFiServer s_server(80);

void statusServerBegin() { s_server.begin(); }

static void respond(WiFiClient& client, const char* status, const char* body = "OK") {
  client.print("HTTP/1.1 ");
  client.print(status);
  client.print("\r\nConnection: close\r\nContent-Type: text/plain\r\nContent-Length: ");
  client.print((unsigned)strlen(body));
  client.print("\r\n\r\n");
  client.print(body);
  delay(10);
  client.stop();
}

// The request head (request line + headers) lands in a fixed buffer. This is
// where an unbounded read used to live: readStringUntil() into a String would
// grow until a client that never sent a newline had squeezed internal RAM —
// the pool mbedTLS and Bluedroid need on this board — dry. Every line still
// keeps its '\n' so header lookup can scan for "\nName:" regardless of line
// endings, and a blank "\r\n" line ends the head early. 1.5 KB and 24 lines
// are far above what the plugin sends (request line + 3 headers ≈ 350 B).
static const size_t   HTTP_HEAD_MAX       = 1536;
static const int      HTTP_HEAD_MAX_LINE  = 24;
// Wall-clock bound for the whole head, not just per byte. The per-byte wait
// below alone only bounds a *silent* client: a peer that dribbles one byte every
// 50 ms could hold loop() for 1536 × 60 ms ≈ 90 s — a frozen UI and no fetch,
// which is exactly what this server must never be able to cause. A normal LAN
// request arrives in a few ms, but a station in power save on a congested AP can
// take a few hundred ms to deliver the head after accept() — 1 s bounds the stall
// while leaving room for that, instead of making a legitimate host fall back to
// BLE.
static const uint32_t HTTP_HEAD_DEADLINE_MS = 1000;

static size_t readRequestHead(WiFiClient& client, char* head, size_t cap) {
  size_t len = 0, lineStart = 0;
  int lines = 0;
  const uint32_t t0 = millis();
  while (len + 1 < cap && lines < HTTP_HEAD_MAX_LINE) {
    if (millis() - t0 >= HTTP_HEAD_DEADLINE_MS) break;
    while (!client.available() && millis() - t0 < HTTP_HEAD_DEADLINE_MS) delay(1);
    if (!client.available()) break;            // peer went quiet or deadline hit
    int b = client.read();
    if (b < 0) break;                          // socket timeout mid-byte
    head[len++] = (char)b;
    if (b == '\n') {
      lines++;
      if (len - lineStart == 2 && head[lineStart] == '\r') break;  // blank line
      lineStart = len;
    }
  }
  head[len] = '\0';
  return len;
}

// ---- the HTTP transport ---------------------------------------------------

extern "C" void handleHttpStatus() {
  WiFiClient client = s_server.accept();
  if (!client) return;

  client.setTimeout(50);

  // Read the request head (request line + headers) into a fixed buffer.
  static char req[HTTP_HEAD_MAX];
  size_t reqLen = readRequestHead(client, req, sizeof(req));

  size_t g;
  if (!httpMemFind(req, reqLen, "GET ", 4, g)) {
    respond(client, "405 Method Not Allowed");
    return;
  }

  // The transport's whole job: pull the query and the HMAC pair out of the wire
  // format, then hand them to the shared handler. The buffers are sized from the
  // channel's own caps (status_channel.h), so a WiFi request — an SSID plus a
  // passphrase with every byte percent-encoded — fits on both transports.
  char query[STATUS_QUERY_MAX] = {0};
  char ts[STATUS_TS_MAX] = {0}, sig[STATUS_SIG_MAX] = {0}, peer[24] = {0};
  bool tooLong = false;
  if (!httpQueryFromHead(req, reqLen, query, sizeof(query), &tooLong)) query[0] = '\0';
  if (tooLong) {
    // Say so instead of answering the health-probe body: a request that was cut
    // to an empty query would come back "200 OK" and quietly do nothing.
    respond(client, "414 URI Too Long");
    return;
  }
  httpHeaderValue(req, reqLen, "X-Ts", ts, sizeof(ts));
  httpHeaderValue(req, reqLen, "X-Sig", sig, sizeof(sig));
  IPAddress ip = client.remoteIP();
  snprintf(peer, sizeof(peer), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);

  char body[STATUS_BODY_MAX];
  StatusReq r{query, ts, sig, peer};
  respond(client, statusChannelHandle(r, body, sizeof(body)), body);
}
