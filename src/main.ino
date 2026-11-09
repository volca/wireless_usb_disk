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
#include <LittleFS.h>
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

// Config SSID and password
const char* SSID        = "wireless-usb-disk";  // Enter your SSID here
const char* PASSWORD    = "12345678";           // Enter your Password here

USBMSC MSC;

// web server
WebServer server(80);
//holds the current upload
File fsUploadFile;

static SemaphoreHandle_t sdMutex = NULL;
static bool sdReady = false;
static uint32_t sdSectorSize = 0;

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
    server.streamFile(file, contentType);
    file.close();
}

// Stream a file from the SD card in small chunks so the USB MSC task can
// interleave its transfers while a large download is in progress.
void streamSDFile(File& file, String contentType) {
    server.setContentLength(file.size());
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

void handleFileUpload() {
    if (server.uri() != "/upload") {
        return;
    }

    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        String filename = upload.filename;
        if (!filename.startsWith("/")) {
            filename = "/" + filename;
        }
        Serial.print("handleFileUpload Name: ");
        Serial.println(filename);
        sdLock();
        fsUploadFile = SD.open(filename, "w");
        sdUnlock();
        filename = String();
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (fsUploadFile) {
            sdLock();
            fsUploadFile.write(upload.buf, upload.currentSize);
            sdUnlock();
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (fsUploadFile) {
            Serial.println("Uploaded");
            sdLock();
            fsUploadFile.close();
            sdUnlock();
        }
        Serial.print("handleFileUpload Size: ");
        Serial.println(upload.totalSize);
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

void startFileServer() {
    server.on("/list", HTTP_GET, handleFileList);
    server.on("/delete", HTTP_POST, handleFileDelete);
    server.on("/upload", HTTP_POST, []() {
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

  delay(1000);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(SSID, PASSWORD);

  delay(1000);
  startFileServer();
}

void loop() {
  server.handleClient();
  delay(2);
}
