#include "voice/chime.h"
#include "voice/i2s_audio.h"
#include <Arduino.h>
#include <cmath>
#include <stdlib.h>
#include <string.h>

// 16 kHz / mono / 16-bit, matching the I2S + ES8311 configuration in
// i2s_audio.cpp. 500 ms covers the longest pattern (two beeps + the silent
// drain tail) with room to spare.
static const uint32_t SR          = 16000;
static const size_t   MAX_SAMPLES = 8000;   // 500 ms
static const size_t   CHUNK       = 256;    // 16 ms per loop() push

// The I2S DMA holds a few descriptors of audio that is queued but not yet
// played (I2S_CHANNEL_DEFAULT_CONFIG → 6×240 frames ≈ 90 ms at 16 kHz). Muting
// the PA the instant the last sample is *queued* chops that tail and pops.
// So every pattern gets a long silent tail and the PA is held on a little
// longer, guaranteeing the queue always drains on silence.
static const float    TAIL_MS  = 150.0f;   // silence after the last beep
static const uint32_t DRAIN_MS = 170;      // …and how long to wait before mute

static int16_t* s_buf = nullptr;
static size_t   s_len = 0;
static size_t   s_pos = 0;

// Mirror of audioTxStart()/audioTxStop(): PLAY streams the buffer, DRAIN keeps
// the PA on while the last queued samples play out, then mutes.
enum Phase : uint8_t { PH_IDLE, PH_PLAY, PH_DRAIN };
static Phase    s_phase   = PH_IDLE;
static uint32_t s_drainAt = 0;

// One plain beep: a flat tone with a raised-cosine fade in/out, so there is no
// click at either edge. `amp` sits just under full scale — with the DAC volume
// at 0 dB (i2s_audio.cpp) that is the loudest clean output; the old crackle came
// from the codec's +12.5 dB digital gain, not the waveform.
static size_t addBeep(size_t at, float freq, float ms, float amp) {
  size_t n = (size_t)((float)SR * ms / 1000.0f);
  if (at + n > MAX_SAMPLES) n = MAX_SAMPLES - at;
  const float step = 2.0f * (float)M_PI * freq / (float)SR;
  size_t fade = (size_t)((float)SR * 0.012f);   // 12 ms
  if (fade * 2 > n) fade = n / 2;
  float phase = 0.0f;
  for (size_t i = 0; i < n; i++) {
    float env = 1.0f;
    if (i < fade)            env = 0.5f - 0.5f * cosf((float)M_PI * (float)i / (float)fade);
    else if (i >= n - fade)  env = 0.5f - 0.5f * cosf((float)M_PI * (float)(n - 1 - i) / (float)fade);
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
  if (s_phase == PH_IDLE) return;
  s_phase = PH_IDLE;
  s_pos = s_len;
  audioTxStop();
}

void chimePlay(ChimeKind kind) {
  if (!s_buf) return;
  // One chime at a time: restarting while the buffer is still playing (or
  // draining) would mix the two into the DMA and sound like a glitch. The
  // status server only fires on real transitions, so dropping a rare overlap is
  // the safe call.
  if (s_phase != PH_IDLE) return;

  size_t at = 0;
  switch (kind) {
    case CHIME_DONE:                          // rising two-beep "ok"
      at = addBeep(at,  880.0f, 110.0f, 0.75f);
      at = addSilence(at, 40.0f);
      at = addBeep(at, 1320.0f, 170.0f, 0.75f);
      break;
    case CHIME_FAILED:                        // one longer low beep
      at = addBeep(at, 440.0f, 260.0f, 0.75f);
      break;
    case CHIME_WAITING:                       // double blip "needs you"
      at = addBeep(at, 1320.0f, 80.0f, 0.75f);
      at = addSilence(at, 70.0f);
      at = addBeep(at, 1320.0f, 80.0f, 0.75f);
      break;
  }
  at = addSilence(at, TAIL_MS);
  if (at == 0) return;

  s_len = at;
  s_pos = 0;
  s_phase = PH_PLAY;
  audioTxStart();
}

bool chimeActive() { return s_phase != PH_IDLE; }

void chimeUpdate(uint32_t now) {
  if (s_phase == PH_IDLE) return;

  if (s_phase == PH_DRAIN) {
    if ((int32_t)(now - s_drainAt) >= 0) {
      s_phase = PH_IDLE;
      audioTxStop();
    }
    return;
  }

  size_t remaining = s_len - s_pos;
  size_t n = remaining < CHUNK ? remaining : CHUNK;
  if (n > 0) {
    audioTxWrite((const uint8_t*)(s_buf + s_pos), n * sizeof(int16_t));
    s_pos += n;
  }
  if (s_pos >= s_len) {
    s_phase   = PH_DRAIN;
    s_drainAt = now + DRAIN_MS;
  }
}
