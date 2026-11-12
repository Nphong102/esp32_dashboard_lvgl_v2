#include "battery_monitor.h"

#include "config.h"

#include <math.h>

#if BATTERY_MEASUREMENT_SOURCE == BATTERY_SOURCE_INA219
#include <Adafruit_INA219.h>
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace {
Adafruit_INA219 ina219(BATTERY_INA219_ADDRESS);
bool ina219Ready = false;
SemaphoreHandle_t ina219Mutex = nullptr;

static bool scanI2cBus() {
#if ENABLE_BATTERY_I2C_SCAN
    uint8_t deviceCount = 0;
    const uint32_t scanStartedMs = millis();
    Serial.printf("[I2C] Scanning SDA GPIO %u, SCL GPIO %u...\n",
                  (unsigned)BATTERY_I2C_SDA_PIN,
                  (unsigned)BATTERY_I2C_SCL_PIN);

    for (uint8_t address = 0x08; address <= 0x77; address++) {
        Wire.beginTransmission(address);
        const uint8_t error = Wire.endTransmission();
        if (error == 0) {
            Serial.printf("[I2C] Device found at 0x%02X\n", address);
            deviceCount++;
        } else if (error == 5) {
            Serial.printf("[I2C] Probe timed out at 0x%02X; stopping scan\n",
                          address);
            return false;
        }

        if (millis() - scanStartedMs >= 1500) {
            Serial.println("[I2C] Scan time limit reached; stopping scan");
            return false;
        }
    }

    if (deviceCount == 0) {
        Serial.println("[I2C] No devices found");
    } else {
        Serial.printf("[I2C] Scan complete: %u device(s)\n",
                      (unsigned)deviceCount);
    }
    return true;
#else
    return true;
#endif
}
}
#endif

bool initializeBatteryMonitor() {
#if BATTERY_MEASUREMENT_SOURCE == BATTERY_SOURCE_ADC
    pinMode(BAT_ADC_PIN, INPUT);
    return true;
#else
    ina219Mutex = xSemaphoreCreateMutex();
    if (ina219Mutex == nullptr) {
        Serial.println("[BATTERY] Could not create INA219 mutex");
        return false;
    }

    if (!Wire.begin(BATTERY_I2C_SDA_PIN, BATTERY_I2C_SCL_PIN)) {
        Serial.println("[BATTERY] Failed to initialize I2C");
        vSemaphoreDelete(ina219Mutex);
        ina219Mutex = nullptr;
        return false;
    }
    Wire.setClock(100000);
    Wire.setTimeOut(25);
    if (!scanI2cBus()) {
        vSemaphoreDelete(ina219Mutex);
        ina219Mutex = nullptr;
        return false;
    }
    if (!ina219.begin()) {
        Serial.printf("[BATTERY] INA219 not detected at I2C address 0x%02X\n",
                      BATTERY_INA219_ADDRESS);
        vSemaphoreDelete(ina219Mutex);
        ina219Mutex = nullptr;
        return false;
    }
    ina219.setCalibration_32V_2A();
    ina219Ready = true;
    delay(5);
    return true;
#endif
}

bool readBatteryMonitor(BatteryReading& reading) {
    reading = BatteryReading{};

#if BATTERY_MEASUREMENT_SOURCE == BATTERY_SOURCE_ADC
    uint32_t totalMilliVolts = 0;
    constexpr uint8_t sampleCount = 8;
    for (uint8_t sample = 0; sample < sampleCount; sample++) {
        totalMilliVolts += analogReadMilliVolts(BAT_ADC_PIN);
        delay(2);
    }

    reading.voltageV =
        (totalMilliVolts / (float)sampleCount) *
        BATTERY_ADC_DIVIDER_RATIO / 1000.0f;
    reading.currentMa = 0.0f;
    reading.currentValid = false;
#else
    if (!ina219Ready) return false;
    if (xSemaphoreTake(ina219Mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        Serial.println("[BATTERY] INA219 read timed out waiting for I2C");
        return false;
    }

    const float busVoltageV = ina219.getBusVoltage_V();
    const float shuntVoltageV = ina219.getShuntVoltage_mV() / 1000.0f;
    reading.voltageV = busVoltageV + shuntVoltageV;
    reading.currentMa = ina219.getCurrent_mA();
    reading.currentValid = isfinite(reading.currentMa);
    xSemaphoreGive(ina219Mutex);
#endif

    if (!isfinite(reading.voltageV) || reading.voltageV <= 0.0f) {
        return false;
    }

    if (reading.currentValid) {
        reading.powerW = reading.voltageV * (reading.currentMa / 1000.0f);
        if (reading.currentMa > 50.0f) {
            reading.chargeState = BATTERY_STATE_DISCHARGING;
        } else if (reading.currentMa < -50.0f) {
            reading.chargeState = BATTERY_STATE_CHARGING;
        } else {
            reading.chargeState = BATTERY_STATE_IDLE;
        }
    } else {
        reading.powerW = 0.0f;
        reading.chargeState = BATTERY_STATE_UNKNOWN;
    }

    const float fullVoltage = 4.2f;
    const float emptyVoltage = 3.0f;
    if (reading.voltageV >= fullVoltage) {
        reading.estimatedSocPercent = 100.0f;
    } else if (reading.voltageV <= emptyVoltage) {
        reading.estimatedSocPercent = 0.0f;
    } else {
        reading.estimatedSocPercent =
            ((reading.voltageV - emptyVoltage) / (fullVoltage - emptyVoltage)) * 100.0f;
    }
    reading.socValid = isfinite(reading.estimatedSocPercent) && reading.estimatedSocPercent >= 0.0f;

    reading.cellTempC = -999.0f;
    reading.monitorTempC = -999.0f;
    return true;
}
