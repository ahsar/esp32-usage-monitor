#include "hw/hw.h"
#include <Arduino.h>
#include <Wire.h>

static void die(const char* what) {
  Serial.printf("hwInit FAIL: %s\n", what);
  while (1) delay(1000);
}

void hwInit() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== claude-buddy waveshare boot ===");

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(400000);

  if (!hwExpanderInit())  die("expander");
  // PMU must be initialised before the display to ensure the display
  // power rail (ALDO3) is on before any QSPI / init commands are sent.
  // On every board this is the only source of ALDO3 enable.
  if (!hwPowerInit())     die("power");
#if BOARD_LCD_RST_VIA_PMU
  // C6 2.16 has no LCD_RST GPIO; the panel is reset by power-cycling
  // AXP ALDO3 (50 ms low between two highs).
  hwPmuRef()->disableALDO3();
  delay(50);
  hwPmuRef()->enableALDO3();
  delay(50);
#endif
  // Toggles PIN_TP_RESET on all boards; PIN_LCD_RESET only on non-PMU
  // boards (gated inside the function via BOARD_LCD_RST_VIA_PMU).
  hwExpanderResetSequence();
  if (!hwDisplayInit())   die("display");
  if (!hwInputInit())     die("input");
  if (!hwImuInit())       die("imu");
  if (!hwRtcInit())       die("rtc");
  // audio (hwAudioInit) intentionally skipped — all beeps removed

  Serial.println("hwInit OK");
}
