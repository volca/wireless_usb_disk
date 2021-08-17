### Wireless USB Disk

Using ESP32-S2 as a USB Disk with Wireless accessibility. HTTP file server be used with both upload and download capability.

### Known Issues

* Files uploaded through web can not be aware by host , so Windows files resource manager can not update the files list automatically. Please remount the disk to update the files list (弹出重新加载) . This bug will be fixed later.
* Files added or removed through USB disk, sometimes can not be found by web refresh.
