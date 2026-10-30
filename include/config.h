#pragma once

#if __has_include("secrets.h")
#include "secrets.h"
#endif

#ifndef WIFI_SSID_VALUE
#define WIFI_SSID_VALUE "CHANGE_ME_WIFI_SSID"
#endif

#ifndef WIFI_PASSWORD_VALUE
#define WIFI_PASSWORD_VALUE "CHANGE_ME_WIFI_PASSWORD"
#endif

#ifndef TRUCK_DATA_URL_VALUE
#define TRUCK_DATA_URL_VALUE "http://192.168.1.100:25555"
#endif

#ifndef OTA_PASSWORD
#define OTA_PASSWORD "CHANGE_ME_OTA_PASSWORD"
#endif

// ==================== Libraries ====================
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <lvgl.h>

// ==================== WiFi / Network ====================
extern char        WIFI_SSID[];
extern const char* WIFI_PASSWORD;
extern const char* TRUCK_DATA_URL;

// ==================== Timing Intervals (ms) ====================
#define WIFI_UPDATE_INTERVAL   200
#define TFT_UPDATE_INTERVAL    10    // LVGL cần gọi thường xuyên
#define TELEMETRY_TIMEOUT_MS   (90UL * 1000UL)
#define SLEEP_WAKE_INTERVAL_SEC 30

// ==================== FreeRTOS ====================
extern TaskHandle_t     WiFiTaskHandle;
extern TaskHandle_t     DisplayTaskHandle;
extern SemaphoreHandle_t dataAccessMutex;

// ==================== Global TFT object ====================
extern TFT_eSPI tft;

// ==================== Timing ====================
extern unsigned long lastWiFiUpdate;
extern unsigned long lastTFTUpdate;
extern char          timeStr[9];
#define BAT_ADC_PIN  9

// ==================== OTA ====================
#define OTA_HOSTNAME  "ets2-dash"
