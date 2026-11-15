// Load ONE specific file from the SD card into PSRAM.
#include <SdPsramImages.h>
using namespace sdpsram;

const SdPins PINS = {38, 40, 39, 41, 48, 47};   // clk, cmd, d0, d1, d2, d3
ImageStore images(4);

void setup() {
  Serial.begin(115200);
  delay(2000);

  if (!SdCard::begin(PINS)) { Serial.println("SD mount failed"); return; }

  // Path is relative to the card root (do NOT write "/sdcard/...").
  Err r = images.loadFile("/ets2/combine.bin");
  if (r != Err::Ok) {
    Serial.printf("Load failed: %s\n", ImageStore::errStr(r));
    return;
  }

  // loadFile() appends to the list, so the new image is the last one...
  const PsramImage *im = images.get(images.count() - 1);
  // ...or look it up by name:
  im = images.find("combine.bin");

  if (im) Serial.printf("%s %ux%u, loaded in %u ms\n", im->name, im->w, im->h, (unsigned)im->loadMs);
  // LovyanGFX: tft.pushImage(0, 0, im->w, im->h, (const lgfx::rgb565_t *)im->pixels);
}

void loop() {}
