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

Toolchain: `platform = platformio/espressif32@6.12.0` (arduino-esp32 2.0.17).
The board is a 4 MB ESP32-S2, so `platformio.ini` selects the `min_spiffs`
partition table (1920 KB app0 + 1920 KB app1 + 128 KB filesystem) instead of the
default 1280 KB layout -- the 2.x firmware does not fit otherwise, and the
128 KB filesystem is enough because `data/` is only ~24 KB.

### Setup

* Default AP SSID: `wireless-usb-disk`
* Default AP password: `12345678`
* Web UI: http://192.168.4.1/ (access-point mode)
* SD card (SPI): SCK=36, MISO=37, MOSI=35, CS=34, 25 MHz
* **SD card: use a Class 10 (U1/A1) or faster card.** The upload path writes
  4 KB blocks while the network is receiving; a slow card turns every block into
  a stall (a cheap card measured 3.2-4.2 s per MB, a Class 10 card 1.0-1.2 s per
  MB), and the block write time then shows up directly as reduced throughput.
* Plug the **native USB** port (not the UART bridge) into the host

### Web UI

* **Upload files** card: pick one or more files, or drop them onto the card
  (drag & drop feeds the same `<input type=file multiple>` through
  `DataTransfer`, so the 20 MB/file limit and the uploader are unchanged).
* **Tools** card: **Reset ESP32** (first, the common action), **WiFi
  settings** (goes straight to the config portal), **Update (OTA)** (last,
  rarely used).
* File list below: click to download, Delete to remove.

### WiFi

Station is the default; there is no mode switch. On boot only the saved network
is tried (`wm.setEnableConfigPortal(false)`), so an unconfigured device comes
up as an access point instead of blocking in the portal:

* **Joined a network**: the router-assigned IP is printed on the serial
  console, mDNS publishes `wud.local`, and the ArduinoOTA server (port 3232)
  starts. Data transfers also disable modem sleep.
* **No credentials, or the network cannot be reached**: the firmware opens its
  own access point, SSID `wireless-usb-disk` / `12345678`, with the file
  manager at http://192.168.4.1/.

**WiFi settings** (`POST /wifisetup`) opens the WiFiManager portal
(`tzapu/WiFiManager@^2.0.17`) immediately, without erasing anything:

1. The device restarts its access point `wireless-usb-disk`;
2. connect to it, open http://192.168.4.1/, enter SSID and password;
3. saving reboots the device, which then joins that network in station mode.

The portal gives up after 5 minutes and the device reboots back into its
previous state; the credentials already stored are kept, so cancelling leaves
the device on the network it knows. The file server pauses while the portal
owns port 80.

In access-point mode there is no `wud.local` and no ArduinoOTA; in station
mode there is no 192.168.4.1.

### Upload path

The browser does not use a multipart form. Each file is cut into 512 KB
chunks and POSTed as a raw body to a second server on port 81:

```
POST /u/<urlencoded name>               first chunk (creates the file)
POST /u/<urlencoded name>?append=1      every following chunk
Content-Length: 524288
```

On the device `handleRawUpload()` parses the request line and headers by hand,
then streams the body through four static 4 KB blocks: the reader hands each
block to `uploadWriterTask`, which does the SD write, so the TCP receive window
keeps moving while the card is busy. The reply is `{"bytes":N,"ms":N}`, or
`{"error":"...","bytes":N,"ms":N}` with a 500 status; a failed chunk deletes
the partially written file and the browser restarts the whole file once.

After the reply the handler waits up to 1.5 s for the peer to close before
calling `stop()`. Closing first leaves the socket in TIME_WAIT for ~120 s, and a
20-chunk upload would exhaust the few lwIP sockets the S2 has.

### OTA update

The **Update (OTA)** dialog offers two targets:

* **Firmware** -> `POST /ota_fw`, writes `firmware.bin` into the inactive app
  partition (1920 KB) with the core `Update` library and sets it as the boot
  partition; the device restarts when the transfer completes.
* **Filesystem (LittleFS)** -> `POST /ota_fs`, unmounts LittleFS, writes the
  image into the `spiffs` data partition (128 KB) and restarts.

Images to upload are the PlatformIO build outputs:

```
.pio/build/wud/firmware.bin     -> Firmware
.pio/build/wud/littlefs.bin     -> Filesystem (LittleFS)
```

Uploading the wrong image to the wrong target is rejected by the update
library (no ESP image magic, or too large for the partition) and the error is
shown in the browser. A failed filesystem update remounts the old filesystem;
a power cycle restores the normal web UI if it does not.

Changing the partition table cannot be done over OTA: it needs one USB flash,
and the filesystem is re-created afterwards (`WUDFS.begin(true)` formats when
the mount fails), so run `-t uploadfs` right after such a flash.

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

### Performance and memory

Measured with a 10 MB upload split into 20 chunks, station mode, 2.4 GHz:

* ~64 KB/s end to end (167 s); SD writes ~500 KB/s; md5 verified, no data
  corruption.
* The ceiling is lwIP, not the card. The core ships
  `CONFIG_LWIP_TCP_WND_DEFAULT=5760` with window scaling disabled, so at most
  ~4.8 segments are in flight; `tcpdump` shows the advertised window pinned at
  5760 and collapsing to 960 while the receiver drains. Raising it needs a
  rebuilt `liblwip.a` (an ESP-IDF build) -- a `lwipopts.h` in the sketch folder
  only affects the sketch's own translation units, not the prebuilt library.
* The remaining 10-20 s stalls are packet loss followed by the 3/6/12 s TCP RTO
  chain, not SD latency: they occur even when the heap is plenty free.
* Card quality matters on its own axis: writes must keep up with ~64 KB/s of
  incoming data plus peaks, so a Class 10 (U1/A1) card is the floor here. On a
  slow card the same firmware dropped to ~30 KB/s because each 4 KB block
  blocked the writer for milliseconds at a time.

Memory notes (heap is 175 KB total, ~67 KB free after setup):

* `SD.begin(..., max_files)` costs ~4.2 KB of heap **per file slot**. Four
  slots instead of eight freed 16.7 KB and removed the low-heap stalls
  entirely (heap floor during upload went from 7 KB to 28-42 KB).
* `WiFi.useStaticBuffers(true)` is not usable here: it moves ~32 KB of buffers
  into `.bss`, shrinking the heap by the same amount, and mDNS/ArduinoOTA then
  fail to allocate -- the device joins the network and drops off right after
  boot.
* Never call `WiFi.persistent(false)` before WiFi is initialised. On this core
  the flag is only read by the first `esp_wifi_init()`, where it selects
  `WIFI_STORAGE_RAM` for the whole session, so credentials saved later by
  WiFiManager live in RAM and vanish on reset.

### Known Issues

* Files uploaded through the web interface are not seen by the host until the
  disk is re-mounted (the host caches the FAT). Eject and replug, or browse
  away and back, to refresh.
* Files added or removed through USB are sometimes not visible on the web UI
  until refresh: `readRAW/writeRAW` bypasses the FatFS cache used by the web
  file API.
* Do not write the same file from the web UI and the host at the same time.
* Upload throughput stays around 64 KB/s because of the lwIP window; see
  "Performance and memory" for what would actually change it.
