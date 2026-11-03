#pragma once

// ==================== Telemetry sources ====================
#define ENABLE_HTTP_TELEMETRY 0

// ==================== Libraries ====================
#include <WiFi.h>
#if ENABLE_HTTP_TELEMETRY
#include <HTTPClient.h>
#include <ArduinoJson.h>
#endif
#include <SPI.h>
#include <TFT_eSPI.h>
#include <lvgl.h>

// ==================== WiFi / Network ====================
extern char        WIFI_SSID[33];
extern char        WIFI_PASSWORD[65];
extern char        TRUCK_DATA_URL[193];

// ==================== Timing Intervals (ms) ====================
#define WIFI_UPDATE_INTERVAL   200
#define TFT_UPDATE_INTERVAL    10    // LVGL cần gọi thường xuyên
#define TELEMETRY_TIMEOUT_MS   (90UL * 1000UL)
#define SLEEP_WAKE_INTERVAL_SEC 30
#define NTP_SYNC_TIMEOUT_MS     8000
#define LOCAL_TIME_ZONE         "ICT-7"
#define NTP_SERVER_PRIMARY      "pool.ntp.org"
#define NTP_SERVER_SECONDARY    "time.google.com"
#define RESCUE_BOOT_PIN         0
#define RESCUE_BOOT_HOLD_MS     2000
#define BUZZER_PIN              2
#define BUZZER_LEDC_CHANNEL     7
#define ENABLE_BATTERY_UDP_REPORT 1
#define BATTERY_UDP_REPORT_PORT 8888
#define TELEMETRY_UDP_PORT     4210
#define TELEMETRY_UDP_WAKE_TIMEOUT_MS 3000
#define ENABLE_TIMING_LOGS      0

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
#define OTA_PASSWORD  "ets2ota"      // ← đổi mật khẩu; phải khớp upload_flags --auth= trong platformio.ini
