#pragma once
// The Clock page's charging animation: where the highlight sits inside the
// battery pill, and the one predicate that decides whether the page may animate
// at all.
//
// Pure and host-tested (test/test_util), no drawing dependency — the renderer
// turns a Band into a few pills, and main.cpp turns `active()` into the frame
// key's animation tick and into `uiSetAnimMs()`. Keeping the predicate here is
// deliberate: the frame key, the render gate and the card all have to agree, and
// when one of them disagrees the symptom is a frozen panel, not a build error.
#include <stdint.h>
#include "ui_model.h"

namespace charge_anim {

// The travelling highlight inside the filled part of the pill.
struct Band {
  int     x;      // left edge of the head, logical px
  int     w;      // head width, logical px; 0 => nothing to draw
  uint8_t alpha;  // 0..255, brightness at the head
};

// Where the band is at `elapsedMs`.
//
// `stripX`/`stripW` describe the strip the band travels (the whole battery track,
// so the shimmer runs the full bar like a phone's), and `capInset` its cap
// radius: the band only travels the straight section, so it can never be drawn
// across a rounded end and square it off.
//
// The brightness fades in at the left cap and out at the right one. That is what
// makes the wrap invisible — a band that reappeared at full brightness on the
// left edge would read as a glitch rather than as flow. And a strip shorter than
// one band plus two caps simply draws nothing, which is the guard for a smaller
// bar on another board rather than a state this one can reach.
inline Band sweep(uint32_t elapsedMs, int stripX, int stripW, uint32_t periodMs,
                  int bandW, int capInset) {
  Band b = { 0, 0, 0 };
  if (periodMs == 0 || bandW <= 0 || stripW <= 0 || capInset < 0) return b;

  const int lo = stripX + capInset;                   // head at the left cap
  const int hi = stripX + stripW - capInset - bandW;  // head at the right cap
  if (hi < lo) return b;

  uint32_t phase = elapsedMs % periodMs;
  float t = (float)phase / (float)periodMs;          // 0..1 over one cycle
  // Parabola instead of a sine: zero at both ends, exactly 1 in the middle, and
  // no libm in a header the host tests link.
  float fade = 4.0f * t * (1.0f - t);

  b.x     = lo + (int)(((int64_t)(hi - lo) * (int64_t)phase) / (int64_t)periodMs);
  b.w     = bandW;
  b.alpha = (uint8_t)(fade * 255.0f + 0.5f);
  return b;
}

// The gate: charging, the panel is awake, and the Clock page is what is showing.
// Anything else keeps the frame key free of the animation tick, so the other
// pages still cost zero renders and a dimmed panel freezes on the last frame
// instead of drawing at 12 fps in the dark.
//
// `page` is a UiPage value kept as an int: pagerPage() speaks int, and this
// header stays free of the pager.
inline bool active(bool charging, bool dimmed, int page) {
  return charging && !dimmed && page == (int)UI_PAGE_CLOCK;
}

}  // namespace charge_anim
