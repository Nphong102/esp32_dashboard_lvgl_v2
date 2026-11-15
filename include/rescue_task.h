#pragma once

#include <Arduino.h>

struct RescueDisplayImageSettings {
    String bootLogo1;
    String bootLogo2;
    String idleImages;
    bool useSdIdleImages = false;
};

void loadSavedNetworkSettings();
bool loadSavedAccessPin(char* output, size_t outputSize);
void loadSavedDisplayImageSettings(RescueDisplayImageSettings& settings);
bool saveDisplayImageSettings(const RescueDisplayImageSettings& settings);
bool isRescueImageCatalogReady();
void setRescueImageCatalogReady(bool ready);
bool startRescueButtonMonitor();
void stopRescueButtonMonitor();
bool startRescueMode();
bool isRescueModeActive();
