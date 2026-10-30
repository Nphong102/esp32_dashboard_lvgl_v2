/**
 * globals.cpp
 * Định nghĩa tất cả biến global — extern trong các header
 */

#include "config.h"
#include "data_types.h"

// ── WiFi ────────────────────────────────────────────────────
char        WIFI_SSID[]    = WIFI_SSID_VALUE;
const char* WIFI_PASSWORD  = WIFI_PASSWORD_VALUE;
const char* TRUCK_DATA_URL = TRUCK_DATA_URL_VALUE;

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
