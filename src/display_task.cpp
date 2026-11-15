/**
 * display_task.cpp
 * - Init LovyanGFX + LVGL
 * - Tạo toàn bộ widget UI (giống layout ảnh chụp)
 * - Cập nhật widget từ truck/game/job/trailer data
 * - Chạy trong DisplayTask trên core 1
 *
 * Buffer cấp phát từ PSRAM (ESP32-S3 có 8MB PSRAM)
 * → không tốn RAM nội nên không bao giờ stack overflow
 */
// ====================== CONFIG LVGL ======================
#define SCREEN_WIDTH    320     // ← Thay bằng độ phân giải thực của màn hình bạn
#define SCREEN_HEIGHT   240     // ← Thay bằng độ phân giải thực của màn hình bạn
#include "config.h"
#include "battery_monitor.h"
#include "data_types.h"
#include "display_task.h"
#include "wifi_task.h"
#include "ota_task.h"
#include "rescue_task.h"
#include "font/myfont.h"
#include <esp_ota_ops.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include <string.h>
//add new fuction
#define SDPSRAM_LVGL
#include <SdPsramImages.h>
using sdpsram::LvImageSet; using sdpsram::SdCard; using sdpsram::SdPins;

static LvImageSet bigImgs(14);    // ảnh đã chọn cho logo boot và slideshow chờ
static LvImageSet icons(32);
static RescueDisplayImageSettings displayImageSettings;

static bool isSafeSdImageName(const String& name) {
    const bool hasBinExtension = name.length() >= 4 &&
        name.substring(name.length() - 4).equalsIgnoreCase(".bin");
    return !name.isEmpty() && name.length() < 64 &&
           name.indexOf('/') < 0 && name.indexOf('\\') < 0 &&
           name.indexOf("..") < 0 && hasBinExtension;
}

static void loadSelectedBigImage(const String& name) {
    if (!isSafeSdImageName(name) || bigImgs.find(name.c_str())) return;

    char path[80];
    snprintf(path, sizeof(path), "/ets/%s", name.c_str());
    const sdpsram::Err result = bigImgs.loadFile(path, name.c_str());
    if (result != sdpsram::Err::Ok) {
        Serial.printf("[SD] Could not load %s: %s\n",
                      name.c_str(), sdpsram::ImageStore::errStr(result));
    }
}

static void loadSelectedBigImages(const String& names) {
    int start = 0;
    while (start < names.length()) {
        int end = names.indexOf('\n', start);
        if (end < 0) end = names.length();
        loadSelectedBigImage(names.substring(start, end));
        start = end + 1;
    }
}

static void loadAssetsFromSd() {
    loadSavedDisplayImageSettings(displayImageSettings);
    setRescueImageCatalogReady(false);
    const SdPins pins = {38, 40, 39, 41, 48, 47};     // clk, cmd, d0..d3 (board 2.8")
    if (!SdCard::begin(pins)) { Serial.println("[SD] mount failed"); return; }
    bigImgs.store.setLog(&Serial);
    icons.store.setLog(&Serial);
    loadSelectedBigImage(displayImageSettings.bootLogo1);
    loadSelectedBigImage(displayImageSettings.bootLogo2);
    if (displayImageSettings.useSdIdleImages) {
        loadSelectedBigImages(displayImageSettings.idleImages);
    }
    icons.loadDir("/ets/icons");
    setRescueImageCatalogReady(true);
   // for (int i = 0; i < 7; i++) speed_dsc[i] = icons.find(speed_names[i]);
    Serial.printf("[SD] PSRAM free: %u\n", (unsigned)ESP.getFreePsram());
}
//end new function
LV_IMG_DECLARE(q_res);  
LV_IMG_DECLARE(IMG_2220Res);
LV_IMG_DECLARE(qq);
/* LV_IMG_DECLARE(x_2);
LV_IMG_DECLARE(x_3);
LV_IMG_DECLARE(x_4);
LV_IMG_DECLARE(x_5); */
//LV_IMG_DECLARE(ribi1);
//LV_IMG_DECLARE(combine);
//LV_IMG_DECLARE(ribi2);
//LV_IMG_DECLARE(ribi3);
LV_IMG_DECLARE(park); 
LV_IMG_DECLARE(left);
LV_IMG_DECLARE(right);
LV_IMG_DECLARE(fuel);
LV_IMG_DECLARE(air);
LV_IMG_DECLARE(_40x30_hightbeam);
LV_IMG_DECLARE(_40x30_lowbeam);
 /* forward-declare the symbol */
#if LV_FONT_MONTSERRAT_16
#pragma message "LV_FONT_MONTSERRAT_48 is enabled"
#else
#error "LV_FONT_MONTSERRAT_16 is NOT enabled!"
#endif
// ════════════════════════════════════════════════════════
// 1. LVGL DRIVER — buffer từ PSRAM
// ════════════════════════════════════════════════════════
#define LVGL_HOR_RES 240
#define LVGL_VER_RES 320
#define LVGL_DRAW_BUFFER_LINES 40
#define LVGL_DRAW_BUFFER_SIZE (LVGL_HOR_RES * LVGL_DRAW_BUFFER_LINES)

static lv_disp_draw_buf_t draw_buf;
static lv_disp_drv_t      disp_drv;

static lv_color_t *lvgl_buf1 = nullptr;     // cấp phát từ PSRAM trong setupDisplay()
static lv_color_t *lvgl_buf2 = nullptr;
static bool in_idle = false;
static bool displayTimingActive = false;
static uint32_t cycleFlushUs = 0;
static uint32_t cycleFlushCount = 0;
static uint32_t cycleTelemetryUpdatedAtUs = 0;
static uint32_t cycleTelemetryGeneration = 0;
static bool cycleTelemetrySnapshotValid = false;
static uint32_t lastLatencyGeneration = 0;
static bool hasLatencyGeneration = false;

struct DisplayTimingWindow {
    uint32_t count = 0;
    uint64_t totalUs = 0;
    uint32_t maxUs = 0;
};

static DisplayTimingWindow uiTiming;
static DisplayTimingWindow lvglTiming;
static DisplayTimingWindow flushTiming;
static DisplayTimingWindow telemetryToFlushTiming;
static uint32_t displayTimingCycles = 0;

static void recordDisplayTiming(DisplayTimingWindow& window, uint32_t elapsedUs) {
#if ENABLE_TIMING_LOGS
    window.count++;
    window.totalUs += elapsedUs;
    if (elapsedUs > window.maxUs) window.maxUs = elapsedUs;
#else
    (void)window;
    (void)elapsedUs;
#endif
}

static void reportDisplayTiming(const char* name, const DisplayTimingWindow& window) {
#if ENABLE_TIMING_LOGS
    if (window.count == 0) {
        Serial.printf("[TIMING] %s n=0\n", name);
        return;
    }

    Serial.printf("[TIMING] %s n=%lu avg=%lu max=%lu us\n",
                  name,
                  (unsigned long)window.count,
                  (unsigned long)(window.totalUs / window.count),
                  (unsigned long)window.maxUs);
#else
    (void)name;
    (void)window;
#endif
}

static void reportDisplayTimingWindow() {
#if ENABLE_TIMING_LOGS
    Serial.printf("[TIMING] Display window: %lu cycles\n",
                  (unsigned long)displayTimingCycles);
    reportDisplayTiming("UI update", uiTiming);
    reportDisplayTiming("LVGL handler", lvglTiming);
    reportDisplayTiming("TFT flush per dirty frame", flushTiming);
    reportDisplayTiming("Telemetry update -> TFT flush", telemetryToFlushTiming);
#endif

    uiTiming = DisplayTimingWindow{};
    lvglTiming = DisplayTimingWindow{};
    flushTiming = DisplayTimingWindow{};
    telemetryToFlushTiming = DisplayTimingWindow{};
    displayTimingCycles = 0;
}

// ====================== BOOT ANIMATION ======================
static lv_obj_t *boot_scr = nullptr;
static lv_obj_t *img_scr = nullptr;
static lv_obj_t *boot_label = nullptr;
static lv_obj_t *boot_progress = nullptr;
static lv_obj_t *boot_status = nullptr;
static lv_obj_t *boot_rescue_indicator = nullptr;
static lv_obj_t *test ;
// ====================== BOOT LOG HELPER ======================
static lv_obj_t *boot_log = nullptr;

static void updateBootRescueIndicator() {
    if (boot_rescue_indicator &&
        isRescueModeActive() &&
        lv_obj_has_flag(boot_rescue_indicator, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_clear_flag(boot_rescue_indicator, LV_OBJ_FLAG_HIDDEN);
    }
}

static void bootLvglHandler() {
    lv_timer_handler();
    updateBootRescueIndicator();
}

// Thêm 1 dòng log kiểu BIOS: [OK] Tên  thông-tin  + cập nhật progress
// tag: "OK" xanh | "WARN" vàng | "FAIL" đỏ
static void bootLine(const char *tag, const char *name, const char *info, int pct) {
    const char *color = "00E676";
    if      (!strcmp(tag, "WARN")) color = "FFC400";
    else if (!strcmp(tag, "FAIL")) color = "FF3D3D";

    // unscii_8: mỗi ký tự rộng 8px → màn 240px chứa ~30 ký tự/dòng
    char line[96];
    snprintf(line, sizeof(line), "#%s [%-4s]# #7FB2FF %-6s# %s\n", color, tag, name, info);
    lv_label_ins_text(boot_log, LV_LABEL_POS_LAST, line);

    lv_bar_set_value(boot_progress, pct, LV_ANIM_ON);
    lv_label_set_text(boot_status, name);

    // Vẽ ngay + chờ một chút cho mắt kịp đọc
    uint32_t t0 = millis();
    while (millis() - t0 < 110) { bootLvglHandler(); delay(5); }
}

void showBootSequence() {
    lv_obj_t *main_scr = lv_scr_act();

    // ── Màn hình boot ─────────────────────────────────────────
    boot_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(boot_scr, BGCOLOR, 0);
    lv_obj_set_style_bg_opa(boot_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(boot_scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(boot_scr);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x00DDFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 8, 8);
    lv_label_set_text(title, "TRUCK DASHBOARD");

    lv_obj_t *subtitle = lv_label_create(boot_scr);
    lv_obj_set_style_text_font(subtitle, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(subtitle, lv_color_hex(0x888888), 0);
    lv_obj_align(subtitle, LV_ALIGN_TOP_LEFT, 8, 30);
    lv_label_set_text_fmt(subtitle, "fw 1.0  %s %s", __DATE__, __TIME__);

    // Đường kẻ dưới tiêu đề
    lv_obj_t *line = lv_obj_create(boot_scr);
    lv_obj_set_size(line, 224, 1);
    lv_obj_set_pos(line, 8, 46);
    lv_obj_set_style_bg_color(line, lv_color_hex(0x3A6EA5), 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_pad_all(line, 0, 0);
    lv_obj_set_style_radius(line, 0, 0);

    // Vùng log
    boot_log = lv_label_create(boot_scr);
    lv_label_set_recolor(boot_log, true);
    lv_label_set_long_mode(boot_log, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(boot_log, 228);
    lv_obj_set_style_text_font(boot_log, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(boot_log, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_line_space(boot_log, 4, 0);
    lv_obj_set_pos(boot_log, 8, 54);
    lv_label_set_text(boot_log, "");

    // Progress bar
    boot_progress = lv_bar_create(boot_scr);
    lv_obj_set_size(boot_progress, 224, 8);
    lv_obj_align(boot_progress, LV_ALIGN_BOTTOM_MID, 0, -34);
    lv_bar_set_range(boot_progress, 0, 100);
    lv_bar_set_value(boot_progress, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(boot_progress, lv_color_hex(0x0A1A33), LV_PART_MAIN);
    lv_obj_set_style_bg_color(boot_progress, lv_color_hex(0x00AAFF), LV_PART_INDICATOR);
    lv_obj_set_style_radius(boot_progress, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(boot_progress, 2, LV_PART_INDICATOR);

    // Status (tên bước đang chạy)
    boot_status = lv_label_create(boot_scr);
    lv_obj_set_style_text_font(boot_status, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(boot_status, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(boot_status, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_label_set_text(boot_status, "Initializing...");

    boot_rescue_indicator = lv_label_create(boot_scr);
    lv_obj_set_size(boot_rescue_indicator, 72, 18);
    lv_obj_align(boot_rescue_indicator, LV_ALIGN_TOP_RIGHT, -4, 4);
    lv_obj_set_style_text_font(boot_rescue_indicator, &roboto, 0);
    lv_obj_set_style_text_color(boot_rescue_indicator, lv_color_white(), 0);
    lv_obj_set_style_bg_color(boot_rescue_indicator, lv_color_hex(0xD00000), 0);
    lv_obj_set_style_bg_opa(boot_rescue_indicator, LV_OPA_COVER, 0);
    lv_obj_set_style_text_align(boot_rescue_indicator, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(boot_rescue_indicator, 4, 0);
    lv_label_set_text(boot_rescue_indicator, "RESCUE");
    lv_obj_add_flag(boot_rescue_indicator, LV_OBJ_FLAG_HIDDEN);

    // ── Logo 1 ────────────────────────────────────────────────
    lv_obj_t *logo_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(logo_scr, lv_color_black(), 0);
    lv_obj_t *logo_img = lv_img_create(logo_scr);
    const lv_img_dsc_t *logo1Source = displayImageSettings.bootLogo1.isEmpty()
        ? &IMG_2220Res
        : bigImgs.find(displayImageSettings.bootLogo1.c_str());
    if (!logo1Source) {
        Serial.printf("[SD] Selected boot logo 1 unavailable: %s; using built-in logo\n",
                      displayImageSettings.bootLogo1.c_str());
        logo1Source = &IMG_2220Res;
    }
    lv_img_set_src(logo_img, logo1Source);
    lv_obj_center(logo_img);
    lv_scr_load_anim(logo_scr, LV_SCR_LOAD_ANIM_FADE_IN, 500, 0, false);
    Serial.printf("anh 1 load  psram=%d\n", ESP.getFreePsram());
    unsigned long t = millis();
    while (millis() - t < 2000) { bootLvglHandler(); delay(5); }

    lv_obj_t *scr_x = nullptr;
    if (!displayImageSettings.bootLogo2.isEmpty()) {
        const lv_img_dsc_t *logo2Source =
            bigImgs.find(displayImageSettings.bootLogo2.c_str());
        if (logo2Source) {
            scr_x = lv_obj_create(NULL);
            lv_obj_set_style_bg_color(scr_x, lv_color_black(), 0);
            lv_obj_t *img_x = lv_img_create(scr_x);
            lv_img_set_src(img_x, logo2Source);
            lv_obj_center(img_x);
            lv_scr_load_anim(scr_x, LV_SCR_LOAD_ANIM_OVER_RIGHT, 500, 0, false);
            Serial.printf("anh 2 load psram=%d\n", ESP.getFreePsram());
            t = millis();
            while (millis() - t < 5000) { bootLvglHandler(); delay(5); }
        } else {
            Serial.printf("[SD] Selected boot logo 2 unavailable: %s\n",
                          displayImageSettings.bootLogo2.c_str());
        }
    }

    // ── Sang boot screen ──────────────────────────────────────
    lv_scr_load_anim(boot_scr, LV_SCR_LOAD_ANIM_FADE_IN, 500, 0, false);
    t = millis();
    while (millis() - t < 600) { bootLvglHandler(); delay(5); }
    lv_obj_del(logo_scr);
    if (scr_x) lv_obj_del(scr_x);

    // ====================== BOOT LOG (DỮ LIỆU THẬT) ======================
    char buf[48];

    // 1. CPU
    snprintf(buf, sizeof(buf), "%s r%d %dc %dMHz", ESP.getChipModel(),
             ESP.getChipRevision(), ESP.getChipCores(), ESP.getCpuFreqMHz());
    bootLine("OK", "CPU", buf, 8);

    // 2. Chip ID (từ eFuse MAC)
    snprintf(buf, sizeof(buf), "%012llX", (unsigned long long)ESP.getEfuseMac());
    bootLine("OK", "ChipID", buf, 16);
    const esp_partition_t* partition = esp_ota_get_running_partition();
        if (partition) {
            snprintf(buf, sizeof(buf), "%s @%lX",
                    partition->label,
                    (unsigned long)partition->address);
            bootLine("OK", "Slot Run", buf, 4);
        } else {
            bootLine("FAIL", "Slot", "unknown", 4);
        }
    // 3. Flash
    snprintf(buf, sizeof(buf), "%uMB @ %uMHz",
             (unsigned)(ESP.getFlashChipSize() / (1024 * 1024)),
             (unsigned)(ESP.getFlashChipSpeed() / 1000000));
    bootLine("OK", "Flash", buf, 24);

    // 4. PSRAM
    if (psramFound()) {
        snprintf(buf, sizeof(buf), "%uMB free %uKB",
                 (unsigned)(ESP.getPsramSize() / (1024 * 1024)),
                 (unsigned)(ESP.getFreePsram() / 1024));
        bootLine("OK", "PSRAM", buf, 32);
    } else {
        bootLine("FAIL", "PSRAM", "not found", 32);
    }

    // 5. Heap
    snprintf(buf, sizeof(buf), "%u/%uKB free",
             (unsigned)(ESP.getFreeHeap() / 1024), (unsigned)(ESP.getHeapSize() / 1024));
    bootLine("OK", "Heap", buf, 40);

    // 6. LVGL + màn hình
    snprintf(buf, sizeof(buf), "v%d.%d.%d %dx%d", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR,
             LVGL_VERSION_PATCH, (int)lv_disp_get_hor_res(NULL), (int)lv_disp_get_ver_res(NULL));
    bootLine("OK", "LVGL", buf, 48);

    // 7. Pin
    BatteryReading batteryReading;
    if (readBatteryMonitor(batteryReading)) {
        snprintf(buf, sizeof(buf), "%.2f V", batteryReading.voltageV);
        bootLine("OK", "Batt", buf, 56);
    } else {
        bootLine("FAIL", "Batt", "measurement unavailable", 56);
    }

    bootLine("OK", "System", "READY", 100);
     // 8. WiFi — đã được begin() từ main.cpp, ở đây chỉ chờ trạng thái thật
    //    (7.6s logo ở trên thường đủ để nó kết nối xong)
    t = millis();
    while (!isRescueModeActive() &&
           WiFi.status() != WL_CONNECTED &&
           millis() - t < 8000) {
        bootLvglHandler();
        delay(50);
    }
    if (WiFi.status() == WL_CONNECTED && !isRescueModeActive()) {
        bootLine("OK", "WiFi", WiFi.localIP().toString().c_str(), 66);
        snprintf(buf, sizeof(buf), "%ddBm  ch%d", WiFi.RSSI(), WiFi.channel());
        bootLine("OK", "Signal", buf, 74);
    } else {
        bootLine("FAIL", "WiFi", "timeout (retry bg)", 74);
    }
    lv_label_set_text(boot_status, LV_SYMBOL_OK " System Ready");
    t = millis();
    while (millis() - t < 2500) { bootLvglHandler(); delay(5); }

    // ── Sang màn hình chính ───────────────────────────────────
    lv_scr_load_anim(main_scr, LV_SCR_LOAD_ANIM_FADE_IN, 500, 0, false);
    t = millis();
    while (millis() - t < 600) { bootLvglHandler(); delay(5); }

    lv_obj_del(boot_scr);
    boot_scr = nullptr;
    boot_log = nullptr;
    boot_rescue_indicator = nullptr;
    stopRescueButtonMonitor();
    Serial.println("[BOOT] Boot sequence completed");
}


static volatile uint32_t frameCount = 0, flushCount = 0;
static volatile uint32_t flushUs = 0, waitUs = 0;

static void reportDisplayPerformance(uint32_t now) {
    static uint32_t lastPrintMs = 0;
    static uint32_t lastFrames = 0;
    static uint32_t lastFlushes = 0;
    static uint32_t lastFlushUs = 0;
    static uint32_t lastWaitUs = 0;

    if (lastPrintMs == 0) {
        lastPrintMs = now;
        return;
    }

    const uint32_t elapsedMs = now - lastPrintMs;
    if (elapsedMs < 1000) return;

    const uint32_t frames = frameCount;
    const uint32_t flushes = flushCount;
    const uint32_t totalFlushUs = flushUs;
    const uint32_t totalWaitUs = waitUs;
    const uint32_t frameDelta = frames - lastFrames;
    const uint32_t flushDelta = flushes - lastFlushes;
    const uint32_t flushUsDelta = totalFlushUs - lastFlushUs;
    const uint32_t waitUsDelta = totalWaitUs - lastWaitUs;

    Serial.printf("[PERF] FPS=%u flush/s=%u avgFlush=%u us avgWaitDMA=%u us\n",
                  (unsigned)(frameDelta * 1000U / elapsedMs),
                  (unsigned)(flushDelta * 1000U / elapsedMs),
                  flushDelta ? (unsigned)(flushUsDelta / flushDelta) : 0,
                  flushDelta ? (unsigned)(waitUsDelta / flushDelta) : 0);

    lastPrintMs = now;
    lastFrames = frames;
    lastFlushes = flushes;
    lastFlushUs = totalFlushUs;
    lastWaitUs = totalWaitUs;
}


static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *a, lv_color_t *c) {
    const uint32_t w = a->x2 - a->x1 + 1;
    const uint32_t h = a->y2 - a->y1 + 1;

    const uint32_t t0 = micros();
    tft.waitDMA();                       // chờ lần DMA trước
    const uint32_t t1 = micros();
    if (tft.getStartCount() == 0) tft.startWrite();
    tft.pushImageDMA(a->x1, a->y1, w, h,
                     reinterpret_cast<const lgfx::rgb565_t *>(&c->full));
    const uint32_t t2 = micros();

    waitUs  += (t1 - t0);                // lớn => DMA trước chưa xong kịp (SPI là nút thắt)
    flushUs += (t2 - t0);
    flushCount++;
    if (lv_disp_flush_is_last(drv)) frameCount++;
    lv_disp_flush_ready(drv);
}

static void lvgl_create_buffers() {
    const uint32_t px = LVGL_DRAW_BUFFER_SIZE;
    lvgl_buf1 = (lv_color_t *)heap_caps_malloc(px * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    lvgl_buf2 = (lv_color_t *)heap_caps_malloc(px * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    Serial.printf("[LVGL] buf1=%p buf2=%p (%u KB mỗi cái)\n", lvgl_buf1, lvgl_buf2,
                  (unsigned)(px * sizeof(lv_color_t) / 1024));
    if (!lvgl_buf1 || !lvgl_buf2) {
        Serial.println("[LVGL] Cấp phát buffer thất bại!");
        while (true) delay(1000);
    }
    lv_disp_draw_buf_init(&draw_buf, lvgl_buf1, lvgl_buf2, px);
}

// ════════════════════════════════════════════════════════
// 2. WIDGET HANDLES
// ════════════════════════════════════════════════════════

// Header
static lv_obj_t *lbl_truck_name;   // "MAN TGX"
static lv_obj_t *lbl_plate;        // "77C-05669"
static lv_obj_t *lbl_gear;         // "4"
static lv_obj_t *lbl_clock;        // "10:13"
static lv_obj_t *icon_blinker_left;
static lv_obj_t *icon_blinker_right;

// Row 2: nhiệt độ + speed
static lv_obj_t *lbl_oil;          // "Oil:75C"
static lv_obj_t *lbl_h2o;          // "H2O:67C"
static lv_obj_t *lbl_speed;        // "0"  (font 48)

// Row 3: ODO + distance
static lv_obj_t *lbl_odo;
static lv_obj_t *lbl_dist;

// Row 4: bars
static lv_obj_t *bar_fuel;
static lv_obj_t *lbl_fuel_val;
static lv_obj_t *icon_highbeam;
static lv_obj_t *bar_air;
static lv_obj_t *lbl_air_val;
static lv_obj_t *icon_lowbeam;

// Row 5: wear
static lv_obj_t *lbl_dam;          // "Dam Info" label
static lv_obj_t *lbl_wear_t;
static lv_obj_t *lbl_wear_tr;
static lv_obj_t *lbl_wear_c;

// Footer
static lv_obj_t *lbl_trailer;
static lv_obj_t *lbl_time;
static lv_obj_t *lbl_income;
static lv_obj_t *icon_park;
static lv_obj_t *lbl_trl_prefix;
static lv_obj_t *lbl_vol;
static lv_obj_t  *spd_limit ;
static lv_obj_t *limit_val ;
static lv_obj_t *encoder_page_panel = nullptr;
static lv_obj_t *encoder_page_title = nullptr;
static lv_obj_t *engine_page_values[8] = {};
static lv_obj_t *trip_page_values[6] = {};
static const lv_img_dsc_t* speed_imgs[] = {&sp30, &sp40, &sp50, &sp60, &sp70, &sp80, &sp90};

static constexpr int ENCODER_PAGE_COUNT = 3;
static volatile int16_t encoderDetents = 0;
static volatile int8_t encoderTransition = 0;
static volatile uint8_t encoderPreviousState = 0;
static portMUX_TYPE encoderMux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t encoderPage = 0;
static char configuredAccessPin[5] = "3258";
static uint8_t enteredAccessPin[4] = {};
static uint8_t selectedAccessDigit = 0;
static bool accessGranted = false;
static lv_obj_t *ui_scr = nullptr;
static lv_obj_t *accessPinScreen = nullptr;
static lv_obj_t *accessPinDigits[4] = {};
static lv_obj_t *accessPinBoxes[4] = {};
static lv_obj_t *accessPinMessage = nullptr;
static int encoderButtonRaw = HIGH;
static int encoderButtonStable = HIGH;
static uint32_t encoderButtonChangedAt = 0;
static constexpr size_t SETTINGS_VISIBLE_ROWS = 6;
static constexpr size_t SETTINGS_MAX_IMAGES = 32;

enum class EncoderSettingsMode : uint8_t {
    Closed,
    Root,
    ImageChoices,
    Saved
};

enum class EncoderSettingsChoice : uint8_t {
    Logo1,
    Logo2,
    IdleMode,
    IdleImages
};

static EncoderSettingsMode encoderSettingsMode = EncoderSettingsMode::Closed;
static EncoderSettingsChoice encoderSettingsChoice = EncoderSettingsChoice::Logo1;
static RescueDisplayImageSettings pendingDisplayImageSettings;
static sdpsram::FileEntry settingsImageEntries[SETTINGS_MAX_IMAGES];
static uint8_t settingsValidImageIndexes[SETTINGS_MAX_IMAGES] = {};
static size_t settingsValidImageCount = 0;
static lv_obj_t *encoderSettingsScreen = nullptr;
static lv_obj_t *encoderSettingsTitle = nullptr;
static lv_obj_t *encoderSettingsRows[SETTINGS_VISIBLE_ROWS] = {};
static lv_obj_t *encoderSettingsRowLabels[SETTINGS_VISIBLE_ROWS] = {};
static lv_obj_t *encoderSettingsStatus = nullptr;
static int encoderSettingsSelection = 0;
static int encoderSettingsFirstVisible = 0;
static String encoderSettingsStatusText;

DRAM_ATTR static const int8_t encoderTransitionTable[16] = {
     0, -1,  1,  0,
     1,  0,  0, -1,
    -1,  0,  0,  1,
     0,  1, -1,  0
};

static void IRAM_ATTR encoder_isr() {
    portENTER_CRITICAL_ISR(&encoderMux);
    const uint8_t state =
        (gpio_get_level((gpio_num_t)ENCODER_A_PIN) << 1) |
         gpio_get_level((gpio_num_t)ENCODER_B_PIN);
    const uint8_t transition = (encoderPreviousState << 2) | state;
    const int8_t movement = encoderTransitionTable[transition];
    encoderPreviousState = state;
    if (movement != 0) {
        encoderTransition += movement;
        if (encoderTransition >= 4) {
            encoderDetents++;
            encoderTransition = 0;
        } else if (encoderTransition <= -4) {
            encoderDetents--;
            encoderTransition = 0;
        }
    }
    portEXIT_CRITICAL_ISR(&encoderMux);
}

static void setupRotaryEncoder() {
    pinMode(ENCODER_A_PIN, INPUT_PULLUP);
    pinMode(ENCODER_B_PIN, INPUT_PULLUP);
    pinMode(ENCODER_SW_PIN, INPUT_PULLUP);
    encoderPreviousState =
        (digitalRead(ENCODER_A_PIN) << 1) | digitalRead(ENCODER_B_PIN);
    encoderButtonRaw = digitalRead(ENCODER_SW_PIN);
    encoderButtonStable = encoderButtonRaw;
    encoderButtonChangedAt = millis();
    attachInterrupt(digitalPinToInterrupt(ENCODER_A_PIN), encoder_isr, CHANGE);
    attachInterrupt(digitalPinToInterrupt(ENCODER_B_PIN), encoder_isr, CHANGE);
    Serial.printf("[ENCODER] Ready: A=GPIO%d B=GPIO%d SW=GPIO%d\n",
                  ENCODER_A_PIN, ENCODER_B_PIN, ENCODER_SW_PIN);
}

static void updateAccessPinDisplay() {
    for (uint8_t i = 0; i < 4; i++) {
        if (i < selectedAccessDigit) {
            lv_label_set_text(accessPinDigits[i], "*");
        } else if (i > selectedAccessDigit) {
            lv_label_set_text(accessPinDigits[i], " ");
        } else {
            lv_label_set_text_fmt(accessPinDigits[i], "%u", enteredAccessPin[i]);
        }
        lv_obj_set_style_border_color(
            accessPinBoxes[i],
            i == selectedAccessDigit ? lv_color_hex(0x00DDFF) : lv_color_hex(0x34445F),
            0);
    }
}

static void createAccessPinScreen() {
    accessPinScreen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(accessPinScreen, BGCOLOR, 0);
    lv_obj_set_style_bg_opa(accessPinScreen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(accessPinScreen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(accessPinScreen);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x00DDFF), 0);
    lv_label_set_text(title, "NHAP MA PIN");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 64);

    lv_obj_t *hint = lv_label_create(accessPinScreen);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0xB0BEC5), 0);
    lv_label_set_text(hint, "Xoay de chon so, nhan de xac nhan");
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 105);

    for (uint8_t i = 0; i < 4; i++) {
        lv_obj_t *box = lv_obj_create(accessPinScreen);
        accessPinBoxes[i] = box;
        lv_obj_set_size(box, 42, 54);
        lv_obj_set_pos(box, 24 + i * 48, 150);
        lv_obj_set_style_bg_color(box, lv_color_hex(0x091525), 0);
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(box, 2, 0);
        lv_obj_set_style_border_color(box, lv_color_hex(0x34445F), 0);
        lv_obj_set_style_radius(box, 7, 0);
        lv_obj_set_style_pad_all(box, 0, 0);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *digit = lv_label_create(box);
        accessPinDigits[i] = digit;
        lv_obj_set_style_text_font(digit, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(digit, lv_color_white(), 0);
        lv_obj_center(digit);
    }

    accessPinMessage = lv_label_create(accessPinScreen);
    lv_obj_set_style_text_font(accessPinMessage, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(accessPinMessage, lv_color_hex(0xFFD700), 0);
    lv_label_set_text(accessPinMessage, " ");
    lv_obj_align(accessPinMessage, LV_ALIGN_TOP_MID, 0, 226);
    updateAccessPinDisplay();
}

static void adjustAccessPinDigit(int8_t direction) {
    int value = (int)enteredAccessPin[selectedAccessDigit] + direction;
    if (value < 0) value += 10;
    if (value > 9) value -= 10;
    enteredAccessPin[selectedAccessDigit] = (uint8_t)value;
    updateAccessPinDisplay();
}

static void confirmAccessPinDigit() {
    if (selectedAccessDigit < 3) {
        selectedAccessDigit++;
        lv_label_set_text(accessPinMessage, " ");
        updateAccessPinDisplay();
        return;
    }

    bool matches = true;
    for (uint8_t i = 0; i < 4; i++) {
        if (enteredAccessPin[i] != (uint8_t)(configuredAccessPin[i] - '0')) {
            matches = false;
            break;
        }
    }

    if (matches) {
        accessGranted = true;
        if (ui_scr) lv_scr_load(ui_scr);
        Serial.println("[AUTH] PIN accepted");
        return;
    }

    lv_label_set_text(accessPinMessage, "SAI MA, THU LAI");
    lv_obj_set_style_text_color(accessPinMessage, lv_color_hex(0xFF5555), 0);
    memset(enteredAccessPin, 0, sizeof(enteredAccessPin));
    selectedAccessDigit = 0;
    updateAccessPinDisplay();
}

static void setLabelTextIfChanged(lv_obj_t *label, const char *text) {
    const char *currentText = lv_label_get_text(label);
    if (currentText == nullptr || strcmp(currentText, text) != 0) {
        lv_label_set_text(label, text);
    }
}

static void pauseIdleForSettings();

static void refreshSettingsImageCatalog() {
    settingsValidImageCount = 0;
    if (!isRescueImageCatalogReady()) {
        encoderSettingsStatusText = "The SD card is not ready";
        return;
    }

    const size_t total = sdpsram::ImageStore::listDir(
        "/ets", settingsImageEntries, SETTINGS_MAX_IMAGES);
    const size_t shown = total < SETTINGS_MAX_IMAGES ? total : SETTINGS_MAX_IMAGES;
    for (size_t i = 0; i < shown; ++i) {
        if (settingsImageEntries[i].valid) {
            settingsValidImageIndexes[settingsValidImageCount++] = (uint8_t)i;
        }
    }
    encoderSettingsStatusText = settingsValidImageCount
        ? "Rotate: select   Press: confirm"
        : "No valid .bin images found in /ets";
}

static size_t selectedIdleImageCount() {
    size_t count = 0;
    for (int start = 0; start < pendingDisplayImageSettings.idleImages.length();) {
        int end = pendingDisplayImageSettings.idleImages.indexOf('\n', start);
        if (end < 0) end = pendingDisplayImageSettings.idleImages.length();
        if (end > start) count++;
        start = end + 1;
    }
    return count;
}

static bool isIdleImageSelected(const char *name) {
    int start = 0;
    const String& names = pendingDisplayImageSettings.idleImages;
    while (start < names.length()) {
        int end = names.indexOf('\n', start);
        if (end < 0) end = names.length();
        if (names.substring(start, end) == name) return true;
        start = end + 1;
    }
    return false;
}

static size_t encoderSettingsOptionCount() {
    switch (encoderSettingsMode) {
        case EncoderSettingsMode::Root: return 6;
        case EncoderSettingsMode::ImageChoices:
            if (encoderSettingsChoice == EncoderSettingsChoice::IdleMode) return 3;
            if (encoderSettingsChoice == EncoderSettingsChoice::IdleImages) {
                return settingsValidImageCount + 1;
            }
            return settingsValidImageCount + 2;
        case EncoderSettingsMode::Saved: return 1;
        case EncoderSettingsMode::Closed: return 0;
    }
    return 0;
}

static String encoderSettingsOptionText(size_t index) {
    if (encoderSettingsMode == EncoderSettingsMode::Root) {
        static const char* const options[] = {
            "Boot logo 1", "Boot logo 2", "Idle slideshow mode",
            "Select idle images", "Save changes", "Cancel"
        };
        return options[index];
    }
    if (encoderSettingsMode == EncoderSettingsMode::Saved) {
        return "Image settings saved";
    }
    if (encoderSettingsMode != EncoderSettingsMode::ImageChoices) return "";

    if (encoderSettingsChoice == EncoderSettingsChoice::IdleMode) {
        if (index == 0) return "Built-in slideshow";
        if (index == 1) return "Use selected SD images";
        return "Back";
    }
    if (encoderSettingsChoice == EncoderSettingsChoice::IdleImages) {
        if (index == settingsValidImageCount) return "Back";
        const char *name = settingsImageEntries[
            settingsValidImageIndexes[index]].name;
        return String(isIdleImageSelected(name) ? "[x] " : "[ ] ") + name;
    }
    if (index == settingsValidImageCount + 1) return "Back";
    if (index == 0) {
        return encoderSettingsChoice == EncoderSettingsChoice::Logo1
            ? "Built-in logo" : "No second logo";
    }
    return settingsImageEntries[
        settingsValidImageIndexes[index - 1]].name;
}

static const char* encoderSettingsPageTitle() {
    if (encoderSettingsMode == EncoderSettingsMode::Root) return "IMAGE SETTINGS";
    if (encoderSettingsMode == EncoderSettingsMode::Saved) return "SETTINGS SAVED";
    switch (encoderSettingsChoice) {
        case EncoderSettingsChoice::Logo1: return "BOOT LOGO 1";
        case EncoderSettingsChoice::Logo2: return "BOOT LOGO 2";
        case EncoderSettingsChoice::IdleMode: return "IDLE SOURCE";
        case EncoderSettingsChoice::IdleImages: return "IDLE IMAGES";
    }
    return "IMAGE SETTINGS";
}

static void renderEncoderSettings() {
    if (!encoderSettingsScreen) return;
    lv_label_set_text(encoderSettingsTitle, encoderSettingsPageTitle());

    const size_t count = encoderSettingsOptionCount();
    if (count == 0) {
        encoderSettingsSelection = 0;
        encoderSettingsFirstVisible = 0;
    } else {
        if (encoderSettingsSelection < 0) encoderSettingsSelection = (int)count - 1;
        if (encoderSettingsSelection >= (int)count) encoderSettingsSelection = 0;
        if (encoderSettingsSelection < encoderSettingsFirstVisible) {
            encoderSettingsFirstVisible = encoderSettingsSelection;
        } else if (encoderSettingsSelection >=
                   encoderSettingsFirstVisible + (int)SETTINGS_VISIBLE_ROWS) {
            encoderSettingsFirstVisible =
                encoderSettingsSelection - (int)SETTINGS_VISIBLE_ROWS + 1;
        }
    }

    for (size_t row = 0; row < SETTINGS_VISIBLE_ROWS; ++row) {
        const size_t option = (size_t)encoderSettingsFirstVisible + row;
        if (option >= count) {
            lv_obj_add_flag(encoderSettingsRows[row], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(encoderSettingsRows[row], LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(encoderSettingsRowLabels[row],
                          encoderSettingsOptionText(option).c_str());
        const bool selected = (int)option == encoderSettingsSelection;
        lv_obj_set_style_bg_color(
            encoderSettingsRows[row],
            selected ? lv_color_hex(0x145A78) : lv_color_hex(0x101D2D), 0);
        lv_obj_set_style_text_color(
            encoderSettingsRowLabels[row],
            selected ? lv_color_hex(0xFFFFFF) : lv_color_hex(0xB8C7D9), 0);
    }

    String status = encoderSettingsStatusText;
    if (encoderSettingsMode == EncoderSettingsMode::Root) {
        if (status.isEmpty()) status = "Rotate: select   Press: open/save";
    } else if (encoderSettingsMode == EncoderSettingsMode::Saved) {
        status = "Restart to apply. Press to return.";
    } else if (encoderSettingsMode == EncoderSettingsMode::ImageChoices &&
               encoderSettingsChoice == EncoderSettingsChoice::IdleImages) {
        status = "Selected " + String(selectedIdleImageCount()) +
                 "/12. Press to toggle; Back to return";
    }
    lv_label_set_text(encoderSettingsStatus, status.c_str());
}

static void createEncoderSettingsScreen() {
    if (encoderSettingsScreen) return;
    encoderSettingsScreen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(encoderSettingsScreen, BGCOLOR, 0);
    lv_obj_set_style_bg_opa(encoderSettingsScreen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(encoderSettingsScreen, LV_OBJ_FLAG_SCROLLABLE);

    encoderSettingsTitle = lv_label_create(encoderSettingsScreen);
    lv_obj_set_style_text_font(encoderSettingsTitle, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(encoderSettingsTitle, lv_color_hex(0x00DDFF), 0);
    lv_obj_set_pos(encoderSettingsTitle, 8, 14);

    for (size_t i = 0; i < SETTINGS_VISIBLE_ROWS; ++i) {
        lv_obj_t *row = lv_obj_create(encoderSettingsScreen);
        encoderSettingsRows[i] = row;
        lv_obj_set_size(row, 224, 31);
        lv_obj_set_pos(row, 8, 48 + i * 34);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 5, 0);
        lv_obj_set_style_pad_hor(row, 8, 0);
        lv_obj_set_style_pad_ver(row, 5, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *label = lv_label_create(row);
        encoderSettingsRowLabels[i] = label;
        lv_obj_set_width(label, 208);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    }

    encoderSettingsStatus = lv_label_create(encoderSettingsScreen);
    lv_obj_set_width(encoderSettingsStatus, 224);
    lv_obj_set_style_text_font(encoderSettingsStatus, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(encoderSettingsStatus, lv_color_hex(0x9AABC1), 0);
    lv_label_set_long_mode(encoderSettingsStatus, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(encoderSettingsStatus, 8, 260);
}

static void closeEncoderSettings() {
    encoderSettingsMode = EncoderSettingsMode::Closed;
    if (ui_scr) lv_scr_load(ui_scr);
}

static void openEncoderSettings() {
    if (otaState != 0) return;
    refreshSettingsImageCatalog();
    pendingDisplayImageSettings = displayImageSettings;
    encoderSettingsMode = EncoderSettingsMode::Root;
    encoderSettingsSelection = 0;
    encoderSettingsFirstVisible = 0;
    createEncoderSettingsScreen();
    pauseIdleForSettings();
    renderEncoderSettings();
    lv_scr_load(encoderSettingsScreen);
}

static void openEncoderSettingsChoice(EncoderSettingsChoice choice) {
    encoderSettingsChoice = choice;
    encoderSettingsMode = EncoderSettingsMode::ImageChoices;
    encoderSettingsSelection = 0;
    encoderSettingsFirstVisible = 0;

    if (choice == EncoderSettingsChoice::Logo1) {
        if (!pendingDisplayImageSettings.bootLogo1.isEmpty()) {
            for (size_t i = 0; i < settingsValidImageCount; ++i) {
                const char *name = settingsImageEntries[
                    settingsValidImageIndexes[i]].name;
                if (pendingDisplayImageSettings.bootLogo1 == name) {
                    encoderSettingsSelection = (int)i + 1;
                    break;
                }
            }
        }
    } else if (choice == EncoderSettingsChoice::Logo2) {
        if (!pendingDisplayImageSettings.bootLogo2.isEmpty()) {
            for (size_t i = 0; i < settingsValidImageCount; ++i) {
                const char *name = settingsImageEntries[
                    settingsValidImageIndexes[i]].name;
                if (pendingDisplayImageSettings.bootLogo2 == name) {
                    encoderSettingsSelection = (int)i + 1;
                    break;
                }
            }
        }
    } else if (choice == EncoderSettingsChoice::IdleMode) {
        encoderSettingsSelection = pendingDisplayImageSettings.useSdIdleImages ? 1 : 0;
    }
    renderEncoderSettings();
}

static void toggleSelectedIdleImage() {
    const size_t index = (size_t)encoderSettingsSelection;
    if (index >= settingsValidImageCount) {
        encoderSettingsMode = EncoderSettingsMode::Root;
        encoderSettingsSelection = 3;
        renderEncoderSettings();
        return;
    }

    const char *name = settingsImageEntries[
        settingsValidImageIndexes[index]].name;
    String updated;
    bool removed = false;
    const String& selectedNames = pendingDisplayImageSettings.idleImages;
    for (int start = 0; start < selectedNames.length();) {
        int end = selectedNames.indexOf('\n', start);
        if (end < 0) end = selectedNames.length();
        const String existing = selectedNames.substring(start, end);
        if (existing == name) {
            removed = true;
        } else if (!existing.isEmpty()) {
            if (!updated.isEmpty()) updated += '\n';
            updated += existing;
        }
        start = end + 1;
    }
    if (!removed) {
        if (selectedIdleImageCount() >= 12) {
            encoderSettingsStatusText = "Maximum is 12 selected idle images";
            renderEncoderSettings();
            return;
        }
        if (!updated.isEmpty()) updated += '\n';
        updated += name;
    }
    pendingDisplayImageSettings.idleImages = updated;
    renderEncoderSettings();
}

static void confirmEncoderSettingsChoice() {
    const size_t selected = (size_t)encoderSettingsSelection;
    if (encoderSettingsChoice == EncoderSettingsChoice::IdleImages) {
        toggleSelectedIdleImage();
        return;
    }

    if (encoderSettingsChoice == EncoderSettingsChoice::IdleMode) {
        if (selected == 2) {
            encoderSettingsMode = EncoderSettingsMode::Root;
            encoderSettingsSelection = 2;
        } else {
            pendingDisplayImageSettings.useSdIdleImages = selected == 1;
            encoderSettingsStatusText = "";
            encoderSettingsMode = EncoderSettingsMode::Root;
            encoderSettingsSelection = 2;
        }
        renderEncoderSettings();
        return;
    }

    if (selected == settingsValidImageCount + 1) {
        encoderSettingsMode = EncoderSettingsMode::Root;
        encoderSettingsSelection =
            encoderSettingsChoice == EncoderSettingsChoice::Logo1 ? 0 : 1;
        renderEncoderSettings();
        return;
    }

    String value;
    if (selected > 0) {
        value = settingsImageEntries[
            settingsValidImageIndexes[selected - 1]].name;
    }
    if (encoderSettingsChoice == EncoderSettingsChoice::Logo1) {
        pendingDisplayImageSettings.bootLogo1 = value;
        encoderSettingsSelection = 0;
    } else {
        pendingDisplayImageSettings.bootLogo2 = value;
        encoderSettingsSelection = 1;
    }
    encoderSettingsStatusText = "";
    encoderSettingsMode = EncoderSettingsMode::Root;
    renderEncoderSettings();
}

static void confirmEncoderSettings() {
    if (encoderSettingsMode == EncoderSettingsMode::Saved) {
        closeEncoderSettings();
        return;
    }
    if (encoderSettingsMode == EncoderSettingsMode::ImageChoices) {
        confirmEncoderSettingsChoice();
        return;
    }

    switch (encoderSettingsSelection) {
        case 0: openEncoderSettingsChoice(EncoderSettingsChoice::Logo1); break;
        case 1: openEncoderSettingsChoice(EncoderSettingsChoice::Logo2); break;
        case 2: openEncoderSettingsChoice(EncoderSettingsChoice::IdleMode); break;
        case 3: openEncoderSettingsChoice(EncoderSettingsChoice::IdleImages); break;
        case 4:
            if (saveDisplayImageSettings(pendingDisplayImageSettings)) {
                displayImageSettings = pendingDisplayImageSettings;
                encoderSettingsMode = EncoderSettingsMode::Saved;
                encoderSettingsStatusText = "";
            } else {
                encoderSettingsStatusText =
                    "Save failed: check SD and select at least one idle image";
            }
            renderEncoderSettings();
            break;
        case 5: closeEncoderSettings(); break;
        default: break;
    }
}

static void moveEncoderSettingsSelection(int16_t direction) {
    const size_t count = encoderSettingsOptionCount();
    if (count == 0) return;
    encoderSettingsSelection += direction;
    if (encoderSettingsSelection < 0) encoderSettingsSelection = (int)count - 1;
    else if (encoderSettingsSelection >= (int)count) encoderSettingsSelection = 0;
    encoderSettingsStatusText = "";
    renderEncoderSettings();
}

static void selectEncoderPage(int16_t direction) {
    int page = (int)encoderPage + direction;
    while (page < 0) page += ENCODER_PAGE_COUNT;
    encoderPage = page % ENCODER_PAGE_COUNT;

    if (encoderPage == 0) {
        lv_obj_add_flag(encoder_page_panel, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(encoder_page_panel, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(encoder_page_title,
                          encoderPage == 1 ? "ENGINE  2/3" : "TRIP / TRAILER  3/3");
        for (lv_obj_t *label : engine_page_values) {
            if (encoderPage == 1) lv_obj_clear_flag(label, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
        }
        for (lv_obj_t *label : trip_page_values) {
            if (encoderPage == 2) lv_obj_clear_flag(label, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
        }
    }
    Serial.printf("[UI] Encoder page %u/%u\n",
                  (unsigned)(encoderPage + 1), ENCODER_PAGE_COUNT);
}

static void pollRotaryEncoder() {
    const int buttonRaw = digitalRead(ENCODER_SW_PIN);
    if (buttonRaw != encoderButtonRaw) {
        encoderButtonRaw = buttonRaw;
        encoderButtonChangedAt = millis();
    }

    bool buttonPressed = false;
    if (millis() - encoderButtonChangedAt >= 30 &&
        encoderButtonStable != encoderButtonRaw) {
        encoderButtonStable = encoderButtonRaw;
        buttonPressed = encoderButtonStable == LOW;
    }

    int16_t detents;
    portENTER_CRITICAL(&encoderMux);
    detents = encoderDetents;
    encoderDetents = 0;
    portEXIT_CRITICAL(&encoderMux);

    if (!accessGranted) {
        while (detents > 0) {
            adjustAccessPinDigit(1);
            detents--;
        }
        while (detents < 0) {
            adjustAccessPinDigit(-1);
            detents++;
        }
        if (buttonPressed) confirmAccessPinDigit();
        return;
    }

    if (encoderSettingsMode == EncoderSettingsMode::Saved) {
        if (buttonPressed) closeEncoderSettings();
        return;
    }
    if (encoderSettingsMode != EncoderSettingsMode::Closed) {
        while (detents > 0) {
            moveEncoderSettingsSelection(1);
            detents--;
        }
        while (detents < 0) {
            moveEncoderSettingsSelection(-1);
            detents++;
        }
        if (buttonPressed) confirmEncoderSettings();
        return;
    }
    if (buttonPressed) {
        openEncoderSettings();
        return;
    }

    while (detents > 0) {
        selectEncoderPage(1);
        detents--;
    }
    while (detents < 0) {
        selectEncoderPage(-1);
        detents++;
    }
}

// ════════════════════════════════════════════════════════
// 3. HELPER: tạo horizontal divider
// ════════════════════════════════════════════════════════
static void make_divider(lv_obj_t *parent, lv_coord_t y, lv_color_t color) {
    lv_obj_t *d = lv_obj_create(parent);
    lv_obj_set_size(d, 240, 2);
    lv_obj_set_pos(d, 0, y);
    lv_obj_set_style_bg_color(d, color, 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(d, 0, 0);
    lv_obj_set_style_radius(d, 0, 0);
    lv_obj_set_style_pad_all(d, 0, 0);
    lv_obj_clear_flag(d, LV_OBJ_FLAG_SCROLLABLE);
}


// ════════════════════════════════════════════════════════
// 3b. RPM BAR — chuyển động mượt, thang đo tự theo engineRpmMax của từng xe
// ════════════════════════════════════════════════════════
#define RPM_MAX_DEFAULT  2500   // dùng khi telemetry chưa có engineRpmMax
#define RPM_WARN_PCT     85     // ≥ 85% thang đo: vàng cam
#define RPM_RED_PCT      95     // ≥ 95% thang đo: đỏ
#define RPM_ANIM_MS      100    // hơi lớn hơn chu kỳ fetch (200ms) → thanh không bao giờ đứng yên

static lv_obj_t *bar_rpm       = nullptr;
static int       rpm_zone      = -1;
static int       rpm_max_cur   = RPM_MAX_DEFAULT;
static int       rpm_warn_val  = RPM_MAX_DEFAULT * RPM_WARN_PCT / 100;
static int       rpm_red_val   = RPM_MAX_DEFAULT * RPM_RED_PCT  / 100;
static int       rpm_last_tgt  = -1;

static void rpm_anim_cb(void *var, int32_t v) {
    lv_obj_t *bar = (lv_obj_t *)var;
    lv_bar_set_value(bar, v, LV_ANIM_OFF);

    int zone = (v >= rpm_red_val) ? 2 : (v >= rpm_warn_val) ? 1 : 0;
    if (zone != rpm_zone) {                       // chỉ đổi màu khi qua vùng khác
        rpm_zone = zone;
        lv_color_t c = zone == 2 ? lv_color_hex(0xFF4400)
                     : zone == 1 ? lv_color_hex(0xFFAA00)
                                 : lv_color_hex(0x00CC66);
        lv_obj_set_style_bg_color(bar, c, LV_PART_INDICATOR);
    }
}

// Đổi thang đo khi xe khác (rpmMax thay đổi)
static void rpm_apply_max(int m) {
    if (m <= 0) m = RPM_MAX_DEFAULT;              // chưa có dữ liệu → dùng mặc định
    if (m == rpm_max_cur) return;
    rpm_max_cur  = m;
    rpm_warn_val = m * RPM_WARN_PCT / 100;
    rpm_red_val  = m * RPM_RED_PCT  / 100;
    lv_bar_set_range(bar_rpm, 0, m);
    rpm_zone     = -1;                            // ép tính lại màu
    rpm_last_tgt = -1;                            // ép chạy lại animation
}

// rpm: vòng tua hiện tại | rpmMax: engineRpmMax của xe (0 = chưa biết)
static void rpm_set_target(int rpm, int rpmMax) {
    if (!bar_rpm) return;
    rpm_apply_max(rpmMax);

    int target = rpm;
    if (target < 0) target = 0;
    if (target > rpm_max_cur) target = rpm_max_cur;

    if (target == rpm_last_tgt) return;           // không đổi → khỏi tạo animation
    rpm_last_tgt = target;

    lv_anim_del(bar_rpm, rpm_anim_cb);            // huỷ anim cũ, bắt đầu từ vị trí đang hiển thị
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, bar_rpm);
    lv_anim_set_exec_cb(&a, rpm_anim_cb);
    lv_anim_set_values(&a, lv_bar_get_value(bar_rpm), target);
    lv_anim_set_time(&a, RPM_ANIM_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_linear); // linear = liền mạch giữa các lần cập nhật
    lv_anim_start(&a);
}

// ════════════════════════════════════════════════════════
// 3c. SPEED ARC — đồng hồ nửa vòng tròn bao quanh lbl_speed (0..199)
// ════════════════════════════════════════════════════════
// Số tốc độ dùng font obitron: mỗi chữ số rộng ~40px, cao 35px → "199" ≈ 100px.
// Để nửa vòng tròn ôm vừa số, bán kính ngoài ≈ 75px. Vì vậy cụm tốc độ được
// đẩy xuống SPD_DY px và các hàng bên dưới dời theo cùng đoạn đó.
#define SPD_DY        27                     // độ dời xuống của cụm tốc độ + các hàng bên dưới
#define SPD_MAX       2500
#define SPD_ARC_R     75                     // bán kính ngoài
#define SPD_ARC_W     8                     // độ dày cung
#define SPD_ARC_CX    120                    // tâm ngang (giữa màn hình 240px)
#define SPD_ARC_CY    (88 + SPD_DY)          // tâm dọc = đường đáy nửa vòng tròn
#define SPD_ANIM_MS   100

static lv_obj_t *arc_speed    = nullptr;
static int       spd_last_tgt = -1;

static void spd_anim_cb(void *var, int32_t v) {
    lv_arc_set_value((lv_obj_t *)var, v);
}

static void speed_arc_set_target(int spd) {
    if (!arc_speed) return;
    if (spd < 0) spd = 0;
    if (spd > SPD_MAX) spd = SPD_MAX;
    if (spd == spd_last_tgt) return;
    spd_last_tgt = spd;

    lv_anim_del(arc_speed, spd_anim_cb);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, arc_speed);
    lv_anim_set_exec_cb(&a, spd_anim_cb);
    lv_anim_set_values(&a, lv_arc_get_value(arc_speed), spd);
    lv_anim_set_time(&a, SPD_ANIM_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_start(&a);
}

static void speed_arc_set_warn(bool warn) {
    if (!arc_speed) return;
    lv_obj_set_style_arc_color(arc_speed,
        warn ? lv_color_hex(0xFF4444) : lv_color_hex(0x00AAFF), LV_PART_INDICATOR);
}

// ════════════════════════════════════════════════════════
// 4b. MÀN HÌNH CHỜ (slideshow ảnh) — hiện khi game chưa kết nối hoặc đang pause
// ════════════════════════════════════════════════════════
// Đổi điều kiện ở đây nếu cần:
#define IDLE_WHEN_DISCONNECTED  1     // game.connected == false → ảnh
#define IDLE_WHEN_PAUSED        1     // game.paused    == true  → ảnh
#define IDLE_ENTER_DELAY_MS     2000  // phải "idle" liên tục 2s mới chuyển (chống nhấp nháy khi 1 lần fetch lỗi)
#define IDLE_EXIT_DELAY_MS      300
#define IDLE_SLIDE_MS           6000  // đổi ảnh mỗi 6s

static lv_obj_t   *idle_scr   = nullptr;   // màn hình ảnh chờ
static lv_obj_t   *idle_img   = nullptr;
static lv_obj_t   *idle_status_time = nullptr;
static lv_obj_t   *idle_status_voltage = nullptr;
static lv_timer_t *idle_timer = nullptr;
static lv_obj_t   *rescue_ui_indicator = nullptr;
static lv_obj_t   *rescue_idle_indicator = nullptr;

static const lv_img_dsc_t* idle_imgs[12] = {};
static size_t idle_img_count = 0;
static int idle_idx = 0;

static void pauseIdleForSettings() {
    in_idle = false;
    if (idle_timer) lv_timer_pause(idle_timer);
}

static void prepareIdleImages() {
    idle_img_count = 0;
    if (displayImageSettings.useSdIdleImages) {
        int start = 0;
        const String& names = displayImageSettings.idleImages;
        while (start < names.length() && idle_img_count < 12) {
            int end = names.indexOf('\n', start);
            if (end < 0) end = names.length();
            const String name = names.substring(start, end);
            const lv_img_dsc_t *image = bigImgs.find(name.c_str());
            if (image) idle_imgs[idle_img_count++] = image;
            else Serial.printf("[SD] Selected idle image unavailable: %s\n", name.c_str());
            start = end + 1;
        }
    }

    if (idle_img_count == 0) {
        static const lv_img_dsc_t* const builtInIdleImages[] = {
           &IMG_2220Res, &qq
        };
        for (const lv_img_dsc_t* image : builtInIdleImages) {
            idle_imgs[idle_img_count++] = image;
        }
        if (displayImageSettings.useSdIdleImages) {
            Serial.println("[SD] No selected idle images loaded; using built-in slideshow");
        }
    }
}

static String idleClockText() {
    if (!game.connected || game.realTime.isEmpty()) {
        char currentDateTime[32];
        if (!getCurrentLocalDateTime(currentDateTime, sizeof(currentDateTime))) {
            return "--:--";
        }
        return String(currentDateTime).substring(11, 16);
    }

    int timeStart = game.realTime.indexOf('T');
    if (timeStart >= 0) {
        timeStart++;
    } else if (game.realTime.length() >= 16 && game.realTime.charAt(10) == ' ') {
        timeStart = 11;
    } else {
        timeStart = 0;
    }

    if (game.realTime.length() >= timeStart + 5) {
        return game.realTime.substring(timeStart, timeStart + 5);
    }
    return "--:--";
}

static void updateIdleStatus() {
    static unsigned long lastUpdate = 0;
    static String previousTime;

    const unsigned long now = millis();
    if (now - lastUpdate < 1000) return;
    lastUpdate = now;

    const String clock = idleClockText();
    if (clock != previousTime) {
        previousTime = clock;
        lv_label_set_text(idle_status_time, clock.c_str());
    }

    char voltage[12];
    BatteryReading batteryReading;
    if (readBatteryMonitor(batteryReading)) {
        snprintf(voltage, sizeof(voltage), "%.2fV", batteryReading.voltageV);
    } else {
        snprintf(voltage, sizeof(voltage), "ERR");
    }
    lv_label_set_text(idle_status_voltage, voltage);
}

static void idle_slide_cb(lv_timer_t *) {
    if (idle_img_count < 2) return;
    idle_idx = (idle_idx + 1) % idle_img_count;
    lv_img_set_src(idle_img, idle_imgs[idle_idx]);
    lv_obj_fade_in(idle_img, 400, 0);
}

static void createIdleScreen() {
    prepareIdleImages();
    idle_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(idle_scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(idle_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(idle_scr, LV_OBJ_FLAG_SCROLLABLE);

    idle_img = lv_img_create(idle_scr);
    lv_img_set_src(idle_img, idle_imgs[0]);
    lv_obj_center(idle_img);

    lv_obj_t *status_bar = lv_obj_create(idle_scr);
    lv_obj_set_size(status_bar, 240, 22);
    lv_obj_set_pos(status_bar, 0, 0);
    lv_obj_set_style_bg_opa(status_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(status_bar, 0, 0);
    lv_obj_set_style_pad_all(status_bar, 0, 0);
    lv_obj_clear_flag(status_bar, LV_OBJ_FLAG_SCROLLABLE);


    idle_status_time = lv_label_create(status_bar);
    lv_obj_set_style_text_font(idle_status_time, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(idle_status_time, lv_color_hex(0x8CFF9B), 0);
    lv_obj_align(idle_status_time, LV_ALIGN_LEFT_MID, 5, 0);
    lv_label_set_text(idle_status_time, "--:--");

    lv_obj_t *battery_icon = lv_obj_create(status_bar);
    lv_obj_set_size(battery_icon, 13, 8);
    lv_obj_align(battery_icon, LV_ALIGN_RIGHT_MID, -51, 0);
    lv_obj_set_style_bg_opa(battery_icon, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(battery_icon, lv_color_hex(0x8CFF9B), 0);
    lv_obj_set_style_border_width(battery_icon, 1, 0);
    lv_obj_set_style_radius(battery_icon, 1, 0);
    lv_obj_set_style_pad_all(battery_icon, 0, 0);
    lv_obj_clear_flag(battery_icon, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *battery_terminal = lv_obj_create(status_bar);
    lv_obj_set_size(battery_terminal, 2, 4);
    lv_obj_align_to(battery_terminal, battery_icon, LV_ALIGN_OUT_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(battery_terminal, lv_color_hex(0x8CFF9B), 0);
    lv_obj_set_style_bg_opa(battery_terminal, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(battery_terminal, 0, 0);
    lv_obj_set_style_pad_all(battery_terminal, 0, 0);
    lv_obj_clear_flag(battery_terminal, LV_OBJ_FLAG_SCROLLABLE);

    idle_status_voltage = lv_label_create(status_bar);
    lv_obj_set_style_text_font(idle_status_voltage, &lv_font_unscii_8, 0);
    lv_obj_set_style_text_color(idle_status_voltage, lv_color_hex(0x8CFF9B), 0);
    lv_obj_align(idle_status_voltage, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_label_set_text(idle_status_voltage, "--.--V");

    idle_timer = lv_timer_create(idle_slide_cb, IDLE_SLIDE_MS, NULL);
    lv_timer_pause(idle_timer);          // chỉ chạy khi đang ở màn chờ
}

static lv_obj_t *createRescueIndicator(lv_obj_t *screen) {
    lv_obj_t *indicator = lv_label_create(screen);
    lv_obj_set_width(indicator, 240);
    lv_obj_align(indicator, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_text_font(indicator, &roboto, 0);
    lv_obj_set_style_text_color(indicator, lv_color_white(), 0);
    lv_obj_set_style_text_align(indicator, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(indicator, lv_color_hex(0xD00000), 0);
    lv_obj_set_style_bg_opa(indicator, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_top(indicator, 2, 0);
    lv_obj_set_style_pad_bottom(indicator, 2, 0);
    lv_label_set_text(indicator, "RESCUE AP ACTIVE - 192.168.4.1");
    lv_obj_add_flag(indicator, LV_OBJ_FLAG_HIDDEN);
    return indicator;
}

static void updateRescueIndicators() {
    const bool active = isRescueModeActive();
    if (!rescue_ui_indicator || !rescue_idle_indicator) return;
    static bool previousActive = false;
    if (active == previousActive) return;
    previousActive = active;

    if (active) {
        lv_obj_clear_flag(rescue_ui_indicator, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(rescue_idle_indicator, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(rescue_ui_indicator, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(rescue_idle_indicator, LV_OBJ_FLAG_HIDDEN);
    }
}

// Gọi trong DisplayTask, BÊN TRONG mutex (đọc game.*)
static void updateScreenMode() {
    if (encoderSettingsMode != EncoderSettingsMode::Closed) return;

    bool want_idle = false;
#if IDLE_WHEN_DISCONNECTED
    if (!game.connected) want_idle = true;
#endif
#if IDLE_WHEN_PAUSED
    if (game.pause) want_idle = true;
#endif

    static unsigned long since = 0;
    static bool last_want = false;
    unsigned long now = millis();
    if (want_idle != last_want) { last_want = want_idle; since = now; }   // trạng thái vừa đổi → đếm lại

    if (want_idle && !in_idle && now - since >= IDLE_ENTER_DELAY_MS) {
        in_idle = true;
        idle_idx = 0;
        lv_img_set_src(idle_img, idle_imgs[0]);
        lv_timer_reset(idle_timer);
        lv_timer_resume(idle_timer);
        lv_scr_load_anim(idle_scr, LV_SCR_LOAD_ANIM_FADE_IN, 400, 0, false);
        Serial.println("[UI] -> IDLE (images)");
    } else if (!want_idle && in_idle && now - since >= IDLE_EXIT_DELAY_MS) {
        in_idle = false;
        lv_timer_pause(idle_timer);
        lv_scr_load_anim(ui_scr, LV_SCR_LOAD_ANIM_FADE_IN, 300, 0, false);
        Serial.println("[UI] -> DASHBOARD");
    }
}

static lv_obj_t *createEncoderPageLabel(lv_obj_t *parent, lv_coord_t x,
                                         lv_coord_t y, lv_coord_t width,
                                         lv_color_t color) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    return label;
}

// ════════════════════════════════════════════════════════
// 4. createUI — xây widget tree một lần duy nhất
// ════════════════════════════════════════════════════════
static void createUI() {
    lv_obj_t *scr = lv_scr_act();
    ui_scr = scr;
    lv_obj_set_style_bg_color(scr, BGCOLOR, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // ── Header y=0..38 ────────────────────────────────────────
    lv_obj_t *hdr = lv_obj_create(scr);
    lv_obj_set_size(hdr, 240, 38);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_style_bg_color(hdr, BGCOLOR, 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 0, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lbl_truck_name = lv_label_create(hdr);
    lv_obj_set_style_text_font(lbl_truck_name, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_truck_name, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_pos(lbl_truck_name, 4, 2);
    lv_label_set_text(lbl_truck_name, "MAN TGX");

    lbl_plate = lv_label_create(hdr);
    lv_obj_set_style_text_font(lbl_plate, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_plate, lv_color_hex(0xFFD700), 0);
    lv_obj_set_pos(lbl_plate, 4, 20);
    lv_label_set_text(lbl_plate, "---");

    lbl_gear = lv_label_create(hdr);
    lv_obj_set_style_text_font(lbl_gear, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_gear, lv_color_hex(0xFFD700), 0);
    lv_obj_align(lbl_gear, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(lbl_gear, "N");

    icon_blinker_left = lv_img_create(scr);
    lv_img_set_src(icon_blinker_left, &left);
    lv_obj_add_flag(icon_blinker_left, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(icon_blinker_left, 8, 41);

    icon_blinker_right = lv_img_create(scr);
    lv_img_set_src(icon_blinker_right, &right);
    lv_obj_add_flag(icon_blinker_right, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(icon_blinker_right, 205, 41);

    lbl_clock = lv_label_create(hdr);
    lv_obj_set_style_text_font(lbl_clock, &roboto, 0);
    lv_obj_set_style_text_color(lbl_clock, lv_color_hex(0xFFD700), 0);
    lv_obj_align(lbl_clock, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_label_set_text(lbl_clock, "--:--");

    lbl_vol = lv_label_create(hdr);
    lv_obj_set_style_text_font(lbl_vol, &roboto, 0);
    lv_obj_set_style_text_color(lbl_vol, lv_color_hex(0xFFD700), 0);
    lv_obj_align(lbl_vol, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_label_set_text(lbl_vol, "V");
    // ── Divider xanh y=38 ─────────────────────────────────────
    make_divider(scr, 38, lv_color_hex(0x3A6EA5));

    // ── Row2: Speed  y=40..99 ─────────────────────────────────
    lbl_oil = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_oil,  &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_oil, lv_color_hex(0xFF9900), 0);
    lv_obj_set_pos(lbl_oil, 4, 108);
    lv_label_set_text(lbl_oil, "Oil:--C");

    lbl_h2o = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_h2o,  &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_h2o, lv_color_hex(0x00AAFF), 0);
    lv_obj_align(lbl_h2o, LV_ALIGN_TOP_RIGHT, -4, 108);
    lv_label_set_text(lbl_h2o, "H2O:--C");

    lbl_speed = lv_label_create(scr);
   // lv_obj_set_style_text_font(lbl_speed, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(lbl_speed, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lbl_speed, &obitron, 0);
    lv_obj_align(lbl_speed, LV_ALIGN_TOP_MID, 0, 45 + SPD_DY);
    lv_label_set_text(lbl_speed, "0");

    // ── Speed arc: nửa vòng tròn bao quanh lbl_speed ──────────
    arc_speed = lv_arc_create(scr);
    lv_obj_set_size(arc_speed, SPD_ARC_R * 2, SPD_ARC_R * 2);
    lv_obj_set_pos(arc_speed, SPD_ARC_CX - SPD_ARC_R, SPD_ARC_CY - SPD_ARC_R + 2 );
    lv_obj_remove_style(arc_speed, NULL, LV_PART_KNOB);          // bỏ núm tròn
    lv_obj_clear_flag(arc_speed, LV_OBJ_FLAG_CLICKABLE);         // chỉ để hiển thị
    lv_obj_set_style_bg_opa(arc_speed, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(arc_speed, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(arc_speed, 0, LV_PART_MAIN);
    lv_arc_set_rotation(arc_speed, 0);
    lv_arc_set_bg_angles(arc_speed, 180, 360);                   // nửa vòng phía trên: trái → phải
    lv_arc_set_range(arc_speed, 0, SPD_MAX);
    lv_arc_set_value(arc_speed, 0);
    lv_obj_set_style_arc_width(arc_speed, SPD_ARC_W, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc_speed, lv_color_hex(0x0A1A33), LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc_speed, SPD_ARC_W, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc_speed, lv_color_hex(0x00AAFF), LV_PART_INDICATOR);

    // ── RPM bar: đường đáy của đồng hồ ────────────────────────
    /* bar_rpm = lv_bar_create(scr);
    lv_obj_set_size(bar_rpm, 232, 8);
    lv_obj_set_pos(bar_rpm, 4, SPD_ARC_CY + 1);
    lv_bar_set_range(bar_rpm, 0, RPM_MAX_DEFAULT);
    lv_bar_set_value(bar_rpm, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar_rpm, lv_color_hex(0x0A0A2A), LV_PART_MAIN);
    lv_obj_set_style_border_color(bar_rpm, lv_color_hex(0x2244AA), LV_PART_MAIN);
    lv_obj_set_style_border_width(bar_rpm, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(bar_rpm, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_rpm, lv_color_hex(0x00CC66), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_rpm, 1, LV_PART_INDICATOR);
    lv_obj_set_style_anim_time(bar_rpm, 0, LV_PART_MAIN);   // tắt anim nội bộ của bar, dùng lv_anim riêng */

    // ── Row3: ODO + Dist  y=100..112 ──────────────────────────
    lbl_odo = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_odo, &fontodo, 0);
    lv_obj_set_style_text_color(lbl_odo, lv_color_hex(0xFFD700), 0);
    lv_obj_align(lbl_odo, LV_ALIGN_TOP_MID, 0, 95 + SPD_DY);
    lv_label_set_text(lbl_odo, "ODO:0km");

    lbl_dist = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_dist, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_dist, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(lbl_dist, LV_ALIGN_TOP_RIGHT, -4, 100 + SPD_DY);
    lv_label_set_text(lbl_dist, "0km");

    // icon fuel + bar + value, icon air + bar + value  y=113..134 ─────────
    lv_obj_t *ic_fuel = lv_img_create(scr);
    lv_img_set_src(ic_fuel, &fuel);
    lv_img_set_zoom(ic_fuel, 90);   // 128% → vừa với bar
    lv_obj_set_pos(ic_fuel, -5, 120);
    


    lv_obj_t *ic_air = lv_img_create(scr);
    lv_img_set_src(ic_air, &air);
    lv_img_set_zoom(ic_air, 135); 
    lv_obj_set_pos(ic_air, 110, 128);
  // 128% → vừa với bar   

    bar_fuel = lv_bar_create(scr);
    lv_obj_set_size(bar_fuel, 94, 13);
    lv_obj_set_pos(bar_fuel, 20, 116 + SPD_DY);
    lv_bar_set_range(bar_fuel, 0, 1000);
    lv_bar_set_value(bar_fuel, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar_fuel, lv_color_hex(0x0A0A2A), LV_PART_MAIN);
    lv_obj_set_style_border_color(bar_fuel, lv_color_hex(0x2244AA), LV_PART_MAIN);
    lv_obj_set_style_border_width(bar_fuel, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(bar_fuel, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_fuel, lv_color_hex(0x2266FF), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_fuel, 1, LV_PART_INDICATOR);

    lbl_fuel_val = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_fuel_val,  &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_fuel_val, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align_to(lbl_fuel_val, bar_fuel, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(lbl_fuel_val, "0");

    bar_air = lv_bar_create(scr);
    lv_obj_set_size(bar_air, 94, 13);
    lv_obj_set_pos(bar_air, 142, 116 + SPD_DY);
    lv_bar_set_range(bar_air, 0, 150);
    lv_bar_set_value(bar_air, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar_air, lv_color_hex(0x0A0A2A), LV_PART_MAIN);
    lv_obj_set_style_border_color(bar_air, lv_color_hex(0x336600), LV_PART_MAIN);
    lv_obj_set_style_border_width(bar_air, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(bar_air, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_air, lv_color_hex(0x88CC00), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_air, 1, LV_PART_INDICATOR);

    lbl_air_val = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_air_val,  &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_air_val, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align_to(lbl_air_val, bar_air, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(lbl_air_val, "0");

    // ── Row5: Dam Info + Wear  y=134..210 ─────────────────────
    lbl_dam = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_dam, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_dam, lv_color_hex(0x4499FF), 0);
    lv_obj_set_pos(lbl_dam, 2, 138 + SPD_DY);
    lv_label_set_text(lbl_dam, "Dam\nInfo");

    lbl_wear_t = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_wear_t,  &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_wear_t, lv_color_hex(0x88CC00), 0);
    lv_obj_set_pos(lbl_wear_t, 42, 150 + SPD_DY);
    lv_label_set_text(lbl_wear_t, "T:0%");

    lbl_wear_tr = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_wear_tr,  &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_wear_tr, lv_color_hex(0x88CC00), 0);
    lv_obj_align(lbl_wear_tr, LV_ALIGN_TOP_MID, 10, 150 + SPD_DY);
    lv_label_set_text(lbl_wear_tr, "Tr:0%");

    lbl_wear_c = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_wear_c,  &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_wear_c, lv_color_hex(0x88CC00), 0);
    lv_obj_align(lbl_wear_c, LV_ALIGN_TOP_RIGHT, -30, 150 + SPD_DY);
    lv_label_set_text(lbl_wear_c, "C:0%");


    static lv_obj_t *icon_row;

    icon_row = lv_obj_create(scr);
    lv_obj_remove_style_all(icon_row);
    lv_obj_set_size(icon_row, 160, 30);
    lv_obj_set_pos(icon_row, 240 - 160, 200);          // mép phải container = mép phải màn hình
    lv_obj_clear_flag(icon_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_set_flex_flow(icon_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(icon_row,
        LV_FLEX_ALIGN_END,      // main axis: dồn về cạnh PHẢI
        LV_FLEX_ALIGN_CENTER,   // cross axis
        LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(icon_row, 2, 0);

    // Thứ tự tạo = từ trái sang phải
    icon_highbeam = lv_img_create(icon_row);
    lv_img_set_src(icon_highbeam, &_40x30_hightbeam);
    lv_obj_add_flag(icon_highbeam, LV_OBJ_FLAG_HIDDEN);

    icon_lowbeam = lv_img_create(icon_row);
    lv_img_set_src(icon_lowbeam, &_40x30_lowbeam);
    lv_obj_add_flag(icon_lowbeam, LV_OBJ_FLAG_HIDDEN);

    icon_park = lv_img_create(icon_row);
    lv_img_set_src(icon_park, &park);
    
        //code  cũ
    /*  icon_park = lv_img_create(scr);
        lv_obj_set_pos(icon_park, 200, 200); 
        lv_img_set_src(icon_park, &park);
    //// code
        icon_lowbeam = lv_img_create(scr);
        lv_img_set_src(icon_lowbeam, &_40x30_lowbeam);
    //   lv_img_set_zoom(icon_lowbeam, 128);
        lv_obj_set_pos(icon_lowbeam, 158, 200);
        lv_obj_add_flag(icon_lowbeam, LV_OBJ_FLAG_HIDDEN);

        icon_highbeam = lv_img_create(scr);
        lv_img_set_src(icon_highbeam, &_40x30_hightbeam);
    // lv_img_set_zoom(icon_highbeam, 128);
        lv_obj_set_pos(icon_highbeam, 116, 200);
        lv_obj_add_flag(icon_highbeam, LV_OBJ_FLAG_HIDDEN);
    */
    //
    // ── Divider đỏ y=212 ──────────────────────────────────────
    make_divider(scr, 280, lv_color_hex(0xCC2200));

    // ── Footer  y=213..319 ────────────────────────────────────
    lbl_trl_prefix = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_trl_prefix, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_trl_prefix, lv_color_hex(0x88CC00), 0);
    lv_obj_set_pos(lbl_trl_prefix, 4, 282);
    lv_label_set_text(lbl_trl_prefix, "TRL:");

    lbl_trailer = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_trailer, &roboto, 0);
    lv_obj_set_style_text_color(lbl_trailer, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_pos(lbl_trailer, 36, 282);
    lv_obj_set_width(lbl_trailer, 200);
    lv_label_set_long_mode(lbl_trailer, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(lbl_trailer, "Không có hàng");

    lbl_time = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_time, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_time, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(lbl_time, "Time: --");
    lv_obj_align(lbl_time, LV_ALIGN_BOTTOM_LEFT, -4, 0);

    lbl_income = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_income, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_income, lv_color_hex(0xFFD700), 0);
    lv_label_set_text(lbl_income, "$:0");
    lv_obj_align(lbl_income, LV_ALIGN_BOTTOM_RIGHT, -4, 0);
        // ── Speed limit icon + value  y=282..319 ─────────────────────
    spd_limit = lv_img_create(scr);
    
    lv_img_set_src(spd_limit, &sp30);
    lv_obj_align_to(spd_limit,lbl_trl_prefix,LV_ALIGN_OUT_TOP_LEFT, -3, -10); 
    /* test = lv_label_create(scr);
    lv_obj_align_to(test,spd_limit,LV_ALIGN_OUT_RIGHT_TOP,5,5);
   // lv_label_set_text(test, "0"); */

    lv_obj_t *limit_draw = lv_obj_create(scr);
    lv_obj_set_size(limit_draw, 40, 40);
    lv_obj_align_to(limit_draw, spd_limit, LV_ALIGN_OUT_RIGHT_MID, 10, 0);
    lv_obj_set_style_bg_color(limit_draw, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_pad_all(limit_draw, 0, 0);
    lv_obj_set_style_radius(limit_draw, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(limit_draw, 5, 0);       // Độ dày viền
    lv_obj_set_style_border_color(limit_draw, lv_color_hex(0xf00), 0); 

    limit_val = lv_label_create(limit_draw);
    lv_obj_set_style_text_font(limit_val, &roboto, 0);
    lv_obj_set_style_text_color(limit_val, lv_color_hex(0x000000), 0);
    lv_label_set_text(limit_val, "0");
    lv_obj_align(limit_val, LV_ALIGN_CENTER, 0, 0);

    encoder_page_panel = lv_obj_create(scr);
    lv_obj_set_pos(encoder_page_panel, 0, 130);
    lv_obj_set_size(encoder_page_panel, 240, 148);
    lv_obj_set_style_bg_color(encoder_page_panel, BGCOLOR, 0);
    lv_obj_set_style_bg_opa(encoder_page_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(encoder_page_panel, 0, 0);
    lv_obj_set_style_radius(encoder_page_panel, 0, 0);
    lv_obj_set_style_pad_all(encoder_page_panel, 0, 0);
    lv_obj_clear_flag(encoder_page_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(encoder_page_panel, LV_OBJ_FLAG_HIDDEN);

    encoder_page_title = createEncoderPageLabel(
        encoder_page_panel, 6, 2, 228, lv_color_hex(0x00CCFF));
    lv_label_set_text(encoder_page_title, "ENGINE  2/3");

    const lv_color_t engineColor = lv_color_hex(0xFFFFFF);
    engine_page_values[0] = createEncoderPageLabel(encoder_page_panel, 6, 20, 108, lv_color_hex(0x00CC66));
    engine_page_values[1] = createEncoderPageLabel(encoder_page_panel, 122, 20, 108, lv_color_hex(0xFFD700));
    engine_page_values[2] = createEncoderPageLabel(encoder_page_panel, 6, 32, 108, lv_color_hex(0xFF9900));
    engine_page_values[3] = createEncoderPageLabel(encoder_page_panel, 122, 32, 108, lv_color_hex(0x00AAFF));
    engine_page_values[4] = createEncoderPageLabel(encoder_page_panel, 6, 44, 108, engineColor);
    engine_page_values[5] = createEncoderPageLabel(encoder_page_panel, 122, 44, 108, lv_color_hex(0xFF4444));
    engine_page_values[6] = createEncoderPageLabel(encoder_page_panel, 6, 56, 108, engineColor);
    engine_page_values[7] = createEncoderPageLabel(encoder_page_panel, 122, 56, 108, engineColor);

    trip_page_values[0] = createEncoderPageLabel(encoder_page_panel, 6, 20, 228, lv_color_hex(0xFFFFFF));
    trip_page_values[1] = createEncoderPageLabel(encoder_page_panel, 6, 34, 108, lv_color_hex(0xFFD700));
    trip_page_values[2] = createEncoderPageLabel(encoder_page_panel, 122, 34, 108, lv_color_hex(0x00CCFF));
    trip_page_values[3] = createEncoderPageLabel(encoder_page_panel, 6, 48, 228, lv_color_hex(0xFFFFFF));
    trip_page_values[4] = createEncoderPageLabel(encoder_page_panel, 6, 62, 108, lv_color_hex(0x88CC00));
    trip_page_values[5] = createEncoderPageLabel(encoder_page_panel, 122, 62, 108, lv_color_hex(0x88CC00));
    for (lv_obj_t *label : trip_page_values) {
        lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_move_foreground(spd_limit);
    lv_obj_move_foreground(limit_draw);

    Serial.println("[UI] Widgets created OK");
}

// ════════════════════════════════════════════════════════
// 5. wearColor helper
// ════════════════════════════════════════════════════════
static lv_color_t wearColor(int pct) {
    if (pct < 20) return lv_color_hex(0x88CC00);  // xanh
    if (pct < 60) return lv_color_hex(0xFFAA00);  // vàng
    return           lv_color_hex(0xFF4400);       // đỏ
}

// ════════════════════════════════════════════════════════
// 6. refreshUI — cập nhật widget từ data (gọi trong DisplayTask)
// ════════════════════════════════════════════════════════
static void refreshUI() {
    char buf[48];

    // ── Clock ─────────────────────────────────────────────────
    static String prev_clock;
    String cur_clock;
    if (game.connected && game.realTime.length() > 0) {
        cur_clock = game.realTime;
    } else {
        char currentDateTime[32];
        cur_clock = getCurrentLocalDateTime(currentDateTime, sizeof(currentDateTime))
            ? String(currentDateTime) : String(timeStr);
    }
    if (cur_clock != prev_clock) {
        prev_clock = cur_clock;
        lv_label_set_text(lbl_clock, cur_clock.c_str());
       
    }
   
    // ── Truck name + plate ────────────────────────────────────
    static String prev_name, prev_plate;
    String name = truck.make + " " + truck.model;
    if (name.length() > 14) name = name.substring(0, 13) + ".";
    if (name != prev_name) {
        prev_name = name;
        lv_label_set_text(lbl_truck_name, name.c_str());
    }
    String cur_plate = truck.licensePlate.length() > 0 ? truck.licensePlate : "---";
    if (cur_plate != prev_plate) {
        prev_plate = cur_plate;
        lv_label_set_text(lbl_plate, cur_plate.c_str());
    }

    // ── Gear ──────────────────────────────────────────────────
    static int prev_gear = -999;
    if (truck.displayedGear != prev_gear) {
        prev_gear = truck.displayedGear;
        if      (truck.displayedGear < 0)  snprintf(buf, sizeof(buf), "R%d", abs(truck.displayedGear));
        else if (truck.displayedGear == 0) snprintf(buf, sizeof(buf), "N");
        else                               snprintf(buf, sizeof(buf), "%d", truck.displayedGear);
        lv_label_set_text(lbl_gear, buf);
    }

    // ── Speed ─────────────────────────────────────────────────
    static int prev_speed = -1;
    static bool prev_speed_warn = false;
    bool speed_warn = truck.speedLimit > 0 && truck.speed > truck.speedLimit + 5;
    speed_arc_set_target(truck.rpm);          // cung tốc độ (mượt)
    if (truck.speed != prev_speed) {
        prev_speed = truck.speed;
        snprintf(buf, sizeof(buf), "%d", truck.speed);
        lv_label_set_text(lbl_speed, buf);
    }
    if (speed_warn != prev_speed_warn) {
        prev_speed_warn = speed_warn;
        speed_arc_set_warn(speed_warn);
        lv_obj_set_style_text_color(lbl_speed,
            speed_warn ? lv_color_hex(0xFF4444) : lv_color_hex(0xFFFFFF), 0);
    }

    static bool prev_blinker_left = false;
    if (truck.blinkerLeftOn != prev_blinker_left) {
        prev_blinker_left = truck.blinkerLeftOn;
        if (truck.blinkerLeftOn) {
            lv_obj_clear_flag(icon_blinker_left, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(icon_blinker_left, LV_OBJ_FLAG_HIDDEN);
        }
    }

    static bool prev_blinker_right = false;
    if (truck.blinkerRightOn != prev_blinker_right) {
        prev_blinker_right = truck.blinkerRightOn;
        if (truck.blinkerRightOn) {
            lv_obj_clear_flag(icon_blinker_right, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(icon_blinker_right, LV_OBJ_FLAG_HIDDEN);
        }
    }

    static bool prev_highbeam = false;
    if (truck.lightsBeamHighOn != prev_highbeam) {
        prev_highbeam = truck.lightsBeamHighOn;
        if (truck.lightsBeamHighOn) {
            lv_obj_clear_flag(icon_highbeam, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(icon_highbeam, LV_OBJ_FLAG_HIDDEN);
        }
    }

    static bool prev_lowbeam = false;
    if (truck.lightsBeamLowOn != prev_lowbeam) {
        prev_lowbeam = truck.lightsBeamLowOn;
        if (truck.lightsBeamLowOn) {
            lv_obj_clear_flag(icon_lowbeam, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(icon_lowbeam, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // ── RPM bar (mượt) ────────────────────────────────────────
    //rpm_set_target(truck.rpm, truck.rpmMax);

    // ── Oil ───────────────────────────────────────────────────
    static int prev_oil = -1;
    if (truck.oilTemperature != prev_oil) {
        prev_oil = truck.oilTemperature;
        snprintf(buf, sizeof(buf), "Oil:%dC", truck.oilTemperature);
        lv_label_set_text(lbl_oil, buf);
        lv_obj_set_style_text_color(lbl_oil,
            truck.oilTemperature > 105 ? lv_color_hex(0xFF4400) : lv_color_hex(0xFF9900), 0);
    }

    // ── H2O ───────────────────────────────────────────────────
    static int prev_h2o = -1;
    if (truck.waterTemperature != prev_h2o) {
        prev_h2o = truck.waterTemperature;
        snprintf(buf, sizeof(buf), "H2O:%dC", truck.waterTemperature);
        lv_label_set_text(lbl_h2o, buf);
        lv_obj_set_style_text_color(lbl_h2o,
            truck.waterTemperature > 95 ? lv_color_hex(0xFF4400) : lv_color_hex(0x00AAFF), 0);
    }

    // ── ODO ───────────────────────────────────────────────────
    static int prev_odo = -1;
    if (truck.odo != prev_odo) {
        prev_odo = truck.odo;
        snprintf(buf, sizeof(buf), "%d", truck.odo);
        lv_label_set_text(lbl_odo, buf);
    }

    // ── Distance ──────────────────────────────────────────────
    static int prev_dist = -1;
    if (navigation.estimatedDistance != prev_dist) {
        prev_dist = navigation.estimatedDistance;
        snprintf(buf, sizeof(buf), "%dkm", navigation.estimatedDistance);
        lv_label_set_text(lbl_dist, buf);
    }

    // ── Fuel bar ──────────────────────────────────────────────
    static int prev_fuel = -1;
    static bool prev_fuel_warn = false;
    if (truck.fuel != prev_fuel || truck.fuelWarningOn != prev_fuel_warn) {
        prev_fuel = truck.fuel;
        prev_fuel_warn = truck.fuelWarningOn;
        int fuelCap = max(1, truck.fuelCapacity);
        lv_bar_set_range(bar_fuel, 0, fuelCap);
        lv_bar_set_value(bar_fuel, truck.fuel, LV_ANIM_ON);
        snprintf(buf, sizeof(buf), "%d", truck.fuel);
        lv_label_set_text(lbl_fuel_val, buf);
        lv_obj_set_style_bg_color(bar_fuel,
            truck.fuelWarningOn ? lv_color_hex(0xFF4400) : lv_color_hex(0x2266FF),
            LV_PART_INDICATOR);
    }

    // ── Air bar ───────────────────────────────────────────────
    static int prev_air = -1;
    static bool prev_air_warn = false;
    if (truck.airPressure != prev_air || truck.airWarningOn != prev_air_warn) {
        prev_air = truck.airPressure;
        prev_air_warn = truck.airWarningOn;
        lv_bar_set_value(bar_air, truck.airPressure, LV_ANIM_ON);
        snprintf(buf, sizeof(buf), "%d", truck.airPressure);
        lv_label_set_text(lbl_air_val, buf);
        lv_obj_set_style_bg_color(bar_air,
            truck.airWarningOn ? lv_color_hex(0xFF4400) : lv_color_hex(0x88CC00),
            LV_PART_INDICATOR);
    }

    // ── Wear ──────────────────────────────────────────────────
    static int prev_wear_t = -1, prev_wear_tr = -1, prev_wear_c = -1;
    if (truck.wear != prev_wear_t) {
        prev_wear_t = truck.wear;
        snprintf(buf, sizeof(buf), "T:%d%%", truck.wear);
        lv_label_set_text(lbl_wear_t, buf);
        lv_obj_set_style_text_color(lbl_wear_t, wearColor(truck.wear), 0);
    }
    if (trailer.wear != prev_wear_tr) {
        prev_wear_tr = trailer.wear;
        snprintf(buf, sizeof(buf), "Tr:%d%%", trailer.wear);
        lv_label_set_text(lbl_wear_tr, buf);
        lv_obj_set_style_text_color(lbl_wear_tr, wearColor(trailer.wear), 0);
    }
    if (trailer.cargoWear != prev_wear_c) {
        prev_wear_c = trailer.cargoWear;
        snprintf(buf, sizeof(buf), "C:%d%%", trailer.cargoWear);
        lv_label_set_text(lbl_wear_c, buf);
        lv_obj_set_style_text_color(lbl_wear_c, wearColor(trailer.cargoWear), 0);
    }

    // ── Trailer ───────────────────────────────────────────────
    static String prev_trailer;
    String cur_trailer;
    if (trailer.attached && trailer.name.length() > 0) {
        int massT = trailer.mass / 1000;
        snprintf(buf, sizeof(buf), "%s (%dT)",
                 trailer.name.c_str(), massT);
        cur_trailer = buf;
    } else {
        cur_trailer = "--------Không có hàng hoá ------";
    }
    if (cur_trailer != prev_trailer) {
        prev_trailer = cur_trailer;
        lv_label_set_text(lbl_trailer, cur_trailer.c_str());
    }

    // ── Time remaining ────────────────────────────────────────
    static String prev_time;
    if (job.remainingTime.length() > 0 && job.remainingTime != prev_time) {
        prev_time = job.remainingTime;
        snprintf(buf, sizeof(buf), "Time: %s", job.remainingTime.c_str());
        lv_label_set_text(lbl_time, buf);
        lv_obj_set_style_text_color(lbl_time,
            job.remainingTime[0] == '-'
            ? lv_color_hex(0xFF4400) : lv_color_hex(0xFFFFFF), 0);
    }

    // ── Income ────────────────────────────────────────────────
    static int prev_income = -1;
    if (job.income != prev_income) {
        prev_income = job.income;
        snprintf(buf, sizeof(buf), "$:%d", job.income);
        lv_label_set_text(lbl_income, buf);
    }

    if (truck.parkingBreak == false){
        lv_obj_add_flag(icon_park, LV_OBJ_FLAG_HIDDEN);

    } else 
    {
        lv_obj_clear_flag(icon_park, LV_OBJ_FLAG_HIDDEN);
    }

     static int lass_spd_limit = 0;
     if (navigation.speed_limit != lass_spd_limit )
     {
        lass_spd_limit = navigation.speed_limit;
        // Gọi hàm lv_img_set_src() để thay đổi hình ảnh hiển thị
            int idx = (navigation.speed_limit - 30) / 10;  // 30→0, 40→1, ...
            if (idx >= 0 && idx <= 6) {
                lv_img_set_src(spd_limit, speed_imgs[idx]);
            }
           snprintf(buf, sizeof(buf), "$:%d", navigation.speed_limit);
            lv_label_set_text(limit_val, buf); 
  
     }

    static unsigned long last_read_vol = 0;
    if (millis() - last_read_vol > 1000) {
        BatteryReading batteryReading;
        if (readBatteryMonitor(batteryReading)) {
            const float currentA = batteryReading.currentValid
                ? batteryReading.currentMa / 1000.0f
                : 0.0f;
            const float powerW = batteryReading.currentValid
                ? batteryReading.voltageV * currentA
                : 0.0f;
            const char* state =
                batteryReading.currentValid
                    ? (batteryReading.currentMa > 50.0f ? "DISCHARGING" :
                       (batteryReading.currentMa < -50.0f ? "CHARGING" : "IDLE"))
                    : "UNKNOWN";
            snprintf(buf, sizeof(buf), "%.2f %+.2f", batteryReading.voltageV, currentA);
            lv_label_set_text(lbl_vol, buf);
            Serial.printf("[BATTERY] LIVE V=%.2fV I=%+.2fA P=%+.2fW state=%s SoC=%s%% cellTemp=%s monitorTemp=%s\n",
                          batteryReading.voltageV,
                          currentA,
                          powerW,
                          state,
                          batteryReading.socValid ? String(batteryReading.estimatedSocPercent, 0).c_str() : "n/a",
                          batteryReading.cellTempC > -500.0f ? String(batteryReading.cellTempC, 1).c_str() : "n/a",
                          batteryReading.monitorTempC > -500.0f ? String(batteryReading.monitorTempC, 1).c_str() : "n/a");
        } else {
            lv_label_set_text(lbl_vol, "--");
           // Serial.println("[BATTERY] Measurement unavailable");
        }
        last_read_vol = millis();
    }

    if (encoderPage == 1) {
        char gearText[8];
        char valueText[48];
        if (truck.displayedGear < 0) {
            snprintf(gearText, sizeof(gearText), "R%d", abs(truck.displayedGear));
        } else if (truck.displayedGear == 0) {
            snprintf(gearText, sizeof(gearText), "N");
        } else {
            snprintf(gearText, sizeof(gearText), "%d", truck.displayedGear);
        }

        snprintf(valueText, sizeof(valueText), "RPM: %d", truck.rpm);
        setLabelTextIfChanged(engine_page_values[0], valueText);
        snprintf(valueText, sizeof(valueText), "Gear: %s", gearText);
        setLabelTextIfChanged(engine_page_values[1], valueText);
        snprintf(valueText, sizeof(valueText), "Oil: %d C", truck.oilTemperature);
        setLabelTextIfChanged(engine_page_values[2], valueText);
        snprintf(valueText, sizeof(valueText), "Coolant: %d C", truck.waterTemperature);
        setLabelTextIfChanged(engine_page_values[3], valueText);
        snprintf(valueText, sizeof(valueText), "Engine: %s", truck.engineOn ? "ON" : "OFF");
        setLabelTextIfChanged(engine_page_values[4], valueText);
        snprintf(valueText, sizeof(valueText), "Check: %s", truck.checkEngOn ? "ON" : "OFF");
        setLabelTextIfChanged(engine_page_values[5], valueText);
        snprintf(valueText, sizeof(valueText), "Cruise: %s", truck.speedCtrlOn ? "ON" : "OFF");
        setLabelTextIfChanged(engine_page_values[6], valueText);
        snprintf(valueText, sizeof(valueText), "Cruise set: %d", truck.speedCtrl);
        setLabelTextIfChanged(engine_page_values[7], valueText);
    } else if (encoderPage == 2) {
        char trailerName[30];
        char valueText[64];
        if (trailer.attached && trailer.name.length() > 0) {
            snprintf(trailerName, sizeof(trailerName), "Trailer: %.22s", trailer.name.c_str());
        } else {
            snprintf(trailerName, sizeof(trailerName), "Trailer: none");
        }
        snprintf(valueText, sizeof(valueText), "Time left: %s",
                 job.remainingTime.length() ? job.remainingTime.c_str() : "--");
        setLabelTextIfChanged(trip_page_values[0], valueText);
        snprintf(valueText, sizeof(valueText), "Income: $%d", job.income);
        setLabelTextIfChanged(trip_page_values[1], valueText);
        snprintf(valueText, sizeof(valueText), "Distance: %d km",
                 navigation.estimatedDistance);
        setLabelTextIfChanged(trip_page_values[2], valueText);
        setLabelTextIfChanged(trip_page_values[3], trailerName);
        snprintf(valueText, sizeof(valueText), "Mass: %d t", trailer.mass / 1000);
        setLabelTextIfChanged(trip_page_values[4], valueText);
        snprintf(valueText, sizeof(valueText), "Wear: %d%% / %d%%",
                 trailer.wear, trailer.cargoWear);
        setLabelTextIfChanged(trip_page_values[5], valueText);
    }

}

static void refreshUIOLD() {
    char buf[48];

    // ── Clock ─────────────────────────────────────────────────
    lv_label_set_text(lbl_clock, game.realTime.length() > 0
                                 ? game.realTime.c_str() : timeStr);

    // ── Truck name + plate ────────────────────────────────────
    String name = truck.make + " " + truck.model;
    if (name.length() > 14) name = name.substring(0, 13) + ".";
    lv_label_set_text(lbl_truck_name, name.c_str());
    lv_label_set_text(lbl_plate,
        truck.licensePlate.length() > 0 ? truck.licensePlate.c_str() : "---");

    // ── Gear ──────────────────────────────────────────────────
    if      (truck.displayedGear < 0)  snprintf(buf, sizeof(buf), "R%d", abs(truck.displayedGear));
    else if (truck.displayedGear == 0) snprintf(buf, sizeof(buf), "N");
    else                               snprintf(buf, sizeof(buf), "%d", truck.displayedGear);
    lv_label_set_text(lbl_gear, buf);

    // ── Speed ─────────────────────────────────────────────────
    snprintf(buf, sizeof(buf), "%d", truck.speed);
    lv_label_set_text(lbl_speed, buf);
    lv_color_t sc = (truck.speedLimit > 0 && truck.speed > truck.speedLimit + 5)
                    ? lv_color_hex(0xFF4444) : lv_color_hex(0xFFFFFF);
    lv_obj_set_style_text_color(lbl_speed, sc, 0);

    // ── Oil ───────────────────────────────────────────────────
    snprintf(buf, sizeof(buf), "Oil:%dC", truck.oilTemperature);
    lv_label_set_text(lbl_oil, buf);
    lv_obj_set_style_text_color(lbl_oil,
        truck.oilTemperature > 105 ? lv_color_hex(0xFF4400) : lv_color_hex(0xFF9900), 0);

    // ── H2O ───────────────────────────────────────────────────
    snprintf(buf, sizeof(buf), "H2O:%dC", truck.waterTemperature);
    lv_label_set_text(lbl_h2o, buf);
    lv_obj_set_style_text_color(lbl_h2o,
        truck.waterTemperature > 95 ? lv_color_hex(0xFF4400) : lv_color_hex(0x00AAFF), 0);

    // ── ODO ───────────────────────────────────────────────────
    snprintf(buf, sizeof(buf), "%d", truck.odo);
    lv_label_set_text(lbl_odo, buf);

    // ── Distance ──────────────────────────────────────────────
    snprintf(buf, sizeof(buf), "%dkm", navigation.estimatedDistance);
    lv_label_set_text(lbl_dist, buf);

    // ── Fuel bar ──────────────────────────────────────────────
    int fuelCap = max(1, truck.fuelCapacity);
    lv_bar_set_range(bar_fuel, 0, fuelCap);
    lv_bar_set_value(bar_fuel, truck.fuel, LV_ANIM_ON);
    snprintf(buf, sizeof(buf), "%d", truck.fuel);
    lv_label_set_text(lbl_fuel_val, buf);
    lv_obj_set_style_bg_color(bar_fuel,
        truck.fuelWarningOn ? lv_color_hex(0xFF4400) : lv_color_hex(0x2266FF),
        LV_PART_INDICATOR);

    // ── Air bar ───────────────────────────────────────────────
    lv_bar_set_value(bar_air, truck.airPressure, LV_ANIM_ON);
    snprintf(buf, sizeof(buf), "%d", truck.airPressure);
    lv_label_set_text(lbl_air_val, buf);
    lv_obj_set_style_bg_color(bar_air,
        truck.airWarningOn ? lv_color_hex(0xFF4400) : lv_color_hex(0x88CC00),
        LV_PART_INDICATOR);

    // ── Wear ──────────────────────────────────────────────────
    snprintf(buf, sizeof(buf), "T:%d%%",  truck.wear);
    lv_label_set_text(lbl_wear_t, buf);
    lv_obj_set_style_text_color(lbl_wear_t, wearColor(truck.wear), 0);

    snprintf(buf, sizeof(buf), "Tr:%d%%", trailer.wear);
    lv_label_set_text(lbl_wear_tr, buf);
    lv_obj_set_style_text_color(lbl_wear_tr, wearColor(trailer.wear), 0);

    snprintf(buf, sizeof(buf), "C:%d%%",  trailer.cargoWear);
    lv_label_set_text(lbl_wear_c, buf);
    lv_obj_set_style_text_color(lbl_wear_c, wearColor(trailer.cargoWear), 0);

    // ── Trailer ───────────────────────────────────────────────
    if (trailer.attached && trailer.name.length() > 0) {
        int massT = trailer.mass / 1000;
        snprintf(buf, sizeof(buf), "%s (%dT)",
                 trailer.name.substring(0, 22).c_str(), massT);
        lv_label_set_text(lbl_trailer, buf);
    } else {
        lv_label_set_text(lbl_trailer, "--------Không có hàng hoá ------");
    }

    // ── Time remaining ────────────────────────────────────────
    if (job.remainingTime.length() > 0) {
        snprintf(buf, sizeof(buf), "Time: %s", job.remainingTime.c_str());
        lv_label_set_text(lbl_time, buf);
        lv_obj_set_style_text_color(lbl_time,
            job.remainingTime[0] == '-'
            ? lv_color_hex(0xFF4400) : lv_color_hex(0xFFFFFF), 0);
    }

    // ── Income ────────────────────────────────────────────────
    snprintf(buf, sizeof(buf), "$:%d", job.income);
    lv_label_set_text(lbl_income, buf);
}

// ════════════════════════════════════════════════════════
// 7. setupDisplay — gọi từ setup() trong main.cpp
// ════════════════════════════════════════════════════════
void setupDisplay() {
    // ── LovyanGFX init ─────────────────────────────────────────
    tft.init();
    tft.setBrightness(255);
    tft.setRotation(2);
    tft.fillScreen(0x0000);
    loadSavedAccessPin(configuredAccessPin, sizeof(configuredAccessPin));

    Serial.println("[TFT] Init OK");
    Serial.println("[PSRAM] Free: " + String(ESP.getFreePsram()));

/*     // ── Cấp phát LVGL buffer từ PSRAM ────────────────────────
    lvgl_buf1 = (lv_color_t*)ps_malloc(BUF_SIZE * sizeof(lv_color_t));
    lvgl_buf2 = (lv_color_t*)ps_malloc(BUF_SIZE * sizeof(lv_color_t));

    if (!lvgl_buf1 || !lvgl_buf2) {
        // PSRAM không đủ → fallback sang RAM thường
        Serial.println("[LVGL] PSRAM alloc failed, using DRAM");
        static lv_color_t static_buf1[BUF_SIZE];
        static lv_color_t static_buf2[BUF_SIZE];
        lvgl_buf1 = static_buf1;
        lvgl_buf2 = static_buf2;
    } else {
        Serial.println("[LVGL] Buffers in PSRAM OK");
    } */

    // ── LVGL init ─────────────────────────────────────────────
    lv_init();
        lvgl_create_buffers();
   // lv_disp_draw_buf_init(&draw_buf, lvgl_buf1, lvgl_buf2, BUF_SIZE);

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res  = LVGL_HOR_RES;
    disp_drv.ver_res  = LVGL_VER_RES;
    disp_drv.flush_cb = flush_cb;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);
    loadAssetsFromSd();
    setupRotaryEncoder();
    if (configuredAccessPin[0] != '\0') {
        createAccessPinScreen();
        lv_scr_load(accessPinScreen);
        while (!accessGranted) {
            pollRotaryEncoder();
            lv_timer_handler();
            delay(5);
        }
    } else {
        accessGranted = true;
        Serial.println("[AUTH] Boot PIN disabled");
    }

    lv_obj_t *dashboardBaseScreen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(dashboardBaseScreen, BGCOLOR, 0);
    lv_obj_set_style_bg_opa(dashboardBaseScreen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(dashboardBaseScreen, LV_OBJ_FLAG_SCROLLABLE);
    lv_scr_load(dashboardBaseScreen);

    showBootSequence();
    Serial.println("[LVGL] Init OK");

    // ── Tạo UI ────────────────────────────────────────────────
    createUI();
    createIdleScreen();
    rescue_ui_indicator = createRescueIndicator(ui_scr);
    rescue_idle_indicator = createRescueIndicator(idle_scr);

    Serial.println("[TFT] Display + Boot sequence OK");
}

// ════════════════════════════════════════════════════════
// 7b. MÀN HÌNH OTA — vẽ tiến trình nạp firmware (chạy trong DisplayTask)
// ════════════════════════════════════════════════════════
static lv_obj_t *ota_scr = nullptr, *ota_bar = nullptr, *ota_num = nullptr,
                *ota_pct_sign = nullptr, *ota_msg = nullptr;
static unsigned long ota_err_since = 0;
static int ota_last_pct = -1;

static void otaScreenCreate() {
    ota_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(ota_scr, lv_color_hex(0x000010), 0);
    lv_obj_set_style_bg_opa(ota_scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(ota_scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(ota_scr);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x00DDFF), 0);
    lv_label_set_text(title, "FIRMWARE UPDATE");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 60);

    ota_num = lv_label_create(ota_scr);
    lv_obj_set_style_text_font(ota_num, &obitron, 0);      // font số của dashboard
    lv_obj_set_style_text_color(ota_num, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(ota_num, "0");
    lv_obj_align(ota_num, LV_ALIGN_CENTER, -8, -10);

    ota_pct_sign = lv_label_create(ota_scr);
    lv_obj_set_style_text_font(ota_pct_sign, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(ota_pct_sign, lv_color_hex(0x888888), 0);
    lv_label_set_text(ota_pct_sign, "%");
    lv_obj_align_to(ota_pct_sign, ota_num, LV_ALIGN_OUT_RIGHT_BOTTOM, 4, -4);

    ota_bar = lv_bar_create(ota_scr);
    lv_obj_set_size(ota_bar, 200, 12);
    lv_obj_align(ota_bar, LV_ALIGN_CENTER, 0, 50);
    lv_bar_set_range(ota_bar, 0, 100);
    lv_obj_set_style_bg_color(ota_bar, lv_color_hex(0x0A1A33), LV_PART_MAIN);
    lv_obj_set_style_bg_color(ota_bar, lv_color_hex(0x00AAFF), LV_PART_INDICATOR);
    lv_obj_set_style_radius(ota_bar, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(ota_bar, 3, LV_PART_INDICATOR);

    ota_msg = lv_label_create(ota_scr);
    lv_obj_set_style_text_font(ota_msg, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ota_msg, lv_color_hex(0xFFD700), 0);
    lv_label_set_text(ota_msg, "Do not power off");
    lv_obj_align(ota_msg, LV_ALIGN_BOTTOM_MID, 0, -50);

    lv_scr_load(ota_scr);       // chuyển ngay, không hiệu ứng
    ota_last_pct = -1;
}

static void updateOtaScreen() {
    if (!ota_scr) otaScreenCreate();

    int pct = otaPct;
    if (pct != ota_last_pct) {
        ota_last_pct = pct;
        lv_bar_set_value(ota_bar, pct, LV_ANIM_OFF);
        lv_label_set_text_fmt(ota_num, "%d", pct);
        lv_obj_align(ota_num, LV_ALIGN_CENTER, -8, -10);
        lv_obj_align_to(ota_pct_sign, ota_num, LV_ALIGN_OUT_RIGHT_BOTTOM, 4, -4);
    }

    if (otaState == 2) {
        lv_label_set_text(ota_msg, "Done - rebooting...");
        lv_obj_set_style_text_color(ota_msg, lv_color_hex(0x00E676), 0);
    } else if (otaState == 3) {
        lv_label_set_text(ota_msg, "Update FAILED");
        lv_obj_set_style_text_color(ota_msg, lv_color_hex(0xFF4444), 0);
        if (ota_err_since == 0) ota_err_since = millis();
        else if (millis() - ota_err_since > 3000) {          // báo lỗi 3s rồi quay lại dashboard
            otaState = 0;
            ota_err_since = 0;
            lv_scr_load(!accessGranted ? accessPinScreen :
                        (in_idle ? idle_scr : ui_scr));
            lv_obj_del(ota_scr);
            ota_scr = nullptr;
        }
    }
}

// ════════════════════════════════════════════════════════
// 8. DisplayTask  (core 1, priority 2)
// ════════════════════════════════════════════════════════
void DisplayTask(void* parameter) {
    Serial.println("[DisplayTask] Started on core " + String(xPortGetCoreID()));
    Serial.println("[DisplayTask] Free heap: " + String(ESP.getFreeHeap()));
    displayTimingActive = true;
    bool startupMelodyPending =
        esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_TIMER;

    for (;;) {
        unsigned long now = millis();
        pollRotaryEncoder();
        reportDisplayPerformance(now);
        updateRescueIndicators();

        if (now - lastTFTUpdate >= TFT_UPDATE_INTERVAL) {
            lastTFTUpdate = now;

            if (otaState != 0) {             // đang nạp firmware qua WiFi → chỉ vẽ màn OTA
                cycleTelemetrySnapshotValid = false;
                updateOtaScreen();
                lv_timer_handler();
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }

            // Đọc data có mutex
            cycleTelemetrySnapshotValid = false;
            if (accessGranted &&
                xSemaphoreTake(dataAccessMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                updateScreenMode();          // chọn dashboard / ảnh chờ
                if (!in_idle) {
                    const uint32_t uiStartedUs = micros();
                    refreshUI();              // đang hiện ảnh thì khỏi cập nhật widget
                    recordDisplayTiming(uiTiming, micros() - uiStartedUs);
                    getTelemetryTimingSnapshot(&cycleTelemetryUpdatedAtUs,
                                               &cycleTelemetryGeneration);
                    cycleTelemetrySnapshotValid = true;
                }
                else updateIdleStatus();
                xSemaphoreGive(dataAccessMutex);
            }

            // lv_task_handler() NGOÀI mutex — flush SPI không cần mutex
            cycleFlushUs = 0;
            cycleFlushCount = 0;
            const uint32_t lvglStartedUs = micros();
            lv_timer_handler();
            recordDisplayTiming(lvglTiming, micros() - lvglStartedUs);
            if (cycleFlushCount > 0) recordDisplayTiming(flushTiming, cycleFlushUs);
            if (startupMelodyPending && cycleFlushCount > 0) {
                startupMelodyPending = false;
                playStartupCompletionMelody();
            }

            if (++displayTimingCycles >= 500) reportDisplayTimingWindow();
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
