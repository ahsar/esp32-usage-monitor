#pragma once
#include <stdint.h>
#include "ui_compose.h"

#ifdef FRAME_DUMP
// Debug-only: walk every screen/card and stream each raw RGB565 canvas over USB
// CDC so a host script can rebuild PNGs. No-op build unless -DFRAME_DUMP.
void frameDumpTick(uint32_t now, const UiData& d);

// Debug-only: the WiFi setup screens, which the page walk above cannot reach —
// loop() returns early while they are open. True while it still owns the screen.
bool frameDumpWifiTick(uint32_t now);
#endif
