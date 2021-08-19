/**
 * Simple MSC device with SD card
 * author: chegewara
 */
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiAP.h>
#include <WebServer.h>
#include "sdusb.h"

#define SD_MISO  37
#define SD_MOSI  35
#define SD_SCK   36
#define SD_CS    34

// Config SSID and password
const char* SSID        = "Wireless USB Disk";  // Enter your SSID here
const char* PASSWORD    = "12345678";           // Enter your Password here

SDCard2USB dev;

// web server
WebServer server(80);

void handleRoot() {
    if (!server.hasArg("dir")) {
        server.send(500, "text/plain", "BAD ARGS");
        return;
    }

    String path = server.arg("dir");
    Serial.println("handleFileList: " + path);

    File root = SD.open(path);
    String output = "<table class=\"fixed\" border=\"1\">"
        "<col width=\"800px\" /><col width=\"300px\" /><col width=\"300px\" /><col width=\"100px\" />"
        "<thead><tr><th>Name</th><th>Type</th><th>Size (Bytes)</th><th>Delete</th></tr></thead>"
        "<tbody>";

    if (root && root.isDirectory()) {
        File file = root.openNextFile();
        while (file) {
            output += "<tr><td><a href=\"/" + String(file.name()) + ">" + String(file.name()) + "</a></td><td>";
            if(file.isDirectory()){
                Serial.print("  DIR : ");
                Serial.println(file.name());
                output += "dir";
            } else {
                Serial.print("  FILE: ");
                Serial.print(file.name());
                output += "file";
            }

            output += "</td><td>" + String(file.size()) + "</td><td>Delete</td></tr>";
            file = root.openNextFile();
        }
    }
    server.send(200, "text/html", output);
}

void createDir(fs::FS &fs, const char * path){
    Serial.printf("Creating Dir: %s\n", path);
    if(fs.mkdir(path)){
        Serial.println("Dir created");
    } else {
        Serial.println("mkdir failed");
    }
}

void removeDir(fs::FS &fs, const char * path){
    Serial.printf("Removing Dir: %s\n", path);
    if(fs.rmdir(path)){
        Serial.println("Dir removed");
    } else {
        Serial.println("rmdir failed");
    }
}

void readFile(fs::FS &fs, const char * path){
    Serial.printf("Reading file: %s\n", path);

    File file = fs.open(path);
    if(!file){
        Serial.println("Failed to open file for reading");
        return;
    }

    Serial.print("Read from file: ");
    while(file.available()){
        Serial.write(file.read());
    }
    file.close();
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

void renameFile(fs::FS &fs, const char * path1, const char * path2){
    Serial.printf("Renaming file %s to %s\n", path1, path2);
    if (fs.rename(path1, path2)) {
        Serial.println("File renamed");
    } else {
        Serial.println("Rename failed");
    }
}

void deleteFile(fs::FS &fs, const char * path){
    Serial.printf("Deleting file: %s\n", path);
    if(fs.remove(path)){
        Serial.println("File deleted");
    } else {
        Serial.println("Delete failed");
    }
}

void testFileIO(fs::FS &fs, const char * path){
    File file = fs.open(path);
    static uint8_t buf[512];
    size_t len = 0;
    uint32_t start = millis();
    uint32_t end = start;
    if(file){
        len = file.size();
        size_t flen = len;
        start = millis();
        while(len){
            size_t toRead = len;
            if(toRead > 512){
                toRead = 512;
            }
            file.read(buf, toRead);
            len -= toRead;
        }
        end = millis() - start;
        Serial.printf("%u bytes read for %u ms\n", flen, end);
        file.close();
    } else {
        Serial.println("Failed to open file for reading");
    }


    file = fs.open(path, FILE_WRITE);
    if(!file){
        Serial.println("Failed to open file for writing");
        return;
    }

    size_t i;
    start = millis();
    for(i=0; i<2048; i++){
        file.write(buf, 512);
    }
    end = millis() - start;
    Serial.printf("%u bytes written for %u ms\n", 2048 * 512, end);
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
    server.on("/", HTTP_GET, handleRoot);
    server.begin();
}

void setup()
{
  Serial.begin(115200);

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

