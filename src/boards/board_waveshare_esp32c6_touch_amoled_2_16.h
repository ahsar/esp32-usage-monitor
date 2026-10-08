// src/boards/board_waveshare_esp32c6_touch_amoled_2_16.h
#pragma once

// Display: SH8601 480×480 rounded-square AMOLED, 2.16" diagonal.
// 184×224 canvas 2× upscaled → 368×448, centred at (56, 16) in the 480×480 panel.
// The entire upscale is streamed in ONE continuous QSPI transaction (CS held
// asserted across all rows) so the panel never sees bus idle between row draws
// (which would cause the screen to go black on this panel revision).
#define LCD_W_PHYS              480
#define LCD_H_PHYS              480
#define BOARD_HW_W              184
#define BOARD_HW_H              224
#define BOARD_SAFE_INSET        8
#define BOARD_DISPLAY_OFFSET_X  56
#define BOARD_DISPLAY_OFFSET_Y  16
#define BOARD_DISPLAY_SCALE     2
// Streamed push: one continuous QSPI transaction instead of per-row draw calls.
// Required on 2.16 because CS toggles between draw16bitRGBBitmap calls leave the
// panel black. 1.8 (also SH8601/QSPI) doesn't need this — S3 at 240 MHz loops
// fast enough that the panel never times out between calls.
#define BOARD_DISPLAY_PUSH_STREAMED  1

// QSPI to SH8601 (per XiaoZhi v2.2.5 board def + schematic verification)
#define PIN_LCD_SDIO0  1
#define PIN_LCD_SDIO1  2
#define PIN_LCD_SDIO2  3
#define PIN_LCD_SDIO3  4
#define PIN_LCD_SCLK   0
#define PIN_LCD_CS     15
// PIN_LCD_RESET intentionally undefined — panel reset is via AXP ALDO3 power-cycle.

// I2C bus (shared: AXP2101, ES8311, ES7210, CST9217, QMI8658, PCF85063)
#define PIN_I2C_SDA   8
#define PIN_I2C_SCL   7

// Touch: CST9217 (CST92xx family, same protocol path as 1.75C's CST92xx)
#define PIN_TP_INT    5
#define PIN_TP_RESET  11

// I2S to ES8311 codec
#define PIN_I2S_MCLK  19
#define PIN_I2S_BCLK  20
#define PIN_I2S_WS    22
#define PIN_I2S_DI    21
#define PIN_I2S_DO    23
#define PIN_PA_CTRL   -1   // No discrete PA enable on 2.16; gated by BOARD_HAS_PA_CTRL=0.

// IMU (QMI8658, polled — INTs reserved but not wired to handlers)
#define PIN_QMI_INT1  16
#define PIN_QMI_INT2  17

// Three physical keys
#define PIN_KEY1      18   // PWR silkscreen. Active-HIGH via BSS138 inverter. Also AXP PWRON.
#define PIN_KEY2      10   // IO10 silkscreen. Active-low.
#define PIN_KEY_BOOT  9    // BOOT silkscreen. Active-low. Synthesises BTN_A_LONG_PRESS.

// Capability flags
#define BOARD_HAS_PSRAM            0
#define BOARD_HAS_TCA9554          0
#define BOARD_HAS_PCF85063         1
#define BOARD_HAS_PA_CTRL          0
#define BOARD_HAS_AXP2101          1
#define BOARD_LCD_RST_VIA_PMU      1
#define BOARD_AXP_PWRON_4S_OFF     1
#define BOARD_DISPLAY_CO5300       0
#define BOARD_DISPLAY_LETTERBOX    0
#define BOARD_TOUCH_CST92XX        1
// Touch orientation: SH8601 on C6-2.16 has no MADCTL rotation, so use the
// same mirrorXY(true,true) that pre-dated this parametrisation.
#define BOARD_TOUCH_SWAP_XY        0
#define BOARD_TOUCH_MIRROR_X       1
#define BOARD_TOUCH_MIRROR_Y       1
#define BOARD_BTN_SWAP_AB          0
#define BOARD_BTN_THIRD            1
#define BOARD_KEY1_ACTIVE_HIGH     1
#define BOARD_HAS_KEY2             1

// Credits page
#define BOARD_MODEL_LINE1  "Waveshare ESP32-C6"
#define BOARD_MODEL_LINE2  "Touch AMOLED 2.16"

#define BOARD_DISPLAY_SH8601_VENDOR_INIT  1
#define BOARD_AXP_ENABLE_AUX_LDOS  1
#define BOARD_CO5300_COL_OFFSET    0   // C6-2.16 uses SH8601, not CO5300; value irrelevant
#define BOARD_DISPLAY_ROTATION     0
#define BOARD_CO5300_MADCTL        0   // 0 = use lib's setRotation MADCTL unchanged
