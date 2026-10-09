#pragma once
#include <stdint.h>

// Horizontal page + vertical weather-card pager: drag tracking, swipe snapping
// and the smoothstep commit animation. Owns all gesture/animation state; the
// main loop only reads the resulting page/card/offset each frame.
//
// Draws nothing itself — it just reports state, so the rendering stays in the
// UI layer and the gesture machine can be reasoned about (and tested) alone.

void pagerInit();

// Feed one frame of input. Returns true when the visible page/card/offset
// changed (so the caller can redraw immediately instead of waiting for the
// next idle tick).
bool pagerUpdate();

int   pagerPage();
int   pagerCard();
float pagerOffset();
bool  pagerAnimating();

// True when the active gesture/animation is on the vertical (weather card) axis.
bool pagerVerticalAxis();

// A tap that only woke the screen must be swallowed so it doesn't also page.
void pagerSwallowNextTap();

// Tap/keyboard advance: next weather card, otherwise next page.
void pagerAdvance();

// Direct jumps (used by the FRAME_DUMP debug walker).
void pagerSetPage(int page);
void pagerSetCard(int card);
void pagerCancelAnim();
