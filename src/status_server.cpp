#include "status_server.h"
#include <Arduino.h>
#include <WiFi.h>

static WiFiServer s_server(80);
static bool s_thinking = false;
static uint32_t s_thinkingSince = 0;
static void (*s_wakeCb)() = nullptr;

void statusServerBegin() { s_server.begin(); }
void statusServerSetWakeCallback(void (*cb)()) { s_wakeCb = cb; }

bool     statusServerThinking()     { return s_thinking; }
uint32_t statusServerThinkingSince() { return s_thinkingSince; }
void     statusServerClearThinking() { s_thinking = false; }

extern "C" void handleHttpStatus() {
  WiFiClient client = s_server.accept();
  if (!client) return;

  client.setTimeout(50);

  uint32_t t0 = millis();
  while (!client.available() && (millis() - t0) < 100) {
    delay(1);
  }

  String req;
  for (int i = 0; i < 15; i++) {
    if (!client.connected() && !client.available()) break;
    if (!client.available()) break;
    String line = client.readStringUntil('\n');
    req += line;
    if (line == "\r") break;
  }

  if (req.indexOf("GET ") < 0) {
    client.println("HTTP/1.1 405 Method Not Allowed\r\nConnection: close\r\n\r\n");
    delay(10);
    client.stop();
    return;
  }

  if (req.indexOf("state=running") >= 0) {
    s_thinking = true;
    s_thinkingSince = millis();
    Serial.println("state=running");
    if (s_wakeCb) s_wakeCb();     // light the panel while the agent is working
  } else if (req.indexOf("state=done") >= 0 || req.indexOf("state=failed") >= 0) {
    s_thinking = false;
    Serial.println("state=done/failed");
  }

  client.println("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nOK");
  delay(10);
  client.stop();
}
