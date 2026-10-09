#include "hw/input.h"
#include "hw/pins.h"
#include "hw/display.h"
#include "hw/expander.h"
#include "hw/power.h"
#include <Arduino.h>

#if BOARD_TOUCH_CST92XX
  #include <Wire.h>
  #include "TouchDrvCSTXXX.hpp"
#else
  #include <Arduino_DriveBus_Library.h>
#endif

static HwBtn   s_a, s_b, s_boot;
static HwTouch s_tp;
static uint8_t s_axpEvt = 0;

#if BOARD_TOUCH_CST92XX
static TouchDrvCST92xx s_cst;
#else
static std::shared_ptr<Arduino_IIC_DriveBus> s_iicBus;
static std::unique_ptr<Arduino_IIC>          s_ft3168;
#endif
static volatile bool                          s_tpIrqFlag = false;

static void IRAM_ATTR onTouchIrq() { s_tpIrqFlag = true; }

bool HwBtn::pressedFor(uint32_t ms) {
  return isPressed && (millis() - pressedAt) >= ms;
}

bool hwInputInit() {
  pinMode(PIN_KEY1, INPUT_PULLUP);   // GPIO0 has external pullup; INPUT_PULLUP is harmless
#if BOARD_HAS_KEY2
  pinMode(PIN_KEY2, INPUT_PULLUP);   // External R18 10K already pulls high; INPUT_PULLUP is harmless
#endif
#if BOARD_BTN_THIRD
  pinMode(PIN_KEY_BOOT, INPUT_PULLUP);   // External R8 10K already pulls high; INPUT_PULLUP is harmless
#endif

#if BOARD_TOUCH_CST92XX
  // CST92xx @ 0x5A via SensorLib. Reset is handled by hwExpanderResetSequence()
  // (TP_RST is shared with LCD_RESET on 1.75C), so pass rstPin=-1 to skip the
  // driver's internal reset — otherwise it would also re-reset the display.
  s_cst.setPins(-1, PIN_TP_INT);
  if (!s_cst.begin(Wire, 0x5A, PIN_I2C_SDA, PIN_I2C_SCL)) {
    Serial.println("hwInput: CST92xx init failed");
    return false;
  }
  Serial.printf("hwInput: CST92xx model=%s\n", s_cst.getModelName());
  s_cst.setMaxCoordinates(LCD_W_PHYS, LCD_H_PHYS);
  // Touch orientation is board-specific: the S3-2.16 rotates the panel 90°
  // via MADCTL=0xA0, so X/Y must be swapped to align with the canvas. Other
  // boards keep the legacy mirrorXY(true,true) with no swap.
  s_cst.setSwapXY(BOARD_TOUCH_SWAP_XY);
  s_cst.setMirrorXY(BOARD_TOUCH_MIRROR_X, BOARD_TOUCH_MIRROR_Y);
  pinMode(PIN_TP_INT, INPUT);
  attachInterrupt(digitalPinToInterrupt(PIN_TP_INT), onTouchIrq, FALLING);
  return true;
#else
  s_iicBus = std::make_shared<Arduino_HWIIC>(PIN_I2C_SDA, PIN_I2C_SCL, &Wire);
  s_ft3168.reset(new Arduino_FT3x68(s_iicBus, FT3168_DEVICE_ADDRESS,
                                    DRIVEBUS_DEFAULT_VALUE, PIN_TP_INT, onTouchIrq));
  for (int i = 0; i < 5; i++) {
    if (s_ft3168->begin()) return true;
    delay(100);
  }
  Serial.println("hwInput: FT3168 init failed");
  return false;
#endif
}

static void scanKey1() {
  uint32_t now = millis();
#if BOARD_KEY1_ACTIVE_HIGH
  bool pressed = digitalRead(PIN_KEY1) == HIGH;
#else
  bool pressed = digitalRead(PIN_KEY1) == LOW;
#endif
  s_a.wasPressed  = pressed && !s_a.isPressed;
  s_a.wasReleased = !pressed && s_a.isPressed;
  if (s_a.wasPressed) s_a.pressedAt = now;
  s_a.isPressed = pressed;
}

#if BOARD_HAS_KEY2
static void scanKey2() {
  uint32_t now = millis();
  bool pressed = digitalRead(PIN_KEY2) == LOW;
  s_b.wasPressed  = pressed && !s_b.isPressed;
  s_b.wasReleased = !pressed && s_b.isPressed;
  if (s_b.wasPressed) s_b.pressedAt = now;
  s_b.isPressed = pressed;
}
#endif

#if BOARD_BTN_THIRD
// BOOT key (GPIO0 on 2.16, rightmost button). Tracks its own state so
// main.cpp can send a custom BLE command on short press.
static void scanBootKey() {
  bool pressed = digitalRead(PIN_KEY_BOOT) == LOW;
  s_boot.wasPressed  = pressed && !s_boot.isPressed;
  s_boot.wasReleased = !pressed && s_boot.isPressed;
  if (s_boot.wasPressed) s_boot.pressedAt = millis();
  s_boot.isPressed = pressed;
}
#endif

#if !BOARD_HAS_KEY2
static void scanAxp() {
  if (hwExpanderAxpIrqLow()) {
    if (hwAxpPekeyShortPress()) s_axpEvt = 0x02;
    if (hwAxpPekeyLongPress())  s_axpEvt = 0x04;
  }
  // Route 0x02 to BtnB pulse for one frame:
  bool pressed = (s_axpEvt == 0x02);
  s_b.wasPressed  = pressed;
  s_b.wasReleased = pressed;
  s_b.isPressed   = false;
  if (pressed) s_axpEvt = 0;
  // 0x04 stays in s_axpEvt until consumed by hwAxpBtnEvent()
}
#endif

static void scanTouch() {
  // Poll when the IRQ fired OR when a finger was down last frame — both FT3168
  // and CST92xx only reliably IRQ on state edges, so a drag wouldn't advance
  // x/y without this. Also keep polling while the IRQ line is still asserted:
  // the level stays low for as long as the glass is touched, so a single
  // failed read mid-drag can't make us drop a finger that is still down.
  bool shouldPoll = s_tpIrqFlag || s_tp.down || digitalRead(PIN_TP_INT) == LOW;
  s_tpIrqFlag = false;

  if (!shouldPoll) {
    s_tp.justPressed  = false;
    s_tp.justReleased = false;
    return;
  }

#if BOARD_TOUCH_CST92XX
  int16_t x[2] = {0}, y[2] = {0};
  uint8_t n = s_cst.getPoint(x, y, s_cst.getSupportTouchPoint());
  if (n > 0) {
    s_tp.justPressed  = !s_tp.down;
    s_tp.justReleased = false;
    // Physical → logical mapping is shared with hwDisplayPush(), so the two
    // can never drift (see hwDisplayMapTouch).
    int lx, ly;
    hwDisplayMapTouch(x[0], y[0], &lx, &ly);
    s_tp.x = (int16_t)lx;
    s_tp.y = (int16_t)ly;
    s_tp.down = true;
    #ifdef DEBUG_TOUCH
      Serial.printf("TP raw=%d,%d  mapped=%d,%d  down=%d jp=%d\n",
                    x[0], y[0], s_tp.x, s_tp.y, s_tp.down, s_tp.justPressed);
    #endif
  } else {
    s_tp.justReleased = s_tp.down;
    s_tp.down = false;
    s_tp.justPressed  = false;
  }
#else
  uint8_t fingers = (uint8_t)s_ft3168->IIC_Read_Device_Value(
      Arduino_IIC_Touch::Value_Information::TOUCH_FINGER_NUMBER);
  if (fingers > 0) {
    int rx = (int)s_ft3168->IIC_Read_Device_Value(
        Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X);
    int ry = (int)s_ft3168->IIC_Read_Device_Value(
        Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y);
    s_tp.justPressed  = !s_tp.down;
    s_tp.justReleased = false;
    int lx, ly;
    hwDisplayMapTouch(rx, ry, &lx, &ly);
    s_tp.x = (int16_t)lx;
    s_tp.y = (int16_t)ly;
    s_tp.down = true;
  } else {
    s_tp.justReleased = s_tp.down;
    s_tp.down = false;
    s_tp.justPressed  = false;
  }
#endif
}

void hwInputUpdate() {
  scanKey1();
#if BOARD_HAS_KEY2
  scanKey2();
#else
  scanAxp();
#endif
#if BOARD_BTN_THIRD
  scanBootKey();
#endif
  scanTouch();
}

#if BOARD_BTN_SWAP_AB
HwBtn& hwBtnA() { return s_b; }
HwBtn& hwBtnB() { return s_a; }
#else
HwBtn& hwBtnA() { return s_a; }
HwBtn& hwBtnB() { return s_b; }
#endif

HwBtn& hwBtnBoot() { return s_boot; }

uint8_t hwAxpBtnEvent() {
  uint8_t e = s_axpEvt;
  if (e == 0x04) s_axpEvt = 0;   // 0x02 already cleared by scanAxp
  return e;
}

const HwTouch& hwTouch() { return s_tp; }
