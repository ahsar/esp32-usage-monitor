#pragma once
#include <stdint.h>
#include "ui_compose.h"

#ifdef FRAME_DUMP
// Debug-only: walk every screen/card and stream each raw RGB565 canvas over USB
// CDC so a host script can rebuild PNGs. No-op build unless -DFRAME_DUMP.
void frameDumpTick(uint32_t now, const UiData& d);
#endif
