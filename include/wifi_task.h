#pragma once

#include <stdint.h>
#include <stddef.h>
#include "battery_monitor.h"

void wifiBegin();   // non-blocking
void setupWiFi();   // blocking (cũ, không còn dùng trong main)
bool fetchTruckData();
bool waitForTelemetryUdp(uint32_t timeoutMs);
bool sendBatteryVoltageUdpReport(
    const BatteryReading* reading = nullptr,
    const char* event = nullptr);
void initializeRealTimeClock(bool timerWake);
bool getCurrentLocalDateTime(char* output, size_t outputSize);
void WiFiTask(void* parameter);
void enterLowPowerSleep(const char* reason);
void enterBatteryProtectionSleep(const char* reason);

String formatTimeHHMM(String datetime);

void getTelemetryTimingSnapshot(uint32_t* updatedAtUs, uint32_t* generation);
