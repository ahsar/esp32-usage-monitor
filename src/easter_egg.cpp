#include "easter_egg.h"
#include "status_server.h"
#include "voice/i2s_audio.h"
#include <Arduino.h>
#include <LittleFS.h>

static const char* AUDIO_PATH = "/555_audio.raw";

static int      s_state = 0;        // 0 idle, 1 playing
static uint32_t s_start = 0;
static File     s_file;
static uint32_t s_total = 0;
static uint32_t s_written = 0;

static void (*s_wakeCb)() = nullptr;

void easterEggSetWakeCallback(void (*cb)()) { s_wakeCb = cb; }

bool     easterEggActive()      { return s_state == 1; }
uint32_t easterEggElapsedMs()   { return millis() - s_start; }

void easterEggStart() {
  if (s_state == 1) return;
  // The easter egg owns the screen: drop the thinking/waiting state and light
  // the panel, exactly like a status-server wake.
  statusServerClearThinking();
  if (s_wakeCb) s_wakeCb();

  s_file = LittleFS.open(AUDIO_PATH, "r");
  if (!s_file) {
    Serial.println("Easter: no audio file");
    return;
  }
  s_total = s_file.size();
  s_written = 0;
  s_start = millis();
  s_state = 1;
  audioTxStart();
  Serial.printf("Easter: playing %u bytes\n", s_total);
}

void easterEggStop() {
  if (s_state == 1) {
    s_file.close();
    audioTxStop();
    s_state = 0;
  }
}

void easterEggUpdate(uint32_t /*now*/) {
  if (s_state != 1) return;

  uint8_t buf[512];
  size_t toRead = sizeof(buf);
  if (s_total - s_written < toRead) toRead = s_total - s_written;
  if (toRead > 0) {
    size_t n = s_file.read(buf, toRead);
    audioTxWrite(buf, n);
    s_written += n;
  }
  if (s_written >= s_total) {
    s_file.close();
    audioTxStop();
    s_state = 0;
    Serial.println("Easter: done");
  }
}
