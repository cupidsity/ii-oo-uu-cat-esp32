# ii oo uu cat on esp32

Plays a looping video on a LilyGO T-Display-S3 (ESP32-S3, 16MB flash, 8MB PSRAM, 1.9" 320x170 ST7789 screen).

The video is stored as one JPEG per frame in its own flash partition. The player memory-maps that partition and decodes each frame straight from flash, so it runs at the full 60 fps without a file system or SD card.

Frames are decoded with [JPEGDEC](https://github.com/bitbank2/JPEGDEC), which uses the ESP32-S3's SIMD instructions (about 7.4 ms per frame here, against a 16.7 ms budget). Two frame buffers let the next frame decode while the previous one is still going out to the screen over DMA. Frames are picked by the clock, so playback stays real-time even if a frame is slow to decode.

## Flashing

Requires [PlatformIO](https://platformio.org/) and `esptool`.

```sh
# 1. build and upload the player
pio run -t upload

# 2. write the video into the video partition
tools/flash_video.sh            # defaults to /dev/cu.usbmodem1101
```

## Using a different video

Needs `ffmpeg`.

```sh
python3 tools/make_video.py "/path/to/video.mov"
tools/flash_video.sh
```

The script scales and center-crops the video to 320x170, samples it at 60 fps, and packs the frames into `data/video.bin`. Audio is dropped since the board has no speaker. The video partition holds about 12.9MB, which is about a minute of video at 60 fps and the default quality. Lower `frames_per_second` in the script to fit longer videos.

## Files

- `src/main.cpp`: display setup and the playback loop
- `partitions.csv`: 3MB app partition plus a ~12.9MB `video` data partition at `0x310000`
- `tools/make_video.py`: converts a video into `data/video.bin`
- `tools/flash_video.sh`: writes `data/video.bin` to the board
