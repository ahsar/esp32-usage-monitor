// src/hw/expander.h
#pragma once
#include "hw/pins.h"

#if BOARD_HAS_TCA9554
#include <Adafruit_XCA9554.h>
// Shared TCA9554 instance — used by expander.cpp only;
// other hw/ files go through hwExpanderAxpIrqLow() etc.
extern Adafruit_XCA9554 g_expander;
#endif

bool hwExpanderInit();          // false on I2C failure (TCA9554 path) or never fails (direct GPIO path)
void hwExpanderResetSequence(); // pull LCD_RESET + TP_RESET low → 20ms → high
bool hwExpanderAxpIrqLow();     // true while AXP IRQ is asserted; always true on 1.75C (poll via I2C)
