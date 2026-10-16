#include "voice/chime.h"
#include "voice/i2s_audio.h"
#include <Arduino.h>
#include <cmath>
#include <stdlib.h>
#include <string.h>

// 16 kHz / mono / 16-bit, matching the I2S + ES8311 configuration in
// i2s_audio.cpp.
static const uint32_t SR    = 16000;
static const size_t   CHUNK = 256;     // 16 ms per loop() push

// The patterns are built once at boot (see chimeInit), not when a cue fires:
// synthesising them takes tens of milliseconds of libm per pattern, and doing
// that on the loop task would stall the UI and delay the sound at exactly the
// moment you are looking at the panel. They are packed into one allocation at
// their *actual* lengths and PAT_MAX is the room each one gets while it is being
// built, so a mis-edited pattern can never run into the next one. The allocation
// itself is the worst case, 3 × PAT_MAX ≈ 66 KB — in PSRAM, not the internal
// heap the pollers and mbedTLS share.
static const size_t PAT_MAX = 11200;   // 700 ms — the longest pattern

// Which volume level those patterns are streamed at: a per-sample gain applied
// while writing (see chimeUpdate), so changing the level never re-synthesises.
// Level 3 is the default and sits well below the old fixed 0.75 peak — the point
// of the table is that "too loud" is a setting, not a rebuild. Even the top of
// the table is a *struck* note, so its energy is a fraction of a flat beep's at
// the same peak.
static const float VOL_GAIN[6] = {0.10f, 0.18f, 0.28f, 0.40f, 0.55f, 0.75f};
static uint8_t s_volLevel = 3;

// The I2S DMA holds a few descriptors of audio that is queued but not yet
// played (I2S_CHANNEL_DEFAULT_CONFIG → 6×240 frames ≈ 90 ms at 16 kHz). Muting
// the PA the instant the last sample is *queued* chops that tail and pops.
// So every pattern gets a long silent tail and the PA is held on a little
// longer, guaranteeing the queue always drains on silence.
static const float    TAIL_MS  = 150.0f;   // silence after the last note
static const uint32_t DRAIN_MS = 170;      // …and how long to wait before mute

// The three patterns, packed into one buffer at boot; a cue is then just
// "point at the right slice".
struct Pattern { size_t off, len; };
static int16_t* s_buf    = nullptr;
static Pattern  s_pat[3] = {{0, 0}, {0, 0}, {0, 0}};
static int16_t  s_chunk[CHUNK];   // gain-scaled output block

static size_t s_off = 0;          // active pattern offset into s_buf
static size_t s_len = 0;          // active pattern length (samples)
static size_t s_pos = 0;          // …and how far it has been streamed

// Build cursor: only valid inside chimeInit(), where addNote()/addSilence()
// append at it. s_lim is the end of the pattern being built.
static size_t s_at  = 0;
static size_t s_lim = 0;

// Mirror of audioTxStart()/audioTxStop(): PLAY streams the buffer, DRAIN keeps
// the PA on while the last queued samples play out, then mutes.
enum Phase : uint8_t { PH_IDLE, PH_PLAY, PH_DRAIN };
static Phase    s_phase   = PH_IDLE;
static uint32_t s_drainAt = 0;

// One struck note: a short raised-cosine attack, then an exponential decay — the
// envelope of a bar being tapped, not a held tone.
//
// Both halves of that matter for how harsh it sounds. A sustained, equal-level
// high tone with a 12 ms edge (the previous shape) is close to the worst case
// for a small speaker: the ear is most sensitive at 1–4 kHz, the abrupt corners
// read as a click, and the flat top keeps the full energy on for the whole
// duration. Decaying in ~a third of the note length drops the average energy far
// below the peak, and the long tail means the note has already faded before the
// release fade closes it, so there is nothing left to click.
//
// `freq` is the fundamental; a little 2nd/3rd harmonic (unit-peak normalised) is
// mixed in because a pure sine is what makes an electronic chirp sound piercing.
// `amp` is 1.0 at build time — the volume level is applied later, on the way to
// the I2S DMA.
static size_t addNote(size_t at, float freq, float ms, float amp) {
  size_t n = (size_t)((float)SR * ms / 1000.0f);
  if (at + n > s_lim) n = (s_lim > at) ? s_lim - at : 0;
  const float step = 2.0f * (float)M_PI * freq / (float)SR;
  size_t atk = (size_t)((float)SR * 0.008f);    // 8 ms attack
  size_t rel = (size_t)((float)SR * 0.012f);    // 12 ms release
  if (atk * 2 > n) atk = n / 2;
  if (rel * 2 > n) rel = n / 2;
  const float tau = ms * 0.32f;                 // decay time constant
  float phase = 0.0f;
  for (size_t i = 0; i < n; i++) {
    float env = expf(-((float)i / (float)SR * 1000.0f) / tau);
    if (i < atk)            env *= 0.5f - 0.5f * cosf((float)M_PI * (float)i / (float)atk);
    else if (i >= n - rel)  env *= 0.5f - 0.5f * cosf((float)M_PI * (float)(n - 1 - i) / (float)rel);
    phase += step;
    float w = (sinf(phase) + 0.18f * sinf(2.0f * phase)
                           + 0.07f * sinf(3.0f * phase)) / 1.25f;
    s_buf[at + i] = (int16_t)(amp * env * w * 32767.0f);
  }
  return at + n;
}

static size_t addSilence(size_t at, float ms) {
  size_t n = (size_t)((float)SR * ms / 1000.0f);
  if (at + n > s_lim) n = (s_lim > at) ? s_lim - at : 0;
  memset(s_buf + at, 0, n * sizeof(int16_t));
  return at + n;
}

// Build one cue at the current cursor. Pitches stay low on purpose: the old
// 1320 Hz blip sat right where the ear is most sensitive.
static size_t buildPattern(ChimeKind kind) {
  size_t start = s_at;
  switch (kind) {
    case CHIME_DONE:                          // rising two-note "ok"
      s_at = addNote(s_at,  660.0f, 180.0f, 1.0f);
      s_at = addSilence(s_at, 60.0f);
      s_at = addNote(s_at,  990.0f, 260.0f, 1.0f);
      break;
    case CHIME_FAILED:                        // one low note, longer decay
      s_at = addNote(s_at,  330.0f, 360.0f, 1.0f);
      break;
    case CHIME_WAITING:                       // two soft taps "needs you"
      s_at = addNote(s_at,  880.0f, 120.0f, 1.0f);
      s_at = addSilence(s_at, 110.0f);
      s_at = addNote(s_at,  880.0f, 120.0f, 1.0f);
      break;
  }
  s_at = addSilence(s_at, TAIL_MS);
  return s_at - start;
}

bool chimeInit() {
  // The worst case: the three patterns, each at PAT_MAX. A malloc() this size
  // goes to PSRAM (CONFIG_SPIRAM_USE_MALLOC), not to the internal heap.
  s_buf = (int16_t*)malloc(3 * PAT_MAX * sizeof(int16_t));
  if (!s_buf) {
    Serial.println("chime: synth buffer alloc failed");
    return false;
  }
  for (uint8_t k = 0; k < 3; k++) {
    s_lim = s_at + PAT_MAX;            // addNote()/addSilence() clip at this
    s_pat[k].off = s_at;
    s_pat[k].len = buildPattern((ChimeKind)k);
    Serial.printf("chime: pattern %u = %u samples\n",
                  (unsigned)k, (unsigned)s_pat[k].len);
  }
  return true;
}

void chimeSetVolume(uint8_t level) {
  s_volLevel = level > 5 ? 5 : level;
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
  if (kind > CHIME_WAITING || !s_pat[kind].len) return;

  s_off = s_pat[kind].off;
  s_len = s_pat[kind].len;
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
    // The patterns are stored at full scale; the volume level is a gain applied
    // here, so changing it never re-synthesises anything.
    const float gain = VOL_GAIN[s_volLevel];
    for (size_t i = 0; i < n; i++)
      s_chunk[i] = (int16_t)((float)s_buf[s_off + s_pos + i] * gain);
    audioTxWrite((const uint8_t*)s_chunk, n * sizeof(int16_t));
    s_pos += n;
  }
  if (s_pos >= s_len) {
    s_phase   = PH_DRAIN;
    s_drainAt = now + DRAIN_MS;
  }
}
