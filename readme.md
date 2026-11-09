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

* Default WiFi SSID: `wireless-usb-disk`
* Default WiFi password: `12345678`
* Web UI: http://192.168.4.1/
* SD card (SPI): SCK=36, MISO=37, MOSI=35, CS=34
* Plug the **native USB** port (not the UART bridge) into the host

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
