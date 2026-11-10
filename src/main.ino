/**
 * Wireless USB disk: the SD card is exposed as a USB Mass Storage device
 * (arduino-esp32 USBMSC) and over WiFi as an HTTP file server.
 *
 * USB stack: arduino-esp32 TinyUSB (core), MSC only. The SD card is accessed
 * through the core SD library; raw sectors go through SD.readRAW/writeRAW.
 */
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiAP.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <WiFiManager.h>
#include <LittleFS.h>
#include <Update.h>
#include <ArduinoOTA.h>
#include <SPI.h>
#include <SD.h>
#include <USB.h>
#include <USBMSC.h>

#define SD_MISO  37
#define SD_MOSI  35
#define SD_SCK   36
#define SD_CS    34

#define SECTOR_SIZE 512

#define WUDFS     LittleFS

// Local access point used by the file manager and by the WiFiManager
// configuration portal.
const char* SSID        = "wireless-usb-disk";
const char* PASSWORD    = "12345678";

// ArduinoOTA server, used by the espota PlatformIO environment.
#define OTA_PORT      3232
#define OTA_PASSWORD  "wud"

USBMSC MSC;
WiFiManager wm;

// web server
WebServer server(80);
//holds the current upload
File fsUploadFile;

static SemaphoreHandle_t sdMutex = NULL;
static bool sdReady = false;
static uint32_t sdSectorSize = 0;
static bool resetPending = false;
static uint32_t resetAt = 0;

static inline void sdLock() {
    if (sdMutex) {
        xSemaphoreTakeRecursive(sdMutex, portMAX_DELAY);
    }
}

static inline void sdUnlock() {
    if (sdMutex) {
        xSemaphoreGiveRecursive(sdMutex);
    }
}

//--------------------------------------------------------------------+
// USB MSC callbacks
//--------------------------------------------------------------------+

static int32_t mscRead(uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
    if (!sdReady || sdSectorSize != SECTOR_SIZE) {
        return -1;
    }

    uint8_t* dst = (uint8_t*)buffer;
    uint32_t sector = lba;
    uint32_t remaining = bufsize;
    uint8_t sec[SECTOR_SIZE] __attribute__((aligned(4)));

    sdLock();
    if (offset) {
        if (offset >= SECTOR_SIZE || !SD.readRAW(sec, sector)) {
            sdUnlock();
            return -1;
        }
        uint32_t n = SECTOR_SIZE - offset;
        if (n > remaining) n = remaining;
        memcpy(dst, sec + offset, n);
        dst += n;
        remaining -= n;
        sector++;
    }
    while (remaining >= SECTOR_SIZE) {
        if (!SD.readRAW(dst, sector)) {
            sdUnlock();
            return -1;
        }
        dst += SECTOR_SIZE;
        remaining -= SECTOR_SIZE;
        sector++;
    }
    if (remaining) {
        if (!SD.readRAW(sec, sector)) {
            sdUnlock();
            return -1;
        }
        memcpy(dst, sec, remaining);
    }
    sdUnlock();

    return (int32_t)bufsize;
}

static int32_t mscWrite(uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize) {
    if (!sdReady || sdSectorSize != SECTOR_SIZE) {
        return -1;
    }

    const uint8_t* src = buffer;
    uint32_t sector = lba;
    uint32_t remaining = bufsize;
    uint8_t sec[SECTOR_SIZE] __attribute__((aligned(4)));

    sdLock();
    if (offset) {
        if (offset >= SECTOR_SIZE || !SD.readRAW(sec, sector)) {
            sdUnlock();
            return -1;
        }
        uint32_t n = SECTOR_SIZE - offset;
        if (n > remaining) n = remaining;
        memcpy(sec + offset, src, n);
        if (!SD.writeRAW(sec, sector)) {
            sdUnlock();
            return -1;
        }
        src += n;
        remaining -= n;
        sector++;
    }
    while (remaining >= SECTOR_SIZE) {
        if (!SD.writeRAW(const_cast<uint8_t*>(src), sector)) {
            sdUnlock();
            return -1;
        }
        src += SECTOR_SIZE;
        remaining -= SECTOR_SIZE;
        sector++;
    }
    if (remaining) {
        if (!SD.readRAW(sec, sector)) {
            sdUnlock();
            return -1;
        }
        memcpy(sec, src, remaining);
        if (!SD.writeRAW(sec, sector)) {
            sdUnlock();
            return -1;
        }
    }
    sdUnlock();

    return (int32_t)bufsize;
}

static bool mscStartStop(uint8_t power_condition, bool start, bool load_eject) {
    (void)power_condition;
    (void)start;
    (void)load_eject;
    return true;
}

//--------------------------------------------------------------------+
// Web server
//--------------------------------------------------------------------+

String getContentType(String filename) {
    if (server.hasArg("download")) {
        return "application/octet-stream";
    } else if (filename.endsWith(".htm")) {
        return "text/html";
    } else if (filename.endsWith(".html")) {
        return "text/html";
    } else if (filename.endsWith(".css")) {
        return "text/css";
    } else if (filename.endsWith(".js")) {
        return "application/javascript";
    } else if (filename.endsWith(".png")) {
        return "image/png";
    } else if (filename.endsWith(".gif")) {
        return "image/gif";
    } else if (filename.endsWith(".jpg")) {
        return "image/jpeg";
    } else if (filename.endsWith(".ico")) {
        return "image/x-icon";
    } else if (filename.endsWith(".xml")) {
        return "text/xml";
    } else if (filename.endsWith(".pdf")) {
        return "application/x-pdf";
    } else if (filename.endsWith(".zip")) {
        return "application/x-zip";
    } else if (filename.endsWith(".gz")) {
        return "application/x-gzip";
    }
    return "text/plain";
}

void handleIndex() {
    String path = "/index.htm";
    String contentType = "text/html";
    File file = WUDFS.open(path.c_str());
    server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
    server.streamFile(file, contentType);
    file.close();
}

// Stream a file from the SD card in small chunks so the USB MSC task can
// interleave its transfers while a large download is in progress.
void streamSDFile(File& file, String contentType) {
    server.setContentLength(file.size());
    server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
    server.send(200, contentType, "");

    uint8_t buf[512];
    while (true) {
        sdLock();
        int n = file.read(buf, sizeof(buf));
        sdUnlock();
        if (n <= 0) {
            break;
        }
        server.sendContent((const char*)buf, n);
    }
}

static String uploadPath;
static uint32_t uploadStartMs = 0;
static uint32_t uploadBytes = 0;

void handleFileUpload() {
    if (server.uri() != "/upload") {
        return;
    }

    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        uploadPath = upload.filename;
        if (!uploadPath.startsWith("/")) {
            uploadPath = "/" + uploadPath;
        }
        uploadStartMs = millis();
        uploadBytes = 0;
        Serial.print("Upload: ");
        Serial.println(uploadPath);
        sdLock();
        fsUploadFile = SD.open(uploadPath, "w");
        sdUnlock();
        if (!fsUploadFile) {
            Serial.println("Upload: cannot open file");
        }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (fsUploadFile) {
            sdLock();
            const size_t written = fsUploadFile.write(upload.buf, upload.currentSize);
            sdUnlock();
            uploadBytes += written;
            if (written != upload.currentSize) {
                Serial.printf("Upload: short write %u/%u\n", (unsigned)written,
                              (unsigned)upload.currentSize);
            }
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (fsUploadFile) {
            sdLock();
            fsUploadFile.close();
            sdUnlock();
        }
        const uint32_t elapsed = millis() - uploadStartMs;
        const uint32_t kbs = elapsed ? (uint32_t)((uint64_t)uploadBytes * 1000 / elapsed / 1024) : 0;
        Serial.printf("Upload done: %u bytes in %u ms (%u KB/s)\n",
                      (unsigned)uploadBytes, (unsigned)elapsed, (unsigned)kbs);
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
        if (fsUploadFile) {
            sdLock();
            fsUploadFile.close();
            sdUnlock();
        }
        if (uploadPath.length()) {
            sdLock();
            SD.remove(uploadPath);
            sdUnlock();
        }
        Serial.println("Upload aborted, partial file removed");
    }
}

bool handleFileRead(String path) {
    Serial.println("handleFileRead: " + path);
    if (path.endsWith("/")) {
        handleIndex();
        return true;
    } else if (path.endsWith("/favicon.ico")) {
        String contentType = "text/x-icon";
        File file = WUDFS.open(path.c_str());
        server.streamFile(file, contentType);
        file.close();
        return true;
    } else {
        String contentType = getContentType(path);
        String pathWithGz = path + ".gz";

        sdLock();
        bool gz = SD.exists(pathWithGz);
        bool plain = SD.exists(path);
        sdUnlock();

        if (gz || plain) {
            if (gz) {
                path += ".gz";
            }

            sdLock();
            File file = SD.open(path, "r");
            bool isDir = file && file.isDirectory();
            sdUnlock();

            if (isDir) {
                file.close();
                handleIndex();
            } else {
                streamSDFile(file, contentType);
                file.close();
            }
            return true;
        }
    }
    return false;
}

void handleFileDelete() {
    if (server.args() == 0) {
        return server.send(500, "text/plain", "BAD ARGS");
    }
    String path = server.arg(0);
    Serial.println("handleFileDelete: " + path);
    if (path == "/") {
        return server.send(500, "text/plain", "BAD PATH");
    }

    if (!path.startsWith("/")) {
        path = String("/") + path;
    }

    sdLock();
    bool exists = SD.exists(path);
    sdUnlock();

    if (!exists) {
        return server.send(404, "text/plain", "FileNotFound");
    }

    Serial.println("Delete: " + path);
    sdLock();
    SD.remove(path);
    sdUnlock();

    server.sendHeader("Location", String("/"), true);
    server.send(302, "text/plain", "");
    path = String();
}

void handleFileList() {
    String path;
    if (!server.hasArg("dir")) {
        path = "/";
    } else {
        path = server.arg("dir");
    }

    Serial.println("handleFileList: " + path);

    String output = "[";

    sdLock();
    File root = SD.open(path);
    if (root && root.isDirectory()) {
        File file = root.openNextFile();
        while (file) {
            if (output != "[") {
                output += ',';
            }

            output += "{\"type\":\"";
            output += (file.isDirectory()) ? "dir" : "file";
            output += "\",\"name\":\"";
            output += String(file.name());
            output += "\",\"size\":";
            output += String(file.size());
            output += "}";
            file = root.openNextFile();
        }
    }
    sdUnlock();

    output += "]";
    server.send(200, "text/json", output);
}

// Start the WiFiManager configuration portal in place (no reboot needed).
// The file server must give up port 80 first, so the switch happens shortly
// after the HTTP response has been sent.
static bool wifiSetupPending = false;
static uint32_t wifiSetupAt = 0;

void handleWifiReset() {
    server.send(200, "text/html",
                "<html><body><h2>Starting WiFi setup...</h2>"
                "<p>The configuration portal will open in a few seconds.</p>"
                "<script>setTimeout(function(){location.href='/';},6000);</script>"
                "</body></html>");
    wifiSetupPending = true;
    wifiSetupAt = millis() + 1500;
}

//--------------------------------------------------------------------+
// OTA update: /ota_fw writes the app partition, /ota_fs the LittleFS one
//--------------------------------------------------------------------+

static bool otaFsTarget = false;
static bool otaOk = false;

void handleOTA() {
    HTTPUpload& upload = server.upload();

    if (upload.status == UPLOAD_FILE_START) {
        otaFsTarget = (server.uri() == "/ota_fs");
        otaOk = false;
        Serial.printf("OTA start (%s): %s\n",
                      otaFsTarget ? "filesystem" : "firmware",
                      upload.filename.c_str());

        if (otaFsTarget) {
            // The new image replaces the mounted partition.
            WUDFS.end();
        }

        if (!Update.begin(UPDATE_SIZE_UNKNOWN, otaFsTarget ? U_SPIFFS : U_FLASH)) {
            Update.printError(Serial);
        }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.isRunning() &&
            Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            Update.printError(Serial);
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (Update.end(true)) {
            otaOk = true;
            Serial.printf("OTA written: %u bytes\n", (unsigned)upload.totalSize);
        } else {
            Update.printError(Serial);
        }
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
        Update.abort();
        Serial.println("OTA aborted");
    }
}

void sendOtaResult() {
    if (otaOk) {
        server.send(200, "text/plain", "Update OK, rebooting");
        resetPending = true;
        resetAt = millis() + 800;
        return;
    }

    const String error = Update.errorString();

    // Bring the file manager back if the filesystem image was not applied.
    if (otaFsTarget) {
        otaFsTarget = false;
        if (!WUDFS.begin(false)) {
            Serial.println("LittleFS remount failed");
        }
    }
    Update.abort();

    server.send(500, "text/plain", error);
}

void handleReset() {
    server.send(200, "text/plain", "Rebooting...");
    // Give the response a moment to leave the TCP stack before restarting.
    resetPending = true;
    resetAt = millis() + 800;
}

void startFileServer() {
    server.on("/list", HTTP_GET, handleFileList);
    server.on("/reset", HTTP_POST, handleReset);
    server.on("/resetwifi", HTTP_POST, handleWifiReset);
    server.on("/ota_fw", HTTP_POST, sendOtaResult, handleOTA);
    server.on("/ota_fs", HTTP_POST, sendOtaResult, handleOTA);
    server.on("/delete", HTTP_POST, handleFileDelete);
    server.on("/upload", HTTP_POST, []() {
        // XHR uploads get a plain ack so multi-file uploads do not re-fetch
        // the page after every request; a plain form post goes back home.
        if (server.hasHeader("X-Requested-With")) {
            server.send(200, "text/plain", "OK");
            return;
        }
        server.sendHeader("Location", String("/"), true);
        server.send(302, "text/plain", "");
    }, handleFileUpload);
    server.onNotFound([]() {
        if (!handleFileRead(server.uri())) {
            server.send(404, "text/plain", "FileNotFound");
        }
    });
    server.begin();
}

//--------------------------------------------------------------------+
// ArduinoOTA: upload target for `pio run -e espota -t upload`
//--------------------------------------------------------------------+

void startArduinoOTA() {
    ArduinoOTA.setHostname("wud");
    ArduinoOTA.setPort(OTA_PORT);
    ArduinoOTA.setPassword(OTA_PASSWORD);
    // mDNS is already running for wud.local; a second mdns_init() would fail,
    // so ArduinoOTA must not register its own instance.
    ArduinoOTA.setMdnsEnabled(false);

    ArduinoOTA.onStart([]() {
        Serial.printf("ArduinoOTA: start (%s)\n",
                      ArduinoOTA.getCommand() == U_FLASH ? "firmware" : "filesystem");
    });
    ArduinoOTA.onEnd([]() {
        Serial.println("\nArduinoOTA: done");
    });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        Serial.printf("ArduinoOTA: %u%%\r", progress * 100 / total);
    });
    ArduinoOTA.onError([](ota_error_t error) {
        Serial.printf("ArduinoOTA error[%u]\n", error);
    });

    ArduinoOTA.begin();
    Serial.printf("ArduinoOTA ready on port %u\n", OTA_PORT);
}

void setup()
{
  Serial.begin(115200);

  sdMutex = xSemaphoreCreateRecursiveMutex();

  const bool formatOnFail = true;
  WUDFS.begin(formatOnFail);

  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  sdReady = SD.begin(SD_CS, SPI, 50000000, "/sd", 8);
  sdSectorSize = sdReady ? SD.sectorSize() : 0;

  Serial.print("SD card: ");
  if (sdReady) {
    Serial.printf("%llu MB\n", (unsigned long long)(SD.cardSize() / (1024ULL * 1024ULL)));
  } else {
    Serial.println("init failed");
  }

  MSC.vendorID("ESP32-S2");
  MSC.productID("Wireless Disk");
  MSC.productRevision("1.0");
  MSC.onStartStop(mscStartStop);
  MSC.onRead(mscRead);
  MSC.onWrite(mscWrite);
  MSC.mediaPresent(sdReady);
  if (sdReady) {
    MSC.begin(SD.numSectors(), sdSectorSize);
  }

  USB.begin();

  // WiFi: use credentials configured through the captive portal. On the
  // first boot, or when the saved network is unreachable, the device starts
  // its own access point so the credentials can be entered.
  wm.setConnectTimeout(20);
  wm.setWiFiAutoReconnect(true);
  wm.setAPCallback([](WiFiManager *) {
    Serial.print("WiFi config portal: http://");
    Serial.println(WiFi.softAPIP());
  });

  wm.setConfigPortalTimeout(180);
  if (wm.autoConnect(SSID, PASSWORD)) {
    Serial.print("WiFi connected: ");
    Serial.print(WiFi.SSID());
    Serial.print(" -> ");
    Serial.println(WiFi.localIP());

    // Station mode only: publish wud.local on the LAN.
    if (MDNS.begin("wud")) {
      MDNS.addService("http", "tcp", 80);
      Serial.println("mDNS: http://wud.local/");
    } else {
      Serial.println("mDNS start failed");
    }

    // espota is only offered on the station interface: in access-point-only
    // mode there is no wud.local to upload to.
    startArduinoOTA();
  } else {
    Serial.println("WiFi not configured, access point only");
  }

  // Keep the local AP running so the file manager is always reachable at
  // 192.168.4.1, even when a station connection exists.
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);  // modem sleep throttles throughput badly
  if (!WiFi.softAP(SSID, PASSWORD)) {
    Serial.println("Failed to start access point");
  }
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());

  delay(1000);
  startFileServer();
}

void loop() {
  if (wifiSetupPending && (int32_t)(millis() - wifiSetupAt) >= 0) {
    wifiSetupPending = false;
    Serial.println("Starting WiFi setup portal");

    // Release port 80 so WiFiManager's portal can serve the form.
    server.stop();
    delay(200);

    wm.resetSettings();
    wm.setConfigPortalTimeout(300);  // give up after 5 minutes of inactivity
    const bool saved = wm.startConfigPortal(SSID, PASSWORD);
    Serial.printf("WiFi setup portal closed, saved=%d\n", saved);

    // Reboot to apply the new network (or to restore the file server).
    delay(500);
    ESP.restart();
  }

  ArduinoOTA.handle();
  server.handleClient();

  if (resetPending && (int32_t)(millis() - resetAt) >= 0) {
    Serial.println("Reboot requested from web UI");
    delay(20);
    ESP.restart();
  }

  delay(1);
}
