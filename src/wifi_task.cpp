/**
 * wifi_task.cpp
 * - Kết nối WiFi
 * - Fetch JSON từ ETS2 telemetry API
 * - Chạy trong WiFiTask trên core 0
 */

#include "config.h"
#include "data_types.h"
#include "wifi_task.h"
#include "ota_task.h"
#include <esp_sleep.h>

// ════════════════════════════════════════════════════════
// setupWiFi
// ════════════════════════════════════════════════════════
// Khởi động WiFi KHÔNG chặn — gọi trước setupDisplay() để WiFi kết nối
// nền trong lúc màn hình chiếu logo/boot
void wifiBegin() {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false); 
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void setupWiFi() {
    WiFi.mode(WIFI_STA);
    
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    Serial.print("[WiFi] Connecting");
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
        delay(500);
        Serial.print(".");
        attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\n[WiFi] Connected: " + WiFi.localIP().toString());
    } else {
        Serial.println("\n[WiFi] Failed — will retry in WiFiTask");
    }
}

// ════════════════════════════════════════════════════════
// formatTimeHHMM  "2024-01-01T14:35:00" → "14:35"
// ════════════════════════════════════════════════════════
String formatTimeHHMM(String datetime) {
    int h = 0, m = 0;
    sscanf(datetime.c_str(), "%*d-%*d-%*dT%d:%d", &h, &m);
    char buf[6];
    snprintf(buf, sizeof(buf), "%02d:%02d", h, m);
    return String(buf);
}

// ════════════════════════════════════════════════════════
// Đánh dấu game mất kết nối (WiFi rớt / server không trả lời / HTTP lỗi)
// → DisplayTask sẽ chuyển sang màn hình ảnh chờ
// ════════════════════════════════════════════════════════
static void markGameDisconnected() {
    if (xSemaphoreTake(dataAccessMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        game.connected = false;
        xSemaphoreGive(dataAccessMutex);
    }
}

// ════════════════════════════════════════════════════════
// fetchTruckData
// ════════════════════════════════════════════════════════
bool fetchTruckData() {
    if (WiFi.status() != WL_CONNECTED) { markGameDisconnected(); return false; }

    HTTPClient http;
    http.begin(TRUCK_DATA_URL);
    http.setTimeout(150);   // timeout ngắn để không block lâu
    int code = http.GET();

    if (code == 200) {
        String payload = http.getString();
        //    Serial.println(payload);   // uncomment nếu muốn xem raw JSON
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, payload);
        if (err) {
            Serial.println("[JSON] Error: " + String(err.c_str()));
            markGameDisconnected();
            http.end();
            return false;
        }

        const bool telemetryActive = doc["game"]["connected"] | false;

        // Lấy mutex trước khi ghi data
        if (xSemaphoreTake(dataAccessMutex, pdMS_TO_TICKS(50)) == pdTRUE) {

            // ── Truck ──────────────────────────────────────
            truck.speed            = abs((int)doc["truck"]["speed"]);
            truck.rpm              = doc["truck"]["engineRpm"];
            truck.rpmMax           = doc["truck"]["engineRpmMax"].as<int>();   // 2300.0 -> 2300
            truck.fuel             = doc["truck"]["fuel"];
            truck.fuelCapacity     = doc["truck"]["fuelCapacity"];
            truck.fuelWarningOn    = doc["truck"]["fuelWarningOn"];
            truck.airPressure      = doc["truck"]["air"];
            truck.airWarningOn     = doc["truck"]["airWarningOn"];
            truck.displayedGear    = doc["truck"]["displayedGear"];
            truck.waterTemperature = doc["truck"]["watterTemp"];
            truck.oilTemperature   = doc["truck"]["oilTemp"];
            truck.speedLimit       = doc["navigation"]["speedLimit"];
            truck.odo              = doc["truck"]["odometer"];
            truck.wear             = (int)(doc["truck"]["maxWear"].as<float>() * 100);
            truck.engineOn         = doc["truck"]["engineOn"];
            truck.parkingBreak     = doc["truck"]["parkingBreak"];
            truck.lightsBeamHighOn = doc["truck"]["lightsBeamHighOn"];
            truck.lightsBeamLowOn  = doc["truck"]["lightsBeamLowOn"];
            truck.blinkerLeftOn    = doc["truck"]["blinkerLeftActive"];
            truck.blinkerRightOn   = doc["truck"]["blinkerRightActive"];
            truck.speedCtrl        = doc["truck"]["cruiseControlSpeed"];
            truck.speedCtrlOn      = doc["truck"]["cruiseControlOn"];
            truck.make             = doc["truck"]["make"].as<String>();
            truck.model            = doc["truck"]["model"].as<String>();
            truck.licensePlate     = doc["truck"]["licensePlate"].as<String>();
            truck.id               = doc["truck"]["id"].as<String>();

            // ── Game ───────────────────────────────────────
            game.time     = formatTimeHHMM(doc["game"]["time"].as<String>());
            game.realTime = doc["game"]["realTime"].as<String>();
            game.pause    = doc["game"]["paused"] | true;      // thiếu key → coi như pause
            game.connected = doc["game"]["connected"] | false;

            // ── Job ────────────────────────────────────────
            job.remainingTime = doc["job"]["timeRemain"].as<String>();
            job.income        = doc["job"]["income"];

            // ── Trailer ────────────────────────────────────
            trailer.attached  = doc["trailer"]["attached"];
            trailer.name      = doc["trailer"]["name"].as<String>();
            trailer.mass      = doc["trailer"]["mass"];
            trailer.wear      = (int)(doc["trailer"]["trailerWear"].as<float>() * 100);
            trailer.cargoWear = (int)(doc["trailer"]["cargoWear"].as<float>()   * 100);
            // Serial.println(truck.speed);
            // ── Navigation ─────────────────────────────────
            navigation.estimatedDistance =
                doc["navigation"]["estimatedDistance"].as<int>() / 1000;
            navigation.speed_limit = round(doc["navigation"]["speedLimit"].as<int>()/5*5);

            xSemaphoreGive(dataAccessMutex);
            http.end();
            return telemetryActive;
        }

        http.end();
        return false;
    } else {
        Serial.println("[HTTP] Code: " + String(code));
        markGameDisconnected();
    }

    http.end();
    return false;
}

void enterLowPowerSleep(const char* reason) {
    Serial.printf("[POWER] %s; sleeping for %u seconds\n",
                  reason, (unsigned)SLEEP_WAKE_INTERVAL_SEC);

#ifdef TFT_BL
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, LOW);
#endif

    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);

    const esp_err_t err = esp_sleep_enable_timer_wakeup(
        (uint64_t)SLEEP_WAKE_INTERVAL_SEC * 1000000ULL);
    if (err != ESP_OK) {
        Serial.printf("[POWER] Failed to configure wake timer: %s\n", esp_err_to_name(err));
#ifdef TFT_BL
        digitalWrite(TFT_BL, TFT_BACKLIGHT_ON);
#endif
        return;
    }

    Serial.flush();
    esp_deep_sleep_start();
}

// ════════════════════════════════════════════════════════
// WiFiTask  (core 0, priority 1)
// ════════════════════════════════════════════════════════
void WiFiTask(void* parameter) {
    Serial.println("[WiFiTask] Started on core " + String(xPortGetCoreID()));
    uint32_t lastTelemetryMs = millis();

    for (;;) {
        // Reconnect nếu mất mạng
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[WiFi] Reconnecting...");
            WiFi.disconnect();
            WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        otaService();    // nhận firmware qua WiFi (không chặn nếu không có phiên OTA)

        unsigned long now = millis();
        if (now - lastWiFiUpdate >= WIFI_UPDATE_INTERVAL) {
            lastWiFiUpdate = now;
            if (fetchTruckData()) {
                lastTelemetryMs = millis();
            }
        }

        if (millis() - lastTelemetryMs >= TELEMETRY_TIMEOUT_MS) {
            enterLowPowerSleep("No active telemetry for 5 minutes");
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
