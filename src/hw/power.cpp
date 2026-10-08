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
  // 2.16: configure AXP to power off on 4 s PWRON-hold (the PWR key is
  // the only software-configurable shutdown path on this board).
  // 0x22 reg: PWRON > OFFLEVEL as POWEROFF source. 0x27 reg: 4s timing.
  s_pmu.writeRegister(0x22, 0b110);
  s_pmu.writeRegister(0x27, 0x10);
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
