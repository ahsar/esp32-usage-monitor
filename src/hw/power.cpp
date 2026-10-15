#include "hw/power.h"
#include "hw/pins.h"
#include <Wire.h>
#include <XPowersLib.h>

static XPowersPMU s_pmu;

bool hwPowerInit() {
  if (!s_pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, PIN_I2C_SDA, PIN_I2C_SCL)) {
    Serial.println("hwPower: AXP2101 begin failed");
    return false;
  }
  s_pmu.setChargeTargetVoltage(XPOWERS_AXP2101_CHG_VOL_4V2);
  s_pmu.setChargerConstantCurr(XPOWERS_AXP2101_CHG_CUR_500MA);
  s_pmu.enableBattDetection();
  s_pmu.enableVbusVoltageMeasure();
  s_pmu.enableBattVoltageMeasure();
  s_pmu.enableTemperatureMeasure();

  // ALDO3 → display rail on all three boards. enableALDO3() is idempotent.
  s_pmu.enableALDO3();

#if BOARD_AXP_ENABLE_AUX_LDOS
  // 2.16: ALDO2 powers DSI_PWR_EN (display power-enable signal via R13);
  // ALDO1/4 power MIC bias and secondary sensor rails. Without this the
  // panel stays dark even with ALDO3 enabled (DSI_PWR_EN floats).
  // Voltages match the XiaoZhi 2.16 reference: ALDO1..4 = 3.3V, DCDC1 = 3.3V.
  s_pmu.setDC1Voltage(3300);
  s_pmu.setALDO1Voltage(3300);
  s_pmu.setALDO2Voltage(3300);
  s_pmu.setALDO4Voltage(3300);
  s_pmu.enableALDO1();
  s_pmu.enableALDO2();
  s_pmu.enableALDO4();
#endif

#if BOARD_AXP_PWRON_4S_OFF
  // 2.16: the middle key is wired to AXP PWRON, so holding it is how the device
  // is powered off. 0x27 packs the three PWRON timers:
  //   bits [1:0] ONLEVEL  (hold to power ON)    = 0 -> 128 ms
  //   bits [3:2] OFFLEVEL (hold to hard power OFF) = 3 -> 10 s
  //   bits [5:4] IRQLEVEL (long-press IRQ)      = 3 -> 2.5 s
  // OFFLEVEL is deliberately the maximum: the firmware acts on the IRQ at
  // 2.5 s, and someone holding the key a little too long to switch the board
  // *on* must not have it hard-cut at the old 4 s. loop() also ignores the IRQ
  // that belongs to the press which powered the board on (see the PWR arming).
  // 0x22: bit 1 = PWRON > OFFLEVEL as POWEROFF source; bit 2 = die over-temp
  // level-2 power-off, kept as a safety net.
  s_pmu.writeRegister(0x22, 0b110);
  s_pmu.writeRegister(0x27, 0b00111100);   // ON 128 ms / OFF 10 s / IRQ 2.5 s
#endif

  s_pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
  // AXP2101::enableIRQ() takes chip-specific bit positions, not the
  // cross-chip XPOWERS_PWR_BTN_* enums (which would write the wrong
  // INTEN register).
  s_pmu.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ | XPOWERS_AXP2101_PKEY_LONG_IRQ);
  s_pmu.clearIrqStatus();
  return true;
}

HwBattery hwBattery() {
  HwBattery b;
  b.mV         = s_pmu.getBattVoltage();
  // AXP2101 (via XPowersLib) does not expose actual battery current,
  // only set charge target. We report 0 mA — info screen shows ±/charging
  // state via hb.charging instead. mA stays in struct for ABI compat.
  b.mA         = 0;
  int p        = (b.mV - 3200) / 10;
  if (p < 0) p = 0; if (p > 100) p = 100;
  b.pct        = p;
  int vbus_mV  = s_pmu.getVbusVoltage();
  b.usbPresent = vbus_mV > 4000;
  b.charging   = s_pmu.isCharging();
  b.tempC      = (int)s_pmu.getTemperature();
  return b;
}

void hwPowerOff() { s_pmu.shutdown(); }

bool hwAxpPekeyShortPress() {
  s_pmu.getIrqStatus();
  bool hit = s_pmu.isPekeyShortPressIrq();
  if (hit) s_pmu.clearIrqStatus();
  return hit;
}

bool hwAxpPekeyLongPress() {
  s_pmu.getIrqStatus();
  bool hit = s_pmu.isPekeyLongPressIrq();
  if (hit) s_pmu.clearIrqStatus();
  return hit;
}

XPowersPMU* hwPmuRef() { return &s_pmu; }
