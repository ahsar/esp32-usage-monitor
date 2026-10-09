#include "easter_egg.h"
#include "status_server.h"
#include "hw/imu.h"
#include "voice/i2s_audio.h"
#include <Arduino.h>
#include <LittleFS.h>

static const char* AUDIO_PATH = "/555_audio.raw";

static int      s_state = 0;        // 0 idle, 1 playing
static uint32_t s_start = 0;
static File     s_file;
static uint32_t s_total = 0;
static uint32_t s_written = 0;

static uint32_t s_lastShake = 0;
static uint8_t  s_shakeCount = 0;

static void (*s_wakeCb)() = nullptr;

void easterEggSetWakeCallback(void (*cb)()) { s_wakeCb = cb; }

bool     easterEggActive()      { return s_state == 1; }
uint32_t easterEggElapsedMs()   { return millis() - s_start; }

void easterEggStop() {
  if (s_state == 1) {
    s_file.close();
    audioTxStop();
    s_state = 0;
  }
}

void easterEggUpdate(uint32_t now) {
  // Shake detection (throttled to 20 Hz — the IMU read is an I2C op). Suppressed
  // while the status server says the agent is thinking.
  if (s_state == 0 && !statusServerThinking() && now - s_lastShake >= 50) {
    s_lastShake = now;
    float ax, ay, az;
    hwImuAccel(&ax, &ay, &az);
    float mag2 = ax*ax + ay*ay + az*az;
    if (mag2 > 4.0f) s_shakeCount++;
    else s_shakeCount = 0;

    if (s_shakeCount >= 3) {
      s_shakeCount = 0;
      Serial.println("Easter: shake detected!");
      statusServerClearThinking();
      if (s_wakeCb) s_wakeCb();
      s_file = LittleFS.open(AUDIO_PATH, "r");
      if (s_file) {
        s_total = s_file.size();
        s_written = 0;
        s_start = now;
        s_state = 1;
        audioTxStart();
        Serial.printf("Easter: playing %u bytes\n", s_total);
      }
    }
  }

  if (s_state == 1) {
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
}
