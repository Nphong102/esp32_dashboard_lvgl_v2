#include "rescue_task.h"

#include <Preferences.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <SdPsramImages.h>

#include "config.h"
#include "ota_task.h"

namespace {

constexpr char kApSsid[] = "ETS2-Dash-Setup";
constexpr char kAuthUser[] = "admin";
constexpr char kPreferencesNamespace[] = "network";
constexpr char kImageDirectory[] = "/ets";
constexpr char kImageUploadTempPath[] = "/ets/.image-upload.tmp";
constexpr size_t kMaxManageableImages = 32;
constexpr size_t kMaxIdleImages = 12;
constexpr size_t kMaxImageUploadBytes = 2 * 1024 * 1024;
constexpr size_t kMaxImageNameLength = sdpsram::kNameLen - 1;

WebServer rescueServer(80);
TaskHandle_t rescueServerTaskHandle = nullptr;
TaskHandle_t rescueButtonTaskHandle = nullptr;
volatile bool rescueModeActive = false;
volatile bool rescueButtonMonitorActive = false;
volatile bool rescueImageCatalogReady = false;

bool uploadAuthorized = false;
bool uploadSucceeded = false;
bool uploadFailed = false;
bool uploadStarted = false;
size_t uploadExpectedSize = 0;
File imageUploadFile;
String imageUploadName;
char imageUploadBackupPath[96] = {0};
bool imageUploadAuthorized = false;
bool imageUploadRequestStarted = false;
bool imageUploadSucceeded = false;
bool imageUploadFailed = false;
size_t imageUploadBytes = 0;
uint16_t imageUploadErrorStatus = 400;
const char* imageUploadErrorMessage = "Image upload failed";

bool storeString(Preferences& preferences, const char* key, const String& value);

void failImageUpload(uint16_t status, const char* message) {
    if (imageUploadFile) imageUploadFile.close();
    if (rescueImageCatalogReady) SD_MMC.remove(kImageUploadTempPath);
    imageUploadFailed = true;
    imageUploadErrorStatus = status;
    imageUploadErrorMessage = message;
}

bool isValidImageUploadName(const String& name) {
    if (name.length() < 5 || name.length() > kMaxImageNameLength ||
        name.startsWith(".") || name.indexOf("..") >= 0 ||
        !name.substring(name.length() - 4).equalsIgnoreCase(".bin")) {
        return false;
    }

    for (size_t i = 0; i < name.length(); ++i) {
        const char c = name[i];
        const bool valid = (c >= 'a' && c <= 'z') ||
                           (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') ||
                           c == '_' || c == '-' || c == '.';
        if (!valid) return false;
    }
    return true;
}

bool authenticateRequest() {
    if (rescueServer.authenticate(kAuthUser, OTA_PASSWORD)) {
        return true;
    }
    rescueServer.requestAuthentication();
    return false;
}

String escapeHtml(const char* value) {
    String escaped;
    if (value == nullptr) return escaped;
    for (const char* p = value; *p; ++p) {
        switch (*p) {
            case '&': escaped += F("&amp;"); break;
            case '<': escaped += F("&lt;"); break;
            case '>': escaped += F("&gt;"); break;
            case '"': escaped += F("&quot;"); break;
            case '\'': escaped += F("&#39;"); break;
            default: escaped += *p; break;
        }
    }
    return escaped;
}

bool hasIdleImage(const String& selectedNames, const char* name) {
    int start = 0;
    while (start < selectedNames.length()) {
        int end = selectedNames.indexOf('\n', start);
        if (end < 0) end = selectedNames.length();
        if (selectedNames.substring(start, end) == name) return true;
        start = end + 1;
    }
    return false;
}

bool listedImageIsValid(const char* name, sdpsram::FileEntry* entries, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (entries[i].valid && strcmp(entries[i].name, name) == 0) return true;
    }
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
        "<span class='badge'><i class='dot'></i> AP đang hoạt động · 192.168.4.1</span>"
        "<p><a href='/display'>Quản lý ảnh logo và màn hình chờ</a></p></header>"
        "<section class='card'><h2>Cấu hình mạng</h2><p class='hint'>Lưu cấu hình sẽ khởi động lại thiết bị.</p>"
        "<form id='settingsForm' method='post' action='/settings'>"
        "<label>Tên Wi-Fi (SSID)<input name='ssid' maxlength='32' autocomplete='off' required></label>"
        "<label>Mật khẩu Wi-Fi<input name='wifi_pass' type='password' maxlength='64' "
        "autocomplete='new-password' placeholder='Để trống để giữ mật khẩu hiện tại'></label>"
        "<label class='check'><input type='checkbox' name='open_wifi' value='1'>Mạng Wi-Fi không có mật khẩu</label>"
        "<label>Địa chỉ telemetry<input name='server_url' type='url' maxlength='192' "
        "placeholder='http://192.168.1.100:25555' required></label>"
        "<div class='actions'><button type='submit'>Lưu cấu hình &amp; khởi động lại</button></div></form></section>"
        "<section class='card'><h2>Mã PIN khởi động</h2>"
        "<p class='hint'>Nhập đúng 4 chữ số để đặt PIN. Để tắt khóa, nhấn Xóa PIN. Thay đổi có hiệu lực từ lần khởi động kế tiếp. Mặc định ban đầu là 3258.</p>"
        "<form method='post' action='/pin'>"
        "<label>Mã PIN mới<input name='pin' type='password' inputmode='numeric' pattern='[0-9]{4}' "
        "minlength='4' maxlength='4' autocomplete='new-password'></label>"
        "<div class='actions'><button type='submit'>Lưu mã PIN</button>"
        "<button class='danger' type='submit' name='delete' value='1' formnovalidate "
        "onclick=\"return confirm('Xóa PIN khởi động?')\">Xóa PIN</button></div></form></section>"
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

void handleDisplayImages() {
    if (!authenticateRequest()) return;

    if (!rescueImageCatalogReady) {
        rescueServer.send(503, "text/plain; charset=utf-8",
                          "SD card is not ready yet; reload this page shortly");
        return;
    }

    static sdpsram::FileEntry entries[kMaxManageableImages];
    const size_t total = sdpsram::ImageStore::listDir(
        kImageDirectory, entries, kMaxManageableImages);
    const size_t shown = total < kMaxManageableImages ? total : kMaxManageableImages;

    RescueDisplayImageSettings settings;
    loadSavedDisplayImageSettings(settings);

    String html =
        "<!doctype html><html lang='vi'><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Chọn ảnh dashboard</title><style>"
        "body{font:16px system-ui;background:#08111f;color:#edf4ff;max-width:760px;margin:24px auto;padding:0 16px}"
        "section{background:#142238;border:1px solid #263851;border-radius:14px;padding:18px;margin:14px 0}"
        "label{display:block;margin:10px 0}select,button,input{padding:10px;font:inherit;border-radius:8px}"
        "select{max-width:100%;background:#091525;color:#edf4ff}button{border:0;background:#36a3ff;font-weight:700}"
        "input{max-width:100%;background:#091525;color:#edf4ff;border:1px solid #263851}"
        "small,p{color:#9aabc1;line-height:1.5}.file{overflow-wrap:anywhere}"
        ".upload-item{margin:12px 0;padding:12px;background:#091525;border-radius:9px}"
        ".upload-name{overflow-wrap:anywhere;font-weight:600}.upload-track{height:10px;background:#263851;"
        "border-radius:8px;overflow:hidden;margin:8px 0}.upload-progress{height:100%;width:0;"
        "background:#36a3ff;transition:width .1s}.upload-status{font-size:13px;color:#9aabc1}</style>"
        "<h1>Quản lý ảnh</h1><p><a href='/' style='color:#7fc4ff'>Quay lại cứu hộ</a></p>"
        "<p>Chọn ảnh LVGL 8 dạng .bin trong /ets. Cấu hình có hiệu lực sau khi khởi động lại.</p>";

    html += "<section><h2>Tải ảnh mới lên thẻ SD</h2>"
            "<p>Chọn được nhiều ảnh cùng lúc. Mỗi ảnh được gửi lần lượt; chỉ nhận ảnh LVGL 8 hợp lệ, tối đa 2 MB/ảnh.</p>"
            "<form id='imageUploadForm'>"
            "<label>File ảnh .bin<input id='imageFiles' type='file' accept='.bin,application/octet-stream' multiple required></label>"
            "<button id='imageUploadButton' type='submit'>Tải ảnh lên</button></form>"
            "<div id='imageUploadList' aria-live='polite'></div></section>"
            "<script>"
            "const imageForm=document.getElementById('imageUploadForm');"
            "imageForm.addEventListener('submit',async ev=>{ev.preventDefault();"
            "const picker=document.getElementById('imageFiles'),button=document.getElementById('imageUploadButton');"
            "const files=Array.from(picker.files),list=document.getElementById('imageUploadList');"
            "if(!files.length)return;button.disabled=true;list.replaceChildren();let failed=0;"
            "for(const file of files){"
            "const item=document.createElement('div');item.className='upload-item';"
            "const name=document.createElement('div');name.className='upload-name';name.textContent=file.name;"
            "const track=document.createElement('div');track.className='upload-track';"
            "const bar=document.createElement('div');bar.className='upload-progress';track.appendChild(bar);"
            "const status=document.createElement('div');status.className='upload-status';status.textContent='Đang chờ...';"
            "item.append(name,track,status);list.appendChild(item);"
            "try{await new Promise((resolve,reject)=>{const xhr=new XMLHttpRequest();"
            "xhr.open('POST','/display/upload');xhr.upload.onprogress=e=>{if(e.lengthComputable){"
            "const p=Math.floor(e.loaded*100/e.total);bar.style.width=p+'%';status.textContent='Đang tải: '+p+'%'}};"
            "xhr.onload=()=>{if(xhr.status>=200&&xhr.status<300){bar.style.width='100%';"
            "status.textContent='Tải thành công';resolve()}else reject(new Error(xhr.responseText||('HTTP '+xhr.status)))};"
            "xhr.onerror=()=>reject(new Error('Mất kết nối'));const data=new FormData();data.append('image',file,file.name);xhr.send(data)});"
            "}catch(err){failed++;bar.style.background='#ff5d69';status.textContent='Lỗi: '+err.message}}"
            "button.disabled=false;if(!failed){setTimeout(()=>location.reload(),700)}else{"
            "const summary=document.createElement('p');summary.textContent='Có '+failed+' file tải thất bại. "
            "Các file thành công vẫn được giữ lại; hãy thử lại các file lỗi.';list.prepend(summary)}});"
            "</script></section>";

    if (total == 0) {
        html += "<section><b>Chưa đọc được ảnh từ thẻ SD.</b>"
                "<p>Kiểm tra thẻ, thư mục /ets và thử tải lại trang sau khi firmware khởi tạo SD.</p></section>";
    } else {
        if (total > kMaxManageableImages) {
            html += "<p>Đang hiển thị tối đa 32 ảnh đầu tiên.</p>";
        }
        html += "<form method='post' action='/display/save'>"
                "<section><h2>Logo khởi động 1</h2><label><select name='logo1'>";
        html += "<option value='builtin'";
        if (settings.bootLogo1.isEmpty()) html += " selected";
        html += ">Logo tích hợp sẵn</option>";
        for (size_t i = 0; i < shown; ++i) {
            if (!entries[i].valid) continue;
            const String name = entries[i].name;
            html += "<option value='";
            html += escapeHtml(entries[i].name);
            html += "'";
            if (settings.bootLogo1 == name) html += " selected";
            html += ">";
            html += escapeHtml(entries[i].name);
            html += " (";
            html += String(entries[i].w);
            html += "x";
            html += String(entries[i].h);
            html += ")</option>";
        }
        html += "</select></label></section><section><h2>Logo khởi động 2</h2>"
                "<label><select name='logo2'><option value='none'";
        if (settings.bootLogo2.isEmpty()) html += " selected";
        html += ">Không dùng logo thứ hai</option>";
        for (size_t i = 0; i < shown; ++i) {
            if (!entries[i].valid) continue;
            const String name = entries[i].name;
            html += "<option value='";
            html += escapeHtml(entries[i].name);
            html += "'";
            if (settings.bootLogo2 == name) html += " selected";
            html += ">";
            html += escapeHtml(entries[i].name);
            html += " (";
            html += String(entries[i].w);
            html += "x";
            html += String(entries[i].h);
            html += ")</option>";
        }
        html += "</select></label></section><section><h2>Ảnh màn hình chờ</h2>"
                "<label><input type='radio' name='idle_source' value='builtin'";
        if (!settings.useSdIdleImages) html += " checked";
        html += "> Dùng slideshow mặc định trong firmware</label>"
                "<label><input type='radio' name='idle_source' value='sd'";
        if (settings.useSdIdleImages) html += " checked";
        html += "> Dùng các ảnh đã chọn trên thẻ SD</label>"
                "<p><small>Chọn tối đa 12 ảnh khi dùng SD. Chỉ các file có header LVGL hợp lệ mới được liệt kê.</small></p>";
        for (size_t i = 0; i < shown; ++i) {
            if (!entries[i].valid) continue;
            html += "<label class='file'><input type='checkbox' name='idle' value='";
            html += escapeHtml(entries[i].name);
            html += "'";
            if (hasIdleImage(settings.idleImages, entries[i].name)) html += " checked";
            html += "> ";
            html += escapeHtml(entries[i].name);
            html += " — ";
            html += String(entries[i].w);
            html += "x";
            html += String(entries[i].h);
            html += ", ";
            html += String((unsigned)(entries[i].size / 1024));
            html += " KB</label>";
        }
        html += "</section><button type='submit'>Lưu lựa chọn ảnh</button></form>";
    }
    html += "</html>";
    rescueServer.send(200, "text/html; charset=utf-8", html);
}

void handleSaveDisplayImages() {
    if (!authenticateRequest()) return;

    if (!rescueImageCatalogReady) {
        rescueServer.send(503, "text/plain; charset=utf-8", "SD card is not ready");
        return;
    }

    static sdpsram::FileEntry entries[kMaxManageableImages];
    const size_t total = sdpsram::ImageStore::listDir(
        kImageDirectory, entries, kMaxManageableImages);
    const size_t shown = total < kMaxManageableImages ? total : kMaxManageableImages;
    if (shown == 0) {
        rescueServer.send(503, "text/plain; charset=utf-8", "SD image catalog is unavailable");
        return;
    }

    const String logo1 = rescueServer.arg("logo1");
    const String logo2 = rescueServer.arg("logo2");
    const String idleSource = rescueServer.arg("idle_source");
    if ((logo1 != "builtin" && !listedImageIsValid(logo1.c_str(), entries, shown)) ||
        (logo2 != "none" && !listedImageIsValid(logo2.c_str(), entries, shown)) ||
        (idleSource != "builtin" && idleSource != "sd")) {
        rescueServer.send(400, "text/plain; charset=utf-8", "Invalid image selection");
        return;
    }

    String idleImages;
    size_t selectedCount = 0;
    for (int i = 0; i < rescueServer.args(); ++i) {
        if (rescueServer.argName(i) != "idle") continue;
        const String name = rescueServer.arg(i);
        if (!listedImageIsValid(name.c_str(), entries, shown)) {
            rescueServer.send(400, "text/plain; charset=utf-8", "Invalid idle image");
            return;
        }
        bool duplicate = false;
        for (int start = 0; start < idleImages.length();) {
            int end = idleImages.indexOf('\n', start);
            if (end < 0) end = idleImages.length();
            if (idleImages.substring(start, end) == name) duplicate = true;
            start = end + 1;
        }
        if (duplicate) continue;
        if (++selectedCount > kMaxIdleImages) {
            rescueServer.send(400, "text/plain; charset=utf-8",
                              "Select no more than 12 idle images");
            return;
        }
        if (!idleImages.isEmpty()) idleImages += '\n';
        idleImages += name;
    }

    if (idleSource == "sd" && selectedCount == 0) {
        rescueServer.send(400, "text/plain; charset=utf-8",
                          "Select at least one SD image for the idle slideshow");
        return;
    }

    Preferences preferences;
    if (!preferences.begin(kPreferencesNamespace, false)) {
        rescueServer.send(500, "text/plain; charset=utf-8",
                          "Could not open settings storage");
        return;
    }
    const String savedLogo1 = logo1 == "builtin" ? String() : logo1;
    const String savedLogo2 = logo2 == "none" ? String() : logo2;
    bool saved = storeString(preferences, "img_logo1", savedLogo1) &&
                 storeString(preferences, "img_logo2", savedLogo2) &&
                 storeString(preferences, "idle_images", idleImages);
    if (saved) {
        const bool sdIdle = idleSource == "sd";
        const bool idleModeSaved = preferences.putBool("img_sd_idle", sdIdle) > 0;
        const bool customFlagSaved = preferences.putBool("img_custom", true) > 0;
        saved = idleModeSaved && customFlagSaved &&
                preferences.getBool("img_sd_idle", false) == sdIdle &&
                preferences.getBool("img_custom", false);
    }
    preferences.end();

    if (!saved) {
        rescueServer.send(500, "text/plain; charset=utf-8",
                          "Could not save image selection");
        return;
    }

    rescueServer.sendHeader("Location", "/display");
    rescueServer.send(303, "text/plain", "Image selections saved; restart to apply");
}

void handleImageUploadResult() {
    if (!authenticateRequest()) {
        imageUploadRequestStarted = false;
        return;
    }

    if (!imageUploadRequestStarted) {
        rescueServer.send(400, "text/plain; charset=utf-8", "No image received");
        return;
    }

    if (imageUploadSucceeded) {
        rescueServer.send(200, "text/plain; charset=utf-8", "Image uploaded");
    } else {
        rescueServer.send(imageUploadErrorStatus, "text/plain; charset=utf-8",
                          imageUploadErrorMessage);
    }
    imageUploadRequestStarted = false;
    imageUploadAuthorized = false;
    imageUploadName = "";
}

void processImageUpload() {
    HTTPUpload& upload = rescueServer.upload();

    if (upload.status == UPLOAD_FILE_START) {
        imageUploadRequestStarted = true;
        imageUploadAuthorized = rescueServer.authenticate(kAuthUser, OTA_PASSWORD);
        imageUploadSucceeded = false;
        imageUploadFailed = !imageUploadAuthorized;
        imageUploadBytes = 0;
        imageUploadErrorStatus = 400;
        imageUploadErrorMessage = "Image upload failed";
        imageUploadName = "";
        imageUploadBackupPath[0] = '\0';
        if (!imageUploadAuthorized) return;

        if (!rescueImageCatalogReady) {
            failImageUpload(503, "SD card is not ready");
            return;
        }

        String submittedName = upload.filename;
        int separator = submittedName.lastIndexOf('/');
        const int backslash = submittedName.lastIndexOf('\\');
        if (backslash > separator) separator = backslash;
        imageUploadName = submittedName.substring(separator + 1);
        if (!isValidImageUploadName(imageUploadName)) {
            failImageUpload(400, "Use a simple .bin filename (letters, numbers, - or _)");
            return;
        }

        if (!SD_MMC.exists(kImageDirectory) && !SD_MMC.mkdir(kImageDirectory)) {
            failImageUpload(500, "Could not create /ets on the SD card");
            return;
        }

        char targetPath[80];
        snprintf(targetPath, sizeof(targetPath), "%s/%s",
                 kImageDirectory, imageUploadName.c_str());
        snprintf(imageUploadBackupPath, sizeof(imageUploadBackupPath),
                 "%s/.%s.bak", kImageDirectory, imageUploadName.c_str());

        if (SD_MMC.exists(imageUploadBackupPath)) {
            const bool recovered = SD_MMC.exists(targetPath)
                ? SD_MMC.remove(imageUploadBackupPath)
                : SD_MMC.rename(imageUploadBackupPath, targetPath);
            if (!recovered) {
                failImageUpload(500, "Could not recover previous image from backup");
                return;
            }
        }

        if (SD_MMC.exists(kImageUploadTempPath) &&
            !SD_MMC.remove(kImageUploadTempPath)) {
            failImageUpload(500, "Could not clear previous temporary upload");
            return;
        }
        imageUploadFile = SD_MMC.open(kImageUploadTempPath, FILE_WRITE);
        if (!imageUploadFile) {
            failImageUpload(500, "Could not create temporary image file on SD");
            return;
        }
        Serial.printf("[RESCUE] Image upload started: %s\n", imageUploadName.c_str());
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (!imageUploadAuthorized || imageUploadFailed) return;

        if (upload.currentSize == 0 || upload.buf == nullptr ||
            upload.currentSize > kMaxImageUploadBytes - imageUploadBytes) {
            failImageUpload(400, "Image is empty or exceeds the 2 MB limit");
            return;
        }
        const size_t written = imageUploadFile.write(upload.buf, upload.currentSize);
        if (written != upload.currentSize) {
            failImageUpload(500, "Could not write image data to SD card");
            Serial.println("[RESCUE] Image upload write failed");
            return;
        }
        imageUploadBytes += written;
    } else if (upload.status == UPLOAD_FILE_END) {
        if (!imageUploadAuthorized || imageUploadFailed) return;
        if (imageUploadFile) imageUploadFile.close();

        char targetPath[80];
        snprintf(targetPath, sizeof(targetPath), "%s/%s",
                 kImageDirectory, imageUploadName.c_str());
        if (imageUploadBytes < 8 ||
            !sdpsram::ImageStore::isValidFile(kImageUploadTempPath)) {
            failImageUpload(400, "Invalid LVGL 8 .bin image");
            return;
        }

        bool originalBackedUp = false;
        if (SD_MMC.exists(targetPath)) {
            if (!SD_MMC.rename(targetPath, imageUploadBackupPath)) {
                failImageUpload(500, "Could not preserve existing image");
                return;
            }
            originalBackedUp = true;
        }

        if (!SD_MMC.rename(kImageUploadTempPath, targetPath)) {
            const bool restored = !originalBackedUp ||
                                  SD_MMC.rename(imageUploadBackupPath, targetPath);
            if (!restored) {
                Serial.printf("[RESCUE] Could not restore previous image: %s\n",
                              imageUploadBackupPath);
            }
            failImageUpload(500, "Could not finalize image on SD card");
            return;
        }

        if (originalBackedUp && !SD_MMC.remove(imageUploadBackupPath)) {
            Serial.printf("[RESCUE] Could not remove image backup: %s\n",
                          imageUploadBackupPath);
        }
        imageUploadSucceeded = true;
        Serial.printf("[RESCUE] Image upload complete: %s (%u bytes)\n",
                      imageUploadName.c_str(), (unsigned)imageUploadBytes);
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
        failImageUpload(400, "Image upload was aborted");
        Serial.println("[RESCUE] Image upload aborted");
    }
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

void handleAccessPin() {
    if (!authenticateRequest()) return;

    const bool deletePin = rescueServer.arg("delete") == "1";
    const String pin = rescueServer.arg("pin");
    if (!deletePin && (pin.length() != 4 ||
        !isDigit(pin[0]) || !isDigit(pin[1]) ||
        !isDigit(pin[2]) || !isDigit(pin[3]))) {
        rescueServer.send(400, "text/plain; charset=utf-8",
                          "PIN must contain exactly four digits");
        return;
    }

    Preferences preferences;
    if (!preferences.begin(kPreferencesNamespace, false)) {
        rescueServer.send(500, "text/plain; charset=utf-8",
                          "Could not open settings storage");
        return;
    }
    const String pinToSave = deletePin ? String() : pin;
    preferences.putString("boot_pin", pinToSave);
    const bool saved = preferences.getString("boot_pin", String("3258")) == pinToSave;
    preferences.end();

    if (!saved) {
        rescueServer.send(500, "text/plain; charset=utf-8",
                          "Could not save PIN");
        return;
    }
    rescueServer.send(200, "text/plain; charset=utf-8",
                      deletePin
                          ? "Boot PIN removed. It will be disabled after the next restart."
                          : "PIN saved. It will be used after the next restart.");
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

void loadSavedDisplayImageSettings(RescueDisplayImageSettings& settings) {
    settings.bootLogo1 = "";
    settings.bootLogo2 = "combine (1).bin";
    settings.idleImages = "";
    settings.useSdIdleImages = false;

    Preferences preferences;
    if (!preferences.begin(kPreferencesNamespace, true)) {
        Serial.println("[SD] Image settings unavailable; using built-in defaults");
        return;
    }

    if (preferences.getBool("img_custom", false)) {
        settings.bootLogo1 = preferences.getString("img_logo1", "");
        settings.bootLogo2 = preferences.getString("img_logo2", "");
        settings.idleImages = preferences.getString("idle_images", "");
        settings.useSdIdleImages = preferences.getBool("img_sd_idle", false);
    }
    preferences.end();
}

bool saveDisplayImageSettings(const RescueDisplayImageSettings& settings) {
    if (!rescueImageCatalogReady) return false;

    static sdpsram::FileEntry entries[kMaxManageableImages];
    const size_t total = sdpsram::ImageStore::listDir(
        kImageDirectory, entries, kMaxManageableImages);
    const size_t shown = total < kMaxManageableImages ? total : kMaxManageableImages;
    if ((!settings.bootLogo1.isEmpty() &&
         !listedImageIsValid(settings.bootLogo1.c_str(), entries, shown)) ||
        (!settings.bootLogo2.isEmpty() &&
         !listedImageIsValid(settings.bootLogo2.c_str(), entries, shown))) {
        return false;
    }

    size_t selectedCount = 0;
    for (int start = 0; start < settings.idleImages.length();) {
        int end = settings.idleImages.indexOf('\n', start);
        if (end < 0) end = settings.idleImages.length();
        const String name = settings.idleImages.substring(start, end);
        if (!listedImageIsValid(name.c_str(), entries, shown) ||
            ++selectedCount > kMaxIdleImages) {
            return false;
        }
        for (int priorStart = 0; priorStart < start;) {
            int priorEnd = settings.idleImages.indexOf('\n', priorStart);
            if (priorEnd < 0) priorEnd = settings.idleImages.length();
            if (settings.idleImages.substring(priorStart, priorEnd) == name) return false;
            priorStart = priorEnd + 1;
        }
        start = end + 1;
    }
    if (settings.useSdIdleImages && selectedCount == 0) return false;

    Preferences preferences;
    if (!preferences.begin(kPreferencesNamespace, false)) return false;
    bool saved = storeString(preferences, "img_logo1", settings.bootLogo1) &&
                 storeString(preferences, "img_logo2", settings.bootLogo2) &&
                 storeString(preferences, "idle_images", settings.idleImages);
    if (saved) {
        const bool idleModeSaved =
            preferences.putBool("img_sd_idle", settings.useSdIdleImages) > 0;
        const bool customFlagSaved = preferences.putBool("img_custom", true) > 0;
        saved = idleModeSaved && customFlagSaved &&
                preferences.getBool("img_sd_idle", false) == settings.useSdIdleImages &&
                preferences.getBool("img_custom", false);
    }
    preferences.end();
    return saved;
}

bool isRescueImageCatalogReady() {
    return rescueImageCatalogReady;
}

void setRescueImageCatalogReady(bool ready) {
    rescueImageCatalogReady = ready;
}

bool loadSavedAccessPin(char* output, size_t outputSize) {
    if (output == nullptr || outputSize < 5) return false;
    snprintf(output, outputSize, "3258");

    Preferences preferences;
    if (!preferences.begin(kPreferencesNamespace, true)) {
        Serial.println("[AUTH] Settings storage unavailable; using default PIN");
        return false;
    }

    const String savedPin = preferences.getString("boot_pin", "3258");
    preferences.end();
    if (savedPin.isEmpty()) {
        output[0] = '\0';
        return true;
    }

    if (savedPin.length() != 4 ||
        !isDigit(savedPin[0]) || !isDigit(savedPin[1]) ||
        !isDigit(savedPin[2]) || !isDigit(savedPin[3])) {
        Serial.println("[AUTH] Stored PIN invalid; using default PIN");
        return false;
    }
    memcpy(output, savedPin.c_str(), 5);
    return true;
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
    rescueServer.on("/display", HTTP_GET, handleDisplayImages);
    rescueServer.on("/display/save", HTTP_POST, handleSaveDisplayImages);
    rescueServer.on("/display/upload", HTTP_POST,
                    handleImageUploadResult, processImageUpload);
    rescueServer.on("/settings", HTTP_POST, handleSettings);
    rescueServer.on("/pin", HTTP_POST, handleAccessPin);
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
