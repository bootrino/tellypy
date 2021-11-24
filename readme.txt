To get it compile you must use the esp_iot_sdk_v1.3.0_15_08_08.zip
http://bbs.espressif.com/download/file.php?id=664

Refer to this page for more information.
https://github.com/hrvach/espple/issues/5


esptool.py -p /dev/cu.usbserial-1470 write_flash --flash_mode dio 0x00000 image.elf-0x00000.bin 0x40000 image.elf-0x40000.bin

