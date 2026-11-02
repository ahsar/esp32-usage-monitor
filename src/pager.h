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

// Step by ±1 (wrapping), used by pagerAdvance(). Positive = next.
void pagerStep(int dir);

// Horizontal page step for the left/right side keys: moves the page axis only
// (even on the Weather page, unlike pagerStep). The axis is an endless
// carousel, so it wraps at the ends exactly like the horizontal swipe, and it
// mirrors the swipe's slide animation. Positive = content slides left / next
// page enters from the right.
void pagerPageStep(int dir);

// The agent screens' own axis. While they own the panel (main.cpp decides, and
// calls this every frame before pagerUpdate()), the horizontal axis steps the
// *session cards* instead of the pages: a tap or a flick reports one step
// through pagerTakeAgentStep() and the page underneath does not move. It used to
// page invisibly under the animation, which is the same class of bug as an
// unresponsive button — the gesture had no feedback at all.
void pagerSetAgentMode(bool on);
// The accumulated card steps since the last call (+1 = next card), or 0.
int  pagerTakeAgentStep();

// Direct jumps (used by the FRAME_DUMP debug walker).
void pagerSetPage(int page);
void pagerSetCard(int card);
void pagerCancelAnim();
