/**
 * ota_task.cpp — nạp firmware qua WiFi (ArduinoOTA)
 * Không đụng LVGL ở đây (LVGL chỉ chạy trong DisplayTask) — chỉ cập nhật
 * otaState / otaPct, DisplayTask sẽ tự vẽ màn hình tiến trình.
 */
#include <ArduinoOTA.h>
#include "config.h"
#include "ota_task.h"

volatile uint8_t otaState = 0;
volatile uint8_t otaPct   = 0;
volatile bool webUpdateInProgress = false;

static bool otaStarted = false;

static void otaBegin() {
    ArduinoOTA.setHostname(OTA_HOSTNAME);
    ArduinoOTA.setPassword(OTA_PASSWORD);

    ArduinoOTA.onStart([]() {
        otaPct   = 0;
        otaState = 1;
        Serial.println("[OTA] Start");
    });
    ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
        otaPct = total ? (uint8_t)((uint64_t)done * 100 / total) : 0;
    });
    ArduinoOTA.onEnd([]() {
        otaPct   = 100;
        otaState = 2;
        Serial.println("[OTA] Done — rebooting");
    });
    ArduinoOTA.onError([](ota_error_t err) {
        otaState = 3;
        Serial.printf("[OTA] Error %u\n", (unsigned)err);
    });

    ArduinoOTA.begin();
    Serial.printf("[OTA] Ready: %s.local  (%s)\n", OTA_HOSTNAME, WiFi.localIP().toString().c_str());
}

void otaService() {
    if (webUpdateInProgress) return;
    if (WiFi.status() != WL_CONNECTED) return;
    if (!otaStarted) { otaBegin(); otaStarted = true; }
    ArduinoOTA.handle();     // khi có phiên nạp, hàm này chạy tới khi xong rồi tự reboot
}
