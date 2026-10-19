#include "wifi_setup.h"
#include "ui/theme.h"
#include "hw/display.h"
#include "hw/input.h"
#include "hw/net.h"
#include <string.h>
#include <Arduino.h>

// All on-screen copy here is ASCII. Text is rendered by LVGL (see
// ui/lvgl_text.h) like the rest of the UI, so it is 4bpp anti-aliased too.
#if BOARD_DISPLAY_UI_SCALE > 1
  #define WIFI_FONT (&lv_font_ui_24)
#else
  #define WIFI_FONT (&lv_font_ui_12)
#endif

using namespace ui_theme;

static const char* WIFI_CHARS[] = {
  "abcdefghijklmnopqrstuvwxyz",
  "ABCDEFGHIJKLMNOPQRSTUVWXYZ",
  "0123456789",
  " !@#$%^&*()_+-=[]{}|;:,.<>?/~",
};
static const char* WIFI_GROUP_NAMES[] = { "abc", "ABC", "123", "!@" };

static NetScanResult wifiNets[20];

struct Rect { int x, y, w, h; };
static bool hit(const Rect& r, int tx, int ty) {
  return tx >= r.x && tx < r.x + r.w && ty >= r.y && ty < r.y + r.h;
}

static int s_touchUsed = 0;

// ── Layout helpers ──────────────────────────────────────────
static const int COLS = 8;
static const int GAP  = 2;
// Cell width and column pitch are two different numbers, and the grid used one
// value for both: eight 20-px cells butted against each other, so a row read as a
// single rounded bar with notches down it — next to the vertical rows, which have
// always had GAP between them. Solve the width from the safe width and centre the
// block instead: 8 × 19 + 7 × 2 = 166 in 168, i.e. 1 px of margin either side
// (CX0 was `SAFE_L + 2`, which left 2 px on the left and 6 px on the right).
static const int CW   = (SAFE_W - (COLS - 1) * GAP - 2) / COLS;
static const int CP   = CW + GAP;              // column pitch, *not* the cell width
static const int CH   = 26;
static const int CX0  = SAFE_L + (SAFE_W - (COLS * CW + (COLS - 1) * GAP)) / 2;

static Rect charRect(int col, int row, int rowOffsetY) {
  return { CX0 + col * CP, rowOffsetY + row * (CH + GAP), CW, CH };
}

static Rect groupBtnRect(int idx, int rowY) {
  int bw = (SAFE_W - 8) / 4;
  return { SAFE_L + idx * (bw + 2), rowY, bw, 18 };
}

static Rect btnRect(int x, int y, int w, int h) {
  return { x, y, w, h };
}

// ScaledCanvas::setCursor() takes a **baseline** (hw/display.h), not the top of a
// text box — and this screen used to position every rect-relative label as if it
// were the latter (`r.y + (r.h - 10) / 2`), which put the text half a line above
// its button, list row or character cell. Centre one line box in `r` and return
// the baseline, from the font's own metrics rather than a magic number:
// lv_font_ui_24 is line_height 26 / base_line 5 at UI_SCALE 2, i.e. a 13-logical-px
// box whose baseline sits 10.5 px below its top (rounded up here so the truncating
// division lands on the true centre at the heights this screen uses: 14/18/20/26).
static int labelBaseline(const Rect& r) {
  const int boxH = WIFI_FONT->line_height / UI_SCALE;
  const int asc  = (WIFI_FONT->line_height - WIFI_FONT->base_line + UI_SCALE - 1) / UI_SCALE;
  return r.y + (r.h - boxH) / 2 + asc;
}

static void fillBtn(const Rect& r, uint16_t color, const char* label) {
  spr.fillRoundRect(r.x, r.y, r.w, r.h, 3, color);
  spr.setFont(WIFI_FONT);
  spr.setTextColor(COL_TEXT, color);
  int tw = lvglTextWidth(WIFI_FONT, label) / UI_SCALE;
  spr.setCursor(r.x + (r.w - tw) / 2, labelBaseline(r));
  spr.print(label);
}

// Grace period for the boot-time connect when credentials are saved. Only if
// this elapses without connecting do we fall back to scanning (the old code
// scanned after 2 s and aborted a perfectly good — just slow — connect).
static const uint32_t CONNECT_GRACE_MS = 30000;

// Baseline for a title sitting on the safe-area top edge. setCursor() takes a
// baseline and the tallest glyph reaches 21 physical px (10.5 logical) above it,
// so baselining the text at SAFE_T draws the top of l/f/t 5 physical px above the
// canvas: "Select WiFi" lost the tops of l, f and t that way.
static const int TITLE_Y = SAFE_T + 3;

// ── Init ────────────────────────────────────────────────────
void wifiSetupInit(WifiState& state, const char* savedSsid, const char* savedPass) {
  memset(&state, 0, sizeof(state));
  state.open = true;
  state.haveCreds = (savedSsid && savedSsid[0]);
  if (state.haveCreds) {
    strncpy(state.savedSsid, savedSsid, sizeof(state.savedSsid) - 1);
    if (savedPass) strncpy(state.savedPass, savedPass, sizeof(state.savedPass) - 1);
  }
  // With saved creds, wait for the connect instead of scanning immediately.
  state.step = state.haveCreds ? 4 : 0;
  state.connStart = millis();
  state.scanStart = millis();
  state.flashIdx = -1;
  state.flashTime = 0;
}

// The way out of the setup screen when a real keyboard is closer than the grid:
// the credentials channel (docs/pc-bridge.md), driven from a PC. Printed on the
// screens that are mostly empty — the password page itself has no room left —
// and kept to two short lines because ~28 characters is all that fits at 12 px.
//
// The step is 14 logical px, like every other stacked line here: the line box is
// 13 (line_height 26 at UI_SCALE 2) and setCursor() positions its *baseline*, so
// 12 left the two rows' ink 2 physical px into each other.
static void drawPcHint(int y) {
  spr.setFont(WIFI_FONT);
  spr.setTextColor(COL_DIM);
  spr.setCursor(SAFE_L, y);
  spr.print("Set from a PC instead:");
  spr.setCursor(SAFE_L, y + 14);
  spr.print("tools/wifi-setup.sh");
}

// A network the device already has a passphrase for: fill the password page in
// so "reconnect" is one tap on Connect. Still editable, and still shown masked.
static void prefillSavedPass(WifiState& state) {
  if (!state.haveCreds) return;
  if (strcmp(wifiNets[state.sel].ssid, state.savedSsid) != 0) return;
  strncpy(state.pass, state.savedPass, sizeof(state.pass) - 1);
  state.pass[sizeof(state.pass) - 1] = 0;
}

// SSIDs are up to 32 bytes and every slot on this screen is fixed, so draw what
// fits and mark the rest with "..": the alternative is the rasteriser running
// past the slot — into the rssi column in the list, or off the panel elsewhere.
static void printClipped(const char* s, int maxW) {
  if (lvglTextWidth(WIFI_FONT, s) / UI_SCALE <= maxW) { spr.print(s); return; }
  char buf[80];
  size_t n = strlen(s);
  if (n >= sizeof(buf) - 3) n = sizeof(buf) - 3;
  memcpy(buf, s, n + 1);
  while (n > 0) {
    buf[n] = 0;
    char probe[84];
    snprintf(probe, sizeof(probe), "%s..", buf);
    if (lvglTextWidth(WIFI_FONT, probe) / UI_SCALE <= maxW) break;
    n--;
  }
  spr.print(buf);
  spr.print("..");
}

// Move to the password page for network `sel`. Shared by the tap and key paths
// (they used to duplicate the reset, and the reset is where the prefill goes).
static void beginPassEntry(WifiState& state, int sel) {
  state.sel = sel;
  state.step = 2;
  state.pass[0] = 0;
  state.charGroup = 0;
  state.charIdx = 0;
  prefillSavedPass(state);
  if (wifiNets[sel].encryption == 0) {   // open network: nothing to type
    state.step = 3;
    state.connDone = false;
  }
}

// ── Drawing ─────────────────────────────────────────────────
void wifiSetupDraw(WifiState& state) {
  spr.fillScreen(COL_BG);
  // Every line here is placed by hand (a wrapped one would land on the line
  // below: see lvglDrawText), and the SSIDs come from a scan, so their length is
  // not ours to choose. Clip instead of wrap; uiBeginFrame() clears this.
  spr.setNoWrap(true);
  s_touchUsed = 0;

  if (state.step == 4) {
    // Booting with saved credentials. Give the connect a real chance before
    // scanning; the main loop closes this screen once WL_CONNECTED. A tap/key
    // (or the grace expiring) skips ahead to a scan.
    spr.setFont(WIFI_FONT);
    spr.setTextColor(COL_TEXT);
    spr.setCursor(SAFE_L, 60);
    spr.print("Connecting...");
    spr.setTextColor(COL_DIM);
    spr.setCursor(SAFE_L, 78);
    printClipped(state.savedSsid, SAFE_W);
    spr.setTextColor(COL_DIM);
    spr.setCursor(SAFE_L, SAFE_B - 42);
    spr.print("Tap for another network");
    drawPcHint(SAFE_B - 26);

    const HwTouch& tp = hwTouch();
    // Key1 is the PWR key on the 2.16 boards, so its release is not a user
    // gesture here: letting go of the power button must not abort the boot
    // connect (a scan calls esp_wifi_disconnect() and kills WiFi.begin()).
    // Skip with a tap or Key2 only.
    bool skip = (tp.justPressed && !s_touchUsed) || hwBtnB().wasPressed;
    if (skip || millis() - state.connStart >= CONNECT_GRACE_MS) {
      state.step = 0;
      state.scanStart = millis();
      s_touchUsed = 1;
    }
    return;
  }

  if (state.step == 0) {
    // Scanning
    spr.setFont(WIFI_FONT);
    spr.setTextColor(COL_TEXT);
    spr.setCursor(SAFE_L, 60);
    spr.print("Scanning...");
    drawPcHint(SAFE_B - 26);

    uint32_t e = (millis() - state.scanStart) / 1000;
    if (e >= 2 && state.netsCount == 0) {
      int r = netScan(wifiNets, 20);
      state.scanFailed = (r < 0);
      state.netsCount = (r < 0) ? 0 : r;
      char saved[33] = "";
      char dummy[64];
      netLoadCred(saved, 33, dummy, 64);
      int si = -1;
      for (int i = 0; i < state.netsCount; i++) {
        if (strcmp(wifiNets[i].ssid, saved) == 0) { si = i; break; }
      }
      state.step = 1;
      state.sel = (si >= 0) ? si : 0;
    }
    return;
  }

  if (state.step == 1) {
    spr.setFont(WIFI_FONT);
    int y = SAFE_T;
    spr.setTextColor(COL_TEXT);
    spr.setCursor(SAFE_L, TITLE_Y); spr.print("Select WiFi"); y += 14;

    if (state.netsCount == 0) {
      // Scan returned nothing or failed
      y += 20;
      spr.setTextColor(COL_DIM);
      spr.setCursor(SAFE_L, y);
      spr.print(state.scanFailed ? "Scan failed" : "No networks found");
      y += 16;
      spr.setCursor(SAFE_L, y); spr.print("Press any key to retry");
      drawPcHint(y + 14);

      const HwTouch& tp = hwTouch();
      if (tp.justPressed || hwBtnA().wasReleased || hwBtnB().wasPressed) {
        state.step = 0;
        state.scanStart = millis();
      }
      return;
    }

    // Network list
    for (int i = 0; i < state.netsCount && y < SAFE_B - 10; i++) {
      Rect r = { SAFE_L, y, SAFE_W, 14 };
      uint16_t c = (i == state.sel) ? COL_ACCENT : COL_BG;
      spr.fillRoundRect(r.x, r.y, r.w, r.h, 3, c);
      spr.setTextColor(COL_TEXT);
      char label[40];
      snprintf(label, sizeof(label), "  %s", wifiNets[i].ssid);
      spr.setCursor(SAFE_L + 4, labelBaseline(r));
      printClipped(label, (SAFE_R - 24) - (SAFE_L + 4) - 4);   // stop short of the rssi
      spr.setCursor(SAFE_R - 24, labelBaseline(r));
      spr.printf("%d", wifiNets[i].rssi);
      y += 16;
    }

    const HwTouch& tp = hwTouch();
    if (tp.justPressed && !s_touchUsed) {
      y = SAFE_T + 14;
      for (int i = 0; i < state.netsCount && y < SAFE_B - 10; i++) {
        Rect r = { SAFE_L, y, SAFE_W, 14 };
        if (hit(r, tp.x, tp.y)) {
          beginPassEntry(state, i);
          s_touchUsed = 1;
          return;
        }
        y += 16;
      }
    }

    // Button fallback
    if (!s_touchUsed) {
      if (hwBtnA().wasReleased) state.sel = (state.sel + 1) % state.netsCount;
      if (hwBtnB().wasPressed) beginPassEntry(state, state.sel);
    }
    return;
  }

  if (state.step == 2) {
    // Password input with touch keyboard
    spr.setFont(WIFI_FONT);
    int y = SAFE_T;

    // Back button (top-right)
    Rect backR = { SAFE_R - 32, y, 32, 14 };
    fillBtn(backR, COL_BTN, "Back");

    // Title
    spr.setTextColor(COL_TEXT);
    spr.setCursor(SAFE_L, TITLE_Y);
    spr.print("Password"); y += 16;

    // SSID name
    spr.setTextColor(COL_DIM);
    spr.setCursor(SAFE_L, y);
    printClipped(wifiNets[state.sel].ssid, SAFE_W); y += 14;

    // Password display
    Rect passR = { SAFE_L, y, SAFE_W, 16 };
    spr.fillRoundRect(passR.x, passR.y, passR.w, passR.h, 3, 0x1082);
    spr.setTextColor(COL_TEXT);
    // Inside the box, not above it: the cursor is a baseline, so `y + 3` (which
    // reads as "3 px below the top" for a text top) put the whole line box above
    // the field and left the box empty.
    spr.setCursor(SAFE_L + 4, labelBaseline(passR));
    int pl = strlen(state.pass);
    for (int i = 0; i < pl; i++) spr.print('*');
    spr.print(pl < 63 ? '_' : ' '); y += 18;

    // Character grid (4 rows × 8 cols — 32 slots, enough for the largest
    // group (29 symbols); fewer rows silently dropped y/z and 5 symbols)
    const char* g = WIFI_CHARS[state.charGroup];
    int gl = (int)strlen(g);
    int charRowY = y;
    const int MAX_ROWS = 4;
    bool flashing = (state.flashIdx >= 0 && (millis() - state.flashTime) < 200);
    int idx = 0;
    for (int row = 0; row < MAX_ROWS; row++) {
      for (int col = 0; col < COLS && idx < gl; col++, idx++) {
        Rect r = charRect(col, row, charRowY);
        // Visual feedback: flash the last-touched cell briefly with accent
        // colour so the user sees the keypress registered.
        uint16_t bg = (flashing && idx == state.flashIdx) ? COL_ACCENT : COL_BTN;
        spr.fillRoundRect(r.x, r.y, r.w, r.h, 3, bg);
        spr.setTextColor(COL_TEXT, bg);
        // The cell's own glyph width, not an assumed 6 logical px: M/W, digits and
        // symbols are wider and were visibly left of centre.
        char ch[2] = { g[idx], 0 };
        int tw = lvglTextWidth(WIFI_FONT, ch) / UI_SCALE;
        spr.setCursor(r.x + (r.w - tw) / 2, labelBaseline(r));
        spr.print(g[idx]);
        // Button-navigation cursor: green outline on the cell BtnB will add.
        if (idx == state.charIdx) {
          spr.drawRoundRect(r.x, r.y, r.w, r.h, 3, COL_GREEN);
        }
      }
    }
    y = charRowY + MAX_ROWS * (CH + GAP) + 4;

    // Group tabs — own full-width row. Capture the Y they're drawn at so the
    // touch hit-test below uses the same Y (previously it used the
    // already-incremented y, landing on the DEL/Connect row instead).
    int groupTabY = y;
    for (int i = 0; i < 4; i++) {
      Rect r = groupBtnRect(i, groupTabY);
      uint16_t bc = (i == state.charGroup) ? COL_ACCENT : COL_BTN;
      fillBtn(r, bc, WIFI_GROUP_NAMES[i]);
    }
    y += 18 + 3;

    // DEL + Connect — separate row, non-overlapping (previously these two
    // rects overlapped and Connect visually painted over DEL)
    Rect delR  = { SAFE_L, y, 50, 20 };
    Rect connR = { SAFE_L + 54, y, SAFE_R - (SAFE_L + 54), 20 };
    fillBtn(delR, COL_BTN, "DEL");
    fillBtn(connR, COL_GREEN, "Connect");

    // Touch handling for step 2
    const HwTouch& tp = hwTouch();
    if (tp.justPressed && !s_touchUsed) {
      // Back button
      if (hit(backR, tp.x, tp.y)) {
        state.step = 1;
        s_touchUsed = 1;
        return;
      }
      // Character grid
      idx = 0;
      for (int row = 0; row < MAX_ROWS; row++) {
        for (int col = 0; col < COLS && idx < gl; col++, idx++) {
          Rect r = charRect(col, row, charRowY);
          if (hit(r, tp.x, tp.y) && pl < 63) {
            state.pass[pl] = g[idx];
            state.pass[pl + 1] = 0;
            state.flashIdx = idx;
            state.flashTime = millis();
            state.charIdx = idx;  // keep cursor on the last-touched cell
            s_touchUsed = 1;
            return;
          }
        }
      }
      // Group buttons (use groupTabY — the Y they were actually drawn at)
      for (int i = 0; i < 4; i++) {
        Rect r = groupBtnRect(i, groupTabY);
        if (hit(r, tp.x, tp.y)) {
          state.charGroup = i;
          state.charIdx = 0;
          s_touchUsed = 1;
          return;
        }
      }
      // DEL
      if (hit(delR, tp.x, tp.y)) {
        int l = strlen(state.pass);
        if (l > 0) state.pass[l - 1] = 0;
        s_touchUsed = 1;
        return;
      }
      // Connect
      if (hit(connR, tp.x, tp.y)) {
        state.step = 3;
        state.connDone = false;
        s_touchUsed = 1;
        return;
      }
    }

    // Button fallback for step 2
    if (!s_touchUsed) {
      if (hwBtnA().pressedFor(600)) {
        state.charGroup = (state.charGroup + 1) % 4;
        state.charIdx = 0;
      } else if (hwBtnA().wasReleased) {
        state.charIdx = (state.charIdx + 1) % gl;
      }
      if (hwBtnB().pressedFor(800)) {
        state.step = 3;
        state.connDone = false;
        state.charIdx = 0;
        state.charGroup = 0;
      } else if (hwBtnB().wasPressed) {
        if (pl < 63) {
          state.pass[pl] = g[state.charIdx % gl];
          state.pass[pl + 1] = 0;
          state.flashIdx = state.charIdx % gl;
          state.flashTime = millis();
        }
      }
    }
    return;
  }

  if (state.step == 3) {
    // Connecting / result
    spr.setFont(WIFI_FONT);
    int y = 60;

    if (!state.connDone) {
      state.connDone = true;
      spr.setTextColor(COL_TEXT);
      spr.setCursor(SAFE_L, y); spr.print("Connecting..."); y += 14;
      spr.setTextColor(COL_DIM);
      spr.setCursor(SAFE_L, y); printClipped(wifiNets[state.sel].ssid, SAFE_W);
      hwDisplayPush();
      state.connOk = netConnect(wifiNets[state.sel].ssid, state.pass);
    }

    if (state.connOk) {
      spr.setTextColor(COL_GREEN);
      spr.setCursor(SAFE_L, y); spr.print("Connected!"); y += 14;
      spr.setTextColor(COL_TEXT);
      spr.setCursor(SAFE_L, y);
      spr.print("IP: "); spr.print(netIP().toString().c_str()); y += 14;
    } else {
      spr.setTextColor(COL_HOT);
      spr.setCursor(SAFE_L, y); spr.print("Failed"); y += 14;
      spr.setTextColor(COL_DIM);
      spr.setCursor(SAFE_L, y); spr.print("Could not connect");
      y += 18;
    }

    Rect exitR = { SAFE_L + SAFE_W / 4, y, SAFE_W / 2, 20 };
    fillBtn(exitR, COL_ACCENT, "Continue");

    const HwTouch& tp = hwTouch();
    if (tp.justPressed && !s_touchUsed) {
      if (hit(exitR, tp.x, tp.y)) {
        state.open = false;
        s_touchUsed = 1;
      }
    }
    // Button fallback
    if (!s_touchUsed) {
      if ((hwBtnA().wasReleased || hwBtnB().wasPressed) && state.connDone) {
        state.open = false;
      }
    }
    return;
  }
}

#ifdef FRAME_DUMP
// Debug-only: the list and password pages index `wifiNets`, which only a real
// scan fills, so a dump of those screens needs something to draw. The first name
// matches the SSID frame_dump.cpp inits the state with, so the password page
// shows the prefill path as well.
void wifiSetupDebugSeed(int count) {
  static const char* names[] = {
    "HomeNet-5G", "eduroam", "AndroidAP_9f2c",
    "ChinaNet-k23x", "iPhone", "A-very-long-network-name-here",
  };
  if (count > 20) count = 20;
  for (int i = 0; i < count; i++) {
    strncpy(wifiNets[i].ssid, names[i % 6], sizeof(wifiNets[i].ssid) - 1);
    wifiNets[i].ssid[sizeof(wifiNets[i].ssid) - 1] = 0;
    wifiNets[i].rssi = -40 - i * 7;
    wifiNets[i].encryption = 3;
  }
}
#endif
