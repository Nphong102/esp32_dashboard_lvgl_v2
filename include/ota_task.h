#pragma once
#include <Arduino.h>

// Trạng thái OTA — ghi từ WiFiTask (core 0), đọc từ DisplayTask (core 1)
extern volatile uint8_t otaState;   // 0 = không OTA, 1 = đang nạp, 2 = xong (sắp reboot), 3 = lỗi
extern volatile uint8_t otaPct;     // 0..100

// Gọi liên tục trong WiFiTask: tự ArduinoOTA.begin() khi WiFi đã kết nối, sau đó handle()
void otaService();
