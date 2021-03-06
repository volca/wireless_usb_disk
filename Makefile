MKFS    	?= mklittlefs
ESPTOOL     ?= esptool.py
DEVICE		:= /dev/cu.usbmodem01

default:
	arduino-cli compile .

.PHONY: buildfs
buildfs: 
	mkdir -p build
	$(MKFS) -c data -p 256 -b 4096 -s 1507328 build/fs.bin

.PHONY: uploadfs
uploadfs:
	$(ESPTOOL) --chip esp32s2 --port ${DEVICE} --baud 921600 --before default_reset --after hard_reset write_flash -z --flash_mode dio 2686976 build/fs.bin

.PHONY: upload
upload:
	arduino-cli upload -p ${DEVICE}
