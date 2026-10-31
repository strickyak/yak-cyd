#include <Arduino.h>
#include <Preferences.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <esp_partition.h>
#include <math.h>
#include <string.h>

#include "gep9_bus.h"
#include "gep9_cpu.h"
#include "os9_assets.h"

// ---------------------------------------------------------------------------
// Hardware Pin Definitions for ESP32-2432S028R (CYD)
// ---------------------------------------------------------------------------
#define XPT2046_IRQ  36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK  25
#define XPT2046_CS   33

#define LED_R_PIN    4
#define LED_G_PIN    16
#define LED_B_PIN    17
#define LDR_PIN      34
#define TFT_BL_PIN   21

static constexpr int SCREEN_W = 320;
static constexpr int SCREEN_H = 240;

// ---------------------------------------------------------------------------
// Peripherals & Emulator Instances
// ---------------------------------------------------------------------------
static TFT_eSPI tft = TFT_eSPI();
static XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);
static Preferences prefs;

static Deep65280Bus gBus;
static Cpu6809 gCpu(&gBus);
static const esp_partition_t* gDiskPart = nullptr;
static uint8_t gFlashEraseBuf[4096];

// ---------------------------------------------------------------------------
// Backlight / LDR Auto-Brightness (reads saved settings from NVS "cyd_cfg")
// ---------------------------------------------------------------------------
static bool gAutoBright = true;
static int gManualBrightPct = 85;
static int gLastAppliedPwm = -1;
static float gSmoothedLdr = 1000.0f;
static uint32_t gLastLdrMs = 0;

static void applyBacklightPercent(int pct) {
  pct = constrain(pct, 5, 100);
  float norm = (float)pct / 100.0f;
  int pwm = (int)lroundf(12.0f + 243.0f * powf(norm, 1.8f));
  pwm = constrain(pwm, 12, 255);
  if (pwm != gLastAppliedPwm) {
    gLastAppliedPwm = pwm;
    analogWrite(TFT_BL_PIN, pwm);
  }
}

static int computeAutoBrightPercent(float ldr) {
  float clamped = constrain(ldr, 200.0f, 3500.0f);
  float t = (clamped - 200.0f) / (3500.0f - 200.0f);
  int pct = (int)lroundf(100.0f - t * 88.0f);
  return constrain(pct, 12, 100);
}

static void initBacklightFromNvs() {
  pinMode(LDR_PIN, INPUT);
  pinMode(TFT_BL_PIN, OUTPUT);

  prefs.begin("cyd_cfg", true);
  gAutoBright = prefs.getBool("autoB", true);
  gManualBrightPct = constrain((int)prefs.getInt("manB", 85), 5, 100);
  prefs.end();

  int raw = analogRead(LDR_PIN);
  gSmoothedLdr = (float)raw;
  if (gAutoBright) {
    applyBacklightPercent(computeAutoBrightPercent(gSmoothedLdr));
  } else {
    applyBacklightPercent(gManualBrightPct);
  }
}

static void updateBrightnessLoop() {
  uint32_t now = millis();
  if (now - gLastLdrMs < 100) return;
  gLastLdrMs = now;
  int raw = analogRead(LDR_PIN);
  gSmoothedLdr = 0.90f * gSmoothedLdr + 0.10f * (float)raw;
  if (gAutoBright) {
    applyBacklightPercent(computeAutoBrightPercent(gSmoothedLdr));
  }
}

// ---------------------------------------------------------------------------
// Touch Calibration (reads saved calibration from NVS "cyd_cal")
// ---------------------------------------------------------------------------
struct TouchCalibration {
  bool swapXY = false;
  float uAtX1 = 450.0f;
  float uAtX2 = 3550.0f;
  float vAtY1 = 550.0f;
  float vAtY2 = 3450.0f;
  int calX1 = 40;
  int calX2 = 280;
  int calY1 = 50;
  int calY2 = 200;
} gCal;

struct TouchSample {
  bool touched;
  int x;
  int y;
};

static void loadCalibrationFromNvs() {
  prefs.begin("cyd_cal", true);
  if (prefs.getBool("valid", false)) {
    gCal.swapXY = prefs.getBool("swapXY", false);
    gCal.uAtX1 = prefs.getFloat("u1", 450.0f);
    gCal.uAtX2 = prefs.getFloat("u2", 3550.0f);
    gCal.vAtY1 = prefs.getFloat("v1", 550.0f);
    gCal.vAtY2 = prefs.getFloat("v2", 3450.0f);
  }
  prefs.end();
}

static TouchSample readTouch() {
  TouchSample out = {false, 0, 0};
  if (!ts.tirqTouched() || !ts.touched()) {
    return out;
  }
  long sumX = 0, sumY = 0;
  int count = 0;
  for (int i = 0; i < 4; i++) {
    TS_Point p = ts.getPoint();
    if (p.z > 200 && p.x > 50 && p.x < 4050 && p.y > 50 && p.y < 4050) {
      sumX += p.x;
      sumY += p.y;
      count++;
    }
  }
  if (count == 0) return out;

  int rawX = (int)(sumX / count);
  int rawY = (int)(sumY / count);
  float u = gCal.swapXY ? (float)rawY : (float)rawX;
  float v = gCal.swapXY ? (float)rawX : (float)rawY;
  float du = gCal.uAtX2 - gCal.uAtX1;
  float dv = gCal.vAtY2 - gCal.vAtY1;
  if (fabsf(du) < 1.0f) du = 1.0f;
  if (fabsf(dv) < 1.0f) dv = 1.0f;

  float fx = gCal.calX1 + (u - gCal.uAtX1) * (float)(gCal.calX2 - gCal.calX1) / du;
  float fy = gCal.calY1 + (v - gCal.vAtY1) * (float)(gCal.calY2 - gCal.calY1) / dv;
  out.touched = true;
  out.x = constrain((int)lroundf(fx), 0, SCREEN_W - 1);
  out.y = constrain((int)lroundf(fy), 0, SCREEN_H - 1);
  return out;
}

// ---------------------------------------------------------------------------
// White-on-Black Terminal Emulator (53 columns x 30 rows, 6x8 font)
// ---------------------------------------------------------------------------
static constexpr int TERM_COLS = 53;
static constexpr int TERM_ROWS = 30;
static constexpr int HIST_SCREENS = 10;
static constexpr int MAX_LINES = TERM_ROWS * HIST_SCREENS;  // 300 rows (10 screens)
static constexpr int LIVE_BASE = MAX_LINES - TERM_ROWS;     // 270 (9 screens of scrollback)
static constexpr int CHAR_W = 6;
static constexpr int CHAR_H = 8;
static constexpr int TERM_ORIGIN_X = 1;
static constexpr int TERM_ORIGIN_Y = 0;

static char gTermBuf[MAX_LINES][TERM_COLS];
static int gCurCol = 0;
static int gCurRow = 0;
static int gScrolledLines = 0;  // Number of lines scrolled into history (0..LIVE_BASE)
static int gViewOffset = 0;     // 0 = live bottom screen; >0 = lines scrolled back
static bool gKeyboardVisible = false;
static bool gTermFullRedraw = false;
static bool gCursorShown = false;
static int gDrawnCurCol = 0;
static int gDrawnCurRow = 0;

static void drawCharCell(int col, int row) {
  if (gKeyboardVisible || gViewOffset > 0) return;
  if (col < 0 || col >= TERM_COLS || row < 0 || row >= TERM_ROWS) return;
  int px = TERM_ORIGIN_X + col * CHAR_W;
  int py = TERM_ORIGIN_Y + row * CHAR_H;
  char ch = gTermBuf[LIVE_BASE + row][col];
  if (ch < 0x20 || ch > 0x7E) ch = ' ';
  tft.drawChar(px, py, ch, TFT_WHITE, TFT_BLACK, 1);
}

static void hideCursorIfShown() {
  if (!gCursorShown) return;
  gCursorShown = false;
  if (gKeyboardVisible || gTermFullRedraw || gViewOffset > 0) return;
  drawCharCell(gDrawnCurCol, gDrawnCurRow);
}

static void showCursorNow() {
  if (gKeyboardVisible || gCursorShown || gViewOffset > 0) return;
  if (gCurCol < 0 || gCurCol >= TERM_COLS || gCurRow < 0 || gCurRow >= TERM_ROWS) return;
  int px = TERM_ORIGIN_X + gCurCol * CHAR_W;
  int py = TERM_ORIGIN_Y + gCurRow * CHAR_H;
  tft.drawFastHLine(px, py + CHAR_H - 1, CHAR_W, TFT_WHITE);
  gDrawnCurCol = gCurCol;
  gDrawnCurRow = gCurRow;
  gCursorShown = true;
}

static void renderFullTerminal() {
  if (gKeyboardVisible) return;
  tft.fillScreen(TFT_BLACK);
  int baseRow = LIVE_BASE - gViewOffset;
  for (int r = 0; r < TERM_ROWS; r++) {
    for (int c = 0; c < TERM_COLS; c++) {
      char ch = gTermBuf[baseRow + r][c];
      if (ch > ' ' && ch <= 0x7E) {
        int px = TERM_ORIGIN_X + c * CHAR_W;
        int py = TERM_ORIGIN_Y + r * CHAR_H;
        tft.drawChar(px, py, ch, TFT_WHITE, TFT_BLACK, 1);
      }
    }
  }
  gTermFullRedraw = false;
  gCursorShown = false;
  if (gViewOffset == 0) {
    showCursorNow();
  }
}

static void flushTerminalDisplay() {
  if (gKeyboardVisible) return;
  if (gTermFullRedraw) {
    renderFullTerminal();
  } else if (!gCursorShown && gViewOffset == 0) {
    showCursorNow();
  }
}

static void clearTerminal() {
  static bool firstInit = true;
  if (firstInit) {
    memset(gTermBuf, ' ', sizeof(gTermBuf));
    firstInit = false;
  } else {
    for (int r = 0; r < TERM_ROWS; r++) {
      memset(gTermBuf[LIVE_BASE + r], ' ', TERM_COLS);
    }
  }
  gCurCol = 0;
  gCurRow = 0;
  gViewOffset = 0;
  gTermFullRedraw = true;
  flushTerminalDisplay();
}

static void scrollTerminalUp() {
  memmove(&gTermBuf[0][0], &gTermBuf[1][0], (MAX_LINES - 1) * TERM_COLS);
  memset(gTermBuf[MAX_LINES - 1], ' ', TERM_COLS);
  if (gScrolledLines < LIVE_BASE) {
    gScrolledLines++;
  }
  gCurRow = TERM_ROWS - 1;
  if (gViewOffset > 0) {
    if (gViewOffset < gScrolledLines) {
      gViewOffset++;
    } else {
      gTermFullRedraw = true;
    }
  } else {
    gTermFullRedraw = true;
  }
}

static void termPutChar(uint8_t rawCh) {
  uint8_t ch = rawCh & 0x7F;
  hideCursorIfShown();

  if (ch == '\r') {
    gCurCol = 0;
  } else if (ch == '\n') {
    gCurRow++;
    if (gCurRow >= TERM_ROWS) {
      scrollTerminalUp();
    }
  } else if (ch == 0x08 || ch == 0x7F) {  // Backspace
    if (gCurCol > 0) {
      gCurCol--;
      gTermBuf[LIVE_BASE + gCurRow][gCurCol] = ' ';
      if (!gTermFullRedraw && gViewOffset == 0) {
        drawCharCell(gCurCol, gCurRow);
      }
    }
  } else if (ch == 0x0C) {  // Form Feed (Clear screen, used by Basic09)
    clearTerminal();
  } else if (ch == 0x01) {  // Home
    gCurCol = 0;
    gCurRow = 0;
  } else if (ch >= 0x20 && ch <= 0x7E) {
    gTermBuf[LIVE_BASE + gCurRow][gCurCol] = (char)ch;
    if (!gTermFullRedraw && gViewOffset == 0) {
      drawCharCell(gCurCol, gCurRow);
    }
    gCurCol++;
    if (gCurCol >= TERM_COLS) {
      gCurCol = 0;
      gCurRow++;
      if (gCurRow >= TERM_ROWS) {
        scrollTerminalUp();
      }
    }
  }
}

static void onConsoleOutput(uint8_t ch, void*) {
  Serial.write(ch & 0x7F);
  termPutChar(ch);
}

// ---------------------------------------------------------------------------
// 1 MB Flash Partition Disk Backend for EMUDSK ($FF80-$FF86)
// ---------------------------------------------------------------------------
static bool onDiskSector(uint8_t drive, bool write, uint32_t lsn, uint8_t* buf256, void*) {
  if (drive != 0 || !gDiskPart || lsn >= kOs9DiskTotalSectors) {
    return false;
  }
  uint32_t byteOffset = lsn * 256u;
  if (!write) {
    return esp_partition_read(gDiskPart, byteOffset, buf256, 256) == ESP_OK;
  } else {
    // 4 KB read-modify-erase-write on SPI NOR Flash
    uint32_t blockBase = (byteOffset / 4096u) * 4096u;
    uint32_t offsetInBlock = byteOffset % 4096u;
    if (esp_partition_read(gDiskPart, blockBase, gFlashEraseBuf, 4096) != ESP_OK) {
      return false;
    }
    if (memcmp(gFlashEraseBuf + offsetInBlock, buf256, 256) == 0) {
      return true;  // Unchanged; skip flash erase cycle
    }
    memcpy(gFlashEraseBuf + offsetInBlock, buf256, 256);
    if (esp_partition_erase_range(gDiskPart, blockBase, 4096) != ESP_OK) {
      return false;
    }
    return esp_partition_write(gDiskPart, blockBase, gFlashEraseBuf, 4096) == ESP_OK;
  }
}

static void termPrintString(const char* s) {
  while (*s) {
    termPutChar((uint8_t)*s++);
  }
  flushTerminalDisplay();
}

static bool initFlashDisk() {
  gDiskPart = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
  if (!gDiskPart || gDiskPart->size < kOs9DiskTotalBytes) {
    Serial.println("[OS9] ERROR: 1MB flash partition not found or too small!");
    termPrintString("ERR: 1MB disk partition missing!\r\n");
    return false;
  }

  uint8_t lsn0[128] = {};
  if (esp_partition_read(gDiskPart, 0, lsn0, sizeof(lsn0)) == ESP_OK &&
      memcmp(lsn0, kOs9DiskLsn0Sig, 16) == 0 &&
      memcmp(lsn0 + 0x70, kOs9DiskBuildSig, 16) == 0) {
    Serial.println("[OS9] Found valid CYDOS9 1MB disk in flash partition.");
    return true;
  }

  Serial.println("[OS9] Initializing 1MB OS-9 disk in flash partition...");
  termPrintString("Initializing 1MB OS-9 Flash Disk...\r\n");

  // Write from last block down to block 0 so LSN 0 signature is only valid when complete
  for (int i = (int)kOs9InitialBlockCount - 1; i >= 0; i--) {
    uint16_t blkIdx = kOs9InitialBlocks[i].blockIndex;
    memcpy(gFlashEraseBuf, kOs9InitialBlocks[i].data, 4096);
    uint32_t offset = uint32_t(blkIdx) * 4096u;
    if (esp_partition_erase_range(gDiskPart, offset, 4096) != ESP_OK ||
        esp_partition_write(gDiskPart, offset, gFlashEraseBuf, 4096) != ESP_OK) {
      Serial.printf("[OS9] Flash write failed at block %u\n", unsigned(blkIdx));
      return false;
    }
  }
  Serial.println("[OS9] 1MB OS-9 Flash Disk initialized.");
  return true;
}

// ---------------------------------------------------------------------------
// On-Screen Stylus Line Keyboard + Immediate ^C Button
// ---------------------------------------------------------------------------
static String gTypedLine = "";
static int gKbPage = 0;  // 0 = lower, 1 = upper, 2 = symbols

static const char* const KB_ROWS[3][4] = {
  {
    "1234567890",
    "qwertyuiop",
    "asdfghjkl/",
    "zxcvbnm._-"
  },
  {
    "1234567890",
    "QWERTYUIOP",
    "ASDFGHJKL/",
    "ZXCVBNM._-"
  },
  {
    "!@#$%^&*()",
    "-_=+[]{}\\|",
    ";:'\",.<>/?",
    "`~$%&*+=-_"
  }
};

static void drawInputLineBox() {
  tft.fillRoundRect(4, 28, 246, 30, 4, TFT_BLACK);
  tft.drawRoundRect(4, 28, 246, 30, 4, TFT_CYAN);

  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  String shown = gTypedLine + "_";
  if (shown.length() > 26) {
    shown = ".." + shown.substring(shown.length() - 25);
  }
  tft.drawString(shown, 10, 43, 2);
}

static void drawKeyboardKeys() {
  tft.fillRect(0, 62, SCREEN_W, SCREEN_H - 62, 0x0841);

  for (int r = 0; r < 4; r++) {
    const char* rowStr = KB_ROWS[gKbPage][r];
    int ky = 64 + r * 35;
    for (int c = 0; c < 10; c++) {
      int kx = 2 + c * 32;
      char ch[2] = {rowStr[c], '\0'};

      tft.fillRoundRect(kx, ky, 29, 32, 4, 0x18E3);
      tft.drawRoundRect(kx, ky, 29, 32, 4, 0x4A69);
      tft.setTextDatum(MC_DATUM);
      tft.setTextColor(TFT_WHITE, 0x18E3);
      tft.drawString(ch, kx + 14, ky + 16, 2);
    }
  }

  int by = 205;
  int bh = 32;

  // [ABC / abc] (x: 2..68)
  uint16_t shiftCol = (gKbPage == 1) ? TFT_YELLOW : 0x2945;
  tft.fillRoundRect(2, by, 66, bh, 4, shiftCol);
  tft.drawRoundRect(2, by, 66, bh, 4, TFT_YELLOW);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor((gKbPage == 1) ? TFT_BLACK : TFT_YELLOW, shiftCol);
  tft.drawString((gKbPage == 1) ? "abc" : "ABC", 35, by + 16, 2);

  // [#+=] (x: 72..134)
  uint16_t symCol = (gKbPage == 2) ? TFT_MAGENTA : 0x2945;
  tft.fillRoundRect(72, by, 62, bh, 4, symCol);
  tft.drawRoundRect(72, by, 62, bh, 4, TFT_MAGENTA);
  tft.setTextColor((gKbPage == 2) ? TFT_BLACK : TFT_MAGENTA, symCol);
  tft.drawString((gKbPage == 2) ? "abc" : "#+=", 103, by + 16, 2);

  // [SPACE] (x: 138..228)
  tft.fillRoundRect(138, by, 90, bh, 4, 0x18E3);
  tft.drawRoundRect(138, by, 90, bh, 4, TFT_WHITE);
  tft.setTextColor(TFT_WHITE, 0x18E3);
  tft.drawString("SPACE", 183, by + 16, 2);

  // [ENTER] (x: 232..317)
  tft.fillRoundRect(232, by, 85, bh, 4, TFT_DARKGREEN);
  tft.drawRoundRect(232, by, 85, bh, 4, TFT_GREEN);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREEN);
  tft.drawString("ENTER", 274, by + 16, 2);
}

static void openKeyboard() {
  gKeyboardVisible = true;
  tft.fillScreen(0x0841);
  tft.fillRect(0, 0, SCREEN_W, 25, TFT_NAVY);

  // [HIDE] (x: 2..58, y: 2..22)
  tft.fillRoundRect(2, 2, 56, 20, 4, 0x2945);
  tft.drawRoundRect(2, 2, 56, 20, 4, TFT_CYAN);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_CYAN, 0x2945);
  tft.drawString("HIDE", 30, 12, 1);

  // [^C] immediate interrupt signal button (x: 64..128, y: 2..22)
  tft.fillRoundRect(64, 2, 64, 20, 4, 0xA800);
  tft.drawRoundRect(64, 2, 64, 20, 4, TFT_RED);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, 0xA800);
  tft.drawString("^C", 96, 12, 2);

  // Title
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_YELLOW, TFT_NAVY);
  tft.drawString("OS-9 LINE", 178, 12, 2);

  // [ENTER] (x: 230..317, y: 2..22)
  tft.fillRoundRect(230, 2, 87, 20, 4, TFT_DARKGREEN);
  tft.drawRoundRect(230, 2, 87, 20, 4, TFT_GREEN);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREEN);
  tft.drawString("ENTER", 273, 12, 2);

  // [DEL] (x: 254..317, y: 28..58)
  tft.fillRoundRect(254, 28, 63, 30, 4, 0x4800);
  tft.drawRoundRect(254, 28, 63, 30, 4, TFT_RED);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, 0x4800);
  tft.drawString("DEL", 285, 43, 2);

  drawInputLineBox();
  drawKeyboardKeys();
}

static void closeKeyboardAndRedraw() {
  gKeyboardVisible = false;
  tft.setTextDatum(TL_DATUM);
  renderFullTerminal();
}

static void submitTypedLine() {
  for (size_t i = 0; i < gTypedLine.length(); i++) {
    gBus.pushConsoleInput((uint8_t)gTypedLine[i]);
  }
  gBus.pushConsoleInput('\r');
  gTypedLine = "";
  gViewOffset = 0;
  closeKeyboardAndRedraw();
}

static void sendCtrlCImmediate() {
  // Discard any buffered line and send 0x03 (^C) right away
  gTypedLine = "";
  gViewOffset = 0;
  gBus.pushConsoleInput(0x03);
  closeKeyboardAndRedraw();
}

static void handleKeyboardTap(const TouchSample& t) {
  // Top bar (y: 0..25)
  if (t.y <= 25) {
    if (t.x <= 60) {
      closeKeyboardAndRedraw();
      return;
    }
    if (t.x >= 62 && t.x <= 132) {
      sendCtrlCImmediate();
      return;
    }
    if (t.x >= 228) {
      submitTypedLine();
      return;
    }
    return;
  }

  // Input line & [DEL] button (y: 26..60)
  if (t.y >= 26 && t.y <= 60) {
    if (t.x >= 250 && gTypedLine.length() > 0) {
      gTypedLine.remove(gTypedLine.length() - 1);
      drawInputLineBox();
    }
    return;
  }

  // 4x10 Character grid (y: 64..204)
  if (t.y >= 64 && t.y < 204) {
    int r = (t.y - 64) / 35;
    int c = t.x / 32;
    if (r >= 0 && r < 4 && c >= 0 && c < 10) {
      char ch = KB_ROWS[gKbPage][r][c];
      if (gTypedLine.length() < 80) {
        gTypedLine += ch;
        int kx = 2 + c * 32;
        int ky = 64 + r * 35;
        char s[2] = {ch, '\0'};
        tft.fillRoundRect(kx, ky, 29, 32, 4, TFT_CYAN);
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(TFT_BLACK, TFT_CYAN);
        tft.drawString(s, kx + 14, ky + 16, 2);
        delay(45);
        tft.fillRoundRect(kx, ky, 29, 32, 4, 0x18E3);
        tft.drawRoundRect(kx, ky, 29, 32, 4, 0x4A69);
        tft.setTextColor(TFT_WHITE, 0x18E3);
        tft.drawString(s, kx + 14, ky + 16, 2);
        drawInputLineBox();
      }
    }
    return;
  }

  // Bottom row (y: 205..240)
  if (t.y >= 205) {
    if (t.x < 70) {
      gKbPage = (gKbPage == 1) ? 0 : 1;
      drawKeyboardKeys();
    } else if (t.x < 136) {
      gKbPage = (gKbPage == 2) ? 0 : 2;
      drawKeyboardKeys();
    } else if (t.x < 230) {
      if (gTypedLine.length() < 80) {
        gTypedLine += ' ';
        drawInputLineBox();
      }
    } else {
      submitTypedLine();
    }
  }
}

// ---------------------------------------------------------------------------
// Arduino Setup & Main Loop
// ---------------------------------------------------------------------------
static bool gTouchWasDown = false;
static uint32_t gLastReleaseMs = 0;
static uint32_t gLastTouchPollMs = 0;
static uint32_t gLastTickUs = 0;
static uint64_t gLastTickCycles = 0;

void setup() {
  Serial.begin(115200);

  pinMode(LED_R_PIN, OUTPUT);
  pinMode(LED_G_PIN, OUTPUT);
  pinMode(LED_B_PIN, OUTPUT);
  digitalWrite(LED_R_PIN, HIGH);
  digitalWrite(LED_G_PIN, HIGH);
  digitalWrite(LED_B_PIN, HIGH);

  tft.init();
  tft.setRotation(1);  // 320x240 landscape
  initBacklightFromNvs();

  ts.begin();
  SPI.end();
  SPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, -1);
  ts.setRotation(1);
  loadCalibrationFromNvs();

  clearTerminal();
  initFlashDisk();

  if (!gBus.initRam()) {
    termPrintString("FATAL: 128KB RAM alloc failed!\r\n");
    while (true) delay(1000);
  }
  gBus.setConsoleCallback(onConsoleOutput, nullptr);
  gBus.setDiskCallback(onDiskSector, nullptr);
  gBus.loadBootImage64K(kDeep65280Img);
  gCpu.reset();

  gLastTickUs = micros();
  gLastTickCycles = gCpu.Cycles;
  Serial.printf("[OS9] Booting NitrOS-9 Level 2 (Free heap: %lu bytes)\n", (unsigned long)ESP.getFreeHeap());
}

void loop() {
  // Run a slice of 6809 instructions (~5,000 cycles per slice)
  int cycles = 0;
  while (cycles < 5000 && !gCpu.Halted) {
    cycles += gCpu.step();
  }
  flushTerminalDisplay();

  // 60 Hz OS-9 system clock tick: require both >=16.67ms wall time and >=25,000 CPU cycles
  // so TFT rendering never starves user-mode instructions with back-to-back timer IRQs.
  uint32_t nowUs = micros();
  if ((uint32_t)(nowUs - gLastTickUs) >= 16667u &&
      (gCpu.Cycles - gLastTickCycles) >= 25000ULL) {
    gLastTickCycles = gCpu.Cycles;
    gLastTickUs += 16667u;
    if ((uint32_t)(nowUs - gLastTickUs) >= 50000u) {
      gLastTickUs = nowUs;
    }
    gBus.clockTick();
  }

  // Forward any USB Serial input into ACIA
  while (Serial.available() > 0) {
    int b = Serial.read();
    if (b == '\n') b = '\r';
    if (b >= 0) {
      gBus.pushConsoleInput((uint8_t)b);
    }
  }

  // Poll touchscreen at ~50 Hz (every 20 ms)
  uint32_t nowMs = millis();
  if (nowMs - gLastTouchPollMs >= 20) {
    gLastTouchPollMs = nowMs;
    TouchSample t = readTouch();
    if (t.touched) {
      if (!gTouchWasDown && (nowMs - gLastReleaseMs > 120)) {
        gTouchWasDown = true;
        if (!gKeyboardVisible) {
          if (t.x >= 240 && t.y < 70) {
            // NE corner: Page Up into scrollback history
            int nextOffset = gViewOffset + TERM_ROWS;
            if (nextOffset > gScrolledLines) nextOffset = gScrolledLines;
            if (nextOffset != gViewOffset) {
              gViewOffset = nextOffset;
              renderFullTerminal();
            }
          } else if (t.x >= 240 && t.y >= 170) {
            // SE corner: Page Down toward live terminal screen
            int nextOffset = gViewOffset - TERM_ROWS;
            if (nextOffset < 0) nextOffset = 0;
            if (nextOffset != gViewOffset) {
              gViewOffset = nextOffset;
              renderFullTerminal();
            }
          } else {
            openKeyboard();
          }
        } else {
          handleKeyboardTap(t);
        }
      }
    } else {
      if (gTouchWasDown) {
        gTouchWasDown = false;
        gLastReleaseMs = nowMs;
      }
    }
  }

  updateBrightnessLoop();
}
