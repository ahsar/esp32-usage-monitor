#include "voice/chime.h"
#include "voice/i2s_audio.h"
#include <Arduino.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

// 16 kHz / mono / 16-bit, matching the I2S + ES8311 configuration in
// i2s_audio.cpp. 500 ms covers the longest sound (two notes + trailing
// silence) with room to spare.
static const uint32_t SR          = 16000;
static const size_t   MAX_SAMPLES = 8000;   // 500 ms
static const size_t   CHUNK       = 256;    // 16 ms per loop() push

static int16_t* s_buf   = nullptr;
static size_t   s_len   = 0;
static size_t   s_pos   = 0;
static bool     s_playing = false;

// Append one plucked note (fast attack, exponential decay).
static size_t addTone(size_t at, float freq, float ms, float amp) {
  size_t n = (size_t)((float)SR * ms / 1000.0f);
  if (at + n > MAX_SAMPLES) n = MAX_SAMPLES - at;
  const float step = 2.0f * 3.14159265f * freq / (float)SR;
  float phase = 0.0f;
  for (size_t i = 0; i < n; i++) {
    float u = (float)i / (float)n;
    float env = expf(-4.0f * u);
    if (u < 0.02f) env *= (u / 0.02f);            // ~1 ms attack ramp
    phase += step;
    s_buf[at + i] = (int16_t)(amp * env * sinf(phase) * 32767.0f);
  }
  return at + n;
}

static size_t addSilence(size_t at, float ms) {
  size_t n = (size_t)((float)SR * ms / 1000.0f);
  if (at + n > MAX_SAMPLES) n = MAX_SAMPLES - at;
  memset(s_buf + at, 0, n * sizeof(int16_t));
  return at + n;
}

bool chimeInit() {
  s_buf = (int16_t*)malloc(MAX_SAMPLES * sizeof(int16_t));
  if (!s_buf) {
    Serial.println("chime: synth buffer alloc failed");
    return false;
  }
  return true;
}

void chimeStop() {
  if (!s_playing) return;
  s_playing = false;
  s_pos = s_len;
  audioTxStop();
}

void chimePlay(ChimeKind kind) {
  if (!s_buf) return;

  size_t at = 0;
  switch (kind) {
    case CHIME_DONE:
      at = addTone(at, 880.0f, 150.0f, 0.55f);     // A5
      at = addTone(at, 1318.5f, 220.0f, 0.60f);    // E6
      break;
    case CHIME_FAILED:
      at = addTone(at, 660.0f, 150.0f, 0.55f);     // E5
      at = addTone(at, 440.0f, 240.0f, 0.60f);     // A4
      break;
    case CHIME_WAITING:
      at = addTone(at, 1046.5f, 90.0f, 0.50f);     // C6
      at = addSilence(at, 70.0f);
      at = addTone(at, 1046.5f, 90.0f, 0.50f);     // C6
      break;
  }
  at = addSilence(at, 40.0f);   // let the DMA drain before the PA is muted
  if (at == 0) return;

  s_len = at;
  s_pos = 0;
  s_playing = true;
  audioTxStart();
}

bool chimeActive() { return s_playing; }

void chimeUpdate(uint32_t /*now*/) {
  if (!s_playing) return;
  size_t remaining = s_len - s_pos;
  size_t n = remaining < CHUNK ? remaining : CHUNK;
  if (n > 0) {
    audioTxWrite((const uint8_t*)(s_buf + s_pos), n * sizeof(int16_t));
    s_pos += n;
  }
  if (s_pos >= s_len) {
    s_playing = false;
    audioTxStop();
  }
}
