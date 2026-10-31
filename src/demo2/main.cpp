#include <Arduino.h>
#include <SPI.h>
#include <Preferences.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <math.h>

// ---------------------------------------------------------------------------
// CYD Hardware Definitions (ESP32-2432S028R / ELEGOO 2.8" ESP32 Display)
// Automatically reuses NVS touch calibration ("cyd_cal") & Wi-Fi ("cyd_wifi")
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

static const int SCREEN_W = 320;
static const int SCREEN_H = 240;

TFT_eSPI tft = TFT_eSPI();
XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);
static Preferences prefs;

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

static void loadCalibrationFromNvs() {
  prefs.begin("cyd_cal", true);
  if (prefs.getBool("valid", false)) {
    cal.swapXY = prefs.getBool("swapXY", false);
    cal.uAtX1  = prefs.getFloat("u1", 450.0f);
    cal.uAtX2  = prefs.getFloat("u2", 3550.0f);
    cal.vAtY1  = prefs.getFloat("v1", 550.0f);
    cal.vAtY2  = prefs.getFloat("v2", 3450.0f);
  }
  prefs.end();
}

static bool readTouchScreen(int &sx, int &sy) {
  if (!ts.tirqTouched() || !ts.touched()) return false;
  TS_Point p = ts.getPoint();
  if (p.z < 200) return false;

  float u = cal.swapXY ? (float)p.y : (float)p.x;
  float v = cal.swapXY ? (float)p.x : (float)p.y;
  float fx = cal.calX1 + (u - cal.uAtX1) * (float)(cal.calX2 - cal.calX1) / (cal.uAtX2 - cal.uAtX1);
  float fy = cal.calY1 + (v - cal.vAtY1) * (float)(cal.calY2 - cal.calY1) / (cal.vAtY2 - cal.vAtY1);
  sx = constrain((int)lroundf(fx), 0, SCREEN_W - 1);
  sy = constrain((int)lroundf(fy), 0, SCREEN_H - 1);
  return true;
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_R_PIN, OUTPUT);
  pinMode(LED_G_PIN, OUTPUT);
  pinMode(LED_B_PIN, OUTPUT);
  digitalWrite(LED_R_PIN, HIGH);
  digitalWrite(LED_G_PIN, HIGH);
  digitalWrite(LED_B_PIN, HIGH);

  tft.init();
  tft.setRotation(1);

  ts.begin();
  SPI.end();
  SPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, -1);
  ts.setRotation(1);
  loadCalibrationFromNvs();

  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString("DEMO 2 READY", SCREEN_W / 2, SCREEN_H / 2, 4);
}

void loop() {
  int x, y;
  if (readTouchScreen(x, y)) {
    tft.fillCircle(x, y, 3, TFT_YELLOW);
  }
  delay(10);
}
