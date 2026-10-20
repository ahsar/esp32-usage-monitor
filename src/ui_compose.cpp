#include "ui_compose.h"
#include "usage_display.h"
#include "pager.h"
#include "hw/display.h"
#include <math.h>

static const int PAGE_W = HW_W;
static const int PAGE_H = HW_H;

static void drawPage(int page, int card, int ox, int oy, const UiData& d) {
  switch (page) {
    case UI_PAGE_USAGE: uiDrawUsage(*d.usage, ox, oy); break;
    case UI_PAGE_TREND: uiDrawTrend(*d.usage, ox, oy); break;
    case UI_PAGE_CLOCK: uiDrawClock(ox, oy); break;
    case UI_PAGE_HEALTH: uiDrawHealth(*d.health, ox, oy); break;
    default:
      uiDrawWeatherCard(card, *d.weather, *d.moon, *d.air, d.hasAir, ox, oy);
      break;
  }
}

void uiRender(const UiData& d) {
  uiBeginFrame();

  bool vaxis = pagerVerticalAxis();
  int off = (int)lroundf(pagerOffset());
  int page = pagerPage();
  int card = pagerCard();

  if (vaxis && page == UI_PAGE_WEATHER) {
    uiDrawWeatherCard(card, *d.weather, *d.moon, *d.air, d.hasAir, 0, off);
    // Endless carousel: the neighbour in the gap wraps at the ends, so the tile
    // is always drawn (no bounds test) — it is what makes Clock↔Weather and
    // Moonrise↔Now feel continuous mid-drag.
    if (off < 0)
      uiDrawWeatherCard(uiCardNext(card), *d.weather, *d.moon, *d.air, d.hasAir, 0, off + PAGE_H);
    else if (off > 0)
      uiDrawWeatherCard(uiCardPrev(card), *d.weather, *d.moon, *d.air, d.hasAir, 0, off - PAGE_H);
  } else {
    drawPage(page, card, off, 0, d);
    if (off < 0)      drawPage(uiPageNext(page), card, off + PAGE_W, 0, d);
    else if (off > 0) drawPage(uiPagePrev(page), card, off - PAGE_W, 0, d);
  }

  uiDrawPageDots(page);
  if (page == UI_PAGE_WEATHER) uiDrawCardDots(card, UI_WC_COUNT);
}
