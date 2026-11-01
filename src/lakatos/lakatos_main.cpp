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
static volatile bool gHasLatchedTouch = false;
static uint32_t gLatchedTouchMs = 0;
static LakatosTouchSample gLatchedTouch = {false, 0, 0, 0, 0, 0};

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

LakatosTouchSample lakatosReadTouch() {
  if (digitalRead(XPT2046_IRQ) == HIGH) {
    return {false, 0, 0, 0, 0, 0};
  }
  if (gTouchMutex) xSemaphoreTake(gTouchMutex, portMAX_DELAY);
  LakatosTouchSample out = readTouchHardwareUnlocked();
  if (gTouchMutex) xSemaphoreGive(gTouchMutex);
  return out;
}

void lakatosPollTouchLatch() {
  // Fast non-blocking check of active-low PENIRQ GPIO 36 (~150 ns when untouched)
  if (digitalRead(XPT2046_IRQ) == HIGH) {
    return;
  }
  if (gTouchMutex) {
    if (xSemaphoreTake(gTouchMutex, 0) != pdTRUE) return;
  }
  LakatosTouchSample s = readTouchHardwareUnlocked();
  if (s.touched) {
    gLatchedTouch = s;
    gLatchedTouchMs = millis();
    gHasLatchedTouch = true;
  }
  if (gTouchMutex) xSemaphoreGive(gTouchMutex);
}

void lakatosClearTouchLatch() {
  if (gTouchMutex) xSemaphoreTake(gTouchMutex, portMAX_DELAY);
  gHasLatchedTouch = false;
  if (gTouchMutex) xSemaphoreGive(gTouchMutex);
}

LakatosTouchSample lakatosReadTouchLatched() {
  LakatosTouchSample live = lakatosReadTouch();
  if (live.touched) {
    gHasLatchedTouch = false;
    return live;
  }
  if (gHasLatchedTouch) {
    if (gTouchMutex) xSemaphoreTake(gTouchMutex, portMAX_DELAY);
    bool valid = gHasLatchedTouch && ((uint32_t)(millis() - gLatchedTouchMs) <= 1000u);
    LakatosTouchSample latched = gLatchedTouch;
    gHasLatchedTouch = false;
    if (gTouchMutex) xSemaphoreGive(gTouchMutex);
    if (valid) {
      return latched;
    }
  }
  return live;
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
// Lakatos Shell & File Browser State
// ---------------------------------------------------------------------------
static constexpr int kMaxDirEntries = 48;
static constexpr int kRowsPerPage = 6;

static LakatosFsEntry* const gDirEntries = (LakatosFsEntry*)gLakatosAppRamArena;
static int gDirCount = 0;
static int gPageIdx = 0;
static char gCurPath[128] = "/lo";
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
  // Don't go above /lo, /sd, or /sh roots
  if (strcmp(gCurPath, "/lo") == 0 || strcmp(gCurPath, "/sd") == 0 ||
      strcmp(gCurPath, "/sh") == 0) {
    return;
  }
  char* lastSlash = strrchr(gCurPath, '/');
  if (!lastSlash || lastSlash == gCurPath) return;
  *lastSlash = '\0';
}

static bool copyFileToLo(const char* srcPath, const char* baseName) {
  char dstPath[128];
  snprintf(dstPath, sizeof(dstPath), "/lo/%s", baseName);

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

static bool gDirListError = false;

static void refreshDirectory() {
  gDirCount = lakatosFsLs(gCurPath, gDirEntries, kMaxDirEntries);
  if (gDirCount < 0) {
    gDirCount = 0;
    gDirListError = true;
    if (strncmp(gCurPath, "/sd", 3) == 0 && !lakatosSdIsMounted()) {
      snprintf(gStatusMsg, sizeof(gStatusMsg), "No MicroSD card detected");
    } else if (strncmp(gCurPath, "/sh", 3) == 0 && !lakatosWifiIsConnected()) {
      snprintf(gStatusMsg, sizeof(gStatusMsg), "Wi-Fi offline (set in CONFIG)");
    } else if (strncmp(gCurPath, "/sh", 3) == 0 && strlen(lakatosGetSuperhighwayPassword()) == 0) {
      snprintf(gStatusMsg, sizeof(gStatusMsg), "Set /sh Pass in CONFIG first");
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

static void drawHeaderBar() {
  tft.fillRect(0, 0, 320, 23, 0x10A2);
  tft.drawFastHLine(0, 23, 320, TFT_CYAN);

  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(TFT_CYAN, 0x10A2);
  tft.drawString("YAK-CYD", 6, 11, 2);

  char idBuf[40];
  snprintf(idBuf, sizeof(idBuf), "%s%s", lakatosGetDeviceName(), lakatosGetSerialNumber());
  tft.setTextColor(TFT_YELLOW, 0x10A2);
  tft.drawString(idBuf, 72, 11, 1);

  char timeBuf[16];
  char clkStr[32];
  if (lakatosFormatLocalTime(timeBuf, sizeof(timeBuf), "%H:%M")) {
    snprintf(clkStr, sizeof(clkStr), "%s %s", lakatosGetAirportCode(), timeBuf);
  } else {
    snprintf(clkStr, sizeof(clkStr), "%s --:--", lakatosGetAirportCode());
  }
  tft.setTextDatum(MR_DATUM);
  tft.setTextColor(TFT_LIGHTGREY, 0x10A2);
  tft.drawString(clkStr, 236, 11, 1);

  bool wifiOk = lakatosWifiIsConnected();
  tft.fillCircle(246, 11, 4, wifiOk ? TFT_GREEN : TFT_RED);

  // [CONFIG] button
  tft.fillRoundRect(256, 2, 60, 19, 4, TFT_MAROON);
  tft.drawRoundRect(256, 2, 60, 19, 4, TFT_WHITE);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_MAROON);
  tft.drawString("CONFIG", 286, 11, 1);

  gLastClockDrawMs = millis();
  gLastWifiState = wifiOk;
}

static void drawShellScreen() {
  tft.fillScreen(TFT_BLACK);
  drawHeaderBar();

  // VFS Tree Tabs (y=26..46)
  struct TabDef {
    const char* prefix;
    const char* label;
    int x;
    int w;
  };
  const TabDef tabs[3] = {
      {"/lo", "/lo Flash", 4, 86},
      {"/sd", "/sd Card",  94, 86},
      {"/sh", "/sh Net",   184, 82},
  };
  for (int i = 0; i < 3; i++) {
    bool active = strncmp(gCurPath, tabs[i].prefix, 3) == 0;
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
  bool inRemoteOrSd = (strncmp(gCurPath, "/lo", 3) != 0);

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
        int rightX = (isApp && inRemoteOrSd) ? 260 : 312;
        tft.setTextDatum(MR_DATUM);
        tft.setTextColor(TFT_CYAN, rowBg);
        tft.drawString(szBuf, rightX, ry + 10, 1);
      }

      if (isApp && inRemoteOrSd) {
        tft.fillRoundRect(266, ry + 2, 46, 17, 3, TFT_PURPLE);
        tft.drawRoundRect(266, ry + 2, 46, 17, 3, TFT_WHITE);
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(TFT_WHITE, TFT_PURPLE);
        tft.drawString("->/lo", 289, ry + 10, 1);
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
  Serial.printf("[TIMING] showFileModal START %s (size=%lld)\r\n",
                fullPath, (long long)entry.size);

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

  bool canCopyLo = (strncmp(fullPath, "/lo/", 4) != 0);
  if (canCopyLo) {
    tft.fillRoundRect(22, 182, 86, 26, 4, TFT_PURPLE);
    tft.drawRoundRect(22, 182, 86, 26, 4, TFT_WHITE);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_PURPLE);
    tft.drawString("COPY -> /lo", 65, 195, 1);
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

  Serial.printf("[TIMING] showFileModal DONE %s total=%lu ms\r\n",
                fullPath, (unsigned long)(millis() - tModalStart));

  if (!waitForTouch) return;

  while (lakatosReadTouch().touched) delay(15);

  while (true) {
    updateLakatosConfigBackground();
    lakatosUpdateRgbLed();
    if (Serial.available()) {
      return;
    }
    LakatosTouchSample ts = lakatosReadTouch();
    if (ts.touched && ts.y <= 25 && ts.x <= 64) {
      while (lakatosReadTouch().touched) delay(15);
      return;
    }
    if (ts.touched && ts.y >= 178 && ts.y <= 212) {
      while (lakatosReadTouch().touched) delay(15);
      if (canCopyLo && ts.x >= 20 && ts.x <= 110) {
        copyFileToLo(fullPath, entry.name);
        return;
      }
      if (ts.x >= 116 && ts.x <= 204) {
        if (lakatosFsRm(fullPath)) {
          snprintf(gStatusMsg, sizeof(gStatusMsg), "Deleted %s", entry.name);
        } else {
          snprintf(gStatusMsg, sizeof(gStatusMsg), "Delete failed: %s", entry.name);
        }
        return;
      }
      if (ts.x >= 210 && ts.x <= 302) {
        return;
      }
    }
    delay(20);
  }
}

static void launchAppAndRestoreShell(const char* appPath) {
  lakatosSetRgbLed(false, false, true, 250);
  int exitCode = 0;
  String errMsg;
  bool ok = lakatosRunAppFromFile(appPath, &exitCode, &errMsg);
  while (lakatosReadTouch().touched) delay(15);
  if (ok) {
    snprintf(gStatusMsg, sizeof(gStatusMsg), "Exited (%d): %s", exitCode, appPath);
    lakatosSetRgbLed(false, true, false, 200);
  } else {
    Serial.printf("[Lakatos] App '%s' launch failed: %s\r\n", appPath, errMsg.c_str());
    snprintf(gStatusMsg, sizeof(gStatusMsg), "Err: %s", errMsg.c_str());
    lakatosSetRgbLed(true, false, false, 400);
  }
  refreshDirectory();
  drawShellScreen();
}

static void handleSerialCommand() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.length() == 0) return;

  if (line == "info") {
    Serial.printf("Lakatos OS v%u | ID: %s%s | Airport: %s | URL: %s | Wi-Fi: %s | Heap: %u\r\n",
                  LAKATOS_API_VERSION,
                  lakatosGetDeviceName(),
                  lakatosGetSerialNumber(),
                  lakatosGetAirportCode(),
                  lakatosGetSuperhighwayUrl(),
                  lakatosWifiIsConnected() ? "CONNECTED" : "OFFLINE",
                  (unsigned)ESP.getFreeHeap());
  } else if (line.startsWith("ls")) {
    String path = line.substring(2);
    path.trim();
    if (path.length() == 0) path = gCurPath;
    LakatosFsEntry entries[32];
    int n = lakatosFsLs(path.c_str(), entries, 32);
    Serial.printf("LS %s -> %d entries\r\n", path.c_str(), n);
    for (int i = 0; i < n; i++) {
      Serial.printf("  %s %-24s %lld\r\n",
                    entries[i].isDir ? "[DIR] " : "[FILE]",
                    entries[i].name,
                    (long long)entries[i].size);
    }
  } else if (line.startsWith("run ")) {
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
      LakatosFsEntry st;
      if (lakatosFsStat(src.c_str(), &st) && !st.isDir && st.size > 0 && st.size <= 65536) {
        uint8_t* buf = (uint8_t*)malloc((size_t)st.size);
        if (buf) {
          int32_t got = lakatosFsRead(src.c_str(), 0, buf, (size_t)st.size);
          bool ok = (got == (int32_t)st.size) && lakatosFsWrite(dst.c_str(), buf, (size_t)st.size);
          free(buf);
          Serial.printf("CP %s -> %s (%lld bytes): %s\r\n",
                        src.c_str(), dst.c_str(), (long long)st.size, ok ? "OK" : "FAILED");
        }
      } else {
        Serial.printf("CP stat failed for %s\r\n", src.c_str());
      }
    }
  } else if (line.startsWith("shpass ")) {
    String pass = line.substring(7);
    pass.trim();
    lakatosSetSuperhighwayPassword(pass.c_str());
    Serial.println("Saved /sh password to NVS (cyd_cfg).");
    refreshDirectory();
    drawShellScreen();
  } else if (line.startsWith("preview ")) {
    String path = line.substring(8);
    path.trim();
    LakatosFsEntry dummy;
    memset(&dummy, 0, sizeof(dummy));
    const char* slash = strrchr(path.c_str(), '/');
    strncpy(dummy.name, slash ? (slash + 1) : path.c_str(), sizeof(dummy.name) - 1);
    showFileModal(path.c_str(), dummy, false);
  } else if (line == "config") {
    runLakatosConfigScreen(false);
    refreshDirectory();
    drawShellScreen();
  }
}

void setup() {
  esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);
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
  xTaskCreatePinnedToCore(touchScannerTaskCore0, "lak_touch", 2048, nullptr, 1, nullptr, 0);

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);

  initLakatosConfig();
  initLakatosVfs();
  initLakatosLoader();

  if (!loadTouchCalibrationFromNvs()) {
    runLakatosConfigScreen(true);
  }

  refreshDirectory();
  drawShellScreen();
}

void loop() {
  updateLakatosConfigBackground();
  lakatosUpdateRgbLed();
  handleSerialCommand();

  // Periodically update clock and Wi-Fi indicator in top header bar
  bool wifiNow = lakatosWifiIsConnected();
  if ((uint32_t)(millis() - gLastClockDrawMs) > 15000 || wifiNow != gLastWifiState) {
    drawHeaderBar();
  }

  LakatosTouchSample ts = lakatosReadTouch();
  if (!ts.touched) {
    delay(15);
    return;
  }

  int tx = ts.x;
  int ty = ts.y;
  while (lakatosReadTouch().touched) delay(15);

  // 1. Header [CONFIG] button
  if (ty < 24 && tx >= 250) {
    runLakatosConfigScreen(false);
    refreshDirectory();
    drawShellScreen();
    return;
  }

  // 2. VFS Tree Tabs (/lo, /sd, /sh) and [.. UP]
  if (ty >= 24 && ty <= 48) {
    if (tx >= 4 && tx <= 90) {
      strncpy(gCurPath, "/lo", sizeof(gCurPath) - 1);
      refreshDirectory();
      drawShellScreen();
    } else if (tx >= 92 && tx <= 180) {
      strncpy(gCurPath, "/sd", sizeof(gCurPath) - 1);
      refreshDirectory();
      drawShellScreen();
    } else if (tx >= 182 && tx <= 266) {
      strncpy(gCurPath, "/sh", sizeof(gCurPath) - 1);
      refreshDirectory();
      drawShellScreen();
    } else if (tx >= 268) {
      navigateUp();
      refreshDirectory();
      drawShellScreen();
    }
    return;
  }

  // 3. [REFRESH] button
  if (ty >= 49 && ty <= 68 && tx >= 248) {
    refreshDirectory();
    drawShellScreen();
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

      bool inRemoteOrSd = (strncmp(gCurPath, "/lo", 3) != 0);
      bool isApp = !entry.isDir && endsWithLapp(entry.name);

      if (isApp && inRemoteOrSd && tx >= 264) {
        copyFileToLo(fullPath, entry.name);
        drawShellScreen();
        return;
      }

      if (entry.isDir) {
        strncpy(gCurPath, fullPath, sizeof(gCurPath) - 1);
        gCurPath[sizeof(gCurPath) - 1] = '\0';
        refreshDirectory();
        drawShellScreen();
      } else if (isApp) {
        launchAppAndRestoreShell(fullPath);
      } else {
        showFileModal(fullPath, entry);
        refreshDirectory();
        drawShellScreen();
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
        drawShellScreen();
      } else if (tx >= 272 && tx <= 318) {
        gPageIdx = (gPageIdx + 1) % totalPages;
        drawShellScreen();
      }
    }
  }
}
