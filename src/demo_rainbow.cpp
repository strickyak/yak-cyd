#include "cyd_shell.h"
#include <math.h>

namespace {

static const int TOP_BAR_H = 24;
static const int PLAY_H = SCREEN_H - TOP_BAR_H; // 216

enum RainbowMode {
  MODE_SPECTRUM = 0, // Static 65,536-color 2D HSV field + touch color picker
  MODE_WAVES    = 1, // Animated concentric rainbow waves centered on stylus
  MODE_PAINT    = 2  // Rainbow ribbon brush on black canvas
};

static RainbowMode mode = MODE_SPECTRUM;
static uint16_t pickedColor565 = 0xF800;
static uint8_t pickedR = 255, pickedG = 0, pickedB = 0;

// Wave animation state
static uint16_t hueLut[256];
static int waveCenterX = SCREEN_W / 2;
static int waveCenterY = TOP_BAR_H + PLAY_H / 2;
static uint16_t wavePhase = 0;

// Paint state
static int lastPaintX = -1;
static int lastPaintY = -1;
static uint16_t paintHue = 0;

// Fast HSV to RGB888 & RGB565 (hue: 0..1535, sat: 0..255, val: 0..255)
void hsvToRgb(uint16_t hue, uint8_t sat, uint8_t val,
              uint8_t &rOut, uint8_t &gOut, uint8_t &bOut) {
  hue %= 1536;
  uint8_t sextant = hue >> 8;
  uint8_t frac = hue & 0xFF;

  uint8_t p = (uint16_t)val * (255 - sat) >> 8;
  uint8_t q = (uint16_t)val * (255 - ((uint16_t)sat * frac >> 8)) >> 8;
  uint8_t t = (uint16_t)val * (255 - ((uint16_t)sat * (255 - frac) >> 8)) >> 8;

  switch (sextant) {
    case 0: rOut = val; gOut = t;   bOut = p;   break;
    case 1: rOut = q;   gOut = val; bOut = p;   break;
    case 2: rOut = p;   gOut = val; bOut = t;   break;
    case 3: rOut = p;   gOut = q;   bOut = val; break;
    case 4: rOut = t;   gOut = p;   bOut = val; break;
    default:rOut = val; gOut = p;   bOut = q;   break;
  }
}

uint16_t packRgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((uint16_t)(r & 0xF8) << 8) |
         ((uint16_t)(g & 0xFC) << 3) |
         (b >> 3);
}

uint16_t hsvTo565(uint16_t hue, uint8_t sat, uint8_t val) {
  uint8_t r, g, b;
  hsvToRgb(hue, sat, val, r, g, b);
  return packRgb565(r, g, b);
}

void initHueLut() {
  for (int i = 0; i < 256; i++) {
    hueLut[i] = hsvTo565((i * 1536) >> 8, 255, 255);
  }
}

void drawSwatchAndHex() {
  tft.fillRect(252, 2, 66, 20, TFT_BLACK);
  tft.fillRoundRect(254, 3, 18, 17, 3, pickedColor565);
  tft.drawRoundRect(254, 3, 18, 17, 3, TFT_WHITE);

  char hexBuf[8];
  snprintf(hexBuf, sizeof(hexBuf), "%04X", pickedColor565);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(hexBuf, 276, 11, 1);
}

void drawTopBar() {
  tft.fillRect(0, 0, SCREEN_W, TOP_BAR_H - 1, TFT_BLACK);
  tft.drawFastHLine(0, TOP_BAR_H - 1, SCREEN_W, TFT_DARKGREY);
  tft.setTextDatum(MC_DATUM);

  // [ MENU ] (x: 2..50)
  tft.drawRoundRect(2, 2, 48, 19, 4, TFT_CYAN);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString("MENU", 26, 11, 1);

  // [ SPECTRUM ] (x: 54..122)
  bool isSpec = (mode == MODE_SPECTRUM);
  tft.fillRoundRect(54, 2, 68, 19, 4, isSpec ? TFT_YELLOW : TFT_BLACK);
  tft.drawRoundRect(54, 2, 68, 19, 4, TFT_YELLOW);
  tft.setTextColor(isSpec ? TFT_BLACK : TFT_YELLOW, isSpec ? TFT_YELLOW : TFT_BLACK);
  tft.drawString("SPECTRUM", 88, 11, 1);

  // [ WAVES ] (x: 126..184)
  bool isWaves = (mode == MODE_WAVES);
  tft.fillRoundRect(126, 2, 58, 19, 4, isWaves ? TFT_MAGENTA : TFT_BLACK);
  tft.drawRoundRect(126, 2, 58, 19, 4, TFT_MAGENTA);
  tft.setTextColor(isWaves ? TFT_BLACK : TFT_MAGENTA, isWaves ? TFT_MAGENTA : TFT_BLACK);
  tft.drawString("WAVES", 155, 11, 1);

  // [ PAINT ] (x: 188..248)
  bool isPaint = (mode == MODE_PAINT);
  tft.fillRoundRect(188, 2, 60, 19, 4, isPaint ? TFT_GREEN : TFT_BLACK);
  tft.drawRoundRect(188, 2, 60, 19, 4, TFT_GREEN);
  tft.setTextColor(isPaint ? TFT_BLACK : TFT_GREEN, isPaint ? TFT_GREEN : TFT_BLACK);
  tft.drawString(isPaint ? "CLR/PNT" : "PAINT", 218, 11, 1);

  drawSwatchAndHex();
}

// Render the full 320x216 2D HSV spectrum (all 65,536 RGB565 colors)
void renderFullSpectrum() {
  static uint16_t lineBuf[SCREEN_W];
  int halfH = PLAY_H / 2; // 108

  for (int y = 0; y < PLAY_H; y++) {
    uint8_t sat, val;
    if (y < halfH) {
      // Top half: White (sat=0, val=255) -> Pure Hue (sat=255, val=255)
      sat = (uint8_t)((y * 255) / (halfH - 1));
      val = 255;
    } else {
      // Bottom half: Pure Hue (sat=255, val=255) -> Black (sat=255, val=0)
      sat = 255;
      int dy = y - halfH;
      val = (uint8_t)(255 - (dy * 255) / (halfH - 1));
    }

    for (int x = 0; x < SCREEN_W; x++) {
      uint16_t hue = (uint16_t)((x * 1536) / SCREEN_W);
      lineBuf[x] = hsvTo565(hue, sat, val);
    }
    tft.pushImage(0, TOP_BAR_H + y, SCREEN_W, 1, lineBuf);
  }
}

void sampleSpectrumAt(int x, int y) {
  int py = constrain(y - TOP_BAR_H, 0, PLAY_H - 1);
  int px = constrain(x, 0, SCREEN_W - 1);
  int halfH = PLAY_H / 2;

  uint8_t sat, val;
  if (py < halfH) {
    sat = (uint8_t)((py * 255) / (halfH - 1));
    val = 255;
  } else {
    sat = 255;
    val = (uint8_t)(255 - ((py - halfH) * 255) / (halfH - 1));
  }
  uint16_t hue = (uint16_t)((px * 1536) / SCREEN_W);

  hsvToRgb(hue, sat, val, pickedR, pickedG, pickedB);
  pickedColor565 = packRgb565(pickedR, pickedG, pickedB);
  drawSwatchAndHex();

  // Drive onboard RGB LED to match dominant channels
  setRgbLed(pickedR > 110, pickedG > 110, pickedB > 110, 200);
}

// Render 1 frame of animated rainbow plasma/waves (2x2 blocks for fast FPS)
void renderWaveFrame() {
  static uint16_t rowBuf[SCREEN_W];
  int cx = waveCenterX;
  int cy = waveCenterY - TOP_BAR_H;

  for (int y = 0; y < PLAY_H; y += 2) {
    int dy = y - cy;
    int dy2 = dy * dy;

    for (int x = 0; x < SCREEN_W; x += 4) {
      int dx = x - cx;
      // Fast integer approximation of distance + diagonal wave interference
      int dist = (abs(dx) + abs(dy)) + ((dx * dy) >> 7) + (dy2 >> 8);
      uint8_t idx = (uint8_t)((dist * 2 + (x >> 1) - wavePhase) & 0xFF);
      uint16_t c = hueLut[idx];
      rowBuf[x]     = c;
      rowBuf[x + 1] = c;
      rowBuf[x + 2] = c;
      rowBuf[x + 3] = c;
    }
    tft.pushImage(0, TOP_BAR_H + y, SCREEN_W, 1, rowBuf);
    tft.pushImage(0, TOP_BAR_H + y + 1, SCREEN_W, 1, rowBuf);
  }

  pickedColor565 = hueLut[wavePhase & 0xFF];
  drawSwatchAndHex();
  wavePhase += 6;
}

void paintRainbowSegment(int x0, int y0, int x1, int y1) {
  int dx = abs(x1 - x0);
  int dy = abs(y1 - y0);
  int sx = (x0 < x1) ? 1 : -1;
  int sy = (y0 < y1) ? 1 : -1;
  int err = dx - dy;

  while (true) {
    uint8_t r, g, b;
    hsvToRgb(paintHue, 255, 255, r, g, b);
    pickedColor565 = packRgb565(r, g, b);
    paintHue = (paintHue + 10) % 1536;

    if (y0 >= TOP_BAR_H + 6) {
      tft.fillCircle(x0, y0, 6, pickedColor565);
    }

    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 > -dy) { err -= dy; x0 += sx; }
    if (e2 <  dx) { err += dx; y0 += sy; }
  }
  drawSwatchAndHex();
}

void switchMode(RainbowMode newMode) {
  mode = newMode;
  lastPaintX = -1;
  lastPaintY = -1;
  drawTopBar();

  if (mode == MODE_SPECTRUM) {
    renderFullSpectrum();
  } else if (mode == MODE_WAVES) {
    waveCenterX = SCREEN_W / 2;
    waveCenterY = TOP_BAR_H + PLAY_H / 2;
    renderWaveFrame();
  } else if (mode == MODE_PAINT) {
    tft.fillRect(0, TOP_BAR_H, SCREEN_W, PLAY_H, TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString("Draw with stylus to paint rainbow ribbons!", SCREEN_W / 2, TOP_BAR_H + PLAY_H / 2, 2);
  }
}

void rainbowEnter() {
  tft.setSwapBytes(true); // pushImage expects byte-swapped RGB565 on ESP32
  initHueLut();
  switchMode(MODE_SPECTRUM);
}

void rainbowLoop() {
  if (mode == MODE_WAVES) {
    renderWaveFrame();
  }
}

void handlePlayfieldTouch(const TouchSample &t, bool isDrag) {
  if (mode == MODE_SPECTRUM) {
    sampleSpectrumAt(t.x, t.y);
  } else if (mode == MODE_WAVES) {
    waveCenterX = t.x;
    waveCenterY = t.y;
  } else if (mode == MODE_PAINT) {
    if (isDrag && lastPaintX >= 0 && lastPaintY >= 0) {
      int dist = abs(t.x - lastPaintX) + abs(t.y - lastPaintY);
      if (dist <= 90) {
        paintRainbowSegment(lastPaintX, lastPaintY, t.x, t.y);
      } else {
        paintRainbowSegment(t.x, t.y, t.x, t.y);
      }
    } else {
      paintRainbowSegment(t.x, t.y, t.x, t.y);
    }
    lastPaintX = t.x;
    lastPaintY = t.y;
  }
}

void rainbowOnTap(const TouchSample &t) {
  if (t.y < TOP_BAR_H) {
    if (t.x <= 52) {
      tft.setSwapBytes(false);
      returnToMenu();
      return;
    }
    if (t.x >= 54 && t.x <= 124) {
      switchMode(MODE_SPECTRUM);
      return;
    }
    if (t.x >= 126 && t.x <= 186) {
      switchMode(MODE_WAVES);
      return;
    }
    if (t.x >= 188 && t.x <= 250) {
      // Tapping PAINT again clears the canvas
      switchMode(MODE_PAINT);
      return;
    }
    return;
  }

  handlePlayfieldTouch(t, false);
}

void rainbowOnDrag(const TouchSample &t) {
  if (t.y < TOP_BAR_H) return;
  handlePlayfieldTouch(t, true);
}

void rainbowOnRelease() {
  lastPaintX = -1;
  lastPaintY = -1;
}

} // namespace

const DemoApp DEMO_RAINBOW = {
  "65K RAINBOW",
  "HSV spectrum, plasma waves & rainbow brush",
  TFT_MAGENTA,
  rainbowEnter,
  rainbowLoop,
  rainbowOnTap,
  rainbowOnDrag,
  rainbowOnRelease
};
