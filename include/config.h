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
#include "lgfx_config.h"
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
#define RESCUE_BOOT_PIN         14
#define RESCUE_BOOT_HOLD_MS     2000
#ifndef BUZZER_PIN
#define BUZZER_PIN              1
#endif
#define BUZZER_LEDC_CHANNEL     7
#ifndef ENCODER_A_PIN
#define ENCODER_A_PIN           2
#endif
#ifndef ENCODER_B_PIN
#define ENCODER_B_PIN           3
#endif
#ifndef ENCODER_SW_PIN
#define ENCODER_SW_PIN          14
#endif
#ifndef ENABLE_BATTERY_LEVEL_BEEP
#define ENABLE_BATTERY_LEVEL_BEEP 1
#endif
#define BATTERY_SOURCE_ADC      0
#define BATTERY_SOURCE_INA219   1
#ifndef BATTERY_MEASUREMENT_SOURCE
#define BATTERY_MEASUREMENT_SOURCE BATTERY_SOURCE_INA219
#endif
#if BATTERY_MEASUREMENT_SOURCE != BATTERY_SOURCE_ADC && \
    BATTERY_MEASUREMENT_SOURCE != BATTERY_SOURCE_INA219
#error "BATTERY_MEASUREMENT_SOURCE must be BATTERY_SOURCE_ADC or BATTERY_SOURCE_INA219"
#endif
#define BATTERY_ADC_DIVIDER_RATIO 2.0f
#ifndef BATTERY_I2C_SDA_PIN
#define BATTERY_I2C_SDA_PIN     21
#endif
#ifndef BATTERY_I2C_SCL_PIN
#define BATTERY_I2C_SCL_PIN     22
#endif
#define BATTERY_INA219_ADDRESS  0x40
#ifndef ENABLE_BATTERY_I2C_SCAN
#define ENABLE_BATTERY_I2C_SCAN 1
#endif
#define BATTERY_LOW_SHUTDOWN_VOLTAGE 2.98f
#ifndef ENABLE_BATTERY_LOW_SHUTDOWN
#define ENABLE_BATTERY_LOW_SHUTDOWN 1
#endif
#define ENABLE_BATTERY_UDP_REPORT 1
#define BATTERY_UDP_REPORT_PORT 8888
#define TELEMETRY_UDP_PORT     4210
#define TELEMETRY_UDP_WAKE_TIMEOUT_MS 3000
#define ENABLE_TIMING_LOGS      0

// ==================== FreeRTOS ====================
extern TaskHandle_t     WiFiTaskHandle;
extern TaskHandle_t     DisplayTaskHandle;
extern SemaphoreHandle_t dataAccessMutex;

// ==================== Global display object ====================
extern LGFX tft;

// ==================== Timing ====================
extern unsigned long lastWiFiUpdate;
extern unsigned long lastTFTUpdate;
extern char          timeStr[9];
#define BAT_ADC_PIN  9

// ==================== OTA ====================
#define OTA_HOSTNAME  "ets2-dash"
#define OTA_PASSWORD  "ets2ota"      // ← đổi mật khẩu; phải khớp upload_flags --auth= trong platformio.ini
