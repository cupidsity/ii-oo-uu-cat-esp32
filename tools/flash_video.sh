#!/bin/sh
# writes data/video.bin into the "video" partition; the offset must match partitions.csv
port="${1:-/dev/cu.usbmodem1101}"
esptool --chip esp32s3 --port "$port" --baud 921600 write-flash 0x310000 "$(dirname "$0")/../data/video.bin"
