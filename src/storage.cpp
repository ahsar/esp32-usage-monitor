#include "storage.h"
#include <Arduino.h>
#include <LittleFS.h>

static bool s_mounted = false;

bool storageMount() {
  if (s_mounted) return true;
  if (!LittleFS.begin(false)) {
    if (!LittleFS.format()) return false;
    if (!LittleFS.begin(false)) return false;
  }
  s_mounted = true;
  return true;
}
