# SdPsramImages

Arduino library for **ESP32-S3**: mount an SD card over SDMMC (1-bit or 4-bit, custom pins) and load
**LVGL 8 binary images** (`.bin`, RGB565) from it into **PSRAM**, so a display can redraw them without
touching the card again. Works with any graphics library (LovyanGFX, TFT_eSPI, LVGL...).

## Requirements
- ESP32-S3 with PSRAM enabled (Arduino IDE: *Tools -> PSRAM -> OPI PSRAM* for ESP32-S3R8 boards)
- Arduino-ESP32 core with `SD_MMC`
- Image files in **LVGL 8 binary format** with `LV_COLOR_DEPTH 16`: 4-byte header + pixels.
  Accepted: `TRUE_COLOR` (cf 4), `TRUE_COLOR_ALPHA` (cf 5, 3 bytes/px), `TRUE_COLOR_CHROMA_KEYED` (cf 6).
  Everything else (LVGL 9 files, indexed, raw/PNG) is rejected (`Err::BadHeader`).

## Quick start
```cpp
#include <SdPsramImages.h>
using namespace sdpsram;

const SdPins PINS = {38, 40, 39, 41, 48, 47};   // clk, cmd, d0, d1, d2, d3
ImageStore images(20);                           // up to 20 images

void setup() {
  Serial.begin(115200);
  SdCard::begin(PINS);
  images.setLog(&Serial);          // optional
  images.loadDir("/ets2");         // every *.bin in the folder

  const PsramImage *im = images.get(0);
  // LovyanGFX: tft.pushImage(0, 0, im->w, im->h, (const lgfx::rgb565_t *)im->pixels);
}
```

## Loading one specific file
```cpp
Err r = images.loadFile("/ets2/combine.bin");
if (r != Err::Ok) Serial.println(ImageStore::errStr(r));
const PsramImage *im = images.find("combine.bin");   // or images.get(images.count() - 1)
```
Paths are relative to the card root, like `SD_MMC.open()` (no `/sdcard` prefix).

## LVGL 8
```cpp
#define SDPSRAM_LVGL          // before the include
#include <SdPsramImages.h>

static lv_img_dsc_t dsc;      // must outlive the LVGL object (global / static)
toLvglDsc(*images.get(0), dsc);
lv_img_set_src(img, &dsc);
```
`toLvglDsc` does not copy pixels; `dsc.data` points into PSRAM. Do not call `images.clear()` while LVGL still
uses the descriptor.

## Browse, delete, choose what gets loaded
```cpp
FileEntry list[32];
size_t total = ImageStore::listDir("/ets2", list, 32);   // reads only the 4-byte header of each file
for (size_t i = 0; i < total && i < 32; i++)
  Serial.printf("%s %ux%u %u KB valid=%d loaded=%d\n", list[i].name, list[i].w, list[i].h,
                (unsigned)(list[i].size / 1024), list[i].valid, images.isLoaded(list[i].name));

images.loadFile("/ets2/a.bin");           // load one
images.unload("a");                        // free it from PSRAM (file stays on the card)
images.deleteFile("/ets2", "a.bin");      // unload + delete from the card
images.saveList("/ets2/loaded.txt");      // remember the selection (one name per line)
images.loadFromList("/ets2", "/ets2/loaded.txt");   // at boot: load only the selection
```
`unload`, `deleteFile`, `clear` and `sortByName` invalidate pointers from `get()`/`find()`. Make sure no display object is
still using an image before unloading it. File names must be shorter than 64 characters; longer ones are ignored.
See `examples/WebManager` for a complete WiFi file manager (list, load/unload, delete, upload).

### `LvImageSet` (LVGL 8 only)
Wraps an `ImageStore` and keeps a permanent `lv_img_dsc_t` per image (files are sorted by name):
```cpp
#define SDPSRAM_LVGL
#include <SdPsramImages.h>
using namespace sdpsram;

static LvImageSet icons(32);
icons.loadDir("/ets2/icons");
lv_img_set_src(obj, icons.find("left"));      // nullptr if the file is missing -> check first
lv_img_set_src(bg,  icons.get(0));
```
`find()` ignores case and the `.bin` extension. Each descriptor keeps a fixed address while its image is loaded,
even if other images are unloaded or the list is re-sorted. Use `LvImageSet`'s own `loadDir/loadFile/loadFromList/unload/deleteFile`
(not `store.load*`) so descriptors stay in sync.

## API

### `SdCard`
| Function | Description |
|---|---|
| `bool begin(const SdPins&, bool mode1bit=false, bool formatIfFail=false, uint32_t freqKHz=20000, const char* mount="/sdcard")` | Mount the card. `formatIfFail=true` erases the card if mounting fails. |
| `bool mkdir(const char* path)` | Create a folder if it does not exist. |
| `void end()` | Unmount. |

### `ImageStore`
| Function | Description |
|---|---|
| `ImageStore(size_t maxImages=20)` | Capacity of the list. |
| `Err loadFile(const char* path, const char* name=nullptr)` | Load one file; appended to the list. |
| `size_t loadDir(const char* dir)` | Load every `*.bin` in a folder; returns how many loaded. Order is directory order, not alphabetical. |
| `size_t loadFromList(dir, listPath)` | Load only the names listed in a text file. |
| `bool saveList(listPath) const` | Save the names of the loaded images. |
| `static size_t listDir(dir, FileEntry*, max)` | List `*.bin` files with size/dimensions without loading them. |
| `static bool isValidFile(path)` | Check one LVGL 8 `.bin` header without allocating PSRAM. |
| `bool unload(name)` / `bool unloadAt(i)` | Free one image from PSRAM. |
| `bool deleteFile(dir, name)` | Unload and delete from the card. |
| `static bool removeFile(dir, name)` | Delete from the card only. |
| `bool isLoaded(name) const` | Is this file in PSRAM? |
| `void sortByName()` | Sort the list by file name (case-insensitive). |
| `size_t count() const` | Number of loaded images. |
| `const PsramImage* get(size_t i) const` | Image by index, `nullptr` if out of range. |
| `const PsramImage* find(const char* name) const` | Image by file name (case-insensitive, `.bin` optional), `nullptr` if absent. |
| `int findIndex(const char* name) const` | Same, returns the index or -1. |
| `void clear()` | Free all images. Every pointer from `get`/`find` becomes invalid. |
| `size_t bytesUsed() const` | PSRAM used by the images. |
| `void setLog(Print*)` | Print load/skip messages (e.g. `&Serial`). |
| `static const char* errStr(Err)` | Error code to text. |

### `PsramImage`
`pixels`, `w`, `h`, `cf` (4/5/6), `hasAlpha()`, `dataSize`, `loadMs` (read time in ms), `name`.
`pixels` is plain RGB565 only when `cf == 4`.

### `Err`
`Ok`, `OpenFail`, `TooSmall`, `NoPsram`, `ReadFail`, `BadHeader`, `Full`.

## Notes
- Pixels are used in place from PSRAM; keep LVGL draw buffers in internal DMA-capable RAM.
- Byte order must match your display setup (`LV_COLOR_16_SWAP` for LVGL, `swap565_t`/`rgb565_t` in LovyanGFX).
- Tested only for syntax against mock headers; test on hardware before relying on it.

## License
Add a LICENSE file of your choice before publishing.
