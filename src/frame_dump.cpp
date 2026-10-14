#ifdef FRAME_DUMP
#include "frame_dump.h"
#include "ui_model.h"
#include "pager.h"
#include "ui_compose.h"
#include "hw/display.h"
#include <Arduino.h>

static uint32_t s_dumpIdx = 0;
static uint32_t s_dumpNext = 0;

void frameDumpTick(uint32_t now, const UiData& d) {
  if (s_dumpIdx >= 7) return;
  if (now < s_dumpNext) return;
  // Wait for real data (or give up after 60s) so the cards aren't placeholders.
  bool ready = d.usage->valid && d.weather->valid && (!d.hasAir || d.air->valid);
  if (s_dumpIdx == 0 && !ready && now < 60000) return;
  s_dumpNext = now + 1500;

  uint32_t idx = s_dumpIdx;
  if (idx < UI_PAGE_COUNT) {
    pagerSetPage((int)idx);
  } else {
    pagerSetPage(UI_PAGE_WEATHER);
    pagerSetCard((int)(idx - UI_PAGE_COUNT + 1));
  }
  pagerCancelAnim();

  uiRender(d);
  hwDisplayPush();

  uint16_t* fb = (uint16_t*)hwCanvas()->getFramebuffer();
  Serial.printf("FRAME %u %d %d\n", idx, HW_CANVAS_W, HW_CANVAS_H);
  delay(30);
  Serial.write((uint8_t*)fb, HW_CANVAS_W * HW_CANVAS_H * 2);
  Serial.write('\n');
  Serial.flush();
  s_dumpIdx++;
}
#endif
