#pragma once
// SdPsramImages - SD_MMC mount helper + LVGL 8 ".bin" image loader into PSRAM,
// with browse / delete / selective-load helpers.
//
// Supported files: LVGL 8 binary (4-byte header) with LV_COLOR_DEPTH 16:
//   cf 4 = TRUE_COLOR (2 bytes/px), cf 5 = TRUE_COLOR_ALPHA (3 bytes/px),
//   cf 6 = TRUE_COLOR_CHROMA_KEYED (2 bytes/px).
// Optional LVGL helper (LvImageSet): define SDPSRAM_LVGL before including this header.

#include <Arduino.h>
#include <FS.h>
#include <SD_MMC.h>
#include <vector>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#ifdef SDPSRAM_LVGL
  #include <lvgl.h>
  #if LVGL_VERSION_MAJOR != 8
    #error "SdPsramImages LVGL helper supports LVGL 8 only"
  #endif
#endif

namespace sdpsram {

constexpr size_t kNameLen = 64;      // max file name length incl. terminator (longer names are skipped)

// ---------------- SD card ----------------
struct SdPins {
  int8_t clk, cmd, d0, d1, d2, d3;   // d1..d3 = -1 for 1-bit mode
};

class SdCard {
public:
  // freqKHz: 20000 = default 20MHz; 40000 needs a good card.
  static bool begin(const SdPins &pins, bool mode1bit = false, bool formatIfFail = false,
                    uint32_t freqKHz = 20000, const char *mount = "/sdcard");
  static bool mkdir(const char *path);   // path relative to card root, e.g. "/ets2"
  static void end();
};

// ---------------- Images ----------------
struct PsramImage {
  uint8_t *raw = nullptr;            // allocation start (header + pixels), freed by ImageStore
  const uint8_t *pixels = nullptr;   // pixel data (raw + 4)
  uint16_t w = 0, h = 0;
  uint8_t cf = 0;                    // LVGL 8 color format: 4, 5 or 6
  uint32_t dataSize = 0;             // w * h * bytes-per-pixel
  uint32_t loadMs = 0;               // time to read the file into PSRAM
  char name[kNameLen] = {0};
  void *user = nullptr;              // free for the caller (used by LvImageSet); follows the image
  bool hasAlpha() const { return cf == 5; }
};

// One *.bin file on the card, read WITHOUT loading it into PSRAM (only the 4-byte header is read).
struct FileEntry {
  char name[kNameLen] = {0};
  uint32_t size = 0;
  uint16_t w = 0, h = 0;
  uint8_t cf = 0;
  bool valid = false;                // header is a supported LVGL 8 16-bit .bin
};

enum class Err : uint8_t { Ok, OpenFail, TooSmall, NoPsram, ReadFail, BadHeader, Full };

class ImageStore {
public:
  explicit ImageStore(size_t maxImages = 20);
  ~ImageStore();
  ImageStore(const ImageStore &) = delete;
  ImageStore &operator=(const ImageStore &) = delete;

  void setLog(Print *p) { log_ = p; }                 // e.g. store.setLog(&Serial);

  // ----- load -----
  // path is relative to the card root ("/ets2/a.bin"), same as SD_MMC.open().
  Err loadFile(const char *path, const char *name = nullptr);
  // Loads every "*.bin" in dir (not recursive). Returns number of images loaded.
  size_t loadDir(const char *dir);
  // Loads only the files named in listPath (one file name per line, '#' = comment).
  size_t loadFromList(const char *dir, const char *listPath);
  // Writes the names of the currently loaded images to listPath (the "selection").
  bool saveList(const char *listPath) const;

  // ----- browse (static: nothing is loaded into PSRAM) -----
  // Fills out[0..maxEntries) sorted by name; returns the TOTAL number of *.bin files in dir.
  static size_t listDir(const char *dir, FileEntry *out, size_t maxEntries);
  // Validate one LVGL 8 .bin file by reading its header only (no PSRAM allocation).
  static bool isValidFile(const char *path);

  // ----- unload / delete -----
  bool unloadAt(size_t i);                            // free one image from PSRAM
  bool unload(const char *name);
  // Remove file from the card only. name must not contain '/', '\\' or "..".
  static bool removeFile(const char *dir, const char *name);
  // Unload (if loaded) AND remove from the card.
  bool deleteFile(const char *dir, const char *name);

  void sortByName();                                  // case-insensitive
  void clear();                                       // free all images

  // ----- query -----
  // NOTE: pointers from get()/find() are invalidated by unload*, deleteFile, clear, sortByName and loads.
  size_t count() const { return imgs_.size(); }
  const PsramImage *get(size_t i) const { return i < imgs_.size() ? &imgs_[i] : nullptr; }
  // Name match is case-insensitive; the ".bin" extension is optional ("left" == "left.bin").
  int findIndex(const char *name) const;              // -1 if not loaded
  const PsramImage *find(const char *name) const;     // nullptr if not loaded
  bool isLoaded(const char *name) const { return findIndex(name) >= 0; }
  void setUser(size_t i, void *p) { if (i < imgs_.size()) imgs_[i].user = p; }
  size_t bytesUsed() const;
  static const char *errStr(Err e);

private:
  std::vector<PsramImage> imgs_;
  size_t max_;
  Print *log_ = nullptr;
};

#ifdef SDPSRAM_LVGL
// Fill an LVGL descriptor from a loaded image. No pixel copy: data points into PSRAM.
inline void toLvglDsc(const PsramImage &im, lv_img_dsc_t &d) {
  memset(&d, 0, sizeof(d));
  d.header.cf = im.cf;
  d.header.w = im.w;
  d.header.h = im.h;
  d.data_size = im.dataSize;
  d.data = im.pixels;
}

// ImageStore + a permanent lv_img_dsc_t per image, ready for lv_img_set_src().
// Each descriptor has a fixed address for as long as its image is loaded, even when other images
// are unloaded or the list is re-sorted. Before unload()/deleteFile()/clear(), make sure no lv_img
// object still shows that image (switch it to another source and refresh).
class LvImageSet {
public:
  explicit LvImageSet(size_t maxImages = 20) : store(maxImages) {}
  ~LvImageSet() { clear(); }
  LvImageSet(const LvImageSet &) = delete;
  LvImageSet &operator=(const LvImageSet &) = delete;

  ImageStore store;                                   // setLog(), listDir(), saveList()... Load only via LvImageSet.

  size_t loadDir(const char *dir) { size_t n = store.loadDir(dir); store.sortByName(); sync(); return n; }
  size_t loadFromList(const char *dir, const char *list) {
    size_t n = store.loadFromList(dir, list); store.sortByName(); sync(); return n;
  }
  Err loadFile(const char *path, const char *name = nullptr) {
    Err r = store.loadFile(path, name); sync(); return r;
  }
  bool unload(const char *name) {
    int i = store.findIndex(name);
    if (i < 0) return false;
    freeDsc((size_t)i);
    return store.unloadAt((size_t)i);
  }
  bool deleteFile(const char *dir, const char *name) {
    unload(name);
    return ImageStore::removeFile(dir, name);
  }
  void clear() {
    for (size_t i = 0; i < store.count(); i++) freeDsc(i);
    store.clear();
  }

  size_t count() const { return store.count(); }
  const lv_img_dsc_t *get(size_t i) const {
    const PsramImage *im = store.get(i);
    return im ? (const lv_img_dsc_t *)im->user : nullptr;
  }
  const lv_img_dsc_t *find(const char *name) const {
    int i = store.findIndex(name);
    return i < 0 ? nullptr : get((size_t)i);
  }

private:
  void sync() {
    for (size_t i = 0; i < store.count(); i++) {
      const PsramImage *im = store.get(i);
      lv_img_dsc_t *d = (lv_img_dsc_t *)im->user;
      if (!d) {
        d = (lv_img_dsc_t *)calloc(1, sizeof(lv_img_dsc_t));
        if (!d) continue;
        store.setUser(i, d);
      }
      toLvglDsc(*im, *d);
    }
  }
  void freeDsc(size_t i) {
    const PsramImage *im = store.get(i);
    if (im && im->user) {
      lv_img_cache_invalidate_src(im->user);
      free(im->user);
      store.setUser(i, nullptr);
    }
  }
};
#endif

}  // namespace sdpsram
