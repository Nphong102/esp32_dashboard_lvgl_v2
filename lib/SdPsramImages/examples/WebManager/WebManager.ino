// Web file manager: browse the .bin files on the card, choose which ones are loaded into PSRAM,
// delete files, upload new ones. The choice is saved in /ets2/loaded.txt and restored at boot.
#include <SdPsramImages.h>
#include <WiFi.h>
#include <WebServer.h>
using namespace sdpsram;

const char *WIFI_SSID = "TEN_WIFI";
const char *WIFI_PASS = "MAT_KHAU";
const SdPins PINS = {38, 40, 39, 41, 48, 47};
const char *IMG_DIR  = "/ets2";
const char *LIST = "/ets2/loaded.txt";

ImageStore images(20);
WebServer server(80);
File uploadFile;

static void redirectHome() { server.sendHeader("Location", "/"); server.send(303); }

void handlePage() {
  static FileEntry list[64];
  size_t total = ImageStore::listDir(IMG_DIR, list, 64);
  size_t shown = total < 64 ? total : 64;

  String h = "<meta name=viewport content='width=device-width'><style>td{padding:4px 8px}</style>";
  h += "<h3>Anh tren the (" + String((unsigned)shown) + ")</h3>";
  h += "<p>Da nap: " + String((unsigned)images.count()) + " anh, " +
       String((unsigned)(images.bytesUsed() / 1024)) + " KB PSRAM</p><table>";
  for (size_t i = 0; i < shown; i++) {
    const FileEntry &e = list[i];
    bool loaded = images.isLoaded(e.name);
    h += "<tr><td>"; h += e.name; h += "</td><td>";
    if (e.valid) h += String(e.w) + "x" + String(e.h) + (e.cf == 5 ? " alpha" : "");
    else h += "<i>khong hop le</i>";
    h += "</td><td>" + String((unsigned)(e.size / 1024)) + " KB</td><td>";
    h += loaded ? "<b>DA NAP</b>" : "-";
    h += "</td><td><form method=POST action=/toggle><input type=hidden name=f value='"; h += e.name;
    h += "'><input type=submit value='"; h += loaded ? "Bo nap" : "Nap"; h += "'";
    if (!e.valid) h += " disabled";
    h += "></form></td><td><form method=POST action=/delete><input type=hidden name=f value='"; h += e.name;
    h += "'><input type=submit value=Xoa onclick=\"return confirm('Xoa file nay?')\"></form></td></tr>";
  }
  h += "</table><h3>Upload</h3><form method=POST action=/upload enctype=multipart/form-data>"
       "<input type=file name=f multiple accept=.bin> <input type=submit value=Upload></form>";
  server.send(200, "text/html", h);
}

void handleToggle() {
  String n = server.arg("f");
  if (images.isLoaded(n.c_str())) {
    images.unload(n.c_str());            // NOTE: make sure your UI is not showing this image
  } else {
    char path[160];
    snprintf(path, sizeof(path), "%s/%s", IMG_DIR, n.c_str());
    images.loadFile(path, n.c_str());
  }
  images.saveList(LIST);
  redirectHome();
}

void handleDelete() {
  images.deleteFile(IMG_DIR, server.arg("f").c_str());
  images.saveList(LIST);
  redirectHome();
}

void handleUpload() {
  HTTPUpload &up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    String name = up.filename;
    name.replace("/", "_"); name.replace("\\", "_"); name.replace("..", "_");
    char path[160];
    snprintf(path, sizeof(path), "%s/%s", IMG_DIR, name.c_str());
    SD_MMC.remove(path);
    uploadFile = SD_MMC.open(path, FILE_WRITE);
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (uploadFile) uploadFile.write(up.buf, up.currentSize);
  } else if (up.status == UPLOAD_FILE_END) {
    if (uploadFile) uploadFile.close();
  }
}

void setup() {
  Serial.begin(115200);
  delay(2000);

  if (!SdCard::begin(PINS)) { Serial.println("SD mount failed"); return; }
  SdCard::mkdir(IMG_DIR);

  images.setLog(&Serial);
  // First run (no list yet): load everything. Later: load only what was chosen.
  if (SD_MMC.exists(LIST)) images.loadFromList(IMG_DIR, LIST);
  else { images.loadDir(IMG_DIR); images.saveList(LIST); }

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) delay(200);
  Serial.println(WiFi.localIP());

  server.on("/", HTTP_GET, handlePage);
  server.on("/toggle", HTTP_POST, handleToggle);
  server.on("/delete", HTTP_POST, handleDelete);
  server.on("/upload", HTTP_POST, redirectHome, handleUpload);
  server.begin();
}

void loop() { server.handleClient(); }
