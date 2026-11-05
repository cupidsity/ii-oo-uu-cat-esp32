#include <Arduino.h>
#include <esp_partition.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <JPEGDEC.h>

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
// jpegdec uses the s3's simd instructions and decodes several times faster than lovyangfx's built-in decoder, which is what makes 60 fps reachable
JPEGDEC jpegDecoder;

const int screenWidth = 320;
const int screenHeight = 170;
// two frame buffers so one frame can be decoded while the previous one is still being sent to the screen by dma
uint16_t *frameBuffers[2] = {nullptr, nullptr};
int decodingBufferIndex = 0;

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

// jpegdec hands over the image in blocks; copy each into the frame buffer, clipping the padding it adds past the 170px height
int copyDecodedBlock(JPEGDRAW *decodedBlock) {
  uint16_t *frameBuffer = frameBuffers[decodingBufferIndex];
  int copyWidth = min(decodedBlock->iWidth, screenWidth - decodedBlock->x);
  for (int blockRow = 0; blockRow < decodedBlock->iHeight; blockRow++) {
    int screenRow = decodedBlock->y + blockRow;
    if (screenRow >= screenHeight) break;
    memcpy(frameBuffer + screenRow * screenWidth + decodedBlock->x,
           decodedBlock->pPixels + blockRow * decodedBlock->iWidth,
           copyWidth * sizeof(uint16_t));
  }
  return 1;
}

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

  for (int bufferIndex = 0; bufferIndex < 2; bufferIndex++) {
    frameBuffers[bufferIndex] = (uint16_t *)heap_caps_malloc(screenWidth * screenHeight * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!frameBuffers[bufferIndex]) {
      showError("not enough ram\nfor frame buffers");
      while (true) delay(1000);
    }
  }
  // dma transfers need the bus held open for the whole run, so it's claimed once here and never released
  screen.startWrite();

  if (!mapVideo()) {
    showError("no video found\nflash data/video.bin");
    while (true) delay(1000);
  }
  Serial.printf("video: %u frames at %u fps\n", videoHeader->frameCount, videoHeader->framesPerSecond);
}

void loop() {
  static uint32_t loopStartMs = millis();
  // computed from the total instead of a whole-millisecond frame duration, since 1000/60 doesn't divide evenly and the video would play ~4% fast
  uint32_t loopLengthMs = (uint64_t)videoHeader->frameCount * 1000 / videoHeader->framesPerSecond;

  // pick the frame from the clock instead of just counting up, so if decoding ever runs slow the video skips frames rather than drifting into slow motion
  uint32_t elapsedMs = (millis() - loopStartMs) % loopLengthMs;
  uint32_t frameNumber = (uint64_t)elapsedMs * videoHeader->framesPerSecond / 1000;
  if (frameNumber >= videoHeader->frameCount) frameNumber = videoHeader->frameCount - 1;

  static int32_t lastDrawnFrame = -1;
  if ((int32_t)frameNumber == lastDrawnFrame) {
    delay(1);
    return;
  }
  lastDrawnFrame = frameNumber;

  const FrameIndexEntry &frame = frameIndex[frameNumber];
  uint32_t decodeStartUs = micros();
  if (jpegDecoder.openRAM((uint8_t *)(videoData + frame.offset), frame.length, copyDecodedBlock)) {
    // big endian is the byte order the screen expects, so the buffer can go out over dma untouched
    jpegDecoder.setPixelType(RGB565_BIG_ENDIAN);
    jpegDecoder.decode(0, 0, 0);
    jpegDecoder.close();
  }
  uint32_t decodeUs = micros() - decodeStartUs;

  // the previous frame's dma transfer used the other buffer, so it only has to be finished before starting this one
  screen.waitDMA();
  screen.pushImageDMA(0, 0, screenWidth, screenHeight, (lgfx::swap565_t *)frameBuffers[decodingBufferIndex]);
  decodingBufferIndex = 1 - decodingBufferIndex;

  // a once-per-loop report so playback speed can be checked over usb
  static uint32_t framesDrawnThisLoop = 0;
  static uint64_t totalDecodeUs = 0;
  static uint32_t previousElapsedMs = 0;
  framesDrawnThisLoop++;
  totalDecodeUs += decodeUs;
  if (elapsedMs < previousElapsedMs) {
    Serial.printf("loop done: drew %u of %u frames, average decode %.1f ms\n", framesDrawnThisLoop,
                  videoHeader->frameCount, totalDecodeUs / 1000.0 / framesDrawnThisLoop);
    framesDrawnThisLoop = 0;
    totalDecodeUs = 0;
  }
  previousElapsedMs = elapsedMs;
}
