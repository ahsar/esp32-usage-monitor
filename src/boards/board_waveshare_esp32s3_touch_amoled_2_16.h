// src/boards/board_waveshare_esp32s3_touch_amoled_2_16.h
#pragma once

// Display: 480×480 rounded-square AMOLED (CO5300). CO5300 panels can't
// tolerate per-row blits (QSPI state issue), so the LETTERBOX path is used:
// PSRAM-backed 368×448 canvas + one-shot draw16bitRGBBitmap.
// The UI is authored on a 184×224 logical grid but rasterised at 2×
// (BOARD_DISPLAY_UI_SCALE) so text/icons stay crisp on the 314-PPI panel;
// the 368×448 result is centred via (LCD-DEST)/2 → (56, 16) on 480×480.
#define LCD_W_PHYS  480
#define LCD_H_PHYS  480

#define BOARD_HW_W        184
#define BOARD_HW_H        224
// Rounded corners fall in the 56 px L/R black border; canvas content is
// fully inside the visible rectangle. Same SAFE_INSET as 1.8.
#define BOARD_SAFE_INSET  8

// QSPI to CO5300 (LCD_RESET / TP_RESET pins differ from 1.75c)
#define PIN_LCD_SDIO0  4
#define PIN_LCD_SDIO1  5
#define PIN_LCD_SDIO2  6
#define PIN_LCD_SDIO3  7
#define PIN_LCD_SCLK   38
#define PIN_LCD_CS     12
#define PIN_LCD_RESET  39   // direct GPIO, not the GPIO 1 of 1.75c
#define PIN_TP_RESET   40   // direct GPIO, not the GPIO 2 of 1.75c

// I2C bus (shared: AXP2101, ES8311, ES7210, CST92xx, QMI8658, PCF85063)
#define PIN_I2C_SDA   15
#define PIN_I2C_SCL   14

// FT3168/CST9217 family touch interrupt
#define PIN_TP_INT    11

// I2S to ES8311 codec — MCLK on GPIO42 (different from 1.75c's GPIO16)
#define PIN_I2S_MCLK  42
#define PIN_I2S_BCLK  9
#define PIN_I2S_WS    45
#define PIN_I2S_DI    10
#define PIN_I2S_DO    8
#define PIN_PA_CTRL   46

// Three physical keys
#define PIN_KEY1      16   // PWR silkscreen, middle. Active-HIGH via BSS138 inverter (PWRON gate).
#define PIN_KEY2      18   // IO18 silkscreen, left. Active-low.
#define PIN_KEY_BOOT  0    // BOOT silkscreen, right. Active-low. Synthesises BTN_A_LONG_PRESS.

// Capability flags
#define BOARD_HAS_PSRAM            1
#define BOARD_HAS_TCA9554          0
#define BOARD_HAS_PCF85063         1
#define BOARD_HAS_PA_CTRL          1
#define BOARD_HAS_AXP2101          1
#define BOARD_LCD_RST_VIA_PMU      0   // has its own GPIO reset
#define BOARD_AXP_PWRON_4S_OFF     1   // PWR key powers off via AXP after 4 s hold
#define BOARD_AXP_ENABLE_AUX_LDOS  0   // DSI_PWR_EN is on VCC3V3 (R17 pull-up), not ALDO2
#define BOARD_DISPLAY_CO5300       1
#define BOARD_CO5300_COL_OFFSET    0   // 480×480 panel uses full window; 1.75c sets 6 (round panel)
#define BOARD_DISPLAY_ROTATION     0
// MADCTL override: Arduino_CO5300's setRotation only does X/Y mirror, not
// row/column swap (MV bit). Setting this to non-zero writes 0x36 directly
// after canvas->begin(). 0x60 = MV+MX (90° CW); 0xA0 = MV+MY (90° CCW); 0 = no override.
#define BOARD_CO5300_MADCTL        0xA0   // MV+MY (90° CCW; opposite rotation of 0x60 / MV+MX)
#define BOARD_DISPLAY_LETTERBOX    1   // CO5300 needs one-shot blit; S3 has PSRAM for full frame buf
#define BOARD_DISPLAY_DEST_W       368   // 184 × 2
#define BOARD_DISPLAY_DEST_H       448   // 224 × 2
// Supersample the logical canvas 2× and blit 1:1. The UI is authored against a
// 184×224 logical grid (BOARD_HW_W/H); drawing it at 368×448 with ~2× fonts
// keeps glyphs/icons at native panel resolution instead of block-upscaling.
// Canvas framebuffer (368×448×2 = 322 KB) lives in PSRAM.
#define BOARD_DISPLAY_UI_SCALE     2
#define BOARD_DISPLAY_SH8601_VENDOR_INIT  0
#define BOARD_DISPLAY_OFFSET_X     0   // letterbox path uses its own (LCD-DEST)/2 centring math
#define BOARD_DISPLAY_OFFSET_Y     0
#define BOARD_DISPLAY_SCALE        1   // letterbox path uses DEST_W/H math; touch falls back to /1
#define BOARD_DISPLAY_PUSH_STREAMED 0
#define BOARD_TOUCH_CST92XX        1
// Touch orientation: CO5300 MADCTL=0xA0 rotates the panel 90° CCW, so the
// CST92xx raw X/Y must be swapped and only X mirrored to align with canvas.
// Without this, touches land in wrong cells (e.g. tapping a key does nothing).
#define BOARD_TOUCH_SWAP_XY        1
#define BOARD_TOUCH_MIRROR_X       1
#define BOARD_TOUCH_MIRROR_Y       0
#define BOARD_BTN_SWAP_AB          0   // scanning lands keys in correct slots; no swap
#define BOARD_BTN_THIRD            1
#define BOARD_KEY1_ACTIVE_HIGH     1
#define BOARD_HAS_KEY2             1

// Credits page
#define BOARD_MODEL_LINE1  "Waveshare ESP32-S3"
#define BOARD_MODEL_LINE2  "Touch AMOLED 2.16"
