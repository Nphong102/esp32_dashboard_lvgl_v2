#include "SdPsramImages.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <algorithm>

namespace sdpsram {

static bool validName(const char *n) {
  if (!n || !*n || strlen(n) >= kNameLen) return false;
  return !strchr(n, '/') && !strchr(n, '\\') && !strstr(n, "..");
}

static bool isBinName(const char *n) {
  size_t len = strlen(n);
  return len >= 5 && strcasecmp(n + len - 4, ".bin") == 0;
}

// Decode an LVGL 8 header (little-endian uint32) and check it against the file size.
static bool parseHeader(uint32_t hdr, size_t fileSize, uint8_t &cf, uint16_t &w, uint16_t &h, uint32_t &bpp) {
  uint32_t c = hdr & 0x1F, ww = (hdr >> 10) & 0x7FF, hh = (hdr >> 21) & 0x7FF;
  bpp = (c == 5) ? 3 : ((c == 4 || c == 6) ? 2 : 0);   // 16-bit color depth
  if (!bpp || ww == 0 || hh == 0 || fileSize != (size_t)ww * hh * bpp + 4) return false;
  cf = (uint8_t)c; w = (uint16_t)ww; h = (uint16_t)hh;
  return true;
}

// ---------------- SdCard ----------------
bool SdCard::begin(const SdPins &p, bool mode1bit, bool formatIfFail,
                   uint32_t freqKHz, const char *mount) {
  if (!SD_MMC.setPins(p.clk, p.cmd, p.d0, p.d1, p.d2, p.d3)) return false;
  return SD_MMC.begin(mount, mode1bit, formatIfFail, (int)freqKHz);
}

bool SdCard::mkdir(const char *path) {
  return SD_MMC.exists(path) || SD_MMC.mkdir(path);
}

void SdCard::end() { SD_MMC.end(); }

// ---------------- ImageStore ----------------
ImageStore::ImageStore(size_t maxImages) : max_(maxImages) { imgs_.reserve(maxImages); }
ImageStore::~ImageStore() { clear(); }

void ImageStore::clear() {
  for (auto &im : imgs_) free(im.raw);
  imgs_.clear();
}

bool ImageStore::unloadAt(size_t i) {
  if (i >= imgs_.size()) return false;
  free(imgs_[i].raw);
  imgs_.erase(imgs_.begin() + (long)i);
  return true;
}

bool ImageStore::unload(const char *name) {
  int i = findIndex(name);
  return i >= 0 && unloadAt((size_t)i);
}

bool ImageStore::removeFile(const char *dir, const char *name) {
  if (!validName(name)) return false;
  char path[160];
  snprintf(path, sizeof(path), "%s/%s", dir, name);
  return SD_MMC.remove(path);
}

bool ImageStore::deleteFile(const char *dir, const char *name) {
  unload(name);
  return removeFile(dir, name);
}

size_t ImageStore::bytesUsed() const {
  size_t n = 0;
  for (auto &im : imgs_) n += im.dataSize + 4;
  return n;
}

void ImageStore::sortByName() {
  std::sort(imgs_.begin(), imgs_.end(),
            [](const PsramImage &a, const PsramImage &b) { return strcasecmp(a.name, b.name) < 0; });
}

int ImageStore::findIndex(const char *name) const {
  char alt[kNameLen + 8];
  snprintf(alt, sizeof(alt), "%s.bin", name);
  for (size_t i = 0; i < imgs_.size(); i++)
    if (!strcasecmp(imgs_[i].name, name) || !strcasecmp(imgs_[i].name, alt)) return (int)i;
  return -1;
}

const PsramImage *ImageStore::find(const char *name) const {
  int i = findIndex(name);
  return i < 0 ? nullptr : &imgs_[(size_t)i];
}

const char *ImageStore::errStr(Err e) {
  switch (e) {
    case Err::Ok:        return "ok";
    case Err::OpenFail:  return "cannot open file";
    case Err::TooSmall:  return "file too small";
    case Err::NoPsram:   return "out of PSRAM";
    case Err::ReadFail:  return "read failed";
    case Err::BadHeader: return "unsupported .bin (need LVGL 8, 16-bit true color)";
    case Err::Full:      return "image list full";
  }
  return "?";
}

Err ImageStore::loadFile(const char *path, const char *name) {
  if (imgs_.size() >= max_) return Err::Full;

  File f = SD_MMC.open(path, FILE_READ);
  if (!f) return Err::OpenFail;

  size_t sz = f.size();
  if (sz < 8) { f.close(); return Err::TooSmall; }

  uint8_t *buf = (uint8_t *)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
  if (!buf) { f.close(); return Err::NoPsram; }

  uint32_t t0 = millis();
  size_t got = f.read(buf, sz);
  uint32_t ms = millis() - t0;
  f.close();
  if (got != sz) { free(buf); return Err::ReadFail; }

  uint32_t hdr, bpp;
  memcpy(&hdr, buf, 4);
  uint8_t cf; uint16_t w, h;
  if (!parseHeader(hdr, sz, cf, w, h, bpp)) { free(buf); return Err::BadHeader; }

  PsramImage im;
  im.raw = buf;
  im.pixels = buf + 4;
  im.w = w;
  im.h = h;
  im.cf = cf;
  im.dataSize = (uint32_t)w * h * bpp;
  im.loadMs = ms;
  if (!name) {
    const char *slash = strrchr(path, '/');
    name = slash ? slash + 1 : path;
  }
  strncpy(im.name, name, kNameLen - 1);
  imgs_.push_back(im);

  if (log_) log_->printf("LOAD %-28s %ux%u cf%u  %u byte  %u ms\n",
                         im.name, (unsigned)w, (unsigned)h, (unsigned)cf, (unsigned)sz, (unsigned)ms);
  return Err::Ok;
}

size_t ImageStore::loadDir(const char *dir) {
  File d = SD_MMC.open(dir);
  if (!d || !d.isDirectory()) {
    if (log_) log_->printf("Directory not found: %s\n", dir);
    return 0;
  }
  size_t loaded = 0;
  uint32_t t0 = millis();
  File e;
  while ((e = d.openNextFile())) {
    char nm[kNameLen];
    strncpy(nm, e.name(), sizeof(nm) - 1);
    nm[sizeof(nm) - 1] = 0;
    bool isDir = e.isDirectory();
    bool tooLong = strlen(e.name()) >= kNameLen;
    e.close();
    if (isDir || tooLong || !isBinName(nm)) continue;

    char path[160];
    snprintf(path, sizeof(path), "%s/%s", dir, nm);
    Err r = loadFile(path, nm);
    if (r == Err::Ok) loaded++;
    else if (log_) log_->printf("SKIP %s (%s)\n", nm, errStr(r));
    if (r == Err::Full) break;
  }
  d.close();
  if (log_) log_->printf("Loaded %u image(s) from %s in %u ms\n", (unsigned)loaded, dir, (unsigned)(millis() - t0));
  return loaded;
}

size_t ImageStore::loadFromList(const char *dir, const char *listPath) {
  File f = SD_MMC.open(listPath, FILE_READ);
  if (!f) return 0;
  size_t sz = f.size();
  if (sz == 0 || sz > 8192) { f.close(); return 0; }
  char *buf = (char *)malloc(sz + 1);
  if (!buf) { f.close(); return 0; }
  size_t got = f.read((uint8_t *)buf, sz);
  f.close();
  buf[got] = 0;

  size_t loaded = 0;
  char *line = buf;
  while (line && *line) {
    char *nl = strpbrk(line, "\r\n");
    if (nl) *nl = 0;
    char *s = line;
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;

    if (*s && *s != '#' && validName(s) && findIndex(s) < 0) {
      char path[160];
      snprintf(path, sizeof(path), "%s/%s", dir, s);
      Err r = loadFile(path, s);
      if (r == Err::Ok) loaded++;
      else if (log_) log_->printf("SKIP %s (%s)\n", s, errStr(r));
    }
    line = nl ? nl + 1 : nullptr;
    while (line && (*line == '\r' || *line == '\n')) line++;
  }
  free(buf);
  return loaded;
}

bool ImageStore::saveList(const char *listPath) const {
  SD_MMC.remove(listPath);
  File f = SD_MMC.open(listPath, FILE_WRITE);
  if (!f) return false;
  for (auto &im : imgs_) {
    f.write((const uint8_t *)im.name, strlen(im.name));
    f.write((const uint8_t *)"\n", 1);
  }
  f.close();
  return true;
}

size_t ImageStore::listDir(const char *dir, FileEntry *out, size_t maxEntries) {
  File d = SD_MMC.open(dir);
  if (!d || !d.isDirectory()) return 0;
  size_t total = 0;
  File e;
  while ((e = d.openNextFile())) {
    char nm[kNameLen];
    strncpy(nm, e.name(), sizeof(nm) - 1);
    nm[sizeof(nm) - 1] = 0;
    bool isDir = e.isDirectory();
    bool tooLong = strlen(e.name()) >= kNameLen;
    size_t sz = e.size();
    e.close();
    if (isDir || tooLong || !isBinName(nm)) continue;

    if (out && total < maxEntries) {
      FileEntry &fe = out[total];
      fe = FileEntry();
      strncpy(fe.name, nm, kNameLen - 1);
      fe.size = (uint32_t)sz;
      char path[160];
      snprintf(path, sizeof(path), "%s/%s", dir, nm);
      File f = SD_MMC.open(path, FILE_READ);
      if (f) {
        uint32_t hdr = 0, bpp;
        if (f.read((uint8_t *)&hdr, 4) == 4) fe.valid = parseHeader(hdr, sz, fe.cf, fe.w, fe.h, bpp);
        f.close();
      }
    }
    total++;
  }
  d.close();
  if (out) {
    size_t n = total < maxEntries ? total : maxEntries;
    std::sort(out, out + n, [](const FileEntry &a, const FileEntry &b) { return strcasecmp(a.name, b.name) < 0; });
  }
  return total;
}

bool ImageStore::isValidFile(const char *path) {
  if (!path) return false;
  File f = SD_MMC.open(path, FILE_READ);
  if (!f) return false;

  uint32_t hdr = 0, bpp;
  uint8_t cf;
  uint16_t w, h;
  const bool valid = f.size() >= 8 &&
                     f.read((uint8_t *)&hdr, 4) == 4 &&
                     parseHeader(hdr, f.size(), cf, w, h, bpp);
  f.close();
  return valid;
}

}  // namespace sdpsram
