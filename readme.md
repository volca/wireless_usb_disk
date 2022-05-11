### Wireless USB Disk

Using ESP32-S2 as an USB Disk with Wireless accessibility. HTTP file server be used with both upload and download capability.

Note: It's a demo code. Don't use it in serious application.

### Compile

For first time, install arduino-cli

* arduino-cli core update-index
* arduino-cli core install esp32:esp32
* arduino-cli core upgrade

compile

* arduino-cli compile .
* arduino-cli upload -p /dev/cu.usbmodem01

### Setup

* Default WiFi SSID: `wireless-usb-disk`
* Default WiFi password: `12345678`

### Known Issues

* Files uploaded through web can not be aware by host , so Windows files resource manager can not update the files list automatically. Please remount the disk to update the files list (弹出重新加载) . This bug will be fixed later.
* Files added or removed through USB disk, sometimes can not be found by web refresh.
