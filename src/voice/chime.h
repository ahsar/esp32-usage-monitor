#pragma once
#include <stdint.h>

// Short completion chimes, played through the shared ES8311/I2S path
// (voice/i2s_audio.*). The PCM is synthesised at boot — no raw file
// on LittleFS — so activating a chime never needs an `uploadfs` (which would
// erase the device's rotated OAuth tokens).
//
// Playback is streamed from loop(): chimePlay() arms a buffer and chimeUpdate()
// pushes one small chunk per iteration, so neither call blocks the UI for
// longer than the I2S DMA needs.

enum ChimeKind : uint8_t {
  CHIME_DONE = 0,   // agent finished  → rising two-note
  CHIME_FAILED,     // agent errored   → falling two-note
  CHIME_WAITING,    // needs you       → double blip
};

// Allocate the synthesis buffer. Call once from setup() (after hwInit()).
bool chimeInit();

// Arm a chime. Restarts playback if one is already running. No-op before
// chimeInit() succeeded.
void chimePlay(ChimeKind kind);

// Stream the armed chime out. Call every loop(); cheap when idle.
void chimeUpdate(uint32_t now);

// True while a chime is being streamed (audio is single-owner).
bool chimeActive();

// Abandon the current chime and mute the PA.
void chimeStop();
