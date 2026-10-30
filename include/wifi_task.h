#pragma once

void wifiBegin();   // non-blocking
void setupWiFi();   // blocking (cũ, không còn dùng trong main)
bool fetchTruckData();
void WiFiTask(void* parameter);
void enterLowPowerSleep(const char* reason);

String formatTimeHHMM(String datetime);
