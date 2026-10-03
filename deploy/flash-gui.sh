#!/bin/sh
# usage: ./flash-gui.sh cyd|cyd_new   (LVGL GUI firmware; flash.sh flashes the older text-UI firmware)
set -e
E=${1:?usage: flash-gui.sh cyd|cyd_new}
D="$(dirname "$0")/fw-gui/$E"
sudo systemctl stop claude-buddy
sudo "$(dirname "$0")/venv/bin/esptool.py" --chip esp32 --port /dev/ttyUSB0 --baud 921600 write_flash -z \
  0x1000 "$D/bootloader.bin" 0x8000 "$D/partitions.bin" 0xe000 "$D/boot_app0.bin" 0x10000 "$D/firmware.bin"
sudo systemctl start claude-buddy
