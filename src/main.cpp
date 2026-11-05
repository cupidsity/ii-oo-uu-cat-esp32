#include <Arduino.h>
#include <esp_partition.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <JPEGDEC.h>

// lilygo t-display-s3 wiring; the screen uses an 8-bit parallel bus rather than spi, which is what makes full-screen video fast enough
const int lcdPowerPin = 15;   // must be HIGH or the screen stays dark when running on battery
const int lcdBacklightPin = 38;
// the button on GPIO14 toggles power; GPIO0 (the other button) is avoided because holding it during a wake-up would put the chip in flashing mode
const int powerButtonPin = 14;
// safe to read at runtime; it only puts the chip in flashing mode if held while the board is starting up
const int batteryButtonPin = 0;
const unsigned long buttonDebounceMs = 30;

// the battery feeds GPIO4 through a divider that halves its voltage, so the adc never sees more than it can measure
const int batteryVoltagePin = 4;
const float batteryDividerRatio = 2.0;
const unsigned long batteryReadIntervalMs = 1000;
// above this the reading is the charger's output rather than the cell, which happens on usb with no battery or while charging
const int usbPowerMillivolts = 4350;

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
// sprites wrapped around the frame buffers so the battery badge can be drawn onto a decoded frame before it goes out
LGFX_Sprite frameSprites[2];

bool isBatteryShown = false;
int batteryMillivolts = 0;

struct DebouncedButton {
  int pin;
  int stableState;
  int lastRawState;
  unsigned long lastRawChangeTime;

  // true only on the moment the button goes down, so holding it doesn't keep toggling
  bool wasJustPressed() {
    int rawState = digitalRead(pin);
    if (rawState != lastRawState) {
      lastRawState = rawState;
      lastRawChangeTime = millis();
    }
    if (rawState != stableState && millis() - lastRawChangeTime > buttonDebounceMs) {
      stableState = rawState;
      return stableState == LOW;
    }
    return false;
  }
};

DebouncedButton powerButton = {powerButtonPin, HIGH, HIGH, 0};
DebouncedButton batteryButton = {batteryButtonPin, HIGH, HIGH, 0};

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

// deep sleep is the closest the esp32 gets to "off": the cpu, wifi and ram shut down and only the wake-up circuit watching the button stays alive
void powerOff() {
  screen.waitDMA();
  screen.endWrite();
  screen.setBrightness(0);
  // tells the st7789 to stop driving the glass, which drops the screen's own draw to almost nothing
  screen.sleep();

  // pins normally float in deep sleep, so the backlight and screen power are latched low or the backlight could glow faintly
  pinMode(lcdBacklightPin, OUTPUT);
  digitalWrite(lcdBacklightPin, LOW);
  digitalWrite(lcdPowerPin, LOW);
  gpio_hold_en((gpio_num_t)lcdBacklightPin);
  gpio_hold_en((gpio_num_t)lcdPowerPin);
  gpio_deep_sleep_hold_en();

  // wake-up triggers on the pin being LOW, so sleeping while the press is still held would wake it straight back up
  while (digitalRead(powerButtonPin) == LOW) delay(10);
  delay(buttonDebounceMs);

  rtc_gpio_pullup_en((gpio_num_t)powerButtonPin);
  rtc_gpio_pulldown_dis((gpio_num_t)powerButtonPin);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)powerButtonPin, 0);
  Serial.println("powering off");
  Serial.flush();
  esp_deep_sleep_start();
}

void updateBatteryReading() {
  static unsigned long lastReadTime = 0;
  if (lastReadTime != 0 && millis() - lastReadTime < batteryReadIntervalMs) return;
  lastReadTime = millis();
  // single adc reads jitter by tens of millivolts, so a handful are averaged to keep the percentage from flickering
  const int sampleCount = 8;
  uint32_t totalMillivolts = 0;
  for (int sampleIndex = 0; sampleIndex < sampleCount; sampleIndex++) {
    totalMillivolts += analogReadMilliVolts(batteryVoltagePin);
  }
  batteryMillivolts = totalMillivolts / sampleCount * batteryDividerRatio;
}

// lipo voltage isn't linear with charge: it sits around 3.7-3.9V for most of the discharge and drops fast below 3.6V
int batteryPercentFromMillivolts(int millivolts) {
  const int curveMillivolts[] = {3300, 3500, 3600, 3700, 3750, 3800, 3850, 3900, 4000, 4100, 4200};
  const int curvePercent[] = {0, 5, 10, 20, 30, 40, 50, 60, 75, 90, 100};
  const int curvePoints = sizeof(curveMillivolts) / sizeof(curveMillivolts[0]);
  if (millivolts <= curveMillivolts[0]) return 0;
  if (millivolts >= curveMillivolts[curvePoints - 1]) return 100;
  for (int pointIndex = 1; pointIndex < curvePoints; pointIndex++) {
    if (millivolts <= curveMillivolts[pointIndex]) {
      int lowMillivolts = curveMillivolts[pointIndex - 1];
      int lowPercent = curvePercent[pointIndex - 1];
      return lowPercent + (millivolts - lowMillivolts) * (curvePercent[pointIndex] - lowPercent) /
                              (curveMillivolts[pointIndex] - lowMillivolts);
    }
  }
  return 100;
}

void drawBatteryBadge(LGFX_Sprite &frameSprite) {
  const int badgeWidth = 74;
  const int badgeHeight = 22;
  const int badgeLeft = screenWidth - badgeWidth - 4;
  const int badgeTop = 4;
  frameSprite.fillRoundRect(badgeLeft, badgeTop, badgeWidth, badgeHeight, 5, TFT_BLACK);

  bool onUsbPower = batteryMillivolts >= usbPowerMillivolts;
  // the charger lifts the reading while usb is in, so this reads high (usually 100%) until usb is unplugged
  int percent = batteryPercentFromMillivolts(batteryMillivolts);
  uint32_t levelColor = percent > 50 ? TFT_GREEN : percent > 20 ? TFT_YELLOW : TFT_RED;

  const int iconLeft = badgeLeft + 6;
  const int iconTop = badgeTop + 5;
  const int iconWidth = 22;
  const int iconHeight = 12;
  frameSprite.drawRect(iconLeft, iconTop, iconWidth, iconHeight, TFT_WHITE);
  frameSprite.fillRect(iconLeft + iconWidth, iconTop + 3, 2, iconHeight - 6, TFT_WHITE);
  int fillWidth = (iconWidth - 4) * percent / 100;
  frameSprite.fillRect(iconLeft + 2, iconTop + 2, fillWidth, iconHeight - 4, levelColor);
  if (onUsbPower) {
    // lightning bolt over the icon so a high reading on usb is understood as "charging" rather than a true full battery
    int boltCenterX = iconLeft + iconWidth / 2;
    frameSprite.fillTriangle(boltCenterX + 2, iconTop - 1, boltCenterX - 4, iconTop + 7, boltCenterX, iconTop + 7, TFT_CYAN);
    frameSprite.fillTriangle(boltCenterX - 2, iconTop + iconHeight + 1, boltCenterX + 4, iconTop + 5, boltCenterX, iconTop + 5, TFT_CYAN);
  }

  char label[8];
  snprintf(label, sizeof(label), "%d%%", percent);
  frameSprite.setFont(&fonts::Font2);
  frameSprite.setTextColor(TFT_WHITE);
  frameSprite.setTextDatum(middle_left);
  frameSprite.drawString(label, iconLeft + iconWidth + 6, badgeTop + badgeHeight / 2);
}

void setup() {
  Serial.begin(115200);
  // waking from deep sleep restarts the program here; the latches from powerOff() have to be released or the screen stays dark
  gpio_hold_dis((gpio_num_t)lcdBacklightPin);
  gpio_hold_dis((gpio_num_t)lcdPowerPin);
  gpio_deep_sleep_hold_dis();
  rtc_gpio_deinit((gpio_num_t)powerButtonPin);
  pinMode(powerButtonPin, INPUT_PULLUP);
  pinMode(batteryButtonPin, INPUT_PULLUP);
  analogReadResolution(12);
  // the press that woke the board is usually still held; waiting it out stops it being read as an immediate "off" press
  while (digitalRead(powerButtonPin) == LOW) delay(10);

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
    // rgb565_2Byte is lovyangfx's byte-swapped 565, the same big-endian layout jpegdec writes, so both can share the buffer
    frameSprites[bufferIndex].setBuffer(frameBuffers[bufferIndex], screenWidth, screenHeight, lgfx::rgb565_2Byte);
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
  if (powerButton.wasJustPressed()) powerOff();
  if (batteryButton.wasJustPressed()) {
    isBatteryShown = !isBatteryShown;
    Serial.printf("battery badge %s (%d mV)\n", isBatteryShown ? "shown" : "hidden", batteryMillivolts);
  }
  updateBatteryReading();

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
  if (isBatteryShown) drawBatteryBadge(frameSprites[decodingBufferIndex]);

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
    Serial.printf("loop done: drew %u of %u frames, average decode %.1f ms, battery %d mV\n", framesDrawnThisLoop,
                  videoHeader->frameCount, totalDecodeUs / 1000.0 / framesDrawnThisLoop, batteryMillivolts);
    framesDrawnThisLoop = 0;
    totalDecodeUs = 0;
  }
  previousElapsedMs = elapsedMs;
}
