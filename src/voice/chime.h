#pragma once
#include <stdint.h>

// Short completion cues, played through the shared ES8311/I2S path
// (voice/i2s_audio.*). The PCM is synthesised at boot — no raw file
// on LittleFS — so activating a chime never needs an `uploadfs` (which would
// erase the device's rotated OAuth tokens).
//
// The cues are struck notes (fast attack, exponential decay, a little 2nd/3rd
// harmonic), not flat beeps: at the same peak a decaying note of a lower pitch
// carries far less energy and stops reading as an alarm. Loudness is a separate
// knob (chimeSetVolume), applied as a gain while streaming.
//
// Playback is streamed from loop(): chimePlay() arms a buffer and chimeUpdate()
// pushes one small chunk per iteration, so neither call blocks the UI for
// longer than the I2S DMA needs. Only one chime plays at a time, and the PA is
// held open past the last sample so the DMA tail drains on silence (no click).

enum ChimeKind : uint8_t {
  CHIME_DONE = 0,   // agent finished  → rising two-note
  CHIME_FAILED,     // agent errored   → one long low note
  CHIME_WAITING,    // needs you       → two soft taps
};

// Build the PCM for every cue. Call once from setup() (after hwInit()).
bool chimeInit();

// Loudness level, 0 (barely there) .. 5 (loudest, still softer than the beeps
// this replaced); values above 5 clamp. Default is 3. Applies from the next
// chimePlay(), so it is safe to call at boot and on a live settings change.
void chimeSetVolume(uint8_t level);

// Arm a chime. Dropped if one is already playing/draining (no overlap). No-op
// before chimeInit() succeeded.
void chimePlay(ChimeKind kind);

// Stream the armed chime out. Call every loop(); cheap when idle.
void chimeUpdate(uint32_t now);

// True while a chime is being streamed (audio is single-owner).
bool chimeActive();

// Abandon the current chime and mute the PA.
void chimeStop();
