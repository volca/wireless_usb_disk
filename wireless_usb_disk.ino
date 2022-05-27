/**
 * Simple MSC device with SD card
 * author: chegewara
 */
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiAP.h>
#include <WebServer.h>
#include <LittleFS.h>
#include "sdusb.h"

#define SD_MISO  37
#define SD_MOSI  35
#define SD_SCK   36
#define SD_CS    34

#define SDFS     LittleFS 

// Config SSID and password
const char* SSID        = "wireless-usb-disk";  // Enter your SSID here
const char* PASSWORD    = "12345678";           // Enter your Password here

SDCard2USB dev;

// web server
WebServer server(80);
//holds the current upload
File fsUploadFile;

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
    File file = SDFS.open(path.c_str());
    server.streamFile(file, contentType);
    file.close();
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
        fsUploadFile = SD.open(filename, "w");
        filename = String();
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (fsUploadFile) {
            fsUploadFile.write(upload.buf, upload.currentSize);
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (fsUploadFile) {
            Serial.println("Uploaded");
            fsUploadFile.close();
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
        File file = SDFS.open(path.c_str());
        server.streamFile(file, contentType);
        file.close();
        return true;
    } else {
        String contentType = getContentType(path);
        String pathWithGz = path + ".gz";
        if (SD.exists(pathWithGz) || SD.exists(path)) {
            if (SD.exists(pathWithGz)) {
                path += ".gz";
            }
            File file = SD.open(path, "r");
            if (file.isDirectory()) {
                handleIndex();
                file.close();
            } else {
                server.streamFile(file, contentType);
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

    if (!SD.exists(path)) {
        return server.send(404, "text/plain", "FileNotFound");
    }

    Serial.println("Delete: " + path);
    SD.remove(path);
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

    File root = SD.open(path);
    String output = "[";

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
    output += "]";
    server.send(200, "text/json", output);
}

void writeFile(fs::FS &fs, const char * path, const char * message){
    Serial.printf("Writing file: %s\n", path);

    File file = fs.open(path, FILE_WRITE);
    if(!file){
        Serial.println("Failed to open file for writing");
        return;
    }
    if(file.print(message)){
        Serial.println("File written");
    } else {
        Serial.println("Write failed");
    }
    file.close();
}

void appendFile(fs::FS &fs, const char * path, const char * message){
    Serial.printf("Appending to file: %s\n", path);

    File file = fs.open(path, FILE_APPEND);
    if(!file){
        Serial.println("Failed to open file for appending");
        return;
    }
    if(file.print(message)){
        Serial.println("Message appended");
    } else {
        Serial.println("Append failed");
    }
    file.close();
}

void testSD()
{

    uint8_t cardType = SD.cardType();
    Serial.print("SD Card Type: ");
    if(cardType == CARD_MMC){
        Serial.println("MMC");
    } else if(cardType == CARD_SD){
        Serial.println("SDSC");
    } else if(cardType == CARD_SDHC){
        Serial.println("SDHC");
    } else {
        Serial.println("UNKNOWN");
    }

    uint64_t cardSize = SD.cardSize() / (1024 * 1024);
    Serial.printf("SD Card Size: %lluMB\n", cardSize);
    Serial.printf("Total space: %lluMB\n", SD.totalBytes() / (1024 * 1024));
    Serial.printf("Used space: %lluMB\n", SD.usedBytes() / (1024 * 1024));
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

  const bool formatOnFail = true;
  SDFS.begin(formatOnFail);

  if(dev.initSD(SD_SCK, SD_MISO, SD_MOSI, SD_CS))
  {
    if(dev.begin()) {
      Serial.println("MSC lun 1 begin");
    } else log_e("LUN 1 failed");
  } else Serial.println("Failed to init SD");

  testSD();

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

