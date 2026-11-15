#include <SdPsramImages.h>
using namespace sdpsram;

// Pins of the 2.8" ESP32-S3 display board (from the vendor sample)
const SdPins PINS = {38 /*clk*/, 40 /*cmd*/, 39 /*d0*/, 41 /*d1*/, 48 /*d2*/, 47 /*d3*/};

ImageStore images(20);

void setup() {
  Serial.begin(115200);
  delay(2000);

  if (!SdCard::begin(PINS)) { Serial.println("SD mount failed"); return; }
  SdCard::mkdir("/ets2");

  images.setLog(&Serial);
  images.loadDir("/ets2");

  for (size_t i = 0; i < images.count(); i++) {
    const PsramImage *im = images.get(i);
    Serial.printf("[%u] %s %ux%u load %u ms\n", (unsigned)i, im->name, im->w, im->h, (unsigned)im->loadMs);
    // LovyanGFX: tft.pushImage(0, 0, im->w, im->h, (const lgfx::rgb565_t *)im->pixels);
    // LVGL 8 (define SDPSRAM_LVGL before the include): toLvglDsc(*im, dsc);
  }
}

void loop() {}
