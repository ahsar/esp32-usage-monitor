#include "voice/chime.h"
#include "voice/i2s_audio.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cmath>
#include <stdlib.h>
#include <string.h>

// 16 kHz / mono / 16-bit, matching the I2S + ES8311 configuration in
// i2s_audio.cpp.
static const uint32_t SR    = 16000;
static const size_t   CHUNK = 256;     // 16 ms per I2S write

// The patterns are built once at boot (see chimeInit), not when a cue fires:
// synthesising them takes tens of milliseconds of libm, and doing that on the
// loop task would stall the UI and delay the sound at exactly the moment you are
// looking at the panel. They are packed into one allocation at their *actual*
// lengths and PAT_MAX is the room each one gets while it is being built, so a
// mis-edited pattern can never run into the next one. The allocation itself is
// the worst case, 3 × PAT_MAX ≈ 66 KB — in PSRAM, not the internal heap the
// pollers and mbedTLS share.
static const size_t PAT_MAX = 11200;   // 700 ms — the longest pattern

// Which volume level those patterns are streamed at: a per-sample gain applied
// while writing (see playPattern), so changing the level never re-synthesises.
// Level 3 is the default and sits well below the old fixed 0.75 peak — the point
// of the table is that "too loud" is a setting, not a rebuild. Even the top of
// the table is a *struck* note, so its energy is a fraction of a flat beep's at
// the same peak.
static const float VOL_GAIN[6] = {0.10f, 0.18f, 0.28f, 0.40f, 0.55f, 0.75f};
static volatile uint8_t s_volLevel = 3;

// The PA needs a moment to come up, and the first DMA fill happens after that.
// Every pattern therefore starts with silence: the amp's turn-on transient and
// the leading edge of the queue land in it instead of on the first note.
static const float    PRE_ROLL_MS = 20.0f;
static const float    TAIL_MS     = 150.0f;   // silence after the last note
static const uint32_t DRAIN_MS    = 170;      // …and how long to wait before mute

// Playback runs on its own task, not in loop(). ONE 256-sample chunk is 16 ms of
// audio, so a writer that is handed one chunk per loop() iteration can never keep
// up: the iteration alone is delay(16) plus the UI/HTTP work, and a full-frame
// render-and-push is 16–70 ms on its own. The DMA (8 × 240 frames = 120 ms) would
// run dry, `auto_clear` would fill the gap with zeros, and every ~16 ms of the
// chime would lose 1–15 ms of tone — measured as the 58 Hz rasp the chimes had.
// Here the write blocks on the DMA itself, which paces the stream at exactly the
// sample rate whatever loop() is doing. Priority 3 > the loop task's 1, so the
// writer preempts a render for a few hundred µs per chunk; it blocks on the DMA
// the rest of the time.
static const uint32_t    CHIME_STACK = 4096;
static const UBaseType_t CHIME_PRIO  = 3;
static TaskHandle_t      s_task      = nullptr;

// Caller → task hand-off. s_req is the armed pattern, s_abort a stop request;
// s_busy is the task telling the caller it is still playing (see chimePlay).
static volatile int8_t s_req   = -1;
static volatile bool   s_busy  = false;
static volatile bool   s_abort = false;

// The three patterns, packed into one buffer at boot; a cue is then just
// "point at the right slice".
struct Pattern { size_t off, len; };
static int16_t* s_buf    = nullptr;
static Pattern  s_pat[3] = {{0, 0}, {0, 0}, {0, 0}};
static int16_t  s_chunk[CHUNK];   // gain-scaled output block, task-local

// Build cursor: only valid inside chimeInit(), where addNote()/addSilence()
// append at it. s_lim is the end of the pattern being built.
static size_t s_at  = 0;
static size_t s_lim = 0;

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
  s_at = addSilence(s_at, PRE_ROLL_MS);
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

// Stream one pattern into the DMA and mute the PA again. Only ever runs on
// s_task (see the CHIME_PRIO comment above).
static void playPattern(ChimeKind kind) {
  const int16_t* src = s_buf + s_pat[kind].off;
  const size_t   len = s_pat[kind].len;

  s_abort = false;
  // PA on (with its settle) — never fails while the codec is up, and stays
  // silent during a WiFi scan.
  if (!audioTxStart()) return;

  const float gain = VOL_GAIN[s_volLevel];
  size_t pos = 0;
  bool   queued = true;
  while (pos < len && !s_abort) {
    size_t n = (len - pos) < CHUNK ? (len - pos) : CHUNK;
    // The patterns are stored at full scale; the volume level is a gain applied
    // here, so changing it never re-synthesises anything.
    for (size_t i = 0; i < n; i++)
      s_chunk[i] = (int16_t)((float)src[pos + i] * gain);
    // Blocks until the DMA has room, which is what paces the stream. A failure
    // means the channel was taken away under us (a scan) — stop cleanly.
    if (!audioTxWrite((const uint8_t*)s_chunk, n * sizeof(int16_t))) {
      queued = false;
      break;
    }
    pos += n;
  }

  // A stop request, or a write that failed because the channel is gone: mute
  // now. There is nothing left in the queue worth waiting for.
  if (s_abort || !queued) {
    audioTxStop();
    return;
  }

  // Everything, including the trailing silence, is queued: the DMA still holds
  // up to a buffer's worth of audio that has not been played yet, and muting the
  // PA the instant the last sample is *queued* chops that tail and pops. The
  // tail is longer than the queue, so waiting here always lands the mute on
  // silence.
  vTaskDelay(pdMS_TO_TICKS(DRAIN_MS));
  audioTxStop();
}

static void chimeTaskFn(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    const int8_t kind = s_req;
    s_req = -1;
    if (kind < 0 || kind > CHIME_WAITING || !s_pat[kind].len) continue;
    s_busy = true;
    playPattern((ChimeKind)kind);
    s_busy = false;
  }
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
  // Pinned to the loop's core: the point of the task is to preempt the render
  // and the rest of the iteration, which all run there.
  if (xTaskCreatePinnedToCore(chimeTaskFn, "chime", CHIME_STACK, nullptr,
                              CHIME_PRIO, &s_task, ARDUINO_RUNNING_CORE) != pdPASS) {
    Serial.println("chime: task create failed (no chimes)");
    s_task = nullptr;
    return false;
  }
  return true;
}

void chimeSetVolume(uint8_t level) {
  s_volLevel = level > 5 ? 5 : level;
}

void chimePlay(ChimeKind kind) {
  // One chime at a time: arming a second one while the first is still playing
  // would overlap them in the DMA and sound like a glitch. The status server
  // only fires on real transitions, so dropping a rare overlap is the safe call.
  if (!s_task || s_busy) return;
  if (kind > CHIME_WAITING || !s_pat[kind].len) return;
  s_req = (int8_t)kind;
  xTaskNotifyGive(s_task);
}

bool chimeActive() { return s_busy; }

void chimeStop() {
  if (!s_task || !s_busy) return;
  s_abort = true;   // honoured at the next chunk boundary (≤ 16 ms)
}
