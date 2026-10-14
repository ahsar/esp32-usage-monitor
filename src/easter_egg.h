#pragma once
#include <stdint.h>

// Shake-to-play "555" easter egg: streaming of /555_audio.raw to the codec.
// Shake detection itself lives in gestures.cpp now (single IMU poll point);
// main.cpp calls easterEggStart() when it sees gesture::EV_SHAKE. The caller
// renders the animation itself (via usageDisplayDrawEaster555) while
// easterEggActive() is true.

void easterEggSetWakeCallback(void (*cb)());   // called when a shake starts playback

// Begin playback (no-op when already playing). Clears the "thinking" state and
// wakes the panel so the animation is visible.
void easterEggStart();

bool     easterEggActive();
uint32_t easterEggElapsedMs();
void     easterEggStop();

// Per-frame: pump the audio out while playing.
void easterEggUpdate(uint32_t now);
