/**
 * globals.cpp
 * Định nghĩa tất cả biến global — extern trong các header
 */

#include "config.h"
#include "data_types.h"
#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.h.example"
#endif

// ── WiFi ────────────────────────────────────────────────────
char        WIFI_SSID[33]       = WIFI_SSID_VALUE;
char        WIFI_PASSWORD[65]  = WIFI_PASSWORD_VALUE;
char        TRUCK_DATA_URL[193] = TRUCK_DATA_URL_VALUE;

// ── Hardware ────────────────────────────────────────────────
TFT_eSPI tft = TFT_eSPI();

// ── FreeRTOS ────────────────────────────────────────────────
TaskHandle_t      WiFiTaskHandle    = nullptr;
TaskHandle_t      DisplayTaskHandle = nullptr;
SemaphoreHandle_t dataAccessMutex  = nullptr;

// ── Data structs ────────────────────────────────────────────
TruckData      truck;
GameData       game;
JobData        job;
TrailerData    trailer;
NavigationData navigation;

// ── Timing ──────────────────────────────────────────────────
unsigned long lastWiFiUpdate = 0;
unsigned long lastTFTUpdate  = 0;
char          timeStr[9]     = "00:00:00";
