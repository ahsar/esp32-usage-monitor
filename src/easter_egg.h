#pragma once
#include <stdint.h>

// Shake-to-play "555" easter egg: IMU shake detection + streaming of
// /555_audio.raw to the codec. The caller renders the animation itself (via
// usageDisplayDrawEaster555) while easterEggActive() is true.

void easterEggSetWakeCallback(void (*cb)());   // called when a shake starts playback

bool     easterEggActive();
uint32_t easterEggElapsedMs();
void     easterEggStop();

// Per-frame: run shake detection and, if playing, pump the audio out.
void easterEggUpdate(uint32_t now);
