#include "cyd_shell.h"
#include <Preferences.h>
#include <SPI.h>
#include <math.h>

TFT_eSPI tft = TFT_eSPI();
XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);
static Preferences prefs;

// Registered demos in the launcher menu (2x3 grid + Settings button)
static const DemoApp *const DEMOS[] = {
  &DEMO_WHACK,
  &DEMO_LIFE,
  &DEMO_RAINBOW,
  &DEMO_TAZ,
  &DEMO_THUMB,
  &DEMO_WEATHER,
};
static const int NUM_DEMOS = sizeof(DEMOS) / sizeof(DEMOS[0]);

// ---------------------------------------------------------------------------
// Active-Low RGB LED helper
// ---------------------------------------------------------------------------
static uint32_t ledOffAtMs = 0;

void setRgbLed(bool r, bool g, bool b, uint32_t durationMs) {
  digitalWrite(LED_R_PIN, r ? LOW : HIGH);
  digitalWrite(LED_G_PIN, g ? LOW : HIGH);
  digitalWrite(LED_B_PIN, b ? LOW : HIGH);
  ledOffAtMs = (durationMs > 0) ? (millis() + durationMs) : 0;
}

static void updateRgbLed() {
  if (ledOffAtMs != 0 && (int32_t)(millis() - ledOffAtMs) >= 0) {
    setRgbLed(false, false, false, 0);
  }
}

// ---------------------------------------------------------------------------
// Touch Calibration & Mapping (with ESP32 NVS Persistence)
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
} cal;

static bool loadCalibrationFromNvs() {
  prefs.begin("cyd_cal", true);
  bool valid = prefs.getBool("valid", false);
  if (valid) {
    cal.swapXY = prefs.getBool("swapXY", false);
    cal.uAtX1  = prefs.getFloat("u1", 450.0f);
    cal.uAtX2  = prefs.getFloat("u2", 3550.0f);
    cal.vAtY1  = prefs.getFloat("v1", 550.0f);
    cal.vAtY2  = prefs.getFloat("v2", 3450.0f);
    Serial.printf("[CYD] Loaded saved calibration from NVS: swapXY=%d u=[%.0f..%.0f] v=[%.0f..%.0f]\n",
                  cal.swapXY ? 1 : 0, cal.uAtX1, cal.uAtX2, cal.vAtY1, cal.vAtY2);
  }
  prefs.end();
  return valid;
}

static void saveCalibrationToNvs() {
  prefs.begin("cyd_cal", false);
  prefs.putBool("valid", true);
  prefs.putBool("swapXY", cal.swapXY);
  prefs.putFloat("u1", cal.uAtX1);
  prefs.putFloat("u2", cal.uAtX2);
  prefs.putFloat("v1", cal.vAtY1);
  prefs.putFloat("v2", cal.vAtY2);
  prefs.end();
  Serial.println("[CYD] Saved calibration to NVS.");
}

static void mapRawToScreen(int rawX, int rawY, int &sx, int &sy) {
  float u = cal.swapXY ? (float)rawY : (float)rawX;
  float v = cal.swapXY ? (float)rawX : (float)rawY;

  float du = cal.uAtX2 - cal.uAtX1;
  float dv = cal.vAtY2 - cal.vAtY1;
  if (fabsf(du) < 1.0f) du = 1.0f;
  if (fabsf(dv) < 1.0f) dv = 1.0f;

  float fx = cal.calX1 + (u - cal.uAtX1) * (float)(cal.calX2 - cal.calX1) / du;
  float fy = cal.calY1 + (v - cal.vAtY1) * (float)(cal.calY2 - cal.calY1) / dv;

  sx = constrain((int)lroundf(fx), 0, SCREEN_W - 1);
  sy = constrain((int)lroundf(fy), 0, SCREEN_H - 1);
}

TouchSample readTouch() {
  TouchSample out = {false, 0, 0, 0, 0, 0};
  if (!ts.tirqTouched() || !ts.touched()) {
    return out;
  }

  long sumX = 0, sumY = 0, sumZ = 0;
  int count = 0;
  for (int i = 0; i < 4; i++) {
    TS_Point p = ts.getPoint();
    if (p.z > 200 && p.x > 50 && p.x < 4050 && p.y > 50 && p.y < 4050) {
      sumX += p.x;
      sumY += p.y;
      sumZ += p.z;
      count++;
    }
  }
  if (count == 0) {
    return out;
  }

  out.touched = true;
  out.rawX = (int)(sumX / count);
  out.rawY = (int)(sumY / count);
  out.rawZ = (int)(sumZ / count);
  mapRawToScreen(out.rawX, out.rawY, out.x, out.y);
  return out;
}

// ---------------------------------------------------------------------------
// Shell State (Calibration, Menu, or Active Demo)
// ---------------------------------------------------------------------------
enum ShellMode {
  MODE_CALIBRATE,
  MODE_MENU,
  MODE_DEMO
};

static ShellMode shellMode = MODE_MENU;
static const DemoApp *activeDemo = nullptr;
static int calStep = 0;
static bool calReturnToMenu = true;
static int calRawX[3] = {0, 0, 0};
static int calRawY[3] = {0, 0, 0};
static bool touchWasDown = false;
static uint32_t lastReleaseMs = 0;
static uint32_t lastMenuFooterMs = 0;

// ---------------------------------------------------------------------------
// 3-Tap Calibration UI
// ---------------------------------------------------------------------------
static void drawCrosshairTarget(int x, int y, uint16_t color, const char *label) {
  tft.drawCircle(x, y, 18, color);
  tft.drawCircle(x, y, 10, color);
  tft.fillCircle(x, y, 4, TFT_WHITE);
  tft.drawFastHLine(x - 24, y, 48, color);
  tft.drawFastVLine(x, y - 24, 48, color);
  tft.setTextColor(color, TFT_BLACK);
  tft.setTextDatum(TC_DATUM);
  tft.drawString(label, x, (y < 120) ? (y + 24) : (y - 36), 2);
}

static void drawCalibrationScreen() {
  tft.fillScreen(TFT_BLACK);

  tft.fillRect(0, 0, SCREEN_W, 26, TFT_NAVY);
  tft.setTextColor(TFT_YELLOW, TFT_NAVY);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("TOUCHSCREEN 3-TAP CALIBRATION", SCREEN_W / 2, 13, 2);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Tap the glowing corner target!", SCREEN_W / 2, 112, 2);

  String stepText = "Target " + String(calStep + 1) + " of 3";
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString(stepText, SCREEN_W / 2, 132, 2);

  if (calStep == 0) {
    drawCrosshairTarget(cal.calX1, cal.calY1, TFT_GREEN, "1: TOP-LEFT");
  } else if (calStep == 1) {
    drawCrosshairTarget(cal.calX2, cal.calY1, TFT_CYAN, "2: TOP-RIGHT");
  } else if (calStep == 2) {
    drawCrosshairTarget(cal.calX2, cal.calY2, TFT_YELLOW, "3: BOT-RIGHT");
  }
}

void startCalibration(bool returnToMenuAfter) {
  shellMode = MODE_CALIBRATE;
  activeDemo = nullptr;
  calStep = 0;
  calReturnToMenu = returnToMenuAfter;
  drawCalibrationScreen();
}

static void handleCalibrationTap(const TouchSample &t) {
  calRawX[calStep] = t.rawX;
  calRawY[calStep] = t.rawY;

  int cx = (calStep == 0) ? cal.calX1 : cal.calX2;
  int cy = (calStep < 2)  ? cal.calY1 : cal.calY2;
  tft.fillCircle(cx, cy, 14, TFT_GREEN);
  setRgbLed(false, true, false, 120);
  delay(180);

  calStep++;
  if (calStep < 3) {
    drawCalibrationScreen();
  } else {
    int dx10 = abs(calRawX[1] - calRawX[0]);
    int dy10 = abs(calRawY[1] - calRawY[0]);

    cal.swapXY = (dy10 > dx10);
    if (!cal.swapXY) {
      cal.uAtX1 = (float)calRawX[0];
      cal.uAtX2 = (float)calRawX[1];
      cal.vAtY1 = (float)(calRawY[0] + calRawY[1]) * 0.5f;
      cal.vAtY2 = (float)calRawY[2];
    } else {
      cal.uAtX1 = (float)calRawY[0];
      cal.uAtX2 = (float)calRawY[1];
      cal.vAtY1 = (float)(calRawX[0] + calRawX[1]) * 0.5f;
      cal.vAtY2 = (float)calRawY[2];
    }

    saveCalibrationToNvs();
    if (calReturnToMenu) {
      returnToMenu();
    } else {
      launchDemo(&DEMO_SETTINGS);
    }
  }
}

// ---------------------------------------------------------------------------
// Home Menu / Demo Selector UI (2-column x 3-row Grid + [SETTINGS] Button)
// ---------------------------------------------------------------------------
static void drawMenuFooter() {
  tft.fillRect(0, 218, SCREEN_W, 22, 0x0841);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_CYAN, 0x0841);
  char buf[64];
  snprintf(buf, sizeof(buf), "Airport: %s (%s) | Bright: %d%%%s | LDR: %d",
           getHomeAirportIata(), getHomeTzAbbrev(),
           getBrightnessPercent(), isAutoBrightness() ? " Auto" : "",
           getSmoothedLdr());
  tft.drawString(buf, SCREEN_W / 2, 229, 1);
}

static void drawMenuScreen() {
  tft.fillScreen(0x0841); // Deep slate

  // Top Header
  tft.fillRect(0, 0, SCREEN_W, 25, TFT_NAVY);
  tft.drawFastHLine(0, 25, SCREEN_W, TFT_CYAN);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(TFT_YELLOW, TFT_NAVY);
  tft.drawString("CYD-ONE : SELECT AN APP", 8, 13, 2);

  // [SETTINGS] button in top-right header
  tft.fillRoundRect(232, 3, 84, 19, 4, 0x2945);
  tft.drawRoundRect(232, 3, 84, 19, 4, TFT_CYAN);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_CYAN, 0x2945);
  tft.drawString("SETTINGS", 274, 12, 1);

  // 2x3 Grid of App Cards (colW = 148, cardH = 56, rowStep = 62)
  for (int i = 0; i < NUM_DEMOS; i++) {
    int col = i % 2;
    int row = i / 2;
    int cardX = (col == 0) ? 8 : 164;
    int cardY = 31 + row * 62;
    int cardW = 148;
    int cardH = 56;
    uint16_t accent = DEMOS[i]->accentColor;

    tft.fillRoundRect(cardX, cardY, cardW, cardH, 6, 0x18E3);
    tft.drawRoundRect(cardX, cardY, cardW, cardH, 6, accent);
    tft.fillRoundRect(cardX + 4, cardY + 5, 5, cardH - 10, 2, accent);

    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_WHITE, 0x18E3);
    tft.drawString(DEMOS[i]->title, cardX + 14, cardY + 7, 2);

    tft.setTextColor(TFT_LIGHTGREY, 0x18E3);
    tft.drawString(DEMOS[i]->subtitle, cardX + 14, cardY + 28, 1);

    // Subtle accent corner badge
    tft.fillRoundRect(cardX + cardW - 26, cardY + cardH - 18, 22, 14, 3, accent);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_BLACK, accent);
    tft.drawString("GO", cardX + cardW - 15, cardY + cardH - 11, 1);
  }

  drawMenuFooter();
}

void returnToMenu() {
  activeDemo = nullptr;
  shellMode = MODE_MENU;
  touchWasDown = true; // Ignore held finger until released
  setRgbLed(false, false, false, 0);
  drawMenuScreen();
}

void launchDemo(const DemoApp *demo) {
  activeDemo = demo;
  shellMode = MODE_DEMO;
  touchWasDown = true; // Require fresh tap inside demo
  setRgbLed(false, false, true, 120);
  Serial.printf("[CYD] Launching demo: %s\n", demo->title);
  if (activeDemo->enter) {
    activeDemo->enter();
  }
}

static void handleMenuTap(const TouchSample &t) {
  // Check [SETTINGS] button in top-right header (x: 228..319, y: 0..27) or bottom status bar (y >= 216)
  if ((t.x >= 228 && t.y <= 27) || t.y >= 216) {
    launchDemo(&DEMO_SETTINGS);
    return;
  }

  // Check 2x3 App Cards
  for (int i = 0; i < NUM_DEMOS; i++) {
    int col = i % 2;
    int row = i / 2;
    int cardX = (col == 0) ? 8 : 164;
    int cardY = 31 + row * 62;
    int cardW = 148;
    int cardH = 56;
    if (t.x >= cardX && t.x <= cardX + cardW && t.y >= cardY && t.y <= cardY + cardH) {
      launchDemo(DEMOS[i]);
      return;
    }
  }
}

// ---------------------------------------------------------------------------
// Arduino Setup & Main Loop
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(150);
  Serial.println("\n==========================================");
  Serial.println("  CYD-ONE Multi-Demo Launcher");
  Serial.println("==========================================");

  pinMode(LED_R_PIN, OUTPUT);
  pinMode(LED_G_PIN, OUTPUT);
  pinMode(LED_B_PIN, OUTPUT);
  setRgbLed(false, false, true, 250);

  pinMode(LDR_PIN, INPUT);
  randomSeed(analogRead(LDR_PIN) ^ micros());

  // Initialize ILI9341 Display on HSPI
  tft.init();
  tft.setRotation(1); // Landscape 320x240

  // Load saved brightness (Auto ON by default) & Home Airport/Timezone from NVS
  initSettings();

  // Initialize XPT2046 Touchscreen on VSPI pins
  ts.begin();
  SPI.end();
  SPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, -1);
  ts.setRotation(1);

  // If NVS has a saved calibration, go straight to Menu; otherwise calibrate first
  if (loadCalibrationFromNvs()) {
    returnToMenu();
  } else {
    startCalibration(true);
  }
}

void loop() {
  updateRgbLed();
  updateBrightnessLoop();

  TouchSample t = readTouch();
  uint32_t now = millis();

  if (shellMode == MODE_MENU && (now - lastMenuFooterMs >= 600)) {
    lastMenuFooterMs = now;
    drawMenuFooter();
  }

  if (t.touched) {
    if (!touchWasDown && (now - lastReleaseMs > 70)) {
      touchWasDown = true;

      if (shellMode == MODE_CALIBRATE) {
        handleCalibrationTap(t);
      } else if (shellMode == MODE_MENU) {
        handleMenuTap(t);
      } else if (shellMode == MODE_DEMO && activeDemo) {
        if (activeDemo->onTap) activeDemo->onTap(t);
      }
    } else if (touchWasDown && shellMode == MODE_DEMO && activeDemo) {
      if (activeDemo->onDrag) activeDemo->onDrag(t);
    }
  } else {
    if (touchWasDown) {
      touchWasDown = false;
      lastReleaseMs = now;
      if (shellMode == MODE_DEMO && activeDemo && activeDemo->onRelease) {
        activeDemo->onRelease();
      }
    }
  }

  if (shellMode == MODE_DEMO && activeDemo && activeDemo->loop) {
    activeDemo->loop();
  }

  delay(8);
}
