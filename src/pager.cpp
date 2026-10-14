#include "pager.h"
#include "ui_model.h"
#include "hw/input.h"
#include "hw/display.h"
#include <Arduino.h>
#include <stdlib.h>

static const int PAGE_W = HW_W;
static const int PAGE_H = HW_H;

static int  s_page = UI_PAGE_WEATHER;
static int  s_card = UI_WC_NOW;

static bool s_touchActive = false;
static int  s_tx0 = 0, s_ty0 = 0;
static int  s_dragAxis = 0;          // 0 none, 1 horizontal, 2 vertical
static float s_off = 0.0f;           // current drag/animation offset in px
static uint32_t s_touchDownMs = 0;

static bool s_animating = false;
static bool s_animAxisV = false;
static int  s_animDir = 0;           // +1 next, -1 prev, 0 snap back
static float s_animFrom = 0.0f, s_animTo = 0.0f;
static uint32_t s_animStart = 0;
static const uint32_t ANIM_MS = 170;

static bool s_swallowTouch = false;

void pagerInit() {
  s_page = UI_PAGE_WEATHER;
  s_card = UI_WC_NOW;
  s_off = 0.0f;
  s_animating = false;
  s_touchActive = false;
  s_swallowTouch = false;
  s_dragAxis = 0;
}

int   pagerPage()      { return s_page; }
int   pagerCard()      { return s_card; }
float pagerOffset()    { return s_off; }
bool  pagerAnimating() { return s_animating; }

bool pagerVerticalAxis() {
  return s_animating ? s_animAxisV : (s_dragAxis == 2);
}

void pagerSwallowNextTap() { s_swallowTouch = true; }

void pagerSetPage(int page) { s_page = page; }
void pagerSetCard(int card) { s_card = card; }
void pagerCancelAnim() { s_animating = false; s_off = 0.0f; }

static void startAnim(bool vertical, float from, float to, int dir) {
  s_animating = true;
  s_animAxisV = vertical;
  s_animDir = dir;
  s_animFrom = from;
  s_animTo = to;
  s_animStart = millis();
}

static void commitAnim() {
  if (s_animDir > 0) {
    if (s_animAxisV) { if (s_card + 1 < UI_WC_COUNT) s_card++; }
    else             { if (s_page + 1 < UI_PAGE_COUNT) s_page++; }
  } else if (s_animDir < 0) {
    if (s_animAxisV) { if (s_card > 0) s_card--; }
    else             { if (s_page > 0) s_page--; }
  }
  s_off = 0;
  s_animating = false;
  s_animDir = 0;
}

void pagerAdvance() {
  pagerStep(1);
}

void pagerStep(int dir) {
  if (dir == 0) return;
  if (s_page == UI_PAGE_WEATHER) {
    s_card = ((s_card + dir) % UI_WC_COUNT + UI_WC_COUNT) % UI_WC_COUNT;
  } else {
    s_page = ((s_page + dir) % UI_PAGE_COUNT + UI_PAGE_COUNT) % UI_PAGE_COUNT;
  }
}

bool pagerUpdate() {
  int  pg0   = s_page;
  int  wc0   = s_card;
  float off0 = s_off;
  bool anim0 = s_animating;

  const HwTouch& tp = hwTouch();
  uint32_t now = millis();

  // A tap that woke the screen must not also page — but a *drag* that woke it
  // should still scroll, so the swallow only survives while the gesture is
  // still an undecided tap (cleared as soon as an axis locks below).
  if (s_swallowTouch && !tp.down) s_swallowTouch = false;

  if (tp.justPressed) {
    // A new touch landing on an in-flight commit used to silently cancel it
    // (the pending card/page change was dropped). Finish it first instead.
    if (s_animating) commitAnim();
    s_touchActive = true;
    s_tx0 = tp.x; s_ty0 = tp.y;
    s_dragAxis = 0;
    s_touchDownMs = now;
    s_off = 0;
  }

  if (s_touchActive && tp.down) {
    int dx = tp.x - s_tx0;
    int dy = tp.y - s_ty0;
    if (s_dragAxis == 0 && (abs(dx) > 8 || abs(dy) > 8))
      s_dragAxis = (abs(dy) > abs(dx)) ? 2 : 1;
    if (s_dragAxis != 0) s_swallowTouch = false;   // real drag → not a wake tap

    if (s_dragAxis == 1) {
      s_off = dx;
      bool canPrev = s_page > 0, canNext = s_page + 1 < UI_PAGE_COUNT;
      if ((s_off > 0 && !canPrev) || (s_off < 0 && !canNext)) s_off /= 3.0f;
    } else if (s_dragAxis == 2 && s_page == UI_PAGE_WEATHER) {
      s_off = dy;
      bool canPrev = s_card > 0, canNext = s_card + 1 < UI_WC_COUNT;
      if ((s_off > 0 && !canPrev) || (s_off < 0 && !canNext)) s_off /= 3.0f;
    } else {
      s_off = 0;
    }
  }

  if (s_touchActive && tp.justReleased) {
    s_touchActive = false;
    bool swallowTap = s_swallowTouch;   // still set → it was a wake tap
    s_swallowTouch = false;
    bool vertical = (s_dragAxis == 2) && (s_page == UI_PAGE_WEATHER);
    int swipeThresh = 38;
    int dir = 0;

    if (s_dragAxis == 0 || (abs((int)s_off) < 8 && now - s_touchDownMs < 350)) {
      if (!swallowTap) pagerAdvance();   // wake tap: don't also page
      s_off = 0;
    } else if (s_dragAxis == 2 && !vertical) {
      s_off = 0;                     // vertical on non-weather pages: nothing
    } else if (s_off <= -swipeThresh) {
      dir = 1;
    } else if (s_off >= swipeThresh) {
      dir = -1;
    }

    if (dir != 0) {
      bool can = vertical ? (dir > 0 ? s_card + 1 < UI_WC_COUNT : s_card > 0)
                          : (dir > 0 ? s_page + 1 < UI_PAGE_COUNT : s_page > 0);
      if (!can) dir = 0;
    }

    float size = vertical ? PAGE_H : PAGE_W;
    float target = (dir > 0) ? -size : (dir < 0 ? size : 0.0f);
    startAnim(vertical, s_off, target, dir);
    s_dragAxis = 0;
  }

  if (s_animating) {
    float t = (float)(now - s_animStart) / (float)ANIM_MS;
    if (t >= 1.0f) {
      commitAnim();
    } else {
      if (t < 0) t = 0;
      t = t * t * (3.0f - 2.0f * t);     // smoothstep
      s_off = s_animFrom + (s_animTo - s_animFrom) * t;
    }
  }

  return (s_page != pg0) || (s_card != wc0) ||
         (s_off != off0) || (s_animating != anim0);
}
