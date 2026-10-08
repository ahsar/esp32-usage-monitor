#include "wifi_setup.h"
#include "hw/display.h"
#include "hw/input.h"
#include "hw/net.h"
#include <string.h>
#include <Arduino.h>

#define spr (*hwCanvas())

// All on-screen copy here is ASCII, so on the supersampled canvas use a 2×
// Helvetica face instead of the (upscaled) 12 px CJK font that only ever
// rendered Latin glyphs anyway.
#if BOARD_DISPLAY_UI_SCALE > 1
  #define WIFI_FONT u8g2_font_helvB24_tf
#else
  #define WIFI_FONT u8g2_font_wqy12_t_gb2312b
#endif

static const uint16_t COL_BG      = 0x0000;
static const uint16_t COL_TEXT    = 0xFFFF;
static const uint16_t COL_DIM     = 0x8410;
static const uint16_t COL_GREEN   = 0x07E0;
static const uint16_t COL_HOT     = 0xFA20;
static const uint16_t COL_BTN     = 0x2108;
static const uint16_t COL_ACCENT  = 0x3949;

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
static const int CW   = (SAFE_W - 4) / COLS;   // ~20
static const int CH   = 26;
static const int CX0  = SAFE_L + 2;
static const int GAP  = 2;

static Rect charRect(int col, int row, int rowOffsetY) {
  return { CX0 + col * CW, rowOffsetY + row * (CH + GAP), CW, CH };
}

static Rect groupBtnRect(int idx, int rowY) {
  int bw = (SAFE_W - 8) / 4;
  return { SAFE_L + idx * (bw + 2), rowY, bw, 18 };
}

static Rect btnRect(int x, int y, int w, int h) {
  return { x, y, w, h };
}

static void fillBtn(const Rect& r, uint16_t color, const char* label) {
  spr.fillRoundRect(r.x, r.y, r.w, r.h, 3, color);
  spr.setTextColor(COL_TEXT, color);
  int tw = strlen(label) * 6;
  spr.setCursor(r.x + (r.w - tw) / 2, r.y + (r.h - 10) / 2);
  spr.print(label);
}

// Grace period for the boot-time connect when credentials are saved. Only if
// this elapses without connecting do we fall back to scanning (the old code
// scanned after 2 s and aborted a perfectly good — just slow — connect).
static const uint32_t CONNECT_GRACE_MS = 15000;

// ── Init ────────────────────────────────────────────────────
void wifiSetupInit(WifiState& state, const char* savedSsid) {
  memset(&state, 0, sizeof(state));
  state.open = true;
  state.haveCreds = (savedSsid && savedSsid[0]);
  if (state.haveCreds) {
    strncpy(state.savedSsid, savedSsid, sizeof(state.savedSsid) - 1);
  }
  // With saved creds, wait for the connect instead of scanning immediately.
  state.step = state.haveCreds ? 4 : 0;
  state.connStart = millis();
  state.scanStart = millis();
  state.flashIdx = -1;
  state.flashTime = 0;
}

// ── Drawing ─────────────────────────────────────────────────
void wifiSetupDraw(WifiState& state) {
  spr.fillScreen(COL_BG);
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
    spr.print(state.savedSsid);
    spr.setTextColor(COL_DIM);
    spr.setCursor(SAFE_L, SAFE_B - 12);
    spr.print("Tap to choose another network");

    const HwTouch& tp = hwTouch();
    bool skip = (tp.justPressed && !s_touchUsed)
                || hwBtnA().wasReleased || hwBtnB().wasPressed;
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
    spr.setCursor(SAFE_L, y); spr.print("Select WiFi"); y += 14;

    if (state.netsCount == 0) {
      // Scan returned nothing or failed
      y += 20;
      spr.setTextColor(COL_DIM);
      spr.setCursor(SAFE_L, y);
      spr.print(state.scanFailed ? "Scan failed" : "No networks found");
      y += 16;
      spr.setCursor(SAFE_L, y); spr.print("Press any key to retry");

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
      spr.setCursor(SAFE_L + 4, y + 2);
      spr.printf("  %s", wifiNets[i].ssid);
      spr.setCursor(SAFE_R - 24, y + 2);
      spr.printf("%d", wifiNets[i].rssi);
      y += 16;
    }

    const HwTouch& tp = hwTouch();
    if (tp.justPressed && !s_touchUsed) {
      y = SAFE_T + 14;
      for (int i = 0; i < state.netsCount && y < SAFE_B - 10; i++) {
        Rect r = { SAFE_L, y, SAFE_W, 14 };
        if (hit(r, tp.x, tp.y)) {
          state.sel = i;
          state.step = 2;
          state.pass[0] = 0;
          state.charGroup = 0;
          state.charIdx = 0;
          if (wifiNets[i].encryption == 0) {  // open network
            state.step = 3;
            state.connDone = false;
          }
          s_touchUsed = 1;
          return;
        }
        y += 16;
      }
    }

    // Button fallback
    if (!s_touchUsed) {
      if (hwBtnA().wasReleased) state.sel = (state.sel + 1) % state.netsCount;
      if (hwBtnB().wasPressed) {
        state.step = 2;
        state.pass[0] = 0;
        state.charGroup = 0;
        state.charIdx = 0;
        if (wifiNets[state.sel].encryption == 0) {
          state.step = 3;
          state.connDone = false;
        }
      }
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
    spr.setCursor(SAFE_L, y + 3);
    spr.print("Password"); y += 16;

    // SSID name
    spr.setTextColor(COL_DIM);
    spr.setCursor(SAFE_L, y);
    spr.print(wifiNets[state.sel].ssid); y += 14;

    // Password display
    Rect passR = { SAFE_L, y, SAFE_W, 16 };
    spr.fillRoundRect(passR.x, passR.y, passR.w, passR.h, 3, 0x1082);
    spr.setTextColor(COL_TEXT);
    spr.setCursor(SAFE_L + 4, y + 3);
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
        int tw = 6;
        spr.setCursor(r.x + (r.w - tw) / 2, r.y + (r.h - 10) / 2);
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
      spr.setCursor(SAFE_L, y); spr.print(wifiNets[state.sel].ssid);
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

void wifiSetupHandleButtons(WifiState& state) {
  (void)state;
}
