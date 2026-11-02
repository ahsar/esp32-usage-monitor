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

// Debug-only: the agent card stack (one screen per OpenCode session). Also
// unreachable by the page walk: those screens are drawn from uiFrame()'s modal
// branch and need live sessions, so this seeds its own cards. True while it still
// owns the screen.
bool frameDumpAgentTick(uint32_t now);
#endif
