// src/boards/board_waveshare_esp32s3_touch_amoled_1_75c.h
#pragma once

// Display: 466×466 round AMOLED (CO5300). Corners outside the visible circle.
#define LCD_W_PHYS  466
#define LCD_H_PHYS  466

// Logical canvas matches 1.8" exactly (184×224). hwDisplayPush letterboxes
// this canvas 1:1 into the centre of the 466×466 physical frame and leaves
// black around it. 184×224 has diagonal ≈290 and fits inside the 466 circle
// with comfortable margin, so the entire canvas is visible — no round-edge
// clipping. main.cpp stays board-agnostic; no per-board layout offsets.
#define BOARD_HW_W        184
#define BOARD_HW_H        224

// Same inset as 1.8" — the 184×224 rectangle is fully inside the circle,
// so SAFE_INSET's purpose here is just visual padding from canvas edges.
#define BOARD_SAFE_INSET  8

// QSPI to SH8601
#define PIN_LCD_SDIO0  4
#define PIN_LCD_SDIO1  5
#define PIN_LCD_SDIO2  6
#define PIN_LCD_SDIO3  7
#define PIN_LCD_SCLK   38
#define PIN_LCD_CS     12

// LCD and TP reset lines are independent, per schematic GPIO table.
// (Waveshare's own pin_config.h incorrectly ties both to GPIO2 — ignore it;
// the schematic is ground truth, and that's what hwExpanderResetSequence drives.)
#define PIN_LCD_RESET  1
#define PIN_TP_RESET   2

// LCD tearing-effect output (not used yet; reserved)
#define PIN_LCD_TE     13

// I2C bus (shared: AXP2101, FT3168, ES8311, QMI8658C)
#define PIN_I2C_SDA   15
#define PIN_I2C_SCL   14

// FT3168 touch interrupt — direct to ESP32
#define PIN_TP_INT    11

// I2S to ES8311 codec (identical to 1.8")
#define PIN_I2S_MCLK  16
#define PIN_I2S_BCLK  9
#define PIN_I2S_WS    45
#define PIN_I2S_DI    10
#define PIN_I2S_DO    8
#define PIN_PA_CTRL   46

// Buttons
#define PIN_KEY1      0   // GPIO0 BOOT key, active-low

// No TCA9554 expander on this board
#define BOARD_HAS_TCA9554  0

// Display: Arduino_CO5300 (1.75C FPC uses CO5300, not SH8601 as 1.8")
#define BOARD_DISPLAY_CO5300  1

// Letterbox with nearest-neighbour scale: draw HW_W×HW_H canvas as
// DEST_W×DEST_H pixels centred in the LCD_W_PHYS×LCD_H_PHYS panel, rest black.
// 1.5× chosen so (276,336) has diagonal ≈435 — comfortably inside the 466 circle
// (2× would be 368×448 diag 580, too big). Source:dest 2:3 pattern.
#define BOARD_DISPLAY_LETTERBOX  1
#define BOARD_DISPLAY_DEST_W     276
#define BOARD_DISPLAY_DEST_H     336

// Touch: CST92xx @ 0x5A via SensorLib (not FT3168/DriveBus as 1.8")
#define BOARD_TOUCH_CST92XX  1
// Touch orientation: no display rotation on 1.75C, so preserve original
// mirrorXY(true,true) that was hardcoded before this was parametrised.
#define BOARD_TOUCH_SWAP_XY   0
#define BOARD_TOUCH_MIRROR_X  1
#define BOARD_TOUCH_MIRROR_Y  1

// Physical PWR / BOOT buttons are on opposite sides vs the 1.8" — swap A/B
// at the accessor so main.cpp's button semantics match user expectation.
#define BOARD_BTN_SWAP_AB  1

// No external RTC on the 1.75C — AXP2101 has an internal RTC which we don't
// currently drive (hwRtc* stubs return zero; info page just shows uptime).
#define BOARD_HAS_PCF85063  0

// Credits-page hardware identification (two short lines).
#define BOARD_MODEL_LINE1  "Waveshare ESP32-S3"
#define BOARD_MODEL_LINE2  "Touch AMOLED 1.75C"

#define BOARD_DISPLAY_SH8601_VENDOR_INIT  0
// AXP_IRQ is not wired to any ESP32 GPIO on the 1.75C; AXP PEK events
// are detected by polling AXP registers via I2C every frame instead.

// New flags introduced by 2.16 port. Defaults preserve current 1.75C behavior.
#define BOARD_HAS_PSRAM            1
#define BOARD_DISPLAY_OFFSET_X     0
#define BOARD_DISPLAY_OFFSET_Y     0
#define BOARD_DISPLAY_SCALE        1
#define BOARD_HAS_PA_CTRL          1
#define BOARD_HAS_AXP2101          1
#define BOARD_LCD_RST_VIA_PMU      0
#define BOARD_AXP_PWRON_4S_OFF     0
#define BOARD_BTN_THIRD            0
#define BOARD_KEY1_ACTIVE_HIGH     0
#define BOARD_HAS_KEY2             0
#define BOARD_AXP_ENABLE_AUX_LDOS  0
#define BOARD_DISPLAY_PUSH_STREAMED  0   // letterbox path wins; streamed path never reached
#define BOARD_CO5300_COL_OFFSET    6   // round panel RAM window starts at column 6
#define BOARD_DISPLAY_ROTATION     0
#define BOARD_CO5300_MADCTL        0   // 0 = use lib's setRotation MADCTL unchanged
