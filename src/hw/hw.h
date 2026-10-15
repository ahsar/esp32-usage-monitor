#pragma once
#include "hw/display.h"
#include "hw/input.h"
#include "hw/power.h"
#include "hw/rtc.h"
#include "hw/expander.h"
#include "hw/pins.h"

void hwInit();   // initialises every subsystem; while(1) on any failure
