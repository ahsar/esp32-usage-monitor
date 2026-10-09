#pragma once
// Shared UI palette. Centralised so the main UI and the WiFi setup screen can
// never drift apart; both pull their colours from here via
// `using namespace ui_theme;`.
#include <stdint.h>

namespace ui_theme {

constexpr uint16_t COL_BG     = 0x0000;
constexpr uint16_t COL_TEXT   = 0xFFFF;
constexpr uint16_t COL_DIM    = 0x9CF3;
constexpr uint16_t COL_MUTED  = 0x528A;
constexpr uint16_t COL_LINE   = 0x3186;
constexpr uint16_t COL_TRACK  = 0x2104;

constexpr uint16_t COL_GREEN  = 0x07E0;
constexpr uint16_t COL_YELLOW = 0xFFE0;
constexpr uint16_t COL_ORANGE = 0xFD20;
constexpr uint16_t COL_RED    = 0xF800;
constexpr uint16_t COL_PURPLE = 0xC81F;
constexpr uint16_t COL_MAROON = 0x8000;
constexpr uint16_t COL_HOT    = 0xFA20;

// Weather / sky artwork.
constexpr uint16_t COL_SUN    = 0xFFE0;
constexpr uint16_t COL_CLOUD  = 0xC618;
constexpr uint16_t COL_RAIN   = 0x5D9F;
constexpr uint16_t COL_SNOW   = 0xE73F;
constexpr uint16_t COL_MOON   = 0xEF7D;
constexpr uint16_t COL_MOONDK = 0x39E7;

// Buttons / accents (WiFi setup).
constexpr uint16_t COL_BTN    = 0x2108;
constexpr uint16_t COL_ACCENT = 0x3949;

}  // namespace ui_theme
