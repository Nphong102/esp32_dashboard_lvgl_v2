#pragma once

#include <Arduino.h>

void loadSavedNetworkSettings();
bool startRescueButtonMonitor();
void stopRescueButtonMonitor();
bool startRescueMode();
bool isRescueModeActive();
