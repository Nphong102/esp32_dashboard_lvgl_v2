/**
 * wifi_task.cpp
 * - Kết nối WiFi
 * - Nhận telemetry UDP từ server/bridge ETS2
 * - Chạy trong WiFiTask trên core 0
 */

#include "config.h"
#include "data_types.h"
#include "wifi_task.h"
#include "ota_task.h"
#include "rescue_task.h"
#include <esp_sleep.h>
#include <esp_attr.h>
#include <sys/time.h>
#include <time.h>
#include <esp_sntp.h>
#include <WiFiUdp.h>

static constexpr time_t kMinimumValidEpoch = 1704067200;
RTC_DATA_ATTR static int64_t retainedEpochSeconds = 0;
RTC_DATA_ATTR static bool retainedClockValid = false;
static volatile bool ntpTimeSynchronized = false;

static portMUX_TYPE telemetryTimingMux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t telemetryUpdatedAtUs = 0;
static uint32_t telemetryGeneration = 0;

struct TimingWindow {
    uint32_t count = 0;
    uint64_t totalUs = 0;
    uint32_t maxUs = 0;
};

#if ENABLE_HTTP_TELEMETRY
static TimingWindow httpTiming;
static TimingWindow parseUpdateTiming;
static TimingWindow fetchTiming;
#endif
static TimingWindow udpTiming;
static WiFiUDP telemetryUdp;
static WiFiUDP batteryReportUdp;
static bool telemetryUdpStarted = false;
static bool telemetryUdpStartAttempted = false;
static uint32_t lastUdpStartAttemptMs = 0;
static bool hasUdpSequence = false;
static uint16_t lastUdpSequence = 0;
static uint32_t lastUdpPacketMs = 0;
static bool batteryReportSent = false;
static bool batteryReportAttempted = false;
static uint32_t lastBatteryReportAttemptMs = 0;

static void setLocalTimeZone() {
    setenv("TZ", LOCAL_TIME_ZONE, 1);
    tzset();
}

static void onNtpTimeSync(struct timeval*) {
    ntpTimeSynchronized = true;
}

void initializeRealTimeClock(bool timerWake) {
    setLocalTimeZone();

    if (!timerWake) {
        retainedClockValid = false;
        return;
    }

    if (!retainedClockValid || retainedEpochSeconds < kMinimumValidEpoch) {
        Serial.println("[TIME] No retained clock value for timer wake");
        return;
    }

    struct timeval restoredTime;
    restoredTime.tv_sec =
        (time_t)(retainedEpochSeconds + SLEEP_WAKE_INTERVAL_SEC);
    restoredTime.tv_usec = 0;
    if (settimeofday(&restoredTime, nullptr) == 0) {
        Serial.printf("[TIME] Restored RTC clock after timer wake: %lld\n",
                      (long long)restoredTime.tv_sec);
    } else {
        Serial.println("[TIME] Failed to restore RTC clock");
    }
}

bool getCurrentLocalDateTime(char* output, size_t outputSize) {
    if (output == nullptr || outputSize < 27) return false;

    const time_t currentEpoch = time(nullptr);
    if (currentEpoch < kMinimumValidEpoch) return false;

    struct tm localTime;
    if (localtime_r(&currentEpoch, &localTime) == nullptr) return false;

    char timestamp[32];
    const size_t length = strftime(
        timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S%z", &localTime);
    if (length != 24 || outputSize < length + 2) return false;

    memcpy(output, timestamp, length - 2);
    output[length - 2] = ':';
    memcpy(output + length - 1, timestamp + length - 2, 3);
    output[length + 1] = '\0';
    return true;
}

static void startNtpSynchronization() {
    setLocalTimeZone();
    ntpTimeSynchronized = false;
    esp_sntp_set_time_sync_notification_cb(onNtpTimeSync);
    configTzTime(LOCAL_TIME_ZONE, NTP_SERVER_PRIMARY, NTP_SERVER_SECONDARY);
    Serial.println("[TIME] Started one-time NTP synchronization");
}

static uint16_t readU16LE(const uint8_t* data, size_t& offset) {
    const uint16_t value = (uint16_t)data[offset] |
                           ((uint16_t)data[offset + 1] << 8);
    offset += 2;
    return value;
}

static int16_t readI16LE(const uint8_t* data, size_t& offset) {
    return (int16_t)readU16LE(data, offset);
}

static int32_t readI32LE(const uint8_t* data, size_t& offset) {
    const uint32_t value = (uint32_t)data[offset] |
                           ((uint32_t)data[offset + 1] << 8) |
                           ((uint32_t)data[offset + 2] << 16) |
                           ((uint32_t)data[offset + 3] << 24);
    offset += 4;
    return (int32_t)value;
}

static void readFixedString(const uint8_t* data, size_t& offset,
                            char* output, size_t fieldSize) {
    memcpy(output, data + offset, fieldSize);
    output[fieldSize] = '\0';
    offset += fieldSize;
}

static void recordTiming(TimingWindow& window, const char* name, uint32_t elapsedUs) {
#if ENABLE_TIMING_LOGS
    window.count++;
    window.totalUs += elapsedUs;
    if (elapsedUs > window.maxUs) window.maxUs = elapsedUs;

    if (window.count >= 100) {
        Serial.printf("[TIMING] %s n=%lu avg=%lu max=%lu us\n",
                      name,
                      (unsigned long)window.count,
                      (unsigned long)(window.totalUs / window.count),
                      (unsigned long)window.maxUs);
        window = TimingWindow{};
    }
#else
    (void)window;
    (void)name;
    (void)elapsedUs;
#endif
}

void getTelemetryTimingSnapshot(uint32_t* updatedAtUs, uint32_t* generation) {
    portENTER_CRITICAL(&telemetryTimingMux);
    *updatedAtUs = telemetryUpdatedAtUs;
    *generation = telemetryGeneration;
    portEXIT_CRITICAL(&telemetryTimingMux);
}

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
#if ENABLE_HTTP_TELEMETRY
    if (WiFi.status() != WL_CONNECTED) { markGameDisconnected(); return false; }

    const uint32_t fetchStartedUs = micros();
    HTTPClient http;
    http.begin(TRUCK_DATA_URL);
    http.setTimeout(150);   // timeout ngắn để không block lâu
    const uint32_t httpStartedUs = micros();
    int code = http.GET();

    if (code == 200) {
        String payload = http.getString();
        recordTiming(httpTiming, "HTTP GET + body", micros() - httpStartedUs);
        //    Serial.println(payload);   // uncomment nếu muốn xem raw JSON
        JsonDocument doc;
        const uint32_t parseStartedUs = micros();
        DeserializationError err = deserializeJson(doc, payload);
        if (err) {
            recordTiming(parseUpdateTiming, "JSON parse + update", micros() - parseStartedUs);
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

            const uint32_t updatedAtUs = micros();
            portENTER_CRITICAL(&telemetryTimingMux);
            telemetryUpdatedAtUs = updatedAtUs;
            telemetryGeneration++;
            portEXIT_CRITICAL(&telemetryTimingMux);
            xSemaphoreGive(dataAccessMutex);
            recordTiming(parseUpdateTiming, "JSON parse + update", updatedAtUs - parseStartedUs);
            recordTiming(fetchTiming, "Valid JSON fetch", updatedAtUs - fetchStartedUs);
            http.end();
            return telemetryActive;
        }

        recordTiming(parseUpdateTiming, "JSON parse + update", micros() - parseStartedUs);
        http.end();
        return false;
    } else {
        recordTiming(httpTiming, "HTTP GET", micros() - httpStartedUs);
        Serial.println("[HTTP] Code: " + String(code));
        markGameDisconnected();
    }

    http.end();
    return false;
#else
    Serial.println("[HTTP] Telemetry fetching disabled; use UDP.");
    return false;
#endif
}

static bool applyTelemetryPacket(const uint8_t* packet, size_t packetSize,
                                 bool& gameConnected) {
    const uint32_t packetStartedUs = micros();
    constexpr size_t kPacketSize = 193;
    constexpr uint8_t kMagic = 0xE2;
    constexpr uint8_t kVersion = 2;
    if (packetSize != kPacketSize || packet[0] != kMagic ||
        packet[1] != kVersion) {
        return false;
    }

    uint8_t checksum = 0;
    for (size_t i = 0; i < kPacketSize - 1; i++) checksum ^= packet[i];
    if (checksum != packet[kPacketSize - 1]) return false;

    size_t offset = 2;
    const uint16_t sequence = readU16LE(packet, offset);
    const uint16_t flags = readU16LE(packet, offset);
    if (hasUdpSequence && millis() - lastUdpPacketMs < 2000) {
        const int16_t sequenceDelta = (int16_t)(sequence - lastUdpSequence);
        if (sequenceDelta <= 0) return false;
    }

    const int16_t speed = readI16LE(packet, offset);
    const int16_t rpm = readI16LE(packet, offset);
    const int16_t rpmMax = readI16LE(packet, offset);
    const int16_t fuel = readI16LE(packet, offset);
    const int16_t fuelCapacity = readI16LE(packet, offset);
    const int16_t airPressure = readI16LE(packet, offset);
    const int16_t waterTemperature = readI16LE(packet, offset);
    const int16_t oilTemperature = readI16LE(packet, offset);
    const int16_t speedLimit = readI16LE(packet, offset);
    const int16_t cruiseSpeed = readI16LE(packet, offset);
    const int16_t estimatedDistance = readI16LE(packet, offset);
    const int8_t gear = (int8_t)packet[offset++];
    const uint8_t truckWear = packet[offset++];
    const uint8_t trailerWear = packet[offset++];
    const uint8_t cargoWear = packet[offset++];
    const int32_t odometer = readI32LE(packet, offset);
    const int32_t income = readI32LE(packet, offset);
    const int32_t trailerMass = readI32LE(packet, offset);

    char make[17];
    char model[21];
    char licensePlate[17];
    char trailerName[65];
    char realTime[13];
    char gameTime[9];
    char remainingTime[13];
    readFixedString(packet, offset, make, 16);
    readFixedString(packet, offset, model, 20);
    readFixedString(packet, offset, licensePlate, 16);
    readFixedString(packet, offset, trailerName, 64);
    readFixedString(packet, offset, realTime, 12);
    readFixedString(packet, offset, gameTime, 8);
    readFixedString(packet, offset, remainingTime, 12);
    if (offset != kPacketSize - 1) return false;

    if (xSemaphoreTake(dataAccessMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return false;
    }

    truck.speed = abs((int)speed);
    truck.rpm = rpm;
    truck.rpmMax = rpmMax;
    truck.fuel = fuel;
    truck.fuelCapacity = fuelCapacity;
    truck.fuelWarningOn = (flags & (1U << 2)) != 0;
    truck.airPressure = airPressure;
    truck.airWarningOn = (flags & (1U << 3)) != 0;
    truck.displayedGear = gear;
    truck.waterTemperature = waterTemperature;
    truck.oilTemperature = oilTemperature;
    truck.speedLimit = speedLimit;
    truck.odo = odometer;
    truck.wear = truckWear;
    truck.engineOn = (flags & (1U << 4)) != 0;
    truck.parkingBreak = (flags & (1U << 5)) != 0;
    truck.lightsBeamHighOn = (flags & (1U << 6)) != 0;
    truck.lightsBeamLowOn = (flags & (1U << 7)) != 0;
    truck.blinkerLeftOn = (flags & (1U << 8)) != 0;
    truck.blinkerRightOn = (flags & (1U << 9)) != 0;
    truck.speedCtrl = cruiseSpeed;
    truck.speedCtrlOn = (flags & (1U << 10)) != 0;
    truck.parkingLight = false;
    truck.hazardWarningLights = false;
    truck.checkEngOn = false;

    if (truck.make != make) truck.make = make;
    if (truck.model != model) truck.model = model;
    if (truck.licensePlate != licensePlate) truck.licensePlate = licensePlate;
    if (truck.id.length() != 0) truck.id = "";

    gameConnected = (flags & (1U << 0)) != 0;
    game.connected = gameConnected;
    game.pause = (flags & (1U << 1)) != 0;
    if (game.time != gameTime) game.time = gameTime;
    if (game.realTime != realTime) game.realTime = realTime;

    job.income = income;
    if (job.remainingTime != remainingTime) job.remainingTime = remainingTime;

    trailer.attached = (flags & (1U << 11)) != 0;
    trailer.mass = trailerMass;
    trailer.wear = trailerWear;
    trailer.cargoWear = cargoWear;
    if (trailer.name != trailerName) trailer.name = trailerName;

    navigation.estimatedDistance = estimatedDistance;
    navigation.speed_limit = (int)round(speedLimit / 5.0) * 5;

    const uint32_t updatedAtUs = micros();
    portENTER_CRITICAL(&telemetryTimingMux);
    telemetryUpdatedAtUs = updatedAtUs;
    telemetryGeneration++;
    portEXIT_CRITICAL(&telemetryTimingMux);
    xSemaphoreGive(dataAccessMutex);

    lastUdpSequence = sequence;
    hasUdpSequence = true;
    lastUdpPacketMs = millis();
    recordTiming(udpTiming, "UDP receive + update", micros() - packetStartedUs);
    return true;
}

static bool receiveTelemetryUdp() {
    bool latestPacketConnected = false;
    bool receivedValidPacket = false;
    uint8_t packet[193];

    for (uint8_t packetsRead = 0; packetsRead < 4; packetsRead++) {
        const int packetSize = telemetryUdp.parsePacket();
        if (packetSize <= 0) break;

        if (packetSize != (int)sizeof(packet)) {
            while (telemetryUdp.available()) telemetryUdp.read();
            continue;
        }

        const int bytesRead = telemetryUdp.read(packet, sizeof(packet));
        if (bytesRead == (int)sizeof(packet)) {
            bool packetConnected = false;
            if (applyTelemetryPacket(packet, sizeof(packet), packetConnected)) {
                latestPacketConnected = packetConnected;
                receivedValidPacket = true;
            }
        }
    }

    return receivedValidPacket && latestPacketConnected;
}

bool waitForTelemetryUdp(uint32_t timeoutMs) {
    if (WiFi.status() != WL_CONNECTED) return false;

    if (!telemetryUdpStarted) {
        telemetryUdpStartAttempted = true;
        lastUdpStartAttemptMs = millis();
        if (!telemetryUdp.begin(TELEMETRY_UDP_PORT)) {
            Serial.printf("[UDP] Failed to listen on port %u during wake check\n",
                          (unsigned)TELEMETRY_UDP_PORT);
            return false;
        }
        telemetryUdpStarted = true;
        Serial.printf("[UDP] Listening for wake telemetry on port %u\n",
                      (unsigned)TELEMETRY_UDP_PORT);
    }

    const uint32_t checkStartedMs = millis();
    while (millis() - checkStartedMs < timeoutMs) {
        if (receiveTelemetryUdp()) return true;
        delay(10);
    }

    return false;
}

void sendBatteryVoltageUdpReport() {
#if ENABLE_BATTERY_UDP_REPORT
    if (batteryReportSent || WiFi.status() != WL_CONNECTED) return;

    const uint32_t now = millis();
    if (batteryReportAttempted &&
        now - lastBatteryReportAttemptMs < 1000) {
        return;
    }
    batteryReportAttempted = true;
    lastBatteryReportAttemptMs = now;

    const float batteryVoltage =
        analogReadMilliVolts(BAT_ADC_PIN) * 2.0f / 1000.0f;
    const char* wakeCause =
        esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER ? "timer" : "boot";

    char timestamp[32];
    const bool timeValid =
        getCurrentLocalDateTime(timestamp, sizeof(timestamp));
    char packet[128];
    const int packetLength = timeValid
        ? snprintf(packet, sizeof(packet),
                   "{\"voltage_v\":%.2f,\"wake\":\"%s\","
                   "\"time\":\"%s\",\"time_valid\":true}",
                   batteryVoltage, wakeCause, timestamp)
        : snprintf(packet, sizeof(packet),
                   "{\"voltage_v\":%.2f,\"wake\":\"%s\","
                   "\"time\":null,\"time_valid\":false}",
                   batteryVoltage, wakeCause);
    if (packetLength <= 0 || packetLength >= (int)sizeof(packet)) {
        Serial.println("[BATTERY UDP] Could not format report");
        return;
    }

    const IPAddress receiverIp(192, 168, 222, 49);
    if (!batteryReportUdp.beginPacket(receiverIp, BATTERY_UDP_REPORT_PORT)) {
        Serial.println("[BATTERY UDP] Could not start packet");
        return;
    }

    batteryReportUdp.write((const uint8_t*)packet, (size_t)packetLength);
    if (batteryReportUdp.endPacket()) {
        batteryReportSent = true;
        Serial.printf("[BATTERY UDP] Sent %.2f V to 192.168.222.49:%u "
                      "(%s, time=%s)\n",
                      batteryVoltage, (unsigned)BATTERY_UDP_REPORT_PORT,
                      wakeCause, timeValid ? timestamp : "unavailable");
    } else {
        Serial.println("[BATTERY UDP] Send failed; will retry");
    }
#endif
}

void enterLowPowerSleep(const char* reason) {
    if (isRescueModeActive()) {
        Serial.println("[POWER] Rescue mode active; deep sleep cancelled.");
        return;
    }

    Serial.printf("[POWER] %s; sleeping for %u seconds\n",
                  reason, (unsigned)SLEEP_WAKE_INTERVAL_SEC);

    if (telemetryUdpStarted) {
        telemetryUdp.stop();
        telemetryUdpStarted = false;
    }

    const time_t currentEpoch = time(nullptr);
    if (currentEpoch >= kMinimumValidEpoch) {
        retainedEpochSeconds = (int64_t)currentEpoch;
        retainedClockValid = true;
    } else {
        retainedClockValid = false;
    }

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
    const bool timerWake =
        esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER;
    bool ntpSyncStarted = timerWake;
    bool ntpSyncComplete = timerWake;
    uint32_t ntpSyncStartedAt = 0;

    for (;;) {
        if (isRescueModeActive()) {
            if (telemetryUdpStarted) {
                telemetryUdp.stop();
                telemetryUdpStarted = false;
            }
            otaService();
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // Reconnect nếu mất mạng
        if (WiFi.status() != WL_CONNECTED) {
            if (telemetryUdpStarted) {
                telemetryUdp.stop();
                telemetryUdpStarted = false;
            }
            markGameDisconnected();
            Serial.println("[WiFi] Reconnecting...");
            WiFi.disconnect();
            WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        otaService();    // nhận firmware qua WiFi (không chặn nếu không có phiên OTA)

        if (!ntpSyncStarted) {
            startNtpSynchronization();
            ntpSyncStarted = true;
            ntpSyncStartedAt = millis();
        }

        if (!ntpSyncComplete) {
            const time_t currentEpoch = time(nullptr);
            if (ntpTimeSynchronized && currentEpoch >= kMinimumValidEpoch) {
                esp_sntp_stop();
                retainedClockValid = true;
                ntpSyncComplete = true;
                Serial.println("[TIME] NTP synchronized; periodic sync stopped");
            } else if (millis() - ntpSyncStartedAt >= NTP_SYNC_TIMEOUT_MS) {
                esp_sntp_stop();
                retainedClockValid = false;
                ntpSyncComplete = true;
                Serial.println("[TIME] NTP synchronization timed out");
            }
        }

        if (ntpSyncComplete) sendBatteryVoltageUdpReport();

        const uint32_t now = millis();
        if (!telemetryUdpStarted &&
            (!telemetryUdpStartAttempted || now - lastUdpStartAttemptMs >= 5000)) {
            telemetryUdpStartAttempted = true;
            lastUdpStartAttemptMs = now;
            if (telemetryUdp.begin(TELEMETRY_UDP_PORT)) {
                telemetryUdpStarted = true;
                Serial.printf("[UDP] Listening for telemetry on port %u\n",
                              (unsigned)TELEMETRY_UDP_PORT);
            } else {
                Serial.printf("[UDP] Failed to listen on port %u\n",
                              (unsigned)TELEMETRY_UDP_PORT);
            }
        }

        if (telemetryUdpStarted && receiveTelemetryUdp()) {
            lastTelemetryMs = millis();
        }

        if (millis() - lastTelemetryMs >= TELEMETRY_TIMEOUT_MS) {
            enterLowPowerSleep("Telemetry timeout reached");
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
