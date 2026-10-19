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
// Playback streams on its own FreeRTOS task (`chime.cpp`), never in loop(): one
// chunk is 16 ms of audio, and a loop() iteration — delay(16) plus a render and
// a full-panel push — is at least that long, so a loop-driven writer starves the
// DMA and the note comes out as a ~58 Hz rasp. chimePlay()/chimeSetVolume()/
// chimeStop() are therefore the *only* entry points and all of them just post to
// that task; the caller never blocks and never touches the I2S bus. Only one
// chime plays at a time, and the PA is held open past the last sample so the DMA
// tail drains on silence (no click).

enum ChimeKind : uint8_t {
  CHIME_DONE = 0,   // agent finished  → rising two-note
  CHIME_FAILED,     // agent errored   → one long low note
  CHIME_WAITING,    // needs you       → two soft taps
};

// Build the PCM for every cue and start the playback task. Call once from
// setup() (after hwInit()). False = no audio (no task, no sound).
bool chimeInit();

// Loudness level, 0 (barely there) .. 5 (loudest, still softer than the beeps
// this replaced); values above 5 clamp. Default is 3. Applies from the next
// chimePlay(), so it is safe to call at boot and on a live settings change.
void chimeSetVolume(uint8_t level);

// Arm a chime on the playback task. Dropped if one is already playing (no
// overlap). No-op before chimeInit() succeeded.
void chimePlay(ChimeKind kind);

// True while a chime is being streamed (audio is single-owner: the task).
bool chimeActive();

// Ask the playback task to abandon the current chime and mute the PA. It stops
// at the next chunk boundary (≤ 16 ms), not instantly.
void chimeStop();
