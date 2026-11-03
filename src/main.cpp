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

static void playTimerWakeMelody() {
    Serial.println("[BUZZER] Timer wake: short quiet beep");
    ledcWriteTone(BUZZER_LEDC_CHANNEL, 1047);
    ledcWrite(BUZZER_LEDC_CHANNEL, 18);
    delay(50);
    ledcWrite(BUZZER_LEDC_CHANNEL, 0);
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
    if (timerWake) playTimerWakeMelody();
    delay(1000);
    Serial.println("\n=============================");
    Serial.println(" ETS2 Dashboard — Booting...");
    Serial.println("=============================");
    Serial.println("Free heap:  " + String(ESP.getFreeHeap()));
    Serial.println("Free PSRAM: " + String(ESP.getFreePsram()));
    pinMode(BAT_ADC_PIN, INPUT);
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
            sendBatteryVoltageUdpReport();
            telemetryFound =
                waitForTelemetryUdp(TELEMETRY_UDP_WAKE_TIMEOUT_MS);
        }

        if (!telemetryFound) {
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
