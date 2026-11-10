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
* Web UI: http://192.168.4.1/
* SD card (SPI): SCK=36, MISO=37, MOSI=35, CS=34
* Plug the **native USB** port (not the UART bridge) into the host

### WiFi

WiFi is handled by [WiFiManager](https://github.com/tzapu/WiFiManager)
(`tzapu/WiFiManager@^2.0.17`):

* On boot the device tries the station credentials stored in NVS.
* If none are stored, or the saved network cannot be reached, it opens the
  captive configuration portal on its own AP (`wireless-usb-disk` /
  `12345678`). The portal times out after 180 s; the device then keeps running
  as access point only.
* The AP stays enabled (`WIFI_AP_STA`), so the file manager is always reachable
  at http://192.168.4.1/. When a station connection also succeeds, the
  router-assigned IP is printed on the serial console.
* With a station connection, mDNS is started so the file manager is also
  reachable at http://wud.local/ (no mDNS in access-point-only mode).
* **WiFi Setup** in the web UI (`POST /resetwifi`) erases the saved credentials
  and opens the configuration portal in place, on the same AP/IP, without a
  reboot. The file server is paused while the portal runs (both use port 80).
  Saving the form reconnects the device and restarts it; closing the portal
  without saving returns to the normal AP + file server after 5 minutes.

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
