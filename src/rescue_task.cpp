#include "rescue_task.h"

#include <Preferences.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>

#include "config.h"
#include "ota_task.h"

namespace {

constexpr char kApSsid[] = "ETS2-Dash-Setup";
constexpr char kAuthUser[] = "admin";
constexpr char kPreferencesNamespace[] = "network";

WebServer rescueServer(80);
TaskHandle_t rescueServerTaskHandle = nullptr;
TaskHandle_t rescueButtonTaskHandle = nullptr;
volatile bool rescueModeActive = false;
volatile bool rescueButtonMonitorActive = false;

bool uploadAuthorized = false;
bool uploadSucceeded = false;
bool uploadFailed = false;
bool uploadStarted = false;
size_t uploadExpectedSize = 0;

bool authenticateRequest() {
    if (rescueServer.authenticate(kAuthUser, OTA_PASSWORD)) {
        return true;
    }
    rescueServer.requestAuthentication();
    return false;
}

void handleRoot() {
    if (!authenticateRequest()) return;

    static const char page[] =
        "<!doctype html><html lang='vi'><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>ETS2 Dashboard | Cứu hộ</title><style>"
        ":root{color-scheme:dark;--bg:#08111f;--card:#111e30;--line:#263851;"
        "--muted:#9aabc1;--text:#edf4ff;--blue:#36a3ff;--green:#32d583;--red:#ff5d69}"
        "*{box-sizing:border-box}body{margin:0;background:radial-gradient(ellipse at top,#142943,var(--bg) 58%);"
        "color:var(--text);font:15px system-ui,-apple-system,Segoe UI,sans-serif;min-height:100vh}"
        ".wrap{max-width:760px;margin:auto;padding:28px 16px 48px}.hero{padding:10px 4px 22px}"
        ".eyebrow{color:#7fc4ff;text-transform:uppercase;letter-spacing:.14em;font-size:12px;font-weight:700}"
        "h1{font-size:clamp(25px,6vw,36px);margin:8px 0}.sub,.hint{color:var(--muted);line-height:1.55}"
        ".badge{display:inline-flex;align-items:center;gap:8px;border:1px solid #28553f;background:#10291f;"
        "color:#83efb4;border-radius:999px;padding:7px 11px;font-size:13px;margin-top:8px}"
        ".dot{width:8px;height:8px;border-radius:50%;background:var(--green);box-shadow:0 0 12px var(--green)}"
        ".card{background:linear-gradient(145deg,#142238,var(--card));border:1px solid var(--line);"
        "border-radius:18px;padding:22px;margin:14px 0;box-shadow:0 12px 32px #0003}"
        ".card h2{font-size:19px;margin:0 0 5px}.card>p{margin:0 0 18px}"
        "label{display:block;font-weight:600;margin:15px 0 7px}input{width:100%;margin-top:7px;"
        "padding:12px 13px;border:1px solid #344964;border-radius:10px;background:#091525;"
        "color:var(--text);font:inherit;outline:none}input:focus{border-color:var(--blue);"
        "box-shadow:0 0 0 3px #36a3ff22}input[type=checkbox]{width:auto;margin:0 8px 0 0;accent-color:var(--blue)}"
        ".check{display:flex;align-items:center;color:var(--muted);font-weight:400}.hint{font-size:13px;margin:8px 0}"
        ".actions{display:flex;flex-wrap:wrap;gap:10px;margin-top:18px}button{border:0;border-radius:10px;"
        "padding:11px 16px;color:#04101d;background:var(--blue);font:700 14px system-ui,sans-serif;cursor:pointer}"
        "button:hover{filter:brightness(1.1)}button:disabled{opacity:.55;cursor:wait}.secondary{background:#263951;color:var(--text)}"
        ".danger{background:#562630;color:#ffdce0}.file{padding:10px}.progress{height:12px;background:#07101c;"
        "border:1px solid var(--line);border-radius:99px;overflow:hidden;margin:16px 0 8px}"
        ".progress>i{display:block;width:0;height:100%;background:linear-gradient(90deg,#238bff,#47ddb0);transition:width .15s}"
        ".status{min-height:22px;color:var(--muted);font-size:14px}.foot{text-align:center;color:var(--muted);font-size:13px;margin-top:22px}"
        "@media(max-width:520px){.card{padding:18px}.wrap{padding:20px 12px 36px}}"
        "</style></head><body><main class='wrap'><header class='hero'>"
        "<div class='eyebrow'>ETS2 Dashboard</div><h1>Chế độ cứu hộ</h1>"
        "<p class='sub'>Cấu hình kết nối hoặc cập nhật firmware cho thiết bị.</p>"
        "<span class='badge'><i class='dot'></i> AP đang hoạt động · 192.168.4.1</span></header>"
        "<section class='card'><h2>Cấu hình mạng</h2><p class='hint'>Lưu cấu hình sẽ khởi động lại thiết bị.</p>"
        "<form id='settingsForm' method='post' action='/settings'>"
        "<label>Tên Wi-Fi (SSID)<input name='ssid' maxlength='32' autocomplete='off' required></label>"
        "<label>Mật khẩu Wi-Fi<input name='wifi_pass' type='password' maxlength='64' "
        "autocomplete='new-password' placeholder='Để trống để giữ mật khẩu hiện tại'></label>"
        "<label class='check'><input type='checkbox' name='open_wifi' value='1'>Mạng Wi-Fi không có mật khẩu</label>"
        "<label>Địa chỉ telemetry<input name='server_url' type='url' maxlength='192' "
        "placeholder='http://192.168.1.100:25555' required></label>"
        "<div class='actions'><button type='submit'>Lưu cấu hình &amp; khởi động lại</button></div></form></section>"
        "<section class='card'><h2>Cập nhật firmware</h2>"
        "<p class='hint'>Chọn đúng file firmware <b>.bin</b>. Không ngắt nguồn trong khi cập nhật.</p>"
        "<form id='uploadForm' enctype='multipart/form-data'>"
        "<input type='hidden' id='firmware_size' name='firmware_size' value='0'>"
        "<label>File firmware<input class='file' id='firmware' name='firmware' type='file' "
        "accept='.bin,application/octet-stream' required></label>"
        "<div class='progress' role='progressbar' aria-label='Tiến độ tải firmware'>"
        "<i id='progressBar'></i></div><div id='uploadStatus' class='status'>Chưa chọn file.</div>"
        "<div class='actions'><button id='uploadButton' type='submit'>Tải firmware lên</button></div></form></section>"
        "<section class='card'><h2>Khởi động lại thiết bị</h2>"
        "<p class='hint'>Dùng sau khi thay đổi cấu hình hoặc khi cần khởi động lại thủ công.</p>"
        "<div class='actions'><button id='restartButton' class='danger' type='button'>Khởi động lại</button></div>"
        "<div id='restartStatus' class='status'></div></section>"
        "<footer class='foot'>Kết nối Wi-Fi <b>ETS2-Dash-Setup</b> · Mở <b>http://192.168.4.1</b></footer></main>"
        "<script>"
        "const fileInput=document.getElementById('firmware'),sizeInput=document.getElementById('firmware_size');"
        "const uploadStatus=document.getElementById('uploadStatus'),progressBar=document.getElementById('progressBar');"
        "const uploadButton=document.getElementById('uploadButton');"
        "fileInput.addEventListener('change',()=>{const f=fileInput.files[0];"
        "sizeInput.value=f?f.size:0;uploadStatus.textContent=f?f.name+' · '+(f.size/1048576).toFixed(2)+' MB':'Chưa chọn file.';"
        "progressBar.style.width='0%'});"
        "document.getElementById('settingsForm').addEventListener('submit',e=>{"
        "if(!confirm('Lưu cấu hình và khởi động lại thiết bị?'))e.preventDefault()});"
        "document.getElementById('uploadForm').addEventListener('submit',e=>{e.preventDefault();"
        "const f=fileInput.files[0];if(!f){uploadStatus.textContent='Vui lòng chọn file .bin.';return}"
        "if(!f.name.toLowerCase().endsWith('.bin')){uploadStatus.textContent='File phải có đuôi .bin.';return}"
        "if(!confirm('Bắt đầu ghi firmware '+f.name+'? Thiết bị sẽ khởi động lại sau khi cập nhật thành công.'))return;"
        "const xhr=new XMLHttpRequest();const data=new FormData(e.currentTarget);uploadButton.disabled=true;"
        "uploadStatus.textContent='Đang tải firmware...';xhr.open('POST','/update');"
        "xhr.upload.onprogress=ev=>{if(ev.lengthComputable){const p=Math.floor(ev.loaded*100/ev.total);"
        "progressBar.style.width=p+'%';uploadStatus.textContent='Đang tải lên: '+p+'% ('+"
        "(ev.loaded/1048576).toFixed(2)+' / '+(ev.total/1048576).toFixed(2)+' MB)'}};"
        "xhr.onload=()=>{uploadButton.disabled=false;if(xhr.status>=200&&xhr.status<300){"
        "progressBar.style.width='100%';uploadStatus.textContent='Cập nhật thành công. Thiết bị đang khởi động lại...';}"
        "else uploadStatus.textContent='Cập nhật thất bại: '+(xhr.responseText||xhr.statusText)};"
        "xhr.onerror=()=>{uploadButton.disabled=false;uploadStatus.textContent='Mất kết nối trong lúc tải. Kiểm tra thiết bị trước khi thử lại.'};"
        "xhr.send(data)});"
        "document.getElementById('restartButton').addEventListener('click',()=>{"
        "if(!confirm('Bạn có chắc muốn khởi động lại thiết bị?'))return;"
        "const b=document.getElementById('restartButton'),s=document.getElementById('restartStatus');b.disabled=true;"
        "s.textContent='Đang yêu cầu khởi động lại...';fetch('/restart',{method:'POST'}).then(r=>{"
        "if(!r.ok)throw new Error('HTTP '+r.status);s.textContent='Thiết bị đang khởi động lại...';})"
        ".catch(err=>{b.disabled=false;s.textContent='Không thể khởi động lại: '+err.message})});"
        "</script></body></html>";
    rescueServer.send(200, "text/html; charset=utf-8", page);
}

bool storeString(Preferences& preferences, const char* key, const String& value) {
    preferences.putString(key, value);
    return preferences.getString(key, String()) == value;
}

void handleSettings() {
    if (!authenticateRequest()) return;

    const String ssid = rescueServer.arg("ssid");
    const String serverUrl = rescueServer.arg("server_url");
    const String password = rescueServer.arg("wifi_pass");
    const bool setOpenWifi = rescueServer.hasArg("open_wifi");

    if (ssid.isEmpty() || ssid.length() > 32 ||
        serverUrl.isEmpty() || serverUrl.length() > 192 ||
        (!serverUrl.startsWith("http://") && !serverUrl.startsWith("https://")) ||
        password.length() > 64) {
        rescueServer.send(400, "text/plain", "Invalid settings");
        return;
    }

    Preferences preferences;
    if (!preferences.begin(kPreferencesNamespace, false)) {
        rescueServer.send(500, "text/plain", "Could not open settings storage");
        return;
    }

    bool saved = storeString(preferences, "ssid", ssid) &&
                 storeString(preferences, "server_url", serverUrl);
    if (saved && (setOpenWifi || !password.isEmpty())) {
        saved = storeString(preferences, "wifi_pass", setOpenWifi ? String() : password);
    }
    preferences.end();

    if (!saved) {
        rescueServer.send(500, "text/plain", "Could not save settings");
        return;
    }

    rescueServer.send(200, "text/plain", "Settings saved; restarting");
    delay(500);
    ESP.restart();
}

void handleFirmwareUpload() {
    if (!authenticateRequest()) return;

    if (uploadSucceeded) {
        rescueServer.send(200, "text/plain", "Firmware accepted; restarting");
        delay(500);
        ESP.restart();
        return;
    }

    if (uploadFailed) {
        rescueServer.send(400, "text/plain", "Firmware upload failed");
        return;
    }

    rescueServer.send(400, "text/plain", "No firmware received");
}

void handleRestart() {
    if (!authenticateRequest()) return;

    rescueServer.send(200, "text/plain; charset=utf-8", "Dang khoi dong lai thiet bi...");
    delay(300);
    ESP.restart();
}

void processFirmwareUpload() {
    HTTPUpload& upload = rescueServer.upload();

    if (upload.status == UPLOAD_FILE_START) {
        uploadAuthorized = rescueServer.authenticate(kAuthUser, OTA_PASSWORD);
        uploadSucceeded = false;
        uploadFailed = !uploadAuthorized;
        uploadStarted = false;
        uploadExpectedSize = rescueServer.arg("firmware_size").toInt();
        if (!uploadAuthorized) return;

        if (!upload.filename.endsWith(".bin") || uploadExpectedSize == 0) {
            uploadFailed = true;
            otaState = 3;
            return;
        }

        webUpdateInProgress = true;
        otaState = 1;
        otaPct = 0;
        Serial.printf("[RESCUE] Firmware upload started: %s\n", upload.filename.c_str());
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (!uploadAuthorized || uploadFailed) return;

        if (!uploadStarted) {
            if (upload.currentSize == 0 || upload.buf[0] != 0xE9) {
                uploadFailed = true;
                otaState = 3;
                webUpdateInProgress = false;
                return;
            }
            if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
                uploadFailed = true;
                otaState = 3;
                webUpdateInProgress = false;
                Serial.printf("[RESCUE] Update.begin failed: %s\n",
                              Update.errorString());
                return;
            }
            uploadStarted = true;
        }

        const size_t written = Update.write(upload.buf, upload.currentSize);
        if (written != upload.currentSize) {
            uploadFailed = true;
            otaState = 3;
            webUpdateInProgress = false;
            Update.abort();
            Serial.printf("[RESCUE] Firmware write failed: %s\n",
                          Update.errorString());
            return;
        }

        if (uploadExpectedSize > 0) {
            const size_t received = upload.totalSize + upload.currentSize;
            const uint64_t percent =
                (uint64_t)received * 100 / uploadExpectedSize;
            otaPct = (uint8_t)(percent > 99 ? 99 : percent);
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (!uploadAuthorized || uploadFailed || !uploadStarted) {
            webUpdateInProgress = false;
            return;
        }

        if (!Update.end(true)) {
            uploadFailed = true;
            otaState = 3;
            Serial.printf("[RESCUE] Firmware finalize failed: %s\n",
                          Update.errorString());
        } else {
            uploadSucceeded = true;
            otaPct = 100;
            otaState = 2;
            Serial.printf("[RESCUE] Firmware upload complete: %u bytes\n",
                          upload.totalSize);
        }
        webUpdateInProgress = false;
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
        if (uploadStarted) Update.abort();
        uploadFailed = true;
        otaState = 3;
        webUpdateInProgress = false;
        Serial.println("[RESCUE] Firmware upload aborted");
    }
}

void rescueServerTask(void*) {
    for (;;) {
        rescueServer.handleClient();
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

void rescueButtonMonitorTask(void*) {
    bool pressInProgress = false;
    bool attemptedForPress = false;
    uint32_t pressStartedAt = 0;
    Serial.printf("[RESCUE] Monitoring BOOT GPIO%d through boot sequence; hold %u ms\n",
                  RESCUE_BOOT_PIN, (unsigned)RESCUE_BOOT_HOLD_MS);

    while (rescueButtonMonitorActive) {
        if (digitalRead(RESCUE_BOOT_PIN) == LOW) {
            if (!pressInProgress) {
                pressStartedAt = millis();
                pressInProgress = true;
                attemptedForPress = false;
                Serial.println("[RESCUE] BOOT press detected");
            } else if (!attemptedForPress &&
                       millis() - pressStartedAt >= RESCUE_BOOT_HOLD_MS) {
                attemptedForPress = true;
                if (startRescueMode()) {
                    Serial.println("[RESCUE] Active at http://192.168.4.1");
                    rescueButtonMonitorActive = false;
                } else {
                    Serial.println("[RESCUE] Failed to start rescue AP/web server");
                }
            }
        } else {
            pressInProgress = false;
            attemptedForPress = false;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    rescueButtonTaskHandle = nullptr;
    vTaskDelete(nullptr);
}

}  // namespace

void loadSavedNetworkSettings() {
    Preferences preferences;
    if (!preferences.begin(kPreferencesNamespace, true)) {
        Serial.println("[RESCUE] Settings storage unavailable; using compiled defaults");
        return;
    }

    preferences.getString("ssid", WIFI_SSID, sizeof(WIFI_SSID));
    preferences.getString("wifi_pass", WIFI_PASSWORD, sizeof(WIFI_PASSWORD));
    preferences.getString("server_url", TRUCK_DATA_URL, sizeof(TRUCK_DATA_URL));
    preferences.end();
}

bool startRescueButtonMonitor() {
    if (rescueButtonTaskHandle != nullptr) return true;

    pinMode(RESCUE_BOOT_PIN, INPUT_PULLUP);
    rescueButtonMonitorActive = true;
    if (xTaskCreatePinnedToCore(
            rescueButtonMonitorTask, "RescueButton", 3072, nullptr, 0,
            &rescueButtonTaskHandle, 0) != pdPASS) {
        rescueButtonMonitorActive = false;
        rescueButtonTaskHandle = nullptr;
        return false;
    }

    return true;
}

void stopRescueButtonMonitor() {
    rescueButtonMonitorActive = false;
}

bool startRescueMode() {
    if (rescueModeActive) return true;

    WiFi.mode(WIFI_AP_STA);
    const IPAddress apIp(192, 168, 4, 1);
    if (!WiFi.softAPConfig(apIp, apIp, IPAddress(255, 255, 255, 0)) ||
        !WiFi.softAP(kApSsid, nullptr, 6)) {
        Serial.println("[RESCUE] Could not start setup access point");
        return false;
    }

    rescueServer.on("/", HTTP_GET, handleRoot);
    rescueServer.on("/settings", HTTP_POST, handleSettings);
    rescueServer.on("/restart", HTTP_POST, handleRestart);
    rescueServer.on("/update", HTTP_POST, handleFirmwareUpload, processFirmwareUpload);
    rescueServer.begin();

    if (xTaskCreatePinnedToCore(
            rescueServerTask, "RescueWeb", 6144, nullptr, 0,
            &rescueServerTaskHandle, 0) != pdPASS) {
        rescueServer.stop();
        WiFi.softAPdisconnect(true);
        Serial.println("[RESCUE] Could not create web server task");
        return false;
    }

    rescueModeActive = true;
    Serial.printf("[RESCUE] AP started: %s (%s)\n",
                  kApSsid, WiFi.softAPIP().toString().c_str());
    return true;
}

bool isRescueModeActive() {
    return rescueModeActive;
}
