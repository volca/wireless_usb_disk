/**
 * Wireless USB disk: the SD card is exposed as a USB Mass Storage device
 * (arduino-esp32 USBMSC) and over WiFi as an HTTP file server.
 *
 * USB stack: arduino-esp32 TinyUSB (core), MSC only. The SD card is accessed
 * through the core SD library; raw sectors go through SD.readRAW/writeRAW.
 */
#include <WiFi.h>
#include <lwip/sockets.h>
#include <freertos/queue.h>
#include <WiFiClient.h>
#include <WiFiAP.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <WiFiManager.h>
#include <Preferences.h>
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

// SD SPI clock. Cheap cards and long jumper wires often fail sustained large
// writes at 50 MHz (retries look like a collapse in throughput); 25 MHz is
// still well above the card's own write speed.
#ifndef SD_SPI_HZ
#define SD_SPI_HZ 25000000UL
#endif

#define SECTOR_SIZE 512

#define WUDFS     LittleFS

// Local access point used by the file manager and by the WiFiManager
// configuration portal.
const char* SSID        = "wireless-usb-disk";
const char* PASSWORD    = "12345678";

// ArduinoOTA server, used by the espota PlatformIO environment.
#define OTA_PORT      3232
#define OTA_PASSWORD  "wud"

// WiFi mode selected in the web UI and stored in NVS.
#define WUD_MODE_AP   0
#define WUD_MODE_STA  1

USBMSC MSC;
WiFiManager wm;

// web server
WebServer server(80);
//holds the current upload

static SemaphoreHandle_t sdMutex = NULL;
static bool sdReady = false;
static uint32_t sdSectorSize = 0;
static bool resetPending = false;
static uint32_t resetAt = 0;
static uint8_t wifiMode = WUD_MODE_AP;

static uint8_t loadWifiMode() {
    Preferences prefs;
    prefs.begin("wud", false);
    const uint8_t mode = prefs.getUChar("wmode", WUD_MODE_AP);
    prefs.end();
    return (mode == WUD_MODE_STA) ? WUD_MODE_STA : WUD_MODE_AP;
}

static void saveWifiMode(uint8_t mode) {
    Preferences prefs;
    prefs.begin("wud", false);
    prefs.putUChar("wmode", mode);
    prefs.end();
}

// lwIP advertises a 5760 byte TCP window by default, which caps throughput at
// roughly window/RTT (about 100 KB/s here). This build has
// CONFIG_LWIP_SO_RCVBUF, so the window can be enlarged per socket.
static void tuneRecvWindow(WiFiClient client, int rcvBuf) {
    const int fd = client.fd();
    if (fd < 0) {
        return;
    }
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvBuf, sizeof(rcvBuf));
}

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

void handleWifiInfo() {
    String json = "{\"mode\":\"";
    json += (wifiMode == WUD_MODE_STA) ? "sta" : "ap";
    json += "\",\"rssi\":";
    json += WiFi.RSSI();
    json += ",\"channel\":";
    json += WiFi.channel();
    json += ",\"ip\":\"";
    json += WiFi.localIP().toString();
    json += "\",\"heap\":";
    json += ESP.getFreeHeap();
    json += ",\"cpu_mhz\":";
    json += getCpuFrequencyMhz();
    json += ",\"sleep\":";
    json += (WiFi.getSleep() == WIFI_PS_NONE) ? 0 : 1;
    json += "}";
    server.send(200, "application/json", json);
}

void handleNetTest() {
    uint32_t kb = server.hasArg("kb") ? (uint32_t)server.arg("kb").toInt() : 256;
    if (kb < 16) {
        kb = 16;
    }
    if (kb > 4096) {
        kb = 4096;
    }

    uint8_t *block = (uint8_t *)malloc(4096);
    if (!block) {
        server.send(500, "text/plain", "oom");
        return;
    }
    memset(block, 0x5a, 4096);

    const uint32_t total = kb * 1024;
    server.setContentLength(total);
    server.send(200, "application/octet-stream", "");
    for (uint32_t sent = 0; sent < total;) {
        size_t n = total - sent;
        if (n > 4096) {
            n = 4096;
        }
        server.sendContent((const char *)block, n);
        sent += n;
    }
    free(block);
}

void handleSdTest() {
    if (!sdReady) {
        server.send(503, "text/plain", "SD not ready");
        return;
    }

    // ?kb=128 writes 128 KB of zeros and reports the measured write speed.
    uint32_t kb = server.hasArg("kb") ? (uint32_t)server.arg("kb").toInt() : 128;
    if (kb < 16) {
        kb = 16;
    }
    if (kb > 10240) {
        kb = 10240;
    }

    // ?bs= sets the write block size, to mirror what the upload path does.
    uint32_t bs = server.hasArg("bs") ? (uint32_t)server.arg("bs").toInt() : 4096;
    if (bs < 512) {
        bs = 512;
    }
    if (bs > 8192) {
        bs = 8192;
    }

    uint8_t *block = (uint8_t *)malloc(bs);
    if (!block) {
        server.send(500, "text/plain", "oom");
        return;
    }
    memset(block, 0x5a, bs);

    const uint32_t t0 = millis();
    size_t written = 0;

    sdLock();
    File f = SD.open("/__sdtest.bin", FILE_WRITE);
    if (f) {
        const uint32_t total = kb * 1024;
        while (written < total) {
            size_t n = total - written;
            if (n > bs) {
                n = bs;
            }
            const size_t w = f.write(block, n);
            if (w == 0) {
                break;
            }
            written += w;
        }
        f.close();
        SD.remove("/__sdtest.bin");
    }
    sdUnlock();

    const uint32_t ms = millis() - t0;
    char json[160];
    snprintf(json, sizeof(json),
             "{\"bytes\":%u,\"ms\":%u,\"kb_s\":%u,\"heap\":%u}",
             (unsigned)written, (unsigned)ms,
             (unsigned)(ms ? (uint64_t)written * 1000 / ms / 1024 : 0),
             (unsigned)ESP.getFreeHeap());
    free(block);
    server.send(200, "application/json", json);
}

void handleWifiMode() {
    if (server.method() == HTTP_GET) {
        server.send(200, "text/plain", wifiMode == WUD_MODE_STA ? "sta" : "ap");
        return;
    }

    const String mode = server.arg("mode");
    if (mode != "ap" && mode != "sta") {
        server.send(400, "text/plain", "mode must be ap or sta");
        return;
    }

    wifiMode = (mode == "sta") ? WUD_MODE_STA : WUD_MODE_AP;
    saveWifiMode(wifiMode);
    Serial.printf("WiFi mode set to %s, rebooting\n",
                  wifiMode == WUD_MODE_STA ? "station" : "access point");

    server.send(200, "text/plain", "Mode saved, rebooting...");
    resetPending = true;
    resetAt = millis() + 800;
}

void handleReset() {
    server.send(200, "text/plain", "Rebooting...");
    // Give the response a moment to leave the TCP stack before restarting.
    resetPending = true;
    resetAt = millis() + 800;
}

//--------------------------------------------------------------------+
// Fast upload path: raw POST body on port 81.
//
// The core WebServer parses multipart bodies one byte at a time, which caps
// throughput at ~90 KB/s. This handler reads the body in 4 KB chunks and
// writes to SD in 8 KB blocks. The page uses it for files; /upload stays as
// a fallback for multipart clients (curl, older pages).
//--------------------------------------------------------------------+

static WiFiServer uploadServer(81);

static void httpRespond(WiFiClient &client, int code, const char *text) {
    client.printf("HTTP/1.1 %d %s\r\n", code, code == 200 ? "OK" : "Error");
    client.print("Access-Control-Allow-Origin: *\r\n");
    client.print("Access-Control-Allow-Methods: POST, OPTIONS\r\n");
    client.print("Access-Control-Allow-Headers: Content-Type\r\n");
    client.print("Content-Type: text/plain\r\n");
    client.printf("Content-Length: %u\r\n", (unsigned)strlen(text));
    client.print("Connection: close\r\n\r\n");
    client.print(text);
}

static bool readHttpLine(WiFiClient &client, String &line, uint32_t timeoutMs) {
    line = "";
    const uint32_t start = millis();
    while (millis() - start < timeoutMs) {
        while (client.available()) {
            const int ch = client.read();
            if (ch < 0) {
                return false;
            }
            if (ch == '\n') {
                if (line.endsWith("\r")) {
                    line.remove(line.length() - 1);
                }
                return true;
            }
            line += (char)ch;
            if (line.length() > 1024) {
                return false;
            }
        }
        if (!client.connected()) {
            return false;
        }
        delay(1);
    }
    return false;
}

static String urlDecodePath(const String &in) {
    String out;
    char hex[3] = {0, 0, 0};
    for (size_t i = 0; i < in.length(); i++) {
        const char ch = in[i];
        if (ch == '%' && i + 2 < in.length()) {
            hex[0] = in[i + 1];
            hex[1] = in[i + 2];
            out += (char)strtol(hex, nullptr, 16);
            i += 2;
        } else if (ch == '+') {
            out += ' ';
        } else {
            out += ch;
        }
    }
    return out;
}

//--------------------------------------------------------------------+
// Upload writer task.
//
// The request handler only reads the socket and hands 4 KB blocks over; the
// SD write happens in this task, so the TCP receive window is not closed
// while the card is busy (that stall was cutting throughput to ~30 KB/s).
//--------------------------------------------------------------------+

#define UPLOAD_BLOCK_SIZE  4096
#define UPLOAD_BLOCK_COUNT 4

typedef struct {
    uint8_t *data;
    size_t len;
} upload_block_t;

static uint8_t uploadBlockMem[UPLOAD_BLOCK_COUNT][UPLOAD_BLOCK_SIZE];
static QueueHandle_t uploadFreeQ = nullptr;
static QueueHandle_t uploadFullQ = nullptr;
static File uploadSink;
static volatile bool uploadWriteError = false;
static uint32_t uploadStatWaitUs = 0;
static uint32_t uploadStatReadUs = 0;
static uint32_t uploadStatSdUs = 0;
static uint32_t uploadStatEmptyReads = 0;

static void uploadWriterTask(void *arg) {
    (void)arg;
    for (;;) {
        upload_block_t item;
        if (xQueueReceive(uploadFullQ, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (!uploadWriteError && item.len) {
            size_t done = 0;
            const uint32_t st = micros();
            sdLock();
            while (done < item.len) {
                const size_t w = uploadSink.write(item.data + done, item.len - done);
                if (w == 0) {
                    uploadWriteError = true;
                    break;
                }
                done += w;
            }
            sdUnlock();
            uploadStatSdUs += micros() - st;
        }
        xQueueSend(uploadFreeQ, &item.data, portMAX_DELAY);
    }
}

static void uploadPoolInit() {
    uploadFreeQ = xQueueCreate(UPLOAD_BLOCK_COUNT, sizeof(uint8_t *));
    uploadFullQ = xQueueCreate(UPLOAD_BLOCK_COUNT, sizeof(upload_block_t));
    for (int i = 0; i < UPLOAD_BLOCK_COUNT; i++) {
        uint8_t *block = uploadBlockMem[i];
        xQueueSend(uploadFreeQ, &block, 0);
    }
    xTaskCreate(uploadWriterTask, "sdwrite", 4096, nullptr, 1, nullptr);
}

// Wait until every block is back in the free queue, i.e. all queued writes
// reached the card. Only then is it safe to close the file.
static bool uploadPoolDrain(uint32_t timeoutMs) {
    uint8_t *blocks[UPLOAD_BLOCK_COUNT];
    for (int i = 0; i < UPLOAD_BLOCK_COUNT; i++) {
        if (xQueueReceive(uploadFreeQ, &blocks[i], pdMS_TO_TICKS(timeoutMs)) != pdTRUE) {
            return false;
        }
    }
    for (int i = 0; i < UPLOAD_BLOCK_COUNT; i++) {
        xQueueSend(uploadFreeQ, &blocks[i], 0);
    }
    return true;
}

static void handleRawUpload(WiFiClient &client) {
    String line;
    if (!readHttpLine(client, line, 5000)) {
        client.stop();
        return;
    }

    const int sp1 = line.indexOf(' ');
    const int sp2 = line.indexOf(' ', sp1 + 1);
    if (sp1 < 0) {
        httpRespond(client, 400, "bad request");
        client.stop();
        return;
    }

    const String method = line.substring(0, sp1);
    const String path = (sp2 > 0) ? line.substring(sp1 + 1, sp2) : line.substring(sp1 + 1);

    size_t contentLength = 0;
    while (readHttpLine(client, line, 5000)) {
        if (line.length() == 0) {
            break;
        }
        if (line.startsWith("Content-Length:")) {
            contentLength = (size_t)line.substring(15).toInt();
        }
    }

    if (method == "OPTIONS") {
        httpRespond(client, 204, "");
        client.stop();
        return;
    }
    if (method != "POST") {
        httpRespond(client, 405, "use POST");
        client.stop();
        return;
    }
    if (contentLength == 0) {
        httpRespond(client, 400, "no body");
        client.stop();
        return;
    }

    bool discard = false;
    bool appendMode = false;
    String name = path;
    const int q = name.indexOf('?');
    if (q >= 0) {
        const String query = name.substring(q + 1);
        name = name.substring(0, q);
        if (query.indexOf("discard=1") >= 0) {
            discard = true;
        }
        if (query.indexOf("append=1") >= 0) {
            appendMode = true;
        }
    }
    if (name.startsWith("/u/")) {
        name = name.substring(3);
    } else if (name.startsWith("/")) {
        name = name.substring(1);
    }
    name = urlDecodePath(name);
    if (name.length() == 0 || name.length() > 120 || name.indexOf('/') >= 0) {
        httpRespond(client, 400, "bad name");
        client.stop();
        return;
    }

    const String full = "/" + name;
    Serial.printf("Raw upload: %s (%u bytes)\n", full.c_str(), (unsigned)contentLength);
    tuneRecvWindow(client, 16384);

    if (!uploadFreeQ || !uploadFullQ) {
        httpRespond(client, 500, "writer not ready");
        client.stop();
        return;
    }

    uploadWriteError = false;
    uploadStatWaitUs = 0;
    uploadStatReadUs = 0;
    uploadStatSdUs = 0;
    uploadStatEmptyReads = 0;
    const uint32_t startedMs = millis();
    uint32_t tOpenMs = 0;
    uint32_t tDrainMs = 0;
    uint32_t tCloseMs = 0;
    if (!discard) {
        uint32_t t = millis();
        sdLock();
        uploadSink = SD.open(full, appendMode ? FILE_APPEND : FILE_WRITE);
        sdUnlock();
        tOpenMs = millis() - t;
        if (!uploadSink) {
            httpRespond(client, 500, "open failed");
            client.stop();
            return;
        }
    }

    static uint8_t sinkBuf[1024];
    size_t remaining = contentLength;
    uint32_t lastData = millis();
    bool ok = true;
    const char *failReason = "closed";

    while (remaining > 0) {
        if (discard) {
            const size_t want = (remaining < sizeof(sinkBuf)) ? remaining : sizeof(sinkBuf);
            const int n = client.read(sinkBuf, want);
            if (n <= 0) {
                if (!client.connected() || millis() - lastData > 30000) {
                    ok = false;
                    failReason = "timeout";
                    break;
                }
                uploadStatEmptyReads++;
                delay(1);
                continue;
            }
            lastData = millis();
            remaining -= n;
            continue;
        }

        uint8_t *block = nullptr;
        uint32_t t = micros();
        if (xQueueReceive(uploadFreeQ, &block, pdMS_TO_TICKS(15000)) != pdTRUE) {
            ok = false;
            failReason = "queue";
            break;
        }
        uploadStatWaitUs += micros() - t;

        const size_t want = (remaining < UPLOAD_BLOCK_SIZE) ? remaining : UPLOAD_BLOCK_SIZE;
        t = micros();
        const int n = client.read(block, want);
        uploadStatReadUs += micros() - t;
        if (n <= 0) {
            xQueueSend(uploadFreeQ, &block, 0);
            if (!client.connected() || millis() - lastData > 30000) {
                ok = false;
                failReason = "timeout";
                break;
            }
            uploadStatEmptyReads++;
            delay(1);
            continue;
        }

        lastData = millis();
        remaining -= n;

        upload_block_t item;
        item.data = block;
        item.len = (size_t)n;
        xQueueSend(uploadFullQ, &item, portMAX_DELAY);
    }

    if (!discard) {
        uint32_t t = millis();
        if (!uploadPoolDrain(20000)) {
            ok = false;
            failReason = "drain";
        }
        tDrainMs = millis() - t;
        if (uploadWriteError) {
            ok = false;
            failReason = "write";
        }
        t = millis();
        sdLock();
        uploadSink.close();
        sdUnlock();
        tCloseMs = millis() - t;
    }

    if (!ok) {
        if (!discard) {
            sdLock();
            SD.remove(full);
            sdUnlock();
        }
        Serial.printf("Raw upload failed: %s (%s)\n", full.c_str(), failReason);
        char errBody[128];
        snprintf(errBody, sizeof(errBody),
                 "{\"error\":\"%s\",\"bytes\":%u,\"ms\":%u}",
                 failReason, (unsigned)(contentLength - remaining),
                 (unsigned)(millis() - startedMs));
        httpRespond(client, 500, errBody);
    } else {
        char body[160];
        snprintf(body, sizeof(body),
                 "{\"ms\":%u,\"read_ms\":%u,\"wait_ms\":%u,\"sd_ms\":%u,"
                 "\"open_ms\":%u,\"drain_ms\":%u,\"close_ms\":%u,\"empty\":%u}",
                 (unsigned)(millis() - startedMs), (unsigned)(uploadStatReadUs / 1000),
                 (unsigned)(uploadStatWaitUs / 1000), (unsigned)(uploadStatSdUs / 1000),
                 (unsigned)tOpenMs, (unsigned)tDrainMs, (unsigned)tCloseMs,
                 (unsigned)uploadStatEmptyReads);
        httpRespond(client, 200, body);
        Serial.printf("Raw upload done: %s %s\n", full.c_str(), body);
    }
    // Let the peer close first: the ESP32 has few sockets, and a chunked
    // upload opens one connection per chunk. Closing first here would leave
    // the socket in TIME_WAIT for 120 s and exhaust LWIP_MAX_SOCKETS.
    const uint32_t waitStart = millis();
    while (client.connected() && millis() - waitStart < 1500) {
        while (client.available()) {
            client.read();
        }
        delay(1);
    }
    client.stop();
}

void startFileServer() {
    server.on("/list", HTTP_GET, handleFileList);
    server.on("/reset", HTTP_POST, handleReset);
    server.on("/resetwifi", HTTP_POST, handleWifiReset);
    server.on("/wifiinfo", HTTP_GET, handleWifiInfo);
    server.on("/nettest", HTTP_GET, handleNetTest);
    server.on("/sdtest", HTTP_GET, handleSdTest);
    server.on("/wifimode", HTTP_GET, handleWifiMode);
    server.on("/wifimode", HTTP_POST, handleWifiMode);
    server.on("/ota_fw", HTTP_POST, sendOtaResult, handleOTA);
    server.on("/ota_fs", HTTP_POST, sendOtaResult, handleOTA);
    server.on("/delete", HTTP_POST, handleFileDelete);
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
    Serial.printf("ArduinoOTA ready on %s:%u\n", WiFi.localIP().toString().c_str(), OTA_PORT);
}

void setup()
{
  Serial.begin(115200);

  sdMutex = xSemaphoreCreateRecursiveMutex();

  const bool formatOnFail = true;
  WUDFS.begin(formatOnFail);

  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  sdReady = SD.begin(SD_CS, SPI, SD_SPI_HZ, "/sd", 8);
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

  // WiFi mode is selected in the web UI: access point only (lower power, no
  // station link) or station (join a network, mDNS and ArduinoOTA available).
  wifiMode = loadWifiMode();
  bool staConnected = false;

  if (wifiMode == WUD_MODE_STA) {
    wm.setConnectTimeout(20);
    wm.setWiFiAutoReconnect(true);
    wm.setAPCallback([](WiFiManager *) {
      Serial.print("WiFi config portal: http://");
      Serial.println(WiFi.softAPIP());
    });
    wm.setConfigPortalTimeout(180);

    staConnected = wm.autoConnect(SSID, PASSWORD);
    if (staConnected) {
      Serial.print("WiFi connected: ");
      Serial.print(WiFi.SSID());
      Serial.print(" -> ");
      Serial.println(WiFi.localIP());

      if (MDNS.begin("wud")) {
        MDNS.addService("http", "tcp", 80);
        Serial.println("mDNS: http://wud.local/");
      } else {
        Serial.println("mDNS start failed");
      }
    } else {
      Serial.println("Station mode: no connection, falling back to access point");
    }
  } else {
    Serial.println("WiFi mode: access point only");
  }

  if (staConnected) {
    // Data transfers are throttled by modem sleep on the station link.
    WiFi.setSleep(false);
    startArduinoOTA();
  } else {
    // Access point only: no station link, no mDNS, no ArduinoOTA.
    WiFi.mode(WIFI_AP);
    WiFi.setSleep(false);
    if (!WiFi.softAP(SSID, PASSWORD)) {
      Serial.println("Failed to start access point");
    }
    Serial.print("AP IP: ");
    Serial.println(WiFi.softAPIP());
  }

  uploadPoolInit();

  delay(1000);
  uploadServer.begin();
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
    if (saved) {
      // Credentials were entered, so the device belongs on the station link.
      saveWifiMode(WUD_MODE_STA);
    }

    // Reboot to apply the new network (or to restore the file server).
    delay(500);
    ESP.restart();
  }

  ArduinoOTA.handle();

  WiFiClient uploadClient = uploadServer.available();
  if (uploadClient) {
    handleRawUpload(uploadClient);
  }

  server.handleClient();

  if (resetPending && (int32_t)(millis() - resetAt) >= 0) {
    Serial.println("Reboot requested from web UI");
    delay(20);
    ESP.restart();
  }

  delay(1);
}
