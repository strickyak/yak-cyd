#include "lakatos_state.h"

#include <SPI.h>
#include <esp_bt.h>
#include <stdio.h>
#include <string.h>

TFT_eSPI tft = TFT_eSPI();
Preferences prefs;

// ---------------------------------------------------------------------------
// Dedicated Bit-Banged SPI for XPT2046 Touch (keeps Hardware VSPI free for SD)
// Matches the exact 8/16-bit pipelined command & 0xD0 PENIRQ-enable protocol
// of Paul Stoffregen's XPT2046_Touchscreen library.
// ---------------------------------------------------------------------------
static uint8_t xptSpiTransfer8(uint8_t outByte) {
  uint8_t inByte = 0;
  for (int i = 7; i >= 0; i--) {
    digitalWrite(XPT2046_MOSI, (outByte >> i) & 1);
    delayMicroseconds(1);
    digitalWrite(XPT2046_CLK, HIGH);
    if (digitalRead(XPT2046_MISO)) {
      inByte |= (1u << i);
    }
    delayMicroseconds(1);
    digitalWrite(XPT2046_CLK, LOW);
  }
  return inByte;
}

static uint16_t xptSpiTransfer16(uint16_t outWord) {
  uint16_t hi = xptSpiTransfer8((uint8_t)(outWord >> 8));
  uint16_t lo = xptSpiTransfer8((uint8_t)(outWord & 0xFF));
  return (hi << 8) | lo;
}

static int16_t bestTwoAvg(int16_t x, int16_t y, int16_t z) {
  int16_t da = (x > y) ? (x - y) : (y - x);
  int16_t db = (x > z) ? (x - z) : (z - x);
  int16_t dc = (z > y) ? (z - y) : (y - z);
  if (da <= db && da <= dc) return (x + y) >> 1;
  if (db <= da && db <= dc) return (x + z) >> 1;
  return (y + z) >> 1;
}

void initTouchPins() {
  pinMode(XPT2046_IRQ, INPUT);
  pinMode(XPT2046_MISO, INPUT);
  pinMode(XPT2046_MOSI, OUTPUT);
  pinMode(XPT2046_CLK, OUTPUT);
  pinMode(XPT2046_CS, OUTPUT);
  digitalWrite(XPT2046_CS, HIGH);
  digitalWrite(XPT2046_CLK, LOW);
  digitalWrite(XPT2046_MOSI, LOW);

  // Send 0xD0 (PD0=0) so XPT2046 powers down ADC reference and enables PENIRQ
  digitalWrite(XPT2046_CS, LOW);
  xptSpiTransfer8(0xD0);
  xptSpiTransfer16(0x0000);
  digitalWrite(XPT2046_CS, HIGH);
}

static SemaphoreHandle_t gTouchMutex = nullptr;
static SemaphoreHandle_t gTftMutex   = nullptr;
static volatile bool gHasLatchedTouch    = false;
static volatile bool gTouchPenDown       = false;
static volatile bool gTouchPressConsumed = false;
static volatile bool gTouchClickConsumed = false;
static uint32_t gLatchedTouchMs = 0;
static LakatosTouchSample gLatchedTouch = {false, 0, 0, 0, 0, 0};

// App lifecycle & Core 0 top-left 12x12 corner exit / timeout state
static constexpr uint16_t kDarkBrownExitColor = 0x5962;  // Dark brown RGB565 (R=88, G=44, B=16)
static volatile bool     gLCodeAppRunning   = false;
static volatile bool     gInConfigScreen    = false;
static volatile bool     gCornerExitEnabled = true;
static volatile bool     gPleaseExitApp       = false;
static volatile bool     gAppExitedByUser     = false;
static volatile uint32_t gAppDeadlineMs       = 0;
static volatile uint32_t gPendingAppTimeoutMs = 0;

void lakatosBeginAppSession() {
  gPleaseExitApp     = false;
  gAppExitedByUser   = false;
  gCornerExitEnabled = true;
  if (gPendingAppTimeoutMs > 0) {
    gAppDeadlineMs = millis() + gPendingAppTimeoutMs;
  }
  gLCodeAppRunning   = true;
}

void lakatosSetInConfigScreen(bool inCfg) {
  gPleaseExitApp  = false;
  gInConfigScreen = inCfg;
}

void lakatosSetCornerExitEnabled(bool en) {
  gCornerExitEnabled = en;
}

bool lakatosGetCornerExitEnabled() {
  return gCornerExitEnabled;
}

bool lakatosIsCornerExitActive() {
  return (gLCodeAppRunning || lakatosIsNativeAppRunning()) && gCornerExitEnabled;
}

void lakatosSetAppTimeoutMs(uint32_t timeoutMs) {
  gPendingAppTimeoutMs = timeoutMs;
  gAppDeadlineMs       = (timeoutMs > 0) ? (millis() + timeoutMs) : 0;
}

void lakatosRequestAppExit() {
  gAppExitedByUser = true;
  gPleaseExitApp   = true;
}

bool lakatosWasAppExitedByUser() {
  return gAppExitedByUser;
}

bool lakatosShouldExitApp() {
  if (gPleaseExitApp) return true;
  uint32_t dl = gAppDeadlineMs;
  if (dl != 0 && (int32_t)(millis() - dl) >= 0) {
    gPleaseExitApp = true;
    return true;
  }
  return false;
}

#include <stdarg.h>

void lakatosDevLogf(const char* fmt, ...) {
  if (!lakatosGetDeveloperMode() || !fmt) return;
  char buf[192];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.printf("[DEV] %s\r\n", buf);
}

void lakatosTftLock() {
  if (gTftMutex) {
    xSemaphoreTakeRecursive(gTftMutex, portMAX_DELAY);
  }
}

bool lakatosTftTryLock(uint32_t waitMs) {
  if (!gTftMutex) return true;
  return xSemaphoreTakeRecursive(gTftMutex, pdMS_TO_TICKS(waitMs)) == pdTRUE;
}

void lakatosTftUnlock() {
  if (gTftMutex) {
    xSemaphoreGiveRecursive(gTftMutex);
  }
}

enum TouchSpotState {
  SPOT_NONE = 0,
  SPOT_WHITE = 1,
  SPOT_YELLOW = 2,
  SPOT_BLANKED = 3
};
static volatile int gSpotState = SPOT_NONE;
static volatile int gSpotX = 0;
static volatile int gSpotY = 0;
static volatile bool gSpotPendingWhite = false;
static volatile bool gSpotPendingBlankOld = false;
static volatile int  gSpotOldX = 0;
static volatile int  gSpotOldY = 0;
static volatile uint32_t gSpotDeadlineMs = 0;
static volatile uint32_t gLastTouchDownMs = 0;
static constexpr int kSpotRadius = 3;

static void onTouchDeliveredToAppOrUi(int x, int y) {
  if (!lakatosGetTouchSpotEnabled()) return;
  bool doDraw = false;
  bool blankOld = false;
  int oldX = 0, oldY = 0;
  if (gTouchMutex) xSemaphoreTake(gTouchMutex, portMAX_DELAY);
  bool moved = (abs(x - gSpotX) + abs(y - gSpotY) >= 8);
  if (gSpotState == SPOT_WHITE || gSpotState == SPOT_NONE || moved) {
    if ((gSpotState == SPOT_WHITE || gSpotState == SPOT_YELLOW) &&
        (gSpotX != x || gSpotY != y)) {
      blankOld = true;
      oldX = gSpotX;
      oldY = gSpotY;
    }
    gSpotX = x;
    gSpotY = y;
    gSpotState = SPOT_YELLOW;
    gSpotPendingWhite = false;
    gSpotPendingBlankOld = false;
    gSpotDeadlineMs = millis() + 50;
    doDraw = true;
  }
  if (gTouchMutex) xSemaphoreGive(gTouchMutex);

  if (doDraw) {
    LakatosTftGuard tftGuard;
    if (blankOld) {
      tft.fillCircle(oldX, oldY, kSpotRadius, TFT_BLACK);
    }
    tft.fillCircle(x, y, kSpotRadius, TFT_YELLOW);
  }
}

static LakatosTouchSample readTouchHardwareUnlocked() {
  LakatosTouchSample out = {false, 0, 0, 0, 0, 0};
  if (digitalRead(XPT2046_IRQ) == HIGH) {
    return out;
  }

  digitalWrite(XPT2046_CS, LOW);
  xptSpiTransfer8(0xB1 /* Z1 */);
  int16_t z1 = xptSpiTransfer16(0xC1 /* Z2 */) >> 3;
  int z = z1 + 4095;
  int16_t z2 = xptSpiTransfer16(0x91 /* X */) >> 3;
  z -= z2;

  int16_t data[6] = {0, 0, 0, 0, 0, 0};
  if (z >= 400) {
    xptSpiTransfer16(0x91 /* dummy X */);
    data[0] = xptSpiTransfer16(0xD1 /* Y */) >> 3;
    data[1] = xptSpiTransfer16(0x91 /* X */) >> 3;
    data[2] = xptSpiTransfer16(0xD1 /* Y */) >> 3;
    data[3] = xptSpiTransfer16(0x91 /* X */) >> 3;
  }
  // 0xD0 powers down ADC and re-enables PENIRQ (GPIO 36) for subsequent touches!
  data[4] = xptSpiTransfer16(0xD0 /* Y + PENIRQ enable */) >> 3;
  data[5] = xptSpiTransfer16(0x0000) >> 3;
  digitalWrite(XPT2046_CS, HIGH);

  if (z < 400) {
    return out;
  }

  int rx = bestTwoAvg(data[0], data[2], data[4]);
  int ry = bestTwoAvg(data[1], data[3], data[5]);
  if (rx <= 50 || rx >= 4050 || ry <= 50 || ry >= 4050) {
    return out;
  }

  out.touched = true;
  out.rawX = rx;
  out.rawY = ry;
  out.rawZ = z;
  mapRawTouchToScreen(rx, ry, out.x, out.y);
  return out;
}

static inline bool isTopLeftExitCorner(int x, int y) {
  int sz = lakatosGetCornerSize();
  return x >= 0 && x < sz && y >= 0 && y < sz;
}

static LakatosTouchSample gCore0LiveTouch = {false, 0, 0, 0, 0, 0};
static volatile uint32_t  gCore0LiveTouchMs = 0;
static constexpr uint32_t kTouchHoldBridgeMs      = 35u;
static constexpr uint32_t kTouchReleaseDebounceMs = 110u;
static constexpr uint32_t kClickCooldownMs        = 250u;
static volatile uint32_t  gLastClickDeliveredMs   = 0;

LakatosTouchSample lakatosReadTouch() {
  // When an app is running, if Core 0's 5ms scanner sampled a valid touch
  // within kTouchHoldBridgeMs (35ms), return that sample without Core 1
  // stalling on SPI and without dropping contact while XPT2046 PENIRQ is
  // momentarily HIGH during Core 0 ADC conversions or brief contact bounce.
  if (gLCodeAppRunning || lakatosIsNativeAppRunning()) {
    uint32_t now = millis();
    if (gCore0LiveTouch.touched && (uint32_t)(now - gCore0LiveTouchMs) <= kTouchHoldBridgeMs) {
      return gCore0LiveTouch;
    }
  }
  if (digitalRead(XPT2046_IRQ) == HIGH) {
    return {false, 0, 0, 0, 0, 0};
  }
  bool triggeredCornerExit = false;
  if (gTouchMutex) xSemaphoreTake(gTouchMutex, portMAX_DELAY);
  LakatosTouchSample out = readTouchHardwareUnlocked();
  if (out.touched) {
    uint32_t tNow = millis();
    gTouchPenDown = true;
    gTouchPressConsumed = true;
    gHasLatchedTouch = false;
    gLastTouchDownMs = tNow;
    gCore0LiveTouch = out;
    gCore0LiveTouchMs = tNow;
    if (lakatosIsCornerExitActive() && isTopLeftExitCorner(out.x, out.y)) {
      gAppExitedByUser = true;
      gPleaseExitApp = true;
      gTouchClickConsumed = true;
      triggeredCornerExit = true;
    }
  }
  if (gTouchMutex) xSemaphoreGive(gTouchMutex);
  if (triggeredCornerExit) {
    lakatosDevLogf("Corner Exit: (%d,%d)", out.x, out.y);
    return {false, 0, 0, 0, 0, 0};
  }
  if (out.touched) {
    onTouchDeliveredToAppOrUi(out.x, out.y);
  }
  return out;
}

void lakatosPollTouchLatch() {
  uint32_t now = millis();

  if ((gLCodeAppRunning || lakatosIsNativeAppRunning()) &&
      gAppDeadlineMs != 0 && (int32_t)(now - gAppDeadlineMs) >= 0) {
    gPleaseExitApp = true;
  }

  // 1. Check if an active white/yellow spot has reached its blank deadline
  if ((gSpotState == SPOT_WHITE || gSpotState == SPOT_YELLOW) &&
      (int32_t)(now - gSpotDeadlineMs) >= 0) {
    int bx = 0, by = 0;
    bool shouldBlank = false;
    if (gTouchMutex && xSemaphoreTake(gTouchMutex, 0) == pdTRUE) {
      if ((gSpotState == SPOT_WHITE || gSpotState == SPOT_YELLOW) &&
          (int32_t)(millis() - gSpotDeadlineMs) >= 0) {
        bx = gSpotX;
        by = gSpotY;
        shouldBlank = true;
      }
      xSemaphoreGive(gTouchMutex);
    }
    if (shouldBlank && lakatosTftTryLock(1)) {
      tft.fillCircle(bx, by, kSpotRadius, TFT_BLACK);
      lakatosTftUnlock();
      if (gTouchMutex && xSemaphoreTake(gTouchMutex, 0) == pdTRUE) {
        if ((gSpotState == SPOT_WHITE || gSpotState == SPOT_YELLOW) &&
            (int32_t)(millis() - gSpotDeadlineMs) >= 0) {
          gSpotState = (digitalRead(XPT2046_IRQ) == LOW) ? SPOT_BLANKED : SPOT_NONE;
          gSpotPendingWhite = false;
          gSpotPendingBlankOld = false;
        }
        xSemaphoreGive(gTouchMutex);
      }
    }
  }

  // 2. Fast non-blocking check of active-low PENIRQ GPIO 36 (~150 ns when untouched)
  if (digitalRead(XPT2046_IRQ) == HIGH) {
    if ((uint32_t)(now - gLastTouchDownMs) >= kTouchReleaseDebounceMs) {
      if (gSpotState == SPOT_BLANKED) {
        gSpotState = SPOT_NONE;
      }
      if ((gTouchPenDown || gTouchClickConsumed || gCore0LiveTouch.touched) &&
          gTouchMutex && xSemaphoreTake(gTouchMutex, 0) == pdTRUE) {
        if (digitalRead(XPT2046_IRQ) == HIGH &&
            (uint32_t)(millis() - gLastTouchDownMs) >= kTouchReleaseDebounceMs) {
          gTouchPenDown = false;
          gTouchPressConsumed = false;
          gTouchClickConsumed = false;
          gCore0LiveTouch.touched = false;
        }
        xSemaphoreGive(gTouchMutex);
      }
    }
    // Flush any deferred white spot draw if TFT was busy on the previous tick
    if (gSpotPendingWhite && gSpotState == SPOT_WHITE && lakatosTftTryLock(1)) {
      int wx = gSpotX, wy = gSpotY;
      bool bOld = gSpotPendingBlankOld;
      int ox = gSpotOldX, oy = gSpotOldY;
      gSpotPendingWhite = false;
      gSpotPendingBlankOld = false;
      if (bOld) tft.fillCircle(ox, oy, kSpotRadius, TFT_BLACK);
      tft.fillCircle(wx, wy, kSpotRadius, TFT_WHITE);
      lakatosTftUnlock();
    }
    return;
  }

  if (gTouchMutex) {
    if (xSemaphoreTake(gTouchMutex, 0) != pdTRUE) return;
  }
  bool triggeredCornerExit = false;
  LakatosTouchSample s = readTouchHardwareUnlocked();
  if (s.touched) {
    uint32_t tNow = millis();
    gLastTouchDownMs = tNow;
    gCore0LiveTouch = s;
    gCore0LiveTouchMs = tNow;
    if (lakatosIsCornerExitActive() && isTopLeftExitCorner(s.x, s.y)) {
      if (!gPleaseExitApp) {
        triggeredCornerExit = true;
      }
      gAppExitedByUser = true;
      gPleaseExitApp = true;
      gTouchPenDown = true;
      gTouchPressConsumed = true;
      gTouchClickConsumed = true;
      gHasLatchedTouch = false;
    } else if (!gTouchPenDown) {
      // New physical press started: latch it in case Core 1 misses the live window
      gTouchPenDown = true;
      gTouchPressConsumed = false;
      gTouchClickConsumed = false;
      gLatchedTouch = s;
      gLatchedTouchMs = tNow;
      gHasLatchedTouch = true;
    } else if (!gTouchPressConsumed) {
      // Still in the same unconsumed press: keep coordinates fresh
      gLatchedTouch = s;
      gLatchedTouchMs = tNow;
    }
    if (!gPleaseExitApp && lakatosGetTouchSpotEnabled()) {
      bool moved = (abs(s.x - gSpotX) + abs(s.y - gSpotY) >= 8);
      if (gSpotState == SPOT_NONE || moved) {
        if ((gSpotState == SPOT_WHITE || gSpotState == SPOT_YELLOW) &&
            !gSpotPendingWhite) {
          gSpotPendingBlankOld = true;
          gSpotOldX = gSpotX;
          gSpotOldY = gSpotY;
        }
        gSpotX = s.x;
        gSpotY = s.y;
        gSpotState = SPOT_WHITE;
        gSpotPendingWhite = true;
        gSpotDeadlineMs = tNow + 250;
      } else if (gSpotState == SPOT_WHITE) {
        gSpotDeadlineMs = tNow + 250;
      }
    }
  } else if (gTouchPenDown || gTouchClickConsumed) {
    // Physical PENIRQ is still LOW on entry even if Z pressure briefly dipped
    // below threshold during hold or slow lift-off: keep release timer fresh.
    gLastTouchDownMs = millis();
  }
  if (gTouchMutex) xSemaphoreGive(gTouchMutex);

  if (triggeredCornerExit) {
    lakatosDevLogf("Core0 Corner Exit: (%d,%d)", s.x, s.y);
  }

  // 3. Draw white spot on Core 0 outside gTouchMutex while holding gTftMutex
  if (gSpotPendingWhite && gSpotState == SPOT_WHITE && lakatosTftTryLock(1)) {
    int wx = gSpotX, wy = gSpotY;
    bool bOld = gSpotPendingBlankOld;
    int ox = gSpotOldX, oy = gSpotOldY;
    gSpotPendingWhite = false;
    gSpotPendingBlankOld = false;
    if (bOld) tft.fillCircle(ox, oy, kSpotRadius, TFT_BLACK);
    tft.fillCircle(wx, wy, kSpotRadius, TFT_WHITE);
    lakatosTftUnlock();
  }
}

void lakatosClearTouchLatch() {
  if (gTouchMutex) xSemaphoreTake(gTouchMutex, portMAX_DELAY);
  gHasLatchedTouch = false;
  gTouchPressConsumed = true;
  gTouchClickConsumed = true;
  gCore0LiveTouch.touched = false;
  if (gTouchMutex) xSemaphoreGive(gTouchMutex);
}

void lakatosWaitTouchRelease() {
  uint32_t quietSince = millis();
  while (true) {
    if (digitalRead(XPT2046_IRQ) == LOW) {
      quietSince = millis();
    } else if ((uint32_t)(millis() - quietSince) >= kTouchReleaseDebounceMs) {
      break;
    }
    delay(10);
  }
  if (gTouchMutex) xSemaphoreTake(gTouchMutex, portMAX_DELAY);
  gHasLatchedTouch = false;
  gTouchPenDown = false;
  gTouchPressConsumed = false;
  gTouchClickConsumed = false;
  gCore0LiveTouch.touched = false;
  gLastClickDeliveredMs = millis();
  if (gTouchMutex) xSemaphoreGive(gTouchMutex);
}

void lakatosEndAppSession() {
  gLCodeAppRunning     = false;
  gPleaseExitApp       = false;
  gCornerExitEnabled   = true;
  gAppDeadlineMs       = 0;
  gPendingAppTimeoutMs = 0;
  lakatosSetAppArg("");

  // Ensure on-board RGB LED is turned off when app exits
  lakatosSetRgbLed(false, false, false, 0);

  // Clear any pending Core 0 touch spot or stray latched touch
  if (gTouchMutex) xSemaphoreTake(gTouchMutex, portMAX_DELAY);
  gSpotState = SPOT_NONE;
  gSpotPendingWhite = false;
  gSpotPendingBlankOld = false;
  gHasLatchedTouch = false;
  gTouchPressConsumed = true;
  gTouchClickConsumed = true;
  gCore0LiveTouch.touched = false;
  if (gTouchMutex) xSemaphoreGive(gTouchMutex);

  // Acknowledge app exit by painting the entire screen dark brown for 100ms
  {
    LakatosTftGuard tftGuard;
    tft.setTextDatum(TL_DATUM);
    tft.fillScreen(kDarkBrownExitColor);
    delay(100);
  }
}

LakatosTouchSample lakatosReadTouchLatched() {
  LakatosTouchSample live = lakatosReadTouch();
  if (live.touched) {
    if (gTouchMutex) xSemaphoreTake(gTouchMutex, portMAX_DELAY);
    gHasLatchedTouch = false;
    gTouchPressConsumed = true;
    gTouchClickConsumed = true;
    if (gTouchMutex) xSemaphoreGive(gTouchMutex);
    return live;
  }
  if (gHasLatchedTouch) {
    if (gTouchMutex) xSemaphoreTake(gTouchMutex, portMAX_DELAY);
    bool valid = gHasLatchedTouch && !gTouchPressConsumed &&
                 ((uint32_t)(millis() - gLatchedTouchMs) <= 250u);
    LakatosTouchSample latched = gLatchedTouch;
    gHasLatchedTouch = false;
    gTouchPressConsumed = true;
    gTouchClickConsumed = true;
    if (valid && lakatosIsCornerExitActive() && isTopLeftExitCorner(latched.x, latched.y)) {
      gAppExitedByUser = true;
      gPleaseExitApp = true;
      valid = false;
    }
    if (gTouchMutex) xSemaphoreGive(gTouchMutex);
    if (valid) {
      onTouchDeliveredToAppOrUi(latched.x, latched.y);
      lakatosDevLogf("Click: (%d,%d)", latched.x, latched.y);
      return latched;
    }
  }
  return live;
}

LakatosTouchSample lakatosReadClick() {
  uint32_t now = millis();
  if (digitalRead(XPT2046_IRQ) == HIGH) {
    if ((uint32_t)(now - gLastTouchDownMs) >= kTouchReleaseDebounceMs) {
      if ((gTouchPenDown || gTouchClickConsumed) && gTouchMutex) {
        xSemaphoreTake(gTouchMutex, portMAX_DELAY);
        if (digitalRead(XPT2046_IRQ) == HIGH &&
            (uint32_t)(millis() - gLastTouchDownMs) >= kTouchReleaseDebounceMs) {
          gTouchPenDown = false;
          gTouchPressConsumed = false;
          gTouchClickConsumed = false;
          gCore0LiveTouch.touched = false;
        }
        xSemaphoreGive(gTouchMutex);
      }
    }
  } else if (gTouchClickConsumed) {
    // Pen is still held down from an already-consumed click: keep release timer fresh
    gLastTouchDownMs = now;
    return {false, 0, 0, 0, 0, 0};
  }

  if (gTouchClickConsumed) {
    return {false, 0, 0, 0, 0, 0};
  }

  LakatosTouchSample live = lakatosReadTouch();
  if (live.touched) {
    uint32_t clickNow = millis();
    if (gTouchMutex) xSemaphoreTake(gTouchMutex, portMAX_DELAY);
    gTouchPenDown = true;
    gTouchPressConsumed = true;
    gHasLatchedTouch = false;
    bool firstClick = !gTouchClickConsumed &&
                      (gLastClickDeliveredMs == 0 ||
                       (uint32_t)(clickNow - gLastClickDeliveredMs) >= kClickCooldownMs);
    gTouchClickConsumed = true;
    if (firstClick) {
      gLastClickDeliveredMs = clickNow;
    }
    if (gTouchMutex) xSemaphoreGive(gTouchMutex);
    if (firstClick) {
      lakatosDevLogf("Click: (%d,%d)", live.x, live.y);
      return live;
    }
    return {false, 0, 0, 0, 0, 0};
  }

  if (gHasLatchedTouch) {
    uint32_t clickNow = millis();
    if (gTouchMutex) xSemaphoreTake(gTouchMutex, portMAX_DELAY);
    bool valid = gHasLatchedTouch && !gTouchPressConsumed && !gTouchClickConsumed &&
                 ((uint32_t)(clickNow - gLatchedTouchMs) <= 250u) &&
                 (gLastClickDeliveredMs == 0 ||
                  (uint32_t)(clickNow - gLastClickDeliveredMs) >= kClickCooldownMs);
    LakatosTouchSample latched = gLatchedTouch;
    gHasLatchedTouch = false;
    gTouchPressConsumed = true;
    gTouchClickConsumed = true;
    if (valid && lakatosIsCornerExitActive() && isTopLeftExitCorner(latched.x, latched.y)) {
      gAppExitedByUser = true;
      gPleaseExitApp = true;
      valid = false;
    } else if (valid) {
      gLastClickDeliveredMs = clickNow;
    }
    if (gTouchMutex) xSemaphoreGive(gTouchMutex);
    if (valid) {
      onTouchDeliveredToAppOrUi(latched.x, latched.y);
      lakatosDevLogf("Click: (%d,%d)", latched.x, latched.y);
      return latched;
    }
  }
  return {false, 0, 0, 0, 0, 0};
}

static void touchScannerTaskCore0(void* /*param*/) {
  for (;;) {
    lakatosPollTouchLatch();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// ---------------------------------------------------------------------------
// RGB LED, LDR, and CPU Temperature Helpers
// ---------------------------------------------------------------------------
static uint32_t gLedOffDeadlineMs = 0;

void lakatosSetRgbLed(bool r, bool g, bool b, uint32_t durationMs) {
  // Active-low RGB LED on CYD
  digitalWrite(LED_R_PIN, r ? LOW : HIGH);
  digitalWrite(LED_G_PIN, g ? LOW : HIGH);
  digitalWrite(LED_B_PIN, b ? LOW : HIGH);
  if ((r || g || b) && durationMs > 0) {
    gLedOffDeadlineMs = millis() + durationMs;
  } else {
    gLedOffDeadlineMs = 0;
  }
}

void lakatosUpdateRgbLed() {
  if (gLedOffDeadlineMs != 0 && (int32_t)(millis() - gLedOffDeadlineMs) >= 0) {
    gLedOffDeadlineMs = 0;
    digitalWrite(LED_R_PIN, HIGH);
    digitalWrite(LED_G_PIN, HIGH);
    digitalWrite(LED_B_PIN, HIGH);
  }
}

int lakatosReadLdrRaw() {
  return analogRead(LDR_PIN);
}

float lakatosReadCpuTempC() {
  return temperatureRead();
}

// ---------------------------------------------------------------------------
// Lakatos Home Screen & FileBrowser State
// ---------------------------------------------------------------------------
enum ShellScreenMode {
  SCREEN_HOME = 0,
  SCREEN_FILE_BROWSER = 1,
};

enum HomeAppSource : uint8_t {
  APP_SRC_SD    = 0,  // /sd/apps/*.lapp    (1st priority)
  APP_SRC_NET   = 1,  // /net/apps/*.lapp   (2nd priority)
  APP_SRC_FLASH = 2,  // /flash/apps/*.lapp (3rd priority)
};

struct HomeAppEntry {
  char    label[28];     // Basename without ".lapp"
  char    fileName[36];  // Full filename with ".lapp"
  uint8_t source;        // HomeAppSource
};

static constexpr int kMaxHomeApps   = 36;
static constexpr int kMaxDirEntries = 48;
static constexpr int kRowsPerPage   = 6;

static ShellScreenMode gScreenMode      = SCREEN_HOME;
static bool            gHomeMenuOpen    = false;
static bool            gHomeScannedWifi = false;
static int             gHomeAppCount    = 0;

static LakatosFsEntry* const gDirEntries = (LakatosFsEntry*)gLakatosAppRamArena;
static HomeAppEntry* const   gHomeApps   =
    (HomeAppEntry*)(gLakatosAppRamArena + sizeof(LakatosFsEntry) * kMaxDirEntries);
static int gDirCount = 0;
static int gPageIdx = 0;
static char gCurPath[128] = "/flash";
static char gStatusMsg[64] = "Ready";
static uint32_t gLastClockDrawMs = 0;
static bool gLastWifiState = false;

static bool endsWithLapp(const char* name) {
  size_t n = strlen(name);
  if (n < 5) return false;
  return strcasecmp(name + n - 5, ".lapp") == 0;
}

static void joinVfsPath(const char* dir, const char* name, char* out, size_t maxLen) {
  size_t dlen = strlen(dir);
  if (dlen > 0 && dir[dlen - 1] == '/') {
    snprintf(out, maxLen, "%s%s", dir, name);
  } else {
    snprintf(out, maxLen, "%s/%s", dir, name);
  }
}

static void navigateUp() {
  // Don't go above /flash, /sd, or /net roots
  if (strcmp(gCurPath, "/flash") == 0 || strcmp(gCurPath, "/sd") == 0 ||
      strcmp(gCurPath, "/net") == 0) {
    return;
  }
  char* lastSlash = strrchr(gCurPath, '/');
  if (!lastSlash || lastSlash == gCurPath) return;
  *lastSlash = '\0';
}

static bool copyFileToFlash(const char* srcPath, const char* baseName) {
  char dstPath[128];
  if (endsWithLapp(baseName)) {
    lakatosFsMkdir("/flash/apps", true);
    snprintf(dstPath, sizeof(dstPath), "/flash/apps/%s", baseName);
  } else {
    snprintf(dstPath, sizeof(dstPath), "/flash/%s", baseName);
  }

  LakatosFsEntry st;
  if (!lakatosFsStat(srcPath, &st) || st.isDir || st.size < 0) {
    snprintf(gStatusMsg, sizeof(gStatusMsg), "Stat failed: %s", baseName);
    return false;
  }

  constexpr size_t kChunk = 2048;
  uint8_t* buf = (uint8_t*)malloc(kChunk);
  if (!buf) {
    snprintf(gStatusMsg, sizeof(gStatusMsg), "Out of memory");
    return false;
  }

  uint32_t offset = 0;
  bool ok = true;
  if (st.size == 0) {
    ok = lakatosFsWrite(dstPath, "", 0);
  } else {
    while (offset < (uint32_t)st.size) {
      uint32_t toRead = (uint32_t)st.size - offset;
      if (toRead > kChunk) toRead = kChunk;
      int32_t got = lakatosFsRead(srcPath, offset, buf, toRead);
      if (got <= 0) {
        ok = false;
        break;
      }
      if (offset == 0) {
        ok = lakatosFsWrite(dstPath, buf, got);
      } else {
        ok = lakatosFsWriteAt(dstPath, offset, buf, got);
      }
      if (!ok) break;
      offset += (uint32_t)got;
    }
  }
  free(buf);
  if (ok) {
    snprintf(gStatusMsg, sizeof(gStatusMsg), "Copied -> %s (%lld B)", dstPath, (long long)st.size);
  } else {
    snprintf(gStatusMsg, sizeof(gStatusMsg), "Copy failed: %s", baseName);
  }
  return ok;
}

static void collectHomeAppsFromDir(const char* dirPath, uint8_t source) {
  int n = lakatosFsLs(dirPath, gDirEntries, kMaxDirEntries);
  if (n <= 0) return;

  for (int i = 0; i < n; i++) {
    const LakatosFsEntry& e = gDirEntries[i];
    if (e.isDir || !endsWithLapp(e.name)) continue;

    size_t nameLen = strlen(e.name);
    size_t stemLen = nameLen - 5;  // strip ".lapp"
    if (stemLen == 0) continue;

    char stem[28];
    if (stemLen >= sizeof(stem)) stemLen = sizeof(stem) - 1;
    memcpy(stem, e.name, stemLen);
    stem[stemLen] = '\0';

    bool exists = false;
    for (int k = 0; k < gHomeAppCount; k++) {
      if (strcasecmp(gHomeApps[k].label, stem) == 0) {
        exists = true;
        break;
      }
    }
    if (exists) continue;

    if (gHomeAppCount < kMaxHomeApps) {
      HomeAppEntry& dst = gHomeApps[gHomeAppCount++];
      strncpy(dst.label, stem, sizeof(dst.label) - 1);
      dst.label[sizeof(dst.label) - 1] = '\0';
      strncpy(dst.fileName, e.name, sizeof(dst.fileName) - 1);
      dst.fileName[sizeof(dst.fileName) - 1] = '\0';
      dst.source = source;
    }
  }
}

static void refreshHomeApps() {
  gHomeAppCount = 0;

  // Priority 1: /sd/apps/*.lapp
  if (lakatosSdIsMounted()) {
    collectHomeAppsFromDir("/sd/apps", APP_SRC_SD);
  }

  // Priority 2: /net/apps/*.lapp
  bool wifiOk = lakatosWifiIsConnected();
  gHomeScannedWifi = wifiOk;
  if (wifiOk && strlen(lakatosGetSuperhighwayPassword()) > 0) {
    collectHomeAppsFromDir("/net/apps", APP_SRC_NET);
  }

  // Priority 3: /flash/apps/*.lapp
  collectHomeAppsFromDir("/flash/apps", APP_SRC_FLASH);

  // Sort union alphabetically by label
  for (int i = 0; i < gHomeAppCount - 1; i++) {
    for (int j = i + 1; j < gHomeAppCount; j++) {
      if (strcasecmp(gHomeApps[i].label, gHomeApps[j].label) > 0) {
        HomeAppEntry tmp = gHomeApps[i];
        gHomeApps[i] = gHomeApps[j];
        gHomeApps[j] = tmp;
      }
    }
  }
}

#define LAKATOS_HOME_TEXT_FLOW 1

#if LAKATOS_HOME_TEXT_FLOW
struct HomeAppWordLayout {
  int16_t x;
  int16_t y;
  int16_t w;
  int16_t h;
  int16_t hitX;
  int16_t hitY;
  int16_t hitW;
  int16_t hitH;
};

static int simulateHomeFlowLines(uint8_t font, uint8_t textSize, int spaceW,
                                 int xLeft, int xRight,
                                 HomeAppWordLayout* outBoxes, int count) {
  if (count <= 0) return 0;
  tft.setTextSize(textSize);
  int fontH = tft.fontHeight(font);
  int line = 0;
  int curX = xLeft;

  for (int i = 0; i < count; i++) {
    int w = tft.textWidth(gHomeApps[i].label, font);
    if (w < 8) w = 8;
    if (curX > xLeft && curX + spaceW + w > xRight) {
      line++;
      curX = xLeft;
    }
    int wx = (curX == xLeft) ? curX : (curX + spaceW);
    if (outBoxes) {
      outBoxes[i].x = (int16_t)wx;
      outBoxes[i].y = (int16_t)line;  // temporarily stores line index
      outBoxes[i].w = (int16_t)w;
      outBoxes[i].h = (int16_t)fontH;
    }
    curX = wx + w;
  }
  tft.setTextSize(1);
  return line + 1;
}

static void computeHomeFlowLayout(int count, uint8_t& outFont, uint8_t& outTextSize,
                                  HomeAppWordLayout* outBoxes) {
  const int xLeft  = 6;
  const int xRight = 314;
  const int yTop   = 26;
  const int availH = 212;  // y = 26..238

  struct Candidate {
    uint8_t font;
    uint8_t textSize;
    int     fontH;
    int     minGapY;
    int     minSpaceW;
    int     maxSpaceW;
  };
  const Candidate kCandidates[] = {
      {4, 2, 52, 6, 20, 36},
      {2, 2, 32, 4, 16, 30},
      {4, 1, 26, 4, 12, 24},
      {2, 1, 16, 4, 10, 24},
      {1, 1,  8, 3,  6, 14},
  };

  uint8_t chosenFont   = 2;
  uint8_t chosenSize   = 1;
  int     chosenSpaceW = 12;
  int     chosenFontH  = 16;

  for (size_t c = 0; c < sizeof(kCandidates) / sizeof(kCandidates[0]); c++) {
    const Candidate& cand = kCandidates[c];
    int lines = simulateHomeFlowLines(cand.font, cand.textSize, cand.minSpaceW,
                                      xLeft, xRight, nullptr, count);
    if (lines * (cand.fontH + cand.minGapY) <= availH ||
        c == (sizeof(kCandidates) / sizeof(kCandidates[0])) - 1) {
      chosenFont   = cand.font;
      chosenSize   = cand.textSize;
      chosenSpaceW = cand.minSpaceW;
      chosenFontH  = cand.fontH;

      int targetLines = availH / (cand.fontH + cand.minGapY + 5);
      if (targetLines < 1) targetLines = 1;
      for (int sw = cand.minSpaceW + 2; sw <= cand.maxSpaceW; sw += 2) {
        int testLines = simulateHomeFlowLines(cand.font, cand.textSize, sw,
                                              xLeft, xRight, nullptr, count);
        if (testLines * (cand.fontH + cand.minGapY) <= availH &&
            testLines <= targetLines) {
          chosenSpaceW = sw;
        } else {
          break;
        }
      }
      break;
    }
  }

  outFont     = chosenFont;
  outTextSize = chosenSize;

  int numLines = simulateHomeFlowLines(chosenFont, chosenSize, chosenSpaceW,
                                       xLeft, xRight, outBoxes, count);
  if (numLines <= 0 || !outBoxes) return;

  int lineStep = availH;
  int yStart   = yTop + (availH - chosenFontH) / 2;
  if (numLines >= 2) {
    int usableH = availH - 8;  // y = 30..234
    lineStep = (usableH - chosenFontH) / (numLines - 1);
    if (lineStep < chosenFontH + 2) lineStep = chosenFontH + 2;
    int totalBlockH = (numLines - 1) * lineStep + chosenFontH;
    yStart = yTop + (availH - totalBlockH) / 2;
    if (yStart < yTop + 2) yStart = yTop + 2;
  }

  for (int i = 0; i < count; i++) {
    int lineIdx = outBoxes[i].y;
    int py = yStart + lineIdx * lineStep;
    outBoxes[i].y = (int16_t)py;

    int padX = chosenSpaceW / 2;
    int padY = (lineStep - chosenFontH) / 2;
    if (padY < 1) padY = 1;
    outBoxes[i].hitX = (int16_t)(outBoxes[i].x - padX);
    outBoxes[i].hitY = (int16_t)(py - padY);
    outBoxes[i].hitW = (int16_t)(outBoxes[i].w + padX * 2);
    outBoxes[i].hitH = (int16_t)(chosenFontH + padY * 2);
  }
}
#else
static void computeHomeGrid(int count, int& outCols, int& outRows,
                            int& outX0, int& outY0, int& outCellW, int& outCellH) {
  if (count <= 0) {
    outCols = 1;
    outRows = 1;
  } else if (count <= 6) {
    outCols = 2;
  } else if (count <= 12) {
    outCols = 3;
  } else if (count <= 24) {
    outCols = 4;
  } else {
    outCols = 5;
  }
  if (count > 0) {
    outRows = (count + outCols - 1) / outCols;
  }
  outX0 = 4;
  outY0 = 26;
  outCellW = 312 / outCols;
  outCellH = 212 / outRows;
  if (outCellH > 52) outCellH = 52;
}
#endif

static bool gDirListError = false;

static void refreshDirectory() {
  gDirCount = lakatosFsLs(gCurPath, gDirEntries, kMaxDirEntries);
  if (gDirCount < 0) {
    gDirCount = 0;
    gDirListError = true;
    if (strncmp(gCurPath, "/sd", 3) == 0 && !lakatosSdIsMounted()) {
      snprintf(gStatusMsg, sizeof(gStatusMsg), "No MicroSD card detected");
    } else if (strncmp(gCurPath, "/net", 4) == 0 && !lakatosWifiIsConnected()) {
      snprintf(gStatusMsg, sizeof(gStatusMsg), "Wi-Fi offline (set in CONFIG)");
    } else if (strncmp(gCurPath, "/net", 4) == 0 && strlen(lakatosGetSuperhighwayPassword()) == 0) {
      snprintf(gStatusMsg, sizeof(gStatusMsg), "Set /net Pass in CONFIG first");
    } else {
      snprintf(gStatusMsg, sizeof(gStatusMsg), "Cannot list %s", gCurPath);
    }
  } else {
    gDirListError = false;
    // Sort directories first, then alphabetically
    for (int i = 0; i < gDirCount - 1; i++) {
      for (int j = i + 1; j < gDirCount; j++) {
        bool swapNeeded = false;
        if (gDirEntries[i].isDir != gDirEntries[j].isDir) {
          swapNeeded = !gDirEntries[i].isDir && gDirEntries[j].isDir;
        } else if (strcasecmp(gDirEntries[i].name, gDirEntries[j].name) > 0) {
          swapNeeded = true;
        }
        if (swapNeeded) {
          LakatosFsEntry tmp = gDirEntries[i];
          gDirEntries[i] = gDirEntries[j];
          gDirEntries[j] = tmp;
        }
      }
    }
    snprintf(gStatusMsg, sizeof(gStatusMsg), "%s: %d item(s) | Heap %uK",
             gCurPath, gDirCount, (unsigned)(ESP.getFreeHeap() / 1024));
  }
  gPageIdx = 0;
}

static void drawHomeHeaderBar() {
  LakatosTftGuard tftGuard;
  tft.fillRect(0, 0, 320, 23, 0x10A2);
  tft.drawFastHLine(0, 23, 320, TFT_CYAN);

  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(TFT_CYAN, 0x10A2);
  tft.drawString("YakFace", 6, 11, 2);

  char idBuf[40];
  snprintf(idBuf, sizeof(idBuf), "%s/%s", lakatosGetDeviceName(), lakatosGetSerialNumber());
  tft.setTextColor(TFT_YELLOW, 0x10A2);
  tft.drawString(idBuf, 76, 11, 1);

  char timeBuf[16];
  char clkStr[32];
  if (lakatosFormatLocalTime(timeBuf, sizeof(timeBuf), "%H:%M")) {
    snprintf(clkStr, sizeof(clkStr), "%s %s", lakatosGetAirportCode(), timeBuf);
  } else {
    snprintf(clkStr, sizeof(clkStr), "%s --:--", lakatosGetAirportCode());
  }
  int sz = lakatosGetCornerSize();
  tft.setTextDatum(MR_DATUM);
  tft.setTextColor(TFT_LIGHTGREY, 0x10A2);
  tft.drawString(clkStr, 320 - sz - 18, 11, 1);

  bool wifiOk = lakatosWifiIsConnected();
  tft.fillCircle(320 - sz - 8, 11, 4, wifiOk ? TFT_GREEN : TFT_RED);

  // Standard NE Menu Wedge at top-right (replaces old [MENU]/[Config] button)
  tft.fillRect(320 - sz, 0, sz, sz, TFT_BLACK);
  for (int y = 0; y * 2 < sz; y++) {
    tft.drawFastHLine(320 - sz + y, y, sz - 2 * y, TFT_WHITE);
  }

  gLastClockDrawMs = millis();
  gLastWifiState = wifiOk;
}

static void drawHomeDropdownMenu() {
  if (!gHomeMenuOpen) return;
  LakatosTftGuard tftGuard;
  // Dropdown panel anchored below NE Menu Wedge at top-right (x: 204..318, y: 24..190)
  tft.fillRoundRect(204, 24, 114, 166, 5, 0x10A2);
  tft.drawRoundRect(204, 24, 114, 166, 5, TFT_CYAN);

  const char* items[6] = {"Mode", "Config", "FileBrowser", "Storage", "Updates", "About"};
  for (int i = 0; i < 6; i++) {
    int iy = 27 + i * 27;
    tft.fillRoundRect(208, iy, 106, 23, 4, 0x18E3);
    tft.drawRoundRect(208, iy, 106, 23, 4, TFT_CYAN);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, 0x18E3);
    tft.drawString(items[i], 261, iy + 11, 2);
  }
}

static void drawHomeScreen() {
  LakatosTftGuard tftGuard;
  tft.fillScreen(TFT_BLACK);
  drawHomeHeaderBar();

  if (gHomeAppCount <= 0) {
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString("No apps in /{sd,net,flash}/apps", 160, 130, 2);
  } else {
#if LAKATOS_HOME_TEXT_FLOW
    HomeAppWordLayout boxes[kMaxHomeApps];
    uint8_t font = 2, textSize = 1;
    computeHomeFlowLayout(gHomeAppCount, font, textSize, boxes);

    tft.setTextSize(textSize);
    tft.setTextDatum(TL_DATUM);
    for (int i = 0; i < gHomeAppCount; i++) {
      const HomeAppEntry& app = gHomeApps[i];
      uint16_t fg = TFT_CYAN;
      if (app.source == APP_SRC_SD) {
        fg = TFT_GREEN;
      } else if (app.source == APP_SRC_NET) {
        fg = TFT_WHITE;
      }
      tft.setTextColor(fg, TFT_BLACK);
      tft.drawString(app.label, boxes[i].x, boxes[i].y, font);
    }
    tft.setTextSize(1);
#else
    int cols = 1, rows = 1, x0 = 4, y0 = 26, cellW = 312, cellH = 48;
    computeHomeGrid(gHomeAppCount, cols, rows, x0, y0, cellW, cellH);

    for (int i = 0; i < gHomeAppCount; i++) {
      int r = i / cols;
      int c = i % cols;
      int bx = x0 + c * cellW + 2;
      int by = y0 + r * cellH + 2;
      int bw = cellW - 4;
      int bh = cellH - 4;

      const HomeAppEntry& app = gHomeApps[i];
      uint16_t bg     = 0x18E3;
      uint16_t border = TFT_CYAN;
      if (app.source == APP_SRC_SD) {
        bg     = 0x1A26;
        border = TFT_GREEN;
      } else if (app.source == APP_SRC_NET) {
        bg     = 0x2084;
        border = TFT_MAGENTA;
      }

      tft.fillRoundRect(bx, by, bw, bh, 5, bg);
      tft.drawRoundRect(bx, by, bw, bh, 5, border);

      uint8_t font = 2;
      if (bh < 22 || tft.textWidth(app.label, 2) > bw - 6) {
        font = 1;
      }
      tft.setTextDatum(MC_DATUM);
      tft.setTextColor(TFT_WHITE, bg);
      tft.drawString(app.label, bx + bw / 2, by + bh / 2, font);
    }
#endif
  }

  if (gHomeMenuOpen) {
    drawHomeDropdownMenu();
  }
}

static void drawFileBrowserHeaderBar() {
  LakatosTftGuard tftGuard;
  tft.fillRect(0, 0, 320, 23, 0x10A2);
  tft.drawFastHLine(0, 23, 320, TFT_CYAN);

  // [HOME] button on top-left
  tft.fillRoundRect(2, 2, 58, 19, 4, 0x2945);
  tft.drawRoundRect(2, 2, 58, 19, 4, TFT_CYAN);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_CYAN, 0x2945);
  tft.drawString("HOME", 31, 11, 2);

  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(TFT_WHITE, 0x10A2);
  tft.drawString("FileBrowser", 66, 11, 2);

  char timeBuf[16];
  char clkStr[32];
  if (lakatosFormatLocalTime(timeBuf, sizeof(timeBuf), "%H:%M")) {
    snprintf(clkStr, sizeof(clkStr), "%s %s", lakatosGetAirportCode(), timeBuf);
  } else {
    snprintf(clkStr, sizeof(clkStr), "%s --:--", lakatosGetAirportCode());
  }
  tft.setTextDatum(MR_DATUM);
  tft.setTextColor(TFT_LIGHTGREY, 0x10A2);
  tft.drawString(clkStr, 298, 11, 1);

  bool wifiOk = lakatosWifiIsConnected();
  tft.fillCircle(308, 11, 4, wifiOk ? TFT_GREEN : TFT_RED);

  gLastClockDrawMs = millis();
  gLastWifiState = wifiOk;
}

static void drawFileBrowserScreen() {
  LakatosTftGuard tftGuard;
  tft.fillScreen(TFT_BLACK);
  drawFileBrowserHeaderBar();

  // VFS Tree Tabs (y=26..46)
  struct TabDef {
    const char* prefix;
    const char* label;
    int x;
    int w;
  };
  const TabDef tabs[3] = {
      {"/flash", "/flash",   4, 86},
      {"/sd",    "/sd Card", 94, 86},
      {"/net",   "/net",     184, 82},
  };
  for (int i = 0; i < 3; i++) {
    size_t plen = strlen(tabs[i].prefix);
    bool active = strncmp(gCurPath, tabs[i].prefix, plen) == 0 &&
                  (gCurPath[plen] == '/' || gCurPath[plen] == '\0');
    uint16_t bg = active ? TFT_DARKCYAN : 0x2104;
    uint16_t fg = active ? TFT_WHITE : TFT_LIGHTGREY;
    tft.fillRoundRect(tabs[i].x, 26, tabs[i].w, 20, 4, bg);
    tft.drawRoundRect(tabs[i].x, 26, tabs[i].w, 20, 4, active ? TFT_CYAN : TFT_DARKGREY);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(fg, bg);
    tft.drawString(tabs[i].label, tabs[i].x + tabs[i].w / 2, 36, 2);
  }

  // [.. UP] button
  tft.fillRoundRect(270, 26, 46, 20, 4, 0x2104);
  tft.drawRoundRect(270, 26, 46, 20, 4, TFT_LIGHTGREY);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_YELLOW, 0x2104);
  tft.drawString(".. UP", 293, 36, 1);

  // Path & [REFRESH] bar (y=49..67)
  tft.fillRect(0, 49, 320, 19, 0x0841);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(TFT_GREENYELLOW, 0x0841);
  tft.drawString(gCurPath, 6, 58, 2);

  tft.fillRoundRect(252, 50, 64, 17, 3, TFT_NAVY);
  tft.drawRoundRect(252, 50, 64, 17, 3, TFT_CYAN);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_CYAN, TFT_NAVY);
  tft.drawString("REFRESH", 284, 58, 1);

  // Directory Rows (y=70..210)
  int startIdx = gPageIdx * kRowsPerPage;
  bool inRemoteOrSd = (strncmp(gCurPath, "/flash", 6) != 0);

  if (gDirCount == 0) {
    tft.setTextDatum(MC_DATUM);
    if (gDirListError) {
      tft.setTextColor(TFT_RED, TFT_BLACK);
      tft.drawString("(Error listing directory)", 160, 140, 2);
    } else {
      tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
      tft.drawString("(Directory is empty)", 160, 140, 2);
    }
  } else {
    for (int row = 0; row < kRowsPerPage; row++) {
      int idx = startIdx + row;
      if (idx >= gDirCount) break;
      const LakatosFsEntry& e = gDirEntries[idx];
      int ry = 70 + row * 23;
      uint16_t rowBg = (row % 2 == 0) ? 0x1082 : 0x0841;
      tft.fillRect(4, ry, 312, 21, rowBg);

      bool isApp = !e.isDir && endsWithLapp(e.name);
      uint16_t badgeBg = e.isDir ? TFT_NAVY : (isApp ? TFT_DARKGREEN : 0x3186);
      uint16_t badgeFg = e.isDir ? TFT_YELLOW : (isApp ? TFT_GREEN : TFT_LIGHTGREY);
      const char* badgeTxt = e.isDir ? "DIR" : (isApp ? "APP" : "FILE");

      tft.fillRoundRect(7, ry + 2, 32, 17, 3, badgeBg);
      tft.setTextDatum(MC_DATUM);
      tft.setTextColor(badgeFg, badgeBg);
      tft.drawString(badgeTxt, 23, ry + 10, 1);

      tft.setTextDatum(ML_DATUM);
      tft.setTextColor(isApp ? TFT_WHITE : (e.isDir ? TFT_YELLOW : TFT_LIGHTGREY), rowBg);
      tft.drawString(e.name, 44, ry + 10, 2);

      if (!e.isDir) {
        char szBuf[24];
        if (e.size >= 1024 * 1024) {
          snprintf(szBuf, sizeof(szBuf), "%lldM", (long long)(e.size / (1024 * 1024)));
        } else if (e.size >= 10240) {
          snprintf(szBuf, sizeof(szBuf), "%lldK", (long long)(e.size / 1024));
        } else {
          snprintf(szBuf, sizeof(szBuf), "%lldB", (long long)e.size);
        }
        int rightX = (isApp && inRemoteOrSd) ? 254 : 312;
        tft.setTextDatum(MR_DATUM);
        tft.setTextColor(TFT_CYAN, rowBg);
        tft.drawString(szBuf, rightX, ry + 10, 1);
      }

      if (isApp && inRemoteOrSd) {
        tft.fillRoundRect(258, ry + 2, 54, 17, 3, TFT_PURPLE);
        tft.drawRoundRect(258, ry + 2, 54, 17, 3, TFT_WHITE);
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(TFT_WHITE, TFT_PURPLE);
        tft.drawString("->/flash", 285, ry + 10, 1);
      }
    }
  }

  // Footer Bar (y=213..239)
  tft.fillRect(0, 213, 320, 27, 0x10A2);
  tft.drawFastHLine(0, 213, 320, TFT_DARKGREY);

  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(TFT_LIGHTGREY, 0x10A2);
  tft.drawString(gStatusMsg, 6, 226, 1);

  int totalPages = (gDirCount + kRowsPerPage - 1) / kRowsPerPage;
  if (totalPages < 1) totalPages = 1;
  if (totalPages > 1) {
    tft.fillRoundRect(228, 216, 42, 21, 3, 0x2104);
    tft.drawRoundRect(228, 216, 42, 21, 3, TFT_LIGHTGREY);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, 0x2104);
    tft.drawString("<", 249, 226, 2);

    tft.fillRoundRect(274, 216, 42, 21, 3, 0x2104);
    tft.drawRoundRect(274, 216, 42, 21, 3, TFT_LIGHTGREY);
    tft.drawString(">", 295, 226, 2);
  }
}

static void showFileModal(const char* fullPath, const LakatosFsEntry& entry,
                          bool waitForTouch = true) {
  uint32_t tModalStart = millis();
  lakatosDevLogf("Open File Modal: %s (%lld B)", fullPath, (long long)entry.size);

  {
    LakatosTftGuard tftGuard;
    // Draw top-left [HOME] button
    tft.fillRoundRect(2, 2, 58, 19, 4, 0x2945);
    tft.drawRoundRect(2, 2, 58, 19, 4, TFT_CYAN);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_CYAN, 0x2945);
    tft.drawString("HOME", 31, 11, 2);

    tft.fillRoundRect(12, 28, 296, 188, 6, 0x1082);
    tft.drawRoundRect(12, 28, 296, 188, 6, TFT_CYAN);

    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_YELLOW, 0x1082);
    tft.drawString(fullPath, 160, 32, 2);

    tft.fillRect(20, 50, 280, 124, TFT_BLACK);
    tft.drawRect(20, 50, 280, 124, TFT_DARKGREY);
  }

  size_t plen = strlen(fullPath);
  bool isPngExt = (plen >= 4 && strcasecmp(fullPath + plen - 4, ".png") == 0);
  bool isJpgExt = (plen >= 4 && strcasecmp(fullPath + plen - 4, ".jpg") == 0) ||
                  (plen >= 5 && strcasecmp(fullPath + plen - 5, ".jpeg") == 0);

  bool drawnImg = false;
  if (isPngExt) {
    drawnImg = lakatosDrawPngFileCentered(fullPath, 21, 51, 278, 122);
  } else if (isJpgExt) {
    drawnImg = lakatosDrawJpgFileCentered(fullPath, 21, 51, 278, 122);
  }

  bool canCopyFlash = (strncmp(fullPath, "/flash/", 7) != 0);

  {
    LakatosTftGuard tftGuard;
    if (drawnImg) {
      tft.fillRect(12, 174, 296, 42, 0x1082);
      tft.drawRoundRect(12, 28, 296, 188, 6, TFT_CYAN);
      tft.drawRect(20, 50, 280, 124, TFT_DARKGREY);
    } else {
      char buf[257];
      memset(buf, 0, sizeof(buf));
      int32_t got = lakatosFsRead(fullPath, 0, buf, 256);
      if (got > 0) {
        tft.setTextDatum(TL_DATUM);
        tft.setTextColor(TFT_GREEN, TFT_BLACK);
        int cx = 24, cy = 60;
        for (int i = 0; i < got && cy < 164; i++) {
          char c = buf[i];
          if (c == '\r') continue;
          if (c == '\n') {
            cx = 24;
            cy += 10;
            continue;
          }
          if (c < 32 || c > 126) c = '.';
          tft.drawChar(cx, cy, c, TFT_GREEN, TFT_BLACK, 1);
          cx += 6;
          if (cx > 290) {
            cx = 24;
            cy += 10;
          }
        }
      } else {
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
        tft.drawString("(Empty or unreadable file)", 160, 115, 2);
      }
    }

    if (canCopyFlash) {
      tft.fillRoundRect(22, 182, 86, 26, 4, TFT_PURPLE);
      tft.drawRoundRect(22, 182, 86, 26, 4, TFT_WHITE);
      tft.setTextDatum(MC_DATUM);
      tft.setTextColor(TFT_WHITE, TFT_PURPLE);
      tft.drawString("COPY -> /flash", 65, 195, 1);
    }

    tft.fillRoundRect(118, 182, 84, 26, 4, TFT_MAROON);
    tft.drawRoundRect(118, 182, 84, 26, 4, TFT_WHITE);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_MAROON);
    tft.drawString("DELETE", 160, 195, 2);

    tft.fillRoundRect(212, 182, 86, 26, 4, TFT_DARKGREEN);
    tft.drawRoundRect(212, 182, 86, 26, 4, TFT_WHITE);
    tft.setTextColor(TFT_WHITE, TFT_DARKGREEN);
    tft.drawString("CLOSE", 255, 195, 2);
  }

  lakatosDevLogf("File Modal Rendered: %s in %lu ms",
                 fullPath, (unsigned long)(millis() - tModalStart));

  if (!waitForTouch) return;

  lakatosWaitTouchRelease();

  while (true) {
    updateLakatosConfigBackground();
    lakatosUpdateRgbLed();
    if (Serial.available()) {
      return;
    }
    LakatosTouchSample ts = lakatosReadTouch();
    if (ts.touched && ts.y <= 25 && ts.x <= 64) {
      lakatosDevLogf("Button: Modal HOME (%d,%d)", ts.x, ts.y);
      gScreenMode = SCREEN_HOME;
      lakatosWaitTouchRelease();
      return;
    }
    if (ts.touched && ts.y >= 178 && ts.y <= 212) {
      lakatosWaitTouchRelease();
      if (canCopyFlash && ts.x >= 20 && ts.x <= 110) {
        lakatosDevLogf("Button: Modal COPY -> /flash %s (%d,%d)", fullPath, ts.x, ts.y);
        copyFileToFlash(fullPath, entry.name);
        return;
      }
      if (ts.x >= 116 && ts.x <= 204) {
        lakatosDevLogf("Button: Modal DELETE %s (%d,%d)", fullPath, ts.x, ts.y);
        if (lakatosFsRm(fullPath)) {
          snprintf(gStatusMsg, sizeof(gStatusMsg), "Deleted %s", entry.name);
        } else {
          snprintf(gStatusMsg, sizeof(gStatusMsg), "Delete failed: %s", entry.name);
        }
        return;
      }
      if (ts.x >= 210 && ts.x <= 302) {
        lakatosDevLogf("Button: Modal CLOSE (%d,%d)", ts.x, ts.y);
        return;
      }
    }
    delay(20);
  }
}

static void refreshActiveScreen() {
  if (gScreenMode == SCREEN_HOME) {
    refreshHomeApps();
    drawHomeScreen();
  } else {
    refreshDirectory();
    drawFileBrowserScreen();
  }
}

static uint32_t gLastHomeActivityMs = 0;

static void showKernelUpgradePromptModal(const char* appPath, int64_t reqVer) {
  int64_t curVer = lakatosGetKernelVersionSec();
  {
    LakatosTftGuard tftGuard;
    tft.fillRoundRect(14, 34, 292, 174, 6, 0x0843);
    tft.drawRoundRect(14, 34, 292, 174, 6, TFT_RED);
    tft.fillRoundRect(16, 36, 288, 22, 4, TFT_MAROON);

    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_YELLOW, TFT_MAROON);
    tft.drawString("KERNEL UPGRADE REQUIRED", 160, 47, 2);

    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_WHITE, 0x0843);
    tft.drawString("This app requires a newer kernel:", 24, 66, 2);

    tft.setTextColor(TFT_CYAN, 0x0843);
    tft.drawString(appPath, 24, 86, 2);

    char buf[64];
    snprintf(buf, sizeof(buf), "Required: %lld", (long long)reqVer);
    tft.setTextColor(TFT_YELLOW, 0x0843);
    tft.drawString(buf, 24, 112, 2);

    snprintf(buf, sizeof(buf), "Running:  %lld", (long long)curVer);
    tft.setTextColor(TFT_ORANGE, 0x0843);
    tft.drawString(buf, 24, 132, 2);

    tft.setTextColor(TFT_SILVER, 0x0843);
    tft.drawString("Upgrade kernel in Home -> Updates?", 24, 152, 1);

    tft.fillRoundRect(24, 172, 142, 28, 4, TFT_DARKGREEN);
    tft.drawRoundRect(24, 172, 142, 28, 4, TFT_WHITE);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_DARKGREEN);
    tft.drawString("OPEN UPDATES", 95, 186, 2);

    tft.fillRoundRect(178, 172, 118, 28, 4, 0x3186);
    tft.drawRoundRect(178, 172, 118, 28, 4, TFT_WHITE);
    tft.setTextColor(TFT_WHITE, 0x3186);
    tft.drawString("CANCEL", 237, 186, 2);
    tft.setTextDatum(TL_DATUM);
  }

  lakatosWaitTouchRelease();
  while (true) {
    updateLakatosConfigBackground();
    lakatosUpdateRgbLed();
    if (Serial.available()) {
      return;
    }
    LakatosTouchSample ts = lakatosReadTouch();
    if (ts.touched) {
      int tx = ts.x;
      int ty = ts.y;
      lakatosWaitTouchRelease();
      if (ty >= 170 && ty <= 202) {
        if (tx >= 22 && tx <= 168) {
          lakatosDevLogf("Modal: OPEN UPDATES (%d,%d)", tx, ty);
          runLakatosUpdatesScreen();
          return;
        }
        if (tx >= 176 && tx <= 298) {
          lakatosDevLogf("Modal: CANCEL (%d,%d)", tx, ty);
          return;
        }
      }
      if (ty < 24 && tx < 24) {
        return;
      }
    }
    delay(20);
  }
}

static void launchAppAndRestoreShell(const char* appPath) {
  lakatosDevLogf("Launch App: %s", appPath);
  lakatosSetRgbLed(false, false, true, 250);
  lakatosSetAppArg("");
  lakatosSetAppTimeoutMs(0);
  int exitCode = 0;
  String errMsg;
  bool ok = lakatosRunAppFromFile(appPath, &exitCode, &errMsg);
  lakatosWaitTouchRelease();
  gLastHomeActivityMs = millis();
  if (ok) {
    lakatosDevLogf("App Exited: %s (code=%d)", appPath, exitCode);
    snprintf(gStatusMsg, sizeof(gStatusMsg), "Exited (%d): %s", exitCode, appPath);
    lakatosSetRgbLed(false, true, false, 200);
  } else {
    Serial.printf("[Lakatos] App '%s' launch failed: %s\r\n", appPath, errMsg.c_str());
    snprintf(gStatusMsg, sizeof(gStatusMsg), "Err: %s", errMsg.c_str());
    lakatosSetRgbLed(true, false, false, 400);
    if (errMsg.startsWith("NEEDS_KERNEL:")) {
      int64_t reqVer = atoll(errMsg.c_str() + 13);
      showKernelUpgradePromptModal(appPath, reqVer);
      gLastHomeActivityMs = millis();
    }
  }
  refreshActiveScreen();
}

static bool resolveAppVfsPathByName(const char* name, char* outPath, size_t outCap) {
  if (!name || !name[0] || !outPath || outCap == 0) return false;
  if (name[0] == '/') {
    LakatosFsEntry st;
    if (lakatosFsStat(name, &st) && !st.isDir) {
      strncpy(outPath, name, outCap - 1);
      outPath[outCap - 1] = '\0';
      return true;
    }
    return false;
  }

  char fileName[48];
  if (endsWithLapp(name)) {
    strncpy(fileName, name, sizeof(fileName) - 1);
    fileName[sizeof(fileName) - 1] = '\0';
  } else {
    snprintf(fileName, sizeof(fileName), "%s.lapp", name);
  }

  for (int i = 0; i < gHomeAppCount; i++) {
    if (strcasecmp(gHomeApps[i].fileName, fileName) == 0 ||
        strcasecmp(gHomeApps[i].label, name) == 0) {
      const char* dirPrefix = "/flash/apps";
      if (gHomeApps[i].source == APP_SRC_SD) {
        dirPrefix = "/sd/apps";
      } else if (gHomeApps[i].source == APP_SRC_NET) {
        dirPrefix = "/net/apps";
      }
      snprintf(outPath, outCap, "%s/%s", dirPrefix, gHomeApps[i].fileName);
      return true;
    }
  }

  const char* const dirs[3] = {"/sd/apps", "/net/apps", "/flash/apps"};
  for (int d = 0; d < 3; d++) {
    if (d == 0 && !lakatosSdIsMounted()) continue;
    if (d == 1 && (!lakatosWifiIsConnected() || strlen(lakatosGetSuperhighwayPassword()) == 0)) continue;
    snprintf(outPath, outCap, "%s/%s", dirs[d], fileName);
    LakatosFsEntry st;
    if (lakatosFsStat(outPath, &st) && !st.isDir) {
      return true;
    }
  }
  return false;
}

static void stepAmbientMode() {
  char appPath[128];
  const char* ambName = lakatosGetAmbientApp();
  if (!resolveAppVfsPathByName(ambName, appPath, sizeof(appPath))) {
    if (!resolveAppVfsPathByName("gallery", appPath, sizeof(appPath))) {
      lakatosSetActivityMode(LAKATOS_MODE_ACTIVE);
      gLastHomeActivityMs = millis();
      refreshActiveScreen();
      return;
    }
  }

  char argBuf[24];
  snprintf(argBuf, sizeof(argBuf), "%d", lakatosGetAmbientSec());
  lakatosSetAppArg(argBuf);
  lakatosSetAppTimeoutMs(0);

  int exitCode = 0;
  String errMsg;
  bool ok = lakatosRunAppFromFile(appPath, &exitCode, &errMsg);
  lakatosWaitTouchRelease();

  if (!ok || lakatosWasAppExitedByUser() || Serial.available() > 0) {
    lakatosSetActivityMode(LAKATOS_MODE_ACTIVE);
    gScreenMode   = SCREEN_HOME;
    gHomeMenuOpen = false;
    gLastHomeActivityMs = millis();
    refreshActiveScreen();
  }
}

static const char* const kDemoCandidateApps[] = {
  "bounce",
  "dendrite",
  "ising",
  "puzzle15",
  "rubik",
  "life",
  "mandelbrot",
  "dice-3d",
  "croquet",
  "wind",
  "gallery",
};
static constexpr int kNumDemoCandidateApps =
    sizeof(kDemoCandidateApps) / sizeof(kDemoCandidateApps[0]);
static int gDemoOrder[kNumDemoCandidateApps];
static int gDemoOrderPos   = 0;
static int gDemoOrderReady = 0;

static void shuffleDemoOrder() {
  for (int i = 0; i < kNumDemoCandidateApps; i++) {
    gDemoOrder[i] = i;
  }
  for (int i = kNumDemoCandidateApps - 1; i > 0; i--) {
    int j = (int)(esp_random() % (uint32_t)(i + 1));
    int tmp = gDemoOrder[i];
    gDemoOrder[i] = gDemoOrder[j];
    gDemoOrder[j] = tmp;
  }
  gDemoOrderPos   = 0;
  gDemoOrderReady = 1;
}

static void stepDemoMode() {
  if (!gDemoOrderReady || gDemoOrderPos >= kNumDemoCandidateApps) {
    shuffleDemoOrder();
  }

  char appPath[128];
  const char* chosenName = nullptr;
  for (int tries = 0; tries < kNumDemoCandidateApps; tries++) {
    if (gDemoOrderPos >= kNumDemoCandidateApps) {
      shuffleDemoOrder();
    }
    const char* cand = kDemoCandidateApps[gDemoOrder[gDemoOrderPos++]];
    if (resolveAppVfsPathByName(cand, appPath, sizeof(appPath))) {
      chosenName = cand;
      break;
    }
  }

  if (!chosenName) {
    lakatosSetActivityMode(LAKATOS_MODE_ACTIVE);
    gLastHomeActivityMs = millis();
    refreshActiveScreen();
    return;
  }

  int demoSec = lakatosGetDemoSec();
  if (demoSec < 5) demoSec = 60;

  char argBuf[24];
  if (strcasecmp(chosenName, "gallery") == 0) {
    // Show 5 random photographs over demoSec seconds (e.g. 60s / 5 = 12s per photo)
    int photoSec = demoSec / 5;
    if (photoSec < 1) photoSec = 1;
    snprintf(argBuf, sizeof(argBuf), "%d", photoSec);
  } else {
    snprintf(argBuf, sizeof(argBuf), "demo %d", demoSec);
  }

  lakatosSetAppArg(argBuf);
  lakatosSetAppTimeoutMs((uint32_t)demoSec * 1000u);

  int exitCode = 0;
  String errMsg;
  bool ok = lakatosRunAppFromFile(appPath, &exitCode, &errMsg);
  lakatosWaitTouchRelease();

  if (!ok || lakatosWasAppExitedByUser() || Serial.available() > 0) {
    lakatosSetActivityMode(LAKATOS_MODE_ACTIVE);
    gScreenMode   = SCREEN_HOME;
    gHomeMenuOpen = false;
    gLastHomeActivityMs = millis();
    refreshActiveScreen();
  }
}

// ---------------------------------------------------------------------------
// Serial FUSE Bridge (@FUSE <seq> <op> ...) for `lakat tether` (/tmp/lakatos)
// ---------------------------------------------------------------------------
static const char kB64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static String fuseBase64Encode(const uint8_t* data, size_t len) {
  if (!data || len == 0) return "-";
  String out;
  out.reserve(((len + 2) / 3) * 4 + 1);
  for (size_t i = 0; i < len; i += 3) {
    uint32_t v = ((uint32_t)data[i]) << 16;
    if (i + 1 < len) v |= ((uint32_t)data[i + 1]) << 8;
    if (i + 2 < len) v |= (uint32_t)data[i + 2];
    out += kB64Alphabet[(v >> 18) & 0x3F];
    out += kB64Alphabet[(v >> 12) & 0x3F];
    out += (i + 1 < len) ? kB64Alphabet[(v >> 6) & 0x3F] : '=';
    out += (i + 2 < len) ? kB64Alphabet[v & 0x3F] : '=';
  }
  return out;
}

static inline int fuseB64Val(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

static size_t fuseBase64Decode(const char* in, size_t inLen, uint8_t* out, size_t maxOut) {
  if (!in || inLen == 0 || (inLen == 1 && in[0] == '-')) return 0;
  size_t outLen = 0;
  uint32_t val = 0;
  int valb = -8;
  for (size_t i = 0; i < inLen; i++) {
    char c = in[i];
    if (c == '=' || c == '\r' || c == '\n' || c == ' ') continue;
    int d = fuseB64Val(c);
    if (d < 0) continue;
    val = (val << 6) | (uint32_t)d;
    valb += 6;
    if (valb >= 0) {
      if (outLen < maxOut) {
        out[outLen++] = (uint8_t)((val >> valb) & 0xFF);
      }
      valb -= 8;
    }
  }
  return outLen;
}

static String   gFuseNetWritePath;
static uint8_t* gFuseNetWriteBuf   = nullptr;
static size_t   gFuseNetWriteCap   = 0;
static size_t   gFuseNetWriteLen   = 0;
static bool     gFuseNetWriteDirty = false;
static bool     gShellNeedsRefresh = false;

static void handleFuseCommand(const String& line) {
  // Format: @FUSE <seq> <op> [args...]
  String rest = line.substring(6);
  rest.trim();
  int sp1 = rest.indexOf(' ');
  if (sp1 <= 0) return;
  String seq = rest.substring(0, sp1);
  String rem = rest.substring(sp1 + 1);
  rem.trim();

  int sp2 = rem.indexOf(' ');
  String op = (sp2 > 0) ? rem.substring(0, sp2) : rem;
  String args = (sp2 > 0) ? rem.substring(sp2 + 1) : "";
  args.trim();

  if (op == "STAT") {
    if (args == "/flash" || args == "/sd" || args == "/net") {
      Serial.printf("@@FUSE %s OK 1 4096 0\r\n", seq.c_str());
      return;
    }
    LakatosFsEntry st;
    if (lakatosFsStat(args.c_str(), &st)) {
      Serial.printf("@@FUSE %s OK %d %lld %lld\r\n",
                    seq.c_str(), st.isDir ? 1 : 0,
                    (long long)st.size, (long long)st.mtime);
    } else {
      Serial.printf("@@FUSE %s ERR ENOENT\r\n", seq.c_str());
    }
  } else if (op == "LS") {
    const char* seqStr = seq.c_str();
    int n = lakatosFsLsEach(
        args.c_str(),
        [](const LakatosFsEntry& e, void* ud) {
          const char* sq = (const char*)ud;
          Serial.printf("@@FUSE_ENT %s %d %lld %lld %s\r\n",
                        sq, e.isDir ? 1 : 0,
                        (long long)e.size, (long long)e.mtime,
                        e.name);
        },
        (void*)seqStr);
    if (n < 0) {
      Serial.printf("@@FUSE %s ERR %s\r\n", seq.c_str(), lakatosFsLastError());
      return;
    }
    Serial.printf("@@FUSE %s OK %d\r\n", seq.c_str(), n);
  } else if (op == "READ") {
    // args: <path> <offset> <length>
    int lastSp = args.lastIndexOf(' ');
    if (lastSp <= 0) {
      Serial.printf("@@FUSE %s ERR EINVAL\r\n", seq.c_str());
      return;
    }
    String beforeLen = args.substring(0, lastSp);
    beforeLen.trim();
    int prevSp = beforeLen.lastIndexOf(' ');
    if (prevSp <= 0) {
      Serial.printf("@@FUSE %s ERR EINVAL\r\n", seq.c_str());
      return;
    }
    String path = beforeLen.substring(0, prevSp);
    path.trim();
    uint32_t offset = (uint32_t)strtoul(beforeLen.substring(prevSp + 1).c_str(), nullptr, 10);
    size_t reqLen = (size_t)strtoul(args.substring(lastSp + 1).c_str(), nullptr, 10);
    if (reqLen > 1024) reqLen = 1024;
    if (reqLen == 0) {
      Serial.printf("@@FUSE %s OK 0 -\r\n", seq.c_str());
      return;
    }
    uint8_t* buf = (uint8_t*)malloc(reqLen);
    if (!buf) {
      Serial.printf("@@FUSE %s ERR ENOMEM\r\n", seq.c_str());
      return;
    }
    int32_t got = lakatosFsRead(path.c_str(), offset, buf, reqLen);
    if (got < 0) {
      free(buf);
      Serial.printf("@@FUSE %s ERR %s\r\n", seq.c_str(), lakatosFsLastError());
      return;
    }
    String b64 = fuseBase64Encode(buf, (size_t)got);
    free(buf);
    Serial.printf("@@FUSE %s OK %ld %s\r\n", seq.c_str(), (long)got, b64.c_str());
  } else if (op == "WRITE") {
    // args: <path> <mode:W|A> <offset> <b64>
    int spB64 = args.lastIndexOf(' ');
    if (spB64 <= 0) {
      Serial.printf("@@FUSE %s ERR EINVAL\r\n", seq.c_str());
      return;
    }
    String b64 = args.substring(spB64 + 1);
    String beforeB64 = args.substring(0, spB64);
    beforeB64.trim();

    int spOff = beforeB64.lastIndexOf(' ');
    if (spOff <= 0) {
      Serial.printf("@@FUSE %s ERR EINVAL\r\n", seq.c_str());
      return;
    }
    uint32_t offset = (uint32_t)strtoul(beforeB64.substring(spOff + 1).c_str(), nullptr, 10);
    String beforeOff = beforeB64.substring(0, spOff);
    beforeOff.trim();

    int spMode = beforeOff.lastIndexOf(' ');
    if (spMode <= 0) {
      Serial.printf("@@FUSE %s ERR EINVAL\r\n", seq.c_str());
      return;
    }
    String mode = beforeOff.substring(spMode + 1);
    String path = beforeOff.substring(0, spMode);
    path.trim();

    size_t maxDec = (b64.length() * 3) / 4 + 4;
    uint8_t* raw = (uint8_t*)malloc(maxDec > 0 ? maxDec : 1);
    if (!raw) {
      Serial.printf("@@FUSE %s ERR ENOMEM\r\n", seq.c_str());
      return;
    }
    size_t decLen = fuseBase64Decode(b64.c_str(), b64.length(), raw, maxDec);

    if (strncmp(path.c_str(), "/net/", 5) == 0) {
      if (mode == "W" || gFuseNetWritePath != path) {
        gFuseNetWritePath  = path;
        gFuseNetWriteLen   = 0;
        gFuseNetWriteDirty = true;
      }
      size_t needed = (size_t)offset + decLen;
      if (needed > 98304) {
        free(raw);
        Serial.printf("@@FUSE %s ERR EFBIG\r\n", seq.c_str());
        return;
      }
      if (needed > gFuseNetWriteCap) {
        size_t nextCap = (needed < 4096) ? 4096 : needed;
        uint8_t* nb = (uint8_t*)realloc(gFuseNetWriteBuf, nextCap);
        if (!nb) {
          free(raw);
          Serial.printf("@@FUSE %s ERR ENOMEM\r\n", seq.c_str());
          return;
        }
        if (nextCap > gFuseNetWriteCap) {
          memset(nb + gFuseNetWriteCap, 0, nextCap - gFuseNetWriteCap);
        }
        gFuseNetWriteBuf = nb;
        gFuseNetWriteCap = nextCap;
      }
      if (decLen > 0) {
        memcpy(gFuseNetWriteBuf + offset, raw, decLen);
      }
      if (needed > gFuseNetWriteLen) {
        gFuseNetWriteLen = needed;
      }
      gFuseNetWriteDirty = true;
      free(raw);
      Serial.printf("@@FUSE %s OK %u\r\n", seq.c_str(), (unsigned)decLen);
      return;
    }

    bool ok = false;
    if (mode == "W" && offset == 0) {
      ok = lakatosFsWrite(path.c_str(), raw, decLen);
    } else if (decLen == 0) {
      ok = true;
    } else {
      ok = lakatosFsWriteAt(path.c_str(), offset, raw, decLen);
    }
    free(raw);
    if (ok) {
      Serial.printf("@@FUSE %s OK %u\r\n", seq.c_str(), (unsigned)decLen);
    } else {
      Serial.printf("@@FUSE %s ERR %s\r\n", seq.c_str(), lakatosFsLastError());
    }
  } else if (op == "COMMIT") {
    bool ok = true;
    if (strncmp(args.c_str(), "/net/", 5) == 0 && gFuseNetWriteDirty && gFuseNetWritePath == args) {
      ok = lakatosFsWrite(args.c_str(),
                          gFuseNetWriteBuf ? gFuseNetWriteBuf : (const uint8_t*)"",
                          gFuseNetWriteLen);
      if (gFuseNetWriteBuf) {
        free(gFuseNetWriteBuf);
        gFuseNetWriteBuf = nullptr;
      }
      gFuseNetWriteCap   = 0;
      gFuseNetWriteLen   = 0;
      gFuseNetWriteDirty = false;
      gFuseNetWritePath  = "";
    }
    gShellNeedsRefresh = true;
    if (ok) {
      Serial.printf("@@FUSE %s OK\r\n", seq.c_str());
    } else {
      Serial.printf("@@FUSE %s ERR %s\r\n", seq.c_str(), lakatosFsLastError());
    }
  } else if (op == "RM") {
    bool ok = lakatosFsRm(args.c_str());
    if (ok) {
      gShellNeedsRefresh = true;
      Serial.printf("@@FUSE %s OK\r\n", seq.c_str());
    } else {
      Serial.printf("@@FUSE %s ERR %s\r\n", seq.c_str(), lakatosFsLastError());
    }
  } else if (op == "MKDIR") {
    bool ok = lakatosFsMkdir(args.c_str(), true);
    if (ok) {
      gShellNeedsRefresh = true;
      Serial.printf("@@FUSE %s OK\r\n", seq.c_str());
    } else {
      Serial.printf("@@FUSE %s ERR %s\r\n", seq.c_str(), lakatosFsLastError());
    }
  } else if (op == "RMDIR") {
    bool ok = lakatosFsRmdir(args.c_str());
    if (ok) {
      gShellNeedsRefresh = true;
      Serial.printf("@@FUSE %s OK\r\n", seq.c_str());
    } else {
      Serial.printf("@@FUSE %s ERR %s\r\n", seq.c_str(), lakatosFsLastError());
    }
  } else {
    Serial.printf("@@FUSE %s ERR ENOSYS\r\n", seq.c_str());
  }
}

static void handleSerialCommandLine(const String& line);

void lakatosPollSerialFuse() {
  while (Serial.available() > 0) {
    int pk = Serial.peek();
    if (pk == '\r' || pk == '\n') {
      Serial.read();
      continue;
    }
    if (pk == '@') {
      String line = Serial.readStringUntil('\n');
      line.trim();
      if (line.startsWith("@FUSE ")) {
        handleFuseCommand(line);
      }
      continue;
    }
    if (!gLCodeAppRunning && !lakatosIsNativeAppRunning()) {
      String line = Serial.readStringUntil('\n');
      line.trim();
      if (line.length() > 0) {
        handleSerialCommandLine(line);
      }
      continue;
    }
    break;
  }
}

static void refreshShellOrMarkDirty() {
  if (gInConfigScreen) {
    gShellNeedsRefresh = true;
  } else {
    refreshActiveScreen();
  }
}

static void handleSerialCommandLine(const String& line) {
  if (line.length() == 0) return;

  if (line.startsWith("@FUSE ")) {
    handleFuseCommand(line);
    return;
  }

  if ((line == "q" || line == "Q" || (line.length() == 1 && line[0] == 3)) && gInConfigScreen) {
    gPleaseExitApp = true;
    return;
  }

  if (line == "info") {
    const char* pw = lakatosGetSuperhighwayPassword();
    Serial.printf("Lakatos OS v%u | ID: %s/%s | Airport: %s | Corner: %dpx | URL: %s | /net PW: %s | Wi-Fi: %s | Heap: %u\r\n",
                  LAKATOS_API_VERSION,
                  lakatosGetDeviceName(),
                  lakatosGetSerialNumber(),
                  lakatosGetAirportCode(),
                  lakatosGetCornerSize(),
                  lakatosGetSuperhighwayUrl(),
                  (pw && pw[0]) ? pw : "(NOT SET)",
                  lakatosWifiIsConnected() ? "CONNECTED" : "OFFLINE",
                  (unsigned)ESP.getFreeHeap());
  } else if (line == "ls" || line.startsWith("ls ")) {
    String path = line.substring(2);
    path.trim();
    if (path.length() == 0) path = gCurPath;
    LakatosFsEntry* entries = (LakatosFsEntry*)malloc(sizeof(LakatosFsEntry) * 32);
    if (entries) {
      int n = lakatosFsLs(path.c_str(), entries, 32);
      if (n < 0 && (path == "/sd" || path.startsWith("/sd/")) && !lakatosSdIsMounted()) {
        Serial.printf("LS %s -> not mounted (no MicroSD card)\r\n", path.c_str());
      } else {
        Serial.printf("LS %s -> %d entries\r\n", path.c_str(), n);
        for (int i = 0; i < n; i++) {
          Serial.printf("  %s %-24s %lld\r\n",
                        entries[i].isDir ? "[DIR] " : "[FILE]",
                        entries[i].name,
                        (long long)entries[i].size);
        }
      }
      free(entries);
    }
  } else if (line.startsWith("run ")) {
    if (gInConfigScreen) {
      Serial.println("Exit CONFIG screen first before running an app.");
      return;
    }
    String path = line.substring(4);
    path.trim();
    launchAppAndRestoreShell(path.c_str());
  } else if (line.startsWith("cp ")) {
    String args = line.substring(3);
    args.trim();
    int sp = args.indexOf(' ');
    if (sp > 0) {
      String src = args.substring(0, sp);
      String dst = args.substring(sp + 1);
      src.trim();
      dst.trim();
      LakatosFsEntry* st = (LakatosFsEntry*)malloc(sizeof(LakatosFsEntry));
      if (st && lakatosFsStat(src.c_str(), st) && !st->isDir && st->size > 0 && st->size <= 65536) {
        uint8_t* buf = (uint8_t*)malloc((size_t)st->size);
        if (buf) {
          int32_t got = lakatosFsRead(src.c_str(), 0, buf, (size_t)st->size);
          bool ok = (got == (int32_t)st->size) && lakatosFsWrite(dst.c_str(), buf, (size_t)st->size);
          free(buf);
          Serial.printf("CP %s -> %s (%lld bytes): %s\r\n",
                        src.c_str(), dst.c_str(), (long long)st->size, ok ? "OK" : "FAILED");
        }
      } else {
        Serial.printf("CP stat failed for %s\r\n", src.c_str());
      }
      if (st) free(st);
    }
  } else if (line == "netpass" || line == "shpass" ||
             line.startsWith("netpass ") || line.startsWith("shpass ")) {
    int sp = line.indexOf(' ');
    if (sp < 0) {
      const char* cur = lakatosGetSuperhighwayPassword();
      Serial.printf("Usage: netpass <password>  (current /net password: %s)\r\n",
                    (cur && cur[0]) ? "SET" : "NOT SET");
    } else {
      String pass = line.substring(sp + 1);
      pass.trim();
      lakatosSetSuperhighwayPassword(pass.c_str());
      Serial.printf("Saved /net password to NVS (cyd_cfg) for Device ID %s/%s.\r\n",
                    lakatosGetDeviceName(), lakatosGetSerialNumber());
      refreshShellOrMarkDirty();
    }
  } else if (line.startsWith("neturl ")) {
    String url = line.substring(7);
    url.trim();
    lakatosSetSuperhighwayUrl(url.c_str());
    Serial.printf("Saved /net URL: %s\r\n", lakatosGetSuperhighwayUrl());
    refreshShellOrMarkDirty();
  } else if (line.startsWith("devname ")) {
    String nm = line.substring(8);
    nm.trim();
    lakatosSetDeviceName(nm.c_str());
    Serial.printf("Saved Device ID: %s/%s\r\n", lakatosGetDeviceName(), lakatosGetSerialNumber());
    refreshShellOrMarkDirty();
  } else if (line.startsWith("serial ")) {
    String sn = line.substring(7);
    sn.trim();
    lakatosSetSerialNumber(sn.c_str());
    Serial.printf("Saved Device ID: %s/%s\r\n", lakatosGetDeviceName(), lakatosGetSerialNumber());
    refreshShellOrMarkDirty();
  } else if (line.startsWith("airport ")) {
    String code = line.substring(8);
    code.trim();
    lakatosSetAirportCode(code.c_str());
    Serial.printf("Saved Airport: %s\r\n", lakatosGetAirportCode());
    refreshShellOrMarkDirty();
  } else if (line.startsWith("wifi ")) {
    String rest = line.substring(5);
    rest.trim();
    int sp = rest.indexOf(' ');
    String ssid = (sp > 0) ? rest.substring(0, sp) : rest;
    String pass = (sp > 0) ? rest.substring(sp + 1) : "";
    ssid.trim();
    pass.trim();
    lakatosSetWifiCredentials(ssid.c_str(), pass.c_str());
    Serial.printf("Saved Wi-Fi SSID '%s' to NVS (cyd_wifi) and connecting...\r\n", ssid.c_str());
  } else if (line.startsWith("preview ")) {
    if (gInConfigScreen) {
      Serial.println("Exit CONFIG screen first before previewing a file.");
      return;
    }
    String path = line.substring(8);
    path.trim();
    LakatosFsEntry* dummy = (LakatosFsEntry*)calloc(1, sizeof(LakatosFsEntry));
    if (dummy) {
      const char* slash = strrchr(path.c_str(), '/');
      strncpy(dummy->name, slash ? (slash + 1) : path.c_str(), sizeof(dummy->name) - 1);
      showFileModal(path.c_str(), *dummy, false);
      free(dummy);
    }
  } else if (line == "config") {
    if (!gInConfigScreen) {
      runLakatosConfigScreen(false);
      refreshActiveScreen();
    }
  } else if (line == "df") {
    LakatosStorageInfo stInfo;
    memset(&stInfo, 0, sizeof(stInfo));
    lakatosGetFlashStorageInfo(&stInfo);
    if (stInfo.flashMounted) {
      Serial.printf("Storage /flash: MOUNTED     | Free: %llu KB (%llu B) | Used: %llu KB / %llu KB\r\n",
                    (unsigned long long)(stInfo.flashFreeBytes / 1024ULL),
                    (unsigned long long)stInfo.flashFreeBytes,
                    (unsigned long long)(stInfo.flashUsedBytes / 1024ULL),
                    (unsigned long long)(stInfo.flashTotalBytes / 1024ULL));
    } else {
      Serial.println("Storage /flash: NOT MOUNTED");
    }
    if (lakatosProbeSdMounted(&stInfo, true) && lakatosGetSdStorageSpace(&stInfo)) {
      Serial.printf("Storage /sd:    MOUNTED (%s) | Free: %llu KB (%llu B) | Used: %llu KB / %llu KB\r\n",
                    stInfo.sdCardType ? stInfo.sdCardType : "SD",
                    (unsigned long long)(stInfo.sdFreeBytes / 1024ULL),
                    (unsigned long long)stInfo.sdFreeBytes,
                    (unsigned long long)(stInfo.sdUsedBytes / 1024ULL),
                    (unsigned long long)(stInfo.sdTotalBytes / 1024ULL));
    } else {
      Serial.println("Storage /sd:    NOT MOUNTED (no MicroSD card detected)");
    }
  } else if (line == "storage") {
    if (!gInConfigScreen) {
      runLakatosStorageScreen();
      refreshActiveScreen();
    }
  } else if (line == "mode" || line.startsWith("mode ")) {
    String arg = line.substring(4);
    arg.trim();
    if (arg == "ambient" || arg == "1") {
      lakatosSetActivityMode(LAKATOS_MODE_AMBIENT);
    } else if (arg == "demo" || arg == "2") {
      lakatosSetActivityMode(LAKATOS_MODE_DEMO);
    } else if (arg == "active" || arg == "3") {
      lakatosSetActivityMode(LAKATOS_MODE_ACTIVE);
      gLastHomeActivityMs = millis();
    } else if (arg == "ui" || arg == "panel") {
      if (!gInConfigScreen) {
        runLakatosModeScreen();
        gLastHomeActivityMs = millis();
        refreshActiveScreen();
      }
    }
    const char* mName = (lakatosGetActivityMode() == LAKATOS_MODE_AMBIENT) ? "Ambient"
                      : (lakatosGetActivityMode() == LAKATOS_MODE_DEMO)    ? "Demo"
                                                                           : "Active";
    Serial.printf("Activity Mode: %s | Ambient: %s (%ds/img) | Demo: %ds/run | Active Idle->Ambient: %ds\r\n",
                  mName, lakatosGetAmbientApp(), lakatosGetAmbientSec(),
                  lakatosGetDemoSec(), lakatosGetActiveIdleSec());
  } else if (line == "cornersize" || line.startsWith("cornersize ")) {
    int sp = line.indexOf(' ');
    if (sp > 0) {
      int sz = line.substring(sp + 1).toInt();
      lakatosSetCornerSize(sz);
      refreshShellOrMarkDirty();
    }
    Serial.printf("Corner Target Size: %d px (stops: 8, 12, 16, 20, 24)\r\n", lakatosGetCornerSize());
  } else if (line == "dev" || line == "spot" ||
             line.startsWith("dev ") || line.startsWith("spot ")) {
    int pfx = line.startsWith("spot") ? 4 : 3;
    String arg = line.substring(pfx);
    arg.trim();
    if (arg == "on" || arg == "1") {
      lakatosSetDeveloperMode(true);
    } else if (arg == "off" || arg == "0") {
      lakatosSetDeveloperMode(false);
    }
    Serial.printf("Developer Mode: %s\r\n", lakatosGetDeveloperMode() ? "ON" : "OFF");
  } else if (line == "help" || line == "?") {
    Serial.println("Commands: info | df | storage | ls [path] | run <path> | preview <path> | cp <src> <dst> | netpass <pw> | neturl <url> | devname <name> | serial <sn> | airport <iata> | wifi <ssid> [pw] | config | dev [on|off]");
  } else {
    Serial.printf("Unknown command '%s'. Type 'help' for commands.\r\n", line.c_str());
  }
}

static void handleSerialCommand() {
  lakatosPollSerialFuse();
}

void setup() {
  esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);
  Serial.setRxBufferSize(1024);
  Serial.begin(115200);
  delay(80);
  Serial.println("\r\n================================================");
  Serial.println("  Lakatos OS & LCode 32-Bit Bytecode Runtime");
  Serial.println("================================================");

  pinMode(LED_R_PIN, OUTPUT);
  pinMode(LED_G_PIN, OUTPUT);
  pinMode(LED_B_PIN, OUTPUT);
  lakatosSetRgbLed(false, false, false, 0);

  initTouchPins();
  gTouchMutex = xSemaphoreCreateMutex();
  gTftMutex   = xSemaphoreCreateRecursiveMutex();

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);

  initLakatosConfig();
  initLakatosVfs();
  initLakatosLoader();

  loadTouchCalibrationFromNvs();
  xTaskCreatePinnedToCore(touchScannerTaskCore0, "lak_touch", 2560, nullptr, 1, nullptr, 0);

  if (!loadTouchCalibrationFromNvs()) {
    runLakatosConfigScreen(true);
  }

  gScreenMode   = SCREEN_HOME;
  gHomeMenuOpen = false;
  gLastHomeActivityMs = millis();
  refreshHomeApps();
  drawHomeScreen();
}

void loop() {
  updateLakatosConfigBackground();
  lakatosUpdateRgbLed();
  handleSerialCommand();

  if (lakatosGetActivityMode() == LAKATOS_MODE_AMBIENT) {
    stepAmbientMode();
    return;
  }
  if (lakatosGetActivityMode() == LAKATOS_MODE_DEMO) {
    stepDemoMode();
    return;
  }

  bool wifiNow = lakatosWifiIsConnected();
  if (gScreenMode == SCREEN_HOME && wifiNow && !gHomeScannedWifi) {
    gShellNeedsRefresh = true;
  }

  if (gShellNeedsRefresh) {
    gShellNeedsRefresh = false;
    refreshActiveScreen();
  }

  // Periodically update clock and Wi-Fi indicator in top header bar
  if ((uint32_t)(millis() - gLastClockDrawMs) > 15000 || wifiNow != gLastWifiState) {
    if (gScreenMode == SCREEN_HOME) {
      drawHomeHeaderBar();
      if (gHomeMenuOpen) drawHomeDropdownMenu();
    } else {
      drawFileBrowserHeaderBar();
    }
  }

  LakatosTouchSample ts = lakatosReadTouchLatched();
  if (!ts.touched) {
    int idleSec = lakatosGetActiveIdleSec();
    if (gScreenMode == SCREEN_HOME && !gHomeMenuOpen && idleSec > 0 &&
        (uint32_t)(millis() - gLastHomeActivityMs) >= (uint32_t)idleSec * 1000u) {
      lakatosDevLogf("Active idle (%ds) -> switching to Ambient mode", idleSec);
      lakatosSetActivityMode(LAKATOS_MODE_AMBIENT);
      return;
    }
    delay(15);
    return;
  }

  gLastHomeActivityMs = millis();
  int tx = ts.x;
  int ty = ts.y;
  lakatosWaitTouchRelease();
  gLastHomeActivityMs = millis();

  // =========================================================================
  // Mode A: Home Screen (App Launcher Grid + [MENU] Dropdown)
  // =========================================================================
  if (gScreenMode == SCREEN_HOME) {
    if (gHomeMenuOpen) {
      // Check if user tapped one of the 6 dropdown items (x: 204..318, y: 26..189)
      if (tx >= 204 && tx <= 318 && ty >= 26 && ty <= 189) {
        int itemIdx = (ty - 27) / 27;
        gHomeMenuOpen = false;
        if (itemIdx == 0) {
          lakatosDevLogf("Menu: Mode (%d,%d)", tx, ty);
          runLakatosModeScreen();
          gLastHomeActivityMs = millis();
          if (lakatosGetActivityMode() == LAKATOS_MODE_ACTIVE) {
            refreshHomeApps();
            drawHomeScreen();
          }
          return;
        } else if (itemIdx == 1) {
          lakatosDevLogf("Menu: Config (%d,%d)", tx, ty);
          runLakatosConfigScreen(false);
          gLastHomeActivityMs = millis();
          refreshHomeApps();
          drawHomeScreen();
          return;
        } else if (itemIdx == 2) {
          lakatosDevLogf("Menu: FileBrowser (%d,%d)", tx, ty);
          gScreenMode = SCREEN_FILE_BROWSER;
          refreshDirectory();
          drawFileBrowserScreen();
          return;
        } else if (itemIdx == 3) {
          lakatosDevLogf("Menu: Storage (%d,%d)", tx, ty);
          runLakatosStorageScreen();
          gLastHomeActivityMs = millis();
          refreshHomeApps();
          drawHomeScreen();
          return;
        } else if (itemIdx == 4) {
          lakatosDevLogf("Menu: Updates (%d,%d)", tx, ty);
          runLakatosUpdatesScreen();
          gLastHomeActivityMs = millis();
          refreshHomeApps();
          drawHomeScreen();
          return;
        } else if (itemIdx == 5) {
          lakatosDevLogf("Menu: About (%d,%d)", tx, ty);
          runLakatosAboutScreen();
          gLastHomeActivityMs = millis();
          refreshHomeApps();
          drawHomeScreen();
          return;
        }
      }
      // Tapped NE Menu Wedge or outside the dropdown -> close menu
      gHomeMenuOpen = false;
      drawHomeScreen();
      return;
    }

    // 1. Top-right NE Menu Wedge
    int cornerSz = lakatosGetCornerSize();
    if (ty < cornerSz && tx >= 320 - cornerSz) {
      lakatosDevLogf("Button: MENU (%d,%d)", tx, ty);
      gHomeMenuOpen = true;
      drawHomeHeaderBar();
      drawHomeDropdownMenu();
      return;
    }

    // 2. App Grid Tiles / Text Links (y >= 26)
    if (ty >= 26 && gHomeAppCount > 0) {
#if LAKATOS_HOME_TEXT_FLOW
      HomeAppWordLayout boxes[kMaxHomeApps];
      uint8_t font = 2, textSize = 1;
      computeHomeFlowLayout(gHomeAppCount, font, textSize, boxes);
      int matchedIdx = -1;
      for (int i = 0; i < gHomeAppCount; i++) {
        if (tx >= boxes[i].hitX && tx < boxes[i].hitX + boxes[i].hitW &&
            ty >= boxes[i].hitY && ty < boxes[i].hitY + boxes[i].hitH) {
          matchedIdx = i;
          break;
        }
      }
      if (matchedIdx >= 0) {
        const HomeAppEntry& app = gHomeApps[matchedIdx];
        const char* dirPrefix = "/flash/apps";
        if (app.source == APP_SRC_SD) {
          dirPrefix = "/sd/apps";
        } else if (app.source == APP_SRC_NET) {
          dirPrefix = "/net/apps";
        }
        char fullPath[128];
        snprintf(fullPath, sizeof(fullPath), "%s/%s", dirPrefix, app.fileName);
        lakatosDevLogf("Home Launch: %s (%d,%d)", fullPath, tx, ty);
        launchAppAndRestoreShell(fullPath);
        return;
      }
#else
      int cols = 1, rows = 1, x0 = 4, y0 = 26, cellW = 312, cellH = 48;
      computeHomeGrid(gHomeAppCount, cols, rows, x0, y0, cellW, cellH);
      int col = (tx - x0) / cellW;
      int row = (ty - y0) / cellH;
      if (tx >= x0 && col >= 0 && col < cols && row >= 0 && row < rows) {
        int idx = row * cols + col;
        if (idx >= 0 && idx < gHomeAppCount) {
          const HomeAppEntry& app = gHomeApps[idx];
          const char* dirPrefix = "/flash/apps";
          if (app.source == APP_SRC_SD) {
            dirPrefix = "/sd/apps";
          } else if (app.source == APP_SRC_NET) {
            dirPrefix = "/net/apps";
          }
          char fullPath[128];
          snprintf(fullPath, sizeof(fullPath), "%s/%s", dirPrefix, app.fileName);
          lakatosDevLogf("Home Launch: %s (%d,%d)", fullPath, tx, ty);
          launchAppAndRestoreShell(fullPath);
          return;
        }
      }
#endif
    }
    return;
  }

  // =========================================================================
  // Mode B: FileBrowser Screen
  // =========================================================================
  // 1. Header [HOME] button on top-left
  if (ty < 24 && tx <= 62) {
    lakatosDevLogf("Button: FileBrowser -> HOME (%d,%d)", tx, ty);
    gScreenMode   = SCREEN_HOME;
    gHomeMenuOpen = false;
    refreshHomeApps();
    drawHomeScreen();
    return;
  }

  // 2. VFS Tree Tabs (/flash, /sd, /net) and [.. UP]
  if (ty >= 24 && ty <= 48) {
    if (tx >= 4 && tx <= 90) {
      lakatosDevLogf("Button: Tab /flash (%d,%d)", tx, ty);
      strncpy(gCurPath, "/flash", sizeof(gCurPath) - 1);
      refreshDirectory();
      drawFileBrowserScreen();
    } else if (tx >= 92 && tx <= 180) {
      lakatosDevLogf("Button: Tab /sd (%d,%d)", tx, ty);
      strncpy(gCurPath, "/sd", sizeof(gCurPath) - 1);
      refreshDirectory();
      drawFileBrowserScreen();
    } else if (tx >= 182 && tx <= 266) {
      lakatosDevLogf("Button: Tab /net (%d,%d)", tx, ty);
      strncpy(gCurPath, "/net", sizeof(gCurPath) - 1);
      refreshDirectory();
      drawFileBrowserScreen();
    } else if (tx >= 268) {
      lakatosDevLogf("Button: .. UP from %s (%d,%d)", gCurPath, tx, ty);
      navigateUp();
      refreshDirectory();
      drawFileBrowserScreen();
    }
    return;
  }

  // 3. [REFRESH] button
  if (ty >= 49 && ty <= 68 && tx >= 248) {
    lakatosDevLogf("Button: REFRESH %s (%d,%d)", gCurPath, tx, ty);
    refreshDirectory();
    drawFileBrowserScreen();
    return;
  }

  // 4. Directory listing rows
  if (ty >= 70 && ty < 208) {
    int row = (ty - 70) / 23;
    int idx = gPageIdx * kRowsPerPage + row;
    if (row >= 0 && row < kRowsPerPage && idx < gDirCount) {
      const LakatosFsEntry entry = gDirEntries[idx];
      char fullPath[160];
      joinVfsPath(gCurPath, entry.name, fullPath, sizeof(fullPath));

      bool inRemoteOrSd = (strncmp(gCurPath, "/flash", 6) != 0);
      bool isApp = !entry.isDir && endsWithLapp(entry.name);

      if (isApp && inRemoteOrSd && tx >= 256) {
        lakatosDevLogf("Button: Row [->/flash] %s (%d,%d)", fullPath, tx, ty);
        copyFileToFlash(fullPath, entry.name);
        drawFileBrowserScreen();
        return;
      }

      if (entry.isDir) {
        lakatosDevLogf("Button: Open Dir %s (%d,%d)", fullPath, tx, ty);
        strncpy(gCurPath, fullPath, sizeof(gCurPath) - 1);
        gCurPath[sizeof(gCurPath) - 1] = '\0';
        refreshDirectory();
        drawFileBrowserScreen();
      } else if (isApp) {
        lakatosDevLogf("Button: Row Launch %s (%d,%d)", fullPath, tx, ty);
        launchAppAndRestoreShell(fullPath);
      } else {
        lakatosDevLogf("Button: Row Open File %s (%d,%d)", fullPath, tx, ty);
        showFileModal(fullPath, entry);
        refreshActiveScreen();
      }
    }
    return;
  }

  // 5. Footer pagination buttons
  if (ty >= 213) {
    int totalPages = (gDirCount + kRowsPerPage - 1) / kRowsPerPage;
    if (totalPages > 1) {
      if (tx >= 224 && tx <= 270) {
        gPageIdx = (gPageIdx + totalPages - 1) % totalPages;
        lakatosDevLogf("Button: Page PREV -> %d/%d (%d,%d)", gPageIdx + 1, totalPages, tx, ty);
        drawFileBrowserScreen();
      } else if (tx >= 272 && tx <= 318) {
        gPageIdx = (gPageIdx + 1) % totalPages;
        lakatosDevLogf("Button: Page NEXT -> %d/%d (%d,%d)", gPageIdx + 1, totalPages, tx, ty);
        drawFileBrowserScreen();
      }
    }
  }
}
