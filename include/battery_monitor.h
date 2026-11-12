#pragma once

enum BatteryChargeState {
    BATTERY_STATE_UNKNOWN = 0,
    BATTERY_STATE_IDLE,
    BATTERY_STATE_CHARGING,
    BATTERY_STATE_DISCHARGING
};

struct BatteryReading {
    float voltageV = 0.0f;
    float currentMa = 0.0f;
    bool currentValid = false;
    float powerW = 0.0f;
    float estimatedSocPercent = 0.0f;
    bool socValid = false;
    float cellTempC = -999.0f;
    float monitorTempC = -999.0f;
    BatteryChargeState chargeState = BATTERY_STATE_UNKNOWN;
};

bool initializeBatteryMonitor();
bool readBatteryMonitor(BatteryReading& reading);
