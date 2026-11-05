#include <Arduino.h>
#include <esp_partition.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// lilygo t-display-s3 wiring; the screen uses an 8-bit parallel bus rather than spi, which is what makes full-screen video fast enough
const int lcdPowerPin = 15;   // must be HIGH or the screen stays dark when running on battery
const int lcdBacklightPin = 38;

class TDisplayS3 : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 panel;
  lgfx::Bus_Parallel8 bus;
  lgfx::Light_PWM backlight;

 public:
  TDisplayS3() {
    auto busConfig = bus.config();
    busConfig.freq_write = 20000000;
    busConfig.pin_wr = 8;
    busConfig.pin_rd = 9;
    busConfig.pin_rs = 7;
    busConfig.pin_d0 = 39;
    busConfig.pin_d1 = 40;
    busConfig.pin_d2 = 41;
    busConfig.pin_d3 = 42;
    busConfig.pin_d4 = 45;
    busConfig.pin_d5 = 46;
    busConfig.pin_d6 = 47;
    busConfig.pin_d7 = 48;
    bus.config(busConfig);
    panel.setBus(&bus);

    auto panelConfig = panel.config();
    panelConfig.pin_cs = 6;
    panelConfig.pin_rst = 5;
    panelConfig.pin_busy = -1;
    // the st7789 has 240x320 of memory but only 170 columns are wired to the glass, starting 35 in
    panelConfig.memory_width = 240;
    panelConfig.memory_height = 320;
    panelConfig.panel_width = 170;
    panelConfig.panel_height = 320;
    panelConfig.offset_x = 35;
    panelConfig.offset_y = 0;
    panelConfig.readable = false;
    panelConfig.invert = true;
    panelConfig.rgb_order = false;
    panelConfig.dlen_16bit = false;
    panelConfig.bus_shared = false;
    panel.config(panelConfig);

    auto backlightConfig = backlight.config();
    backlightConfig.pin_bl = lcdBacklightPin;
    backlightConfig.invert = false;
    backlightConfig.freq = 22000;
    backlightConfig.pwm_channel = 7;
    backlight.config(backlightConfig);
    panel.setLight(&backlight);

    setPanel(&panel);
  }
};

TDisplayS3 screen;

// matches the layout written by tools/make_video.py
struct VideoHeader {
  char magic[4];
  uint32_t frameCount;
  uint32_t framesPerSecond;
  uint32_t width;
  uint32_t height;
};

struct FrameIndexEntry {
  uint32_t offset;
  uint32_t length;
};

// the whole video partition is memory-mapped, so frames are decoded straight out of flash with no copying or file system
const uint8_t *videoData = nullptr;
const VideoHeader *videoHeader = nullptr;
const FrameIndexEntry *frameIndex = nullptr;

void showError(const char *message) {
  screen.fillScreen(TFT_BLACK);
  screen.setTextColor(TFT_RED);
  screen.setTextSize(2);
  screen.setCursor(10, 10);
  screen.print(message);
  Serial.println(message);
}

bool mapVideo() {
  const esp_partition_t *videoPartition =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "video");
  if (!videoPartition) return false;
  esp_partition_mmap_handle_t mapHandle;
  const void *mappedAddress;
  if (esp_partition_mmap(videoPartition, 0, videoPartition->size, ESP_PARTITION_MMAP_DATA, &mappedAddress, &mapHandle) != ESP_OK) {
    return false;
  }
  videoData = (const uint8_t *)mappedAddress;
  videoHeader = (const VideoHeader *)videoData;
  frameIndex = (const FrameIndexEntry *)(videoData + sizeof(VideoHeader));
  return memcmp(videoHeader->magic, "VID1", 4) == 0 && videoHeader->frameCount > 0;
}

void setup() {
  Serial.begin(115200);
  pinMode(lcdPowerPin, OUTPUT);
  digitalWrite(lcdPowerPin, HIGH);

  screen.init();
  // landscape, matching the video's 320x170 shape
  screen.setRotation(1);
  screen.setBrightness(255);
  screen.fillScreen(TFT_BLACK);

  if (!mapVideo()) {
    showError("no video found\nflash data/video.bin");
    while (true) delay(1000);
  }
  Serial.printf("video: %u frames at %u fps\n", videoHeader->frameCount, videoHeader->framesPerSecond);
}

void loop() {
  static uint32_t loopStartMs = millis();
  uint32_t frameDurationMs = 1000 / videoHeader->framesPerSecond;
  uint32_t loopLengthMs = videoHeader->frameCount * frameDurationMs;

  // pick the frame from the clock instead of just counting up, so if decoding ever runs slow the video skips frames rather than drifting into slow motion
  uint32_t elapsedMs = (millis() - loopStartMs) % loopLengthMs;
  uint32_t frameNumber = elapsedMs / frameDurationMs;

  static int32_t lastDrawnFrame = -1;
  if ((int32_t)frameNumber == lastDrawnFrame) {
    delay(1);
    return;
  }
  lastDrawnFrame = frameNumber;

  const FrameIndexEntry &frame = frameIndex[frameNumber];
  screen.drawJpg(videoData + frame.offset, frame.length, 0, 0);

  // a once-per-loop report so playback speed can be checked over usb
  static uint32_t framesDrawnThisLoop = 0;
  static uint32_t previousElapsedMs = 0;
  framesDrawnThisLoop++;
  if (elapsedMs < previousElapsedMs) {
    Serial.printf("loop done: drew %u of %u frames\n", framesDrawnThisLoop, videoHeader->frameCount);
    framesDrawnThisLoop = 0;
  }
  previousElapsedMs = elapsedMs;
}
