### Wireless USB Disk

ESP32-S2 firmware that exposes an SD card as a USB Mass Storage device and
serves the same card over WiFi with an HTTP file manager (upload, download,
delete, directory listing).

Note: this is demo code. Don't use it in a serious application.

### Compile (PlatformIO)

```
pio run
pio run -t upload
pio run -t uploadfs     # writes data/index.htm to LittleFS
```

### Setup

* Default AP SSID: `wireless-usb-disk`
* Default AP password: `12345678`
* Web UI: http://192.168.4.1/ (access-point mode)
* SD card (SPI): SCK=36, MISO=37, MOSI=35, CS=34
* Plug the **native USB** port (not the UART bridge) into the host

### WiFi

The web UI has a **WiFi** card that selects the radio mode; the choice is
stored in NVS and applied on boot. Only one of AP / STA runs at a time.

* **Access point only** (default): `WIFI_AP` with SSID `wireless-usb-disk` /
  `12345678`, file manager at http://192.168.4.1/. No station link, no mDNS,
  no ArduinoOTA. The radio never time-shares between AP and STA, which keeps
  the power draw down.
* **Station**: joins the network configured through the WiFiManager portal
  (`tzapu/WiFiManager@^2.0.17`). The router-assigned IP is printed on the
  serial console, mDNS publishes `wud.local`, and the ArduinoOTA server
  (port 3232) starts. If no credentials are stored, or the network cannot be
  reached, the configuration portal opens on a temporary AP; after the 180 s
  timeout the device falls back to access-point-only mode so it stays
  reachable.
* Switching modes (`POST /wifimode` with `mode=ap` or `mode=sta`) saves the
  choice and reboots the device.
* **WiFi Setup** (`POST /resetwifi`) erases the saved credentials and opens
  the WiFiManager portal in place; the file server pauses while the portal
  owns port 80. Saving credentials switches the device to station mode and
  reboots.

In access-point mode there is no `wud.local` and no ArduinoOTA; in station
mode there is no 192.168.4.1.

### OTA update

The web UI has an **Update (OTA)** section. Pick the target first:

* **Firmware** -> `POST /ota_fw`, writes `firmware.bin` into the inactive app
  partition (`app1`, 1280 KB) with the core `Update` library and sets it as the
  boot partition; the device restarts when the transfer completes.
* **Filesystem (LittleFS)** -> `POST /ota_fs`, unmounts LittleFS, writes the
  image into the `spiffs` data partition (1408 KB) and restarts.

Images to upload are the PlatformIO build outputs:

```
.pio/build/wud/firmware.bin     -> Firmware
.pio/build/wud/littlefs.bin     -> Filesystem (LittleFS)
```

Uploading the wrong image to the wrong target is rejected by the update
library (no ESP image magic, or too large for the partition) and the error is
shown in the browser. A failed filesystem update remounts the old filesystem;
a power cycle restores the normal web UI if it does not.

### OTA over the network (espota)

`platformio.ini` defines an `espota` environment that talks to the
`ArduinoOTA` server in the firmware (port 3232, password `wud`). The server is
started only when the device has joined a station network, together with mDNS,
so `upload_port` is the hostname `wud.local`:

```
pio run -e espota -t upload       # firmware.bin over WiFi
pio run -e espota -t uploadfs     # littlefs.bin over WiFi
```

The environment extends `env:wud`, so it builds the same firmware. In
access-point-only mode (no saved station credentials, or the network is
unreachable) ArduinoOTA is not started and there is no `wud.local`; use the web
UI's OTA section or reflash over USB instead.

The first flash always has to go over USB (`pio run -e wud -t upload`), since
`espota` needs the ArduinoOTA server to be running already.

### USB stack

The USB side uses arduino-esp32's built-in TinyUSB MSC class
(`cores/esp32/USBMSC.cpp`) instead of an external USB library:

* `USBMSC` runs in OTG mode (`ARDUINO_USB_MODE=0`,
  `ARDUINO_USB_CDC_ON_BOOT=0`), so `Serial` stays on UART0.
* `MSC.onRead()/onWrite()` map the host's LBA requests to
  `SD.readRAW()/SD.writeRAW()`: the whole card is exported and the host
  mounts the FAT partition itself.
* A FreeRTOS recursive mutex serializes SD access between the USB task and the
  web server task; large web downloads are streamed in 512-byte chunks so USB
  transfers can interleave.
* `board_build.filesystem = littlefs` makes `pio run -t uploadfs` build a
  LittleFS image for the web UI.

The API used here matches arduino-esp32 3.3.x's `USBMSC`; on 2.0.x the only
difference is that `isWritable()` does not exist (media is writable by
default).

### Known Issues

* Files uploaded through the web interface are not seen by the host until the
  disk is re-mounted (the host caches the FAT). Eject and replug, or browse
  away and back, to refresh.
* Files added or removed through USB are sometimes not visible on the web UI
  until refresh: `readRAW/writeRAW` bypasses the FatFS cache used by the web
  file API.
* Do not write the same file from the web UI and the host at the same time.
