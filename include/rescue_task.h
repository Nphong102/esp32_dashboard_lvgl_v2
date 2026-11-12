#pragma once

#include <Arduino.h>

void loadSavedNetworkSettings();
bool loadSavedAccessPin(char* output, size_t outputSize);
bool startRescueButtonMonitor();
void stopRescueButtonMonitor();
bool startRescueMode();
bool isRescueModeActive();
