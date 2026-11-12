/**
 * main.cpp — ETS2 Dashboard (minimal: WiFi + LVGL TFT)
 *
 * Core 0: WiFiTask  — nhận telemetry UDP
 * Core 1: DisplayTask — LVGL render mỗi 10ms
 */

#include "config.h"
#include "data_types.h"
#include "wifi_task.h"
#include "display_task.h"
#include "rescue_task.h"
#include <esp_sleep.h>

struct BuzzerNote {
    uint16_t frequencyHz;
    uint16_t durationMs;
};

static void playBuzzerNote(const BuzzerNote& note) {
    ledcWriteTone(BUZZER_LEDC_CHANNEL, note.frequencyHz);
    delay(note.durationMs);
    ledcWrite(BUZZER_LEDC_CHANNEL, 0);
    delay(35);
}

void playStartupCompletionMelody() {
    Serial.println("[BUZZER] Startup: completion melody");
    const BuzzerNote startupMelody[] = {
        {659, 120}, {784, 120}, {1047, 180}, {784, 100}, {1047, 360}
    };
    for (const BuzzerNote& note : startupMelody) playBuzzerNote(note);
}

#if !ENABLE_BATTERY_LEVEL_BEEP
static void playTimerWakeMelody() {
    Serial.println("[BUZZER] Timer wake: short quiet beep");
    ledcWriteTone(BUZZER_LEDC_CHANNEL, 1047);
    ledcWrite(BUZZER_LEDC_CHANNEL, 18);
    delay(50);
    ledcWrite(BUZZER_LEDC_CHANNEL, 0);
}
#endif

#if ENABLE_BATTERY_LEVEL_BEEP
static void playBatteryLevelAlert(float batteryVoltage) {
    uint8_t beepCount;
    uint16_t beepDurationMs;
    const char* level;

    if (batteryVoltage >= 3.7f) {
        beepCount = 1;
        beepDurationMs = 100;
        level = "good";
    } else if (batteryVoltage >= 3.5f) {
        beepCount = 2;
        beepDurationMs = 100;
        level = "notice";
    } else if (batteryVoltage >= 3.2f) {
        beepCount = 3;
        beepDurationMs = 100;
        level = "low";
    } else {
        beepCount = 4;
        beepDurationMs = 400;
        level = "recharge";
    }

    Serial.printf("[BATTERY] Wake voltage: %.2f V (%s)\n",
                  batteryVoltage, level);
    for (uint8_t beep = 0; beep < beepCount; beep++) {
        playBuzzerNote({1047, beepDurationMs});
        if (beep + 1 < beepCount) delay(100);
    }
}
#endif

static void printBatterySummary(const char* label, const BatteryReading& reading) {
    const bool tempCellValid = reading.cellTempC > -500.0f;
    const bool tempMonitorValid = reading.monitorTempC > -500.0f;
    const float powerW = reading.currentValid ? reading.voltageV * (reading.currentMa / 1000.0f) : 0.0f;
    const char* state =
        reading.currentValid
            ? (reading.currentMa > 50.0f ? "DISCHARGING" :
               (reading.currentMa < -50.0f ? "CHARGING" : "IDLE"))
            : "UNKNOWN";

    Serial.printf("[BATTERY] %s V=%.2fV I=%+.2fA P=%+.2fW state=%s SoC=%s%% cellTemp=%s monitorTemp=%s\n",
                  label,
                  reading.voltageV,
                  reading.currentValid ? (reading.currentMa / 1000.0f) : 0.0f,
                  powerW,
                  state,
                  reading.socValid ? String(reading.estimatedSocPercent, 0).c_str() : "n/a",
                  tempCellValid ? String(reading.cellTempC, 1).c_str() : "n/a",
                  tempMonitorValid ? String(reading.monitorTempC, 1).c_str() : "n/a");
}

void setup() {
    Serial.begin(115200);
    const bool timerWake = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER;
    initializeRealTimeClock(timerWake);
    if (!timerWake &&
        !startRescueButtonMonitor()) {
        Serial.println("[RESCUE] Could not start BOOT button monitor");
    }
    ledcSetup(BUZZER_LEDC_CHANNEL, 1047, 8);
    ledcAttachPin(BUZZER_PIN, BUZZER_LEDC_CHANNEL);
    ledcWrite(BUZZER_LEDC_CHANNEL, 0);
#if !ENABLE_BATTERY_LEVEL_BEEP
    if (timerWake) playTimerWakeMelody();
#endif
    delay(1000);
    Serial.println("\n=============================");
    Serial.println(" ETS2 Dashboard — Booting...");
    Serial.println("=============================");
    Serial.println("Free heap:  " + String(ESP.getFreeHeap()));
    Serial.println("Free PSRAM: " + String(ESP.getFreePsram()));
    const bool batteryMonitorReady = initializeBatteryMonitor();
    BatteryReading wakeBatteryReading{};
    const bool wakeBatteryReadingValid =
        batteryMonitorReady && readBatteryMonitor(wakeBatteryReading);
    if (!wakeBatteryReadingValid) {
        Serial.println("[BATTERY] Could not read battery measurement");
    }
    const bool lowBatteryShutdown =
#if ENABLE_BATTERY_LOW_SHUTDOWN
        timerWake && wakeBatteryReadingValid &&
        wakeBatteryReading.voltageV < BATTERY_LOW_SHUTDOWN_VOLTAGE;
#else
        false;
#endif
    if (timerWake && wakeBatteryReadingValid) {
        printBatterySummary("WAKE", wakeBatteryReading);
    }
#if ENABLE_BATTERY_LEVEL_BEEP
    if (timerWake && wakeBatteryReadingValid && !lowBatteryShutdown) {
        playBatteryLevelAlert(wakeBatteryReading.voltageV);
    }
#endif
    // 1. Mutex — tạo trước tất cả
    dataAccessMutex = xSemaphoreCreateMutex();
    if (!dataAccessMutex) {
        Serial.println("[ERROR] Mutex create failed!");
        while (1);
    }
    Serial.println("[OK] Mutex created");

    loadSavedNetworkSettings();

    // 2a. WiFi bắt đầu kết nối nền trừ khi rescue mode đã được chọn.
    if (!isRescueModeActive()) {
        wifiBegin();
    }

    // Khi tỉnh bằng timer, kiểm tra telemetry trước để tránh bật TFT nếu vẫn mất kết nối.
    if (timerWake) {
        Serial.println("[POWER] Timer wake; checking telemetry before starting display");
        const uint32_t connectStarted = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - connectStarted < 10000) {
            delay(100);
        }

        bool telemetryFound = false;
        if (WiFi.status() == WL_CONNECTED) {
            const bool reportSent = sendBatteryVoltageUdpReport(
                wakeBatteryReadingValid ? &wakeBatteryReading : nullptr,
                lowBatteryShutdown ? "penaty powerlot, shut down" : nullptr);
            if (!reportSent) {
                Serial.println("[BATTERY UDP] Wake report was not sent");
            }
            if (!lowBatteryShutdown) {
                telemetryFound =
                    waitForTelemetryUdp(TELEMETRY_UDP_WAKE_TIMEOUT_MS);
            }
        } else {
            Serial.println("[BATTERY] Wi-Fi unavailable; could not send wake report");
        }

        if (lowBatteryShutdown) {
            printBatterySummary("PRE_SLEEP", wakeBatteryReading);
            enterBatteryProtectionSleep(
                "Battery below protection threshold; sleeping without timer wake");
            while (true) delay(1000);
        }

        if (!telemetryFound) {
            if (wakeBatteryReadingValid) {
                printBatterySummary("PRE_SLEEP", wakeBatteryReading);
            }
            enterLowPowerSleep("Telemetry still unavailable after timer wake");
        } else {
            Serial.println("[POWER] Telemetry restored; starting dashboard");
        }
    }
    // 2b. TFT + LVGL — phải trước khi tạo DisplayTask
    setupDisplay();
    Serial.println("[OK] Display setup done");

    // 3. WiFi đã được xử lý trong boot sequence + WiFiTask tự reconnect

    // 4. Tạo tasks
    //    DisplayTask: core 1, priority 2, stack 6KB
    //    (buffer trong PSRAM nên stack task nhỏ cũng được)
    xTaskCreatePinnedToCore(
        DisplayTask, "Display",
        10240,                   // 6KB stack
        NULL, 2,
        &DisplayTaskHandle, 1
    );
    Serial.println("[OK] DisplayTask created");

    //    WiFiTask: core 0, priority 1, stack 8KB
    xTaskCreatePinnedToCore(
        WiFiTask, "WiFi",
        8192,                   // 8KB stack
        NULL, 1,
        &WiFiTaskHandle, 0
    );
    Serial.println("[OK] WiFiTask created");

    Serial.println("[OK] All done — entering loop()");
}

void loop() {
    // Tất cả logic trong FreeRTOS tasks
    // loop() chỉ in heap mỗi 10s để debug
    static unsigned long lastPrint = 0;
    if (millis() - lastPrint >= 10000) {
        lastPrint = millis();
        Serial.printf("[heap] free=%d  psram=%d\n",
                      ESP.getFreeHeap(), ESP.getFreePsram());
        Serial.printf("DisplayTask stack free: %d\n",
                      uxTaskGetStackHighWaterMark(DisplayTaskHandle));
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
}
