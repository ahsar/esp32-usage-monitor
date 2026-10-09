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
    case UI_PAGE_CLOCK: uiDrawClock(ox, oy); break;
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
    if (off < 0 && card + 1 < UI_WC_COUNT)
      uiDrawWeatherCard(card + 1, *d.weather, *d.moon, *d.air, d.hasAir, 0, off + PAGE_H);
    else if (off > 0 && card - 1 >= 0)
      uiDrawWeatherCard(card - 1, *d.weather, *d.moon, *d.air, d.hasAir, 0, off - PAGE_H);
  } else {
    drawPage(page, card, off, 0, d);
    if (off < 0 && page + 1 < UI_PAGE_COUNT) drawPage(page + 1, card, off + PAGE_W, 0, d);
    else if (off > 0 && page - 1 >= 0) drawPage(page - 1, card, off - PAGE_W, 0, d);
  }

  uiDrawPageDots(page);
  if (page == UI_PAGE_WEATHER) uiDrawCardDots(card, UI_WC_COUNT);
}
