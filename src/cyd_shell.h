#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>

// ---------------------------------------------------------------------------
// Hardware Pin Definitions for ESP32-2432S028R (CYD / ELEGOO 2.8" ESP32)
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

struct TouchSample {
  bool touched;
  int rawX;
  int rawY;
  int rawZ;
  int x;
  int y;
};

struct DemoApp {
  const char *title;
  const char *subtitle;
  uint16_t accentColor;
  void (*enter)();
  void (*loop)();
  void (*onTap)(const TouchSample &t);
  void (*onDrag)(const TouchSample &t);
  void (*onRelease)();
};

// Shared objects & helpers provided by launcher.cpp
extern TFT_eSPI tft;
extern XPT2046_Touchscreen ts;

void setRgbLed(bool r, bool g, bool b, uint32_t durationMs = 0);
TouchSample readTouch();
void returnToMenu();
void startCalibration(bool returnToMenuAfter = true);

// Demo registrations
extern const DemoApp DEMO_WHACK;
extern const DemoApp DEMO_LIFE;
extern const DemoApp DEMO_RAINBOW;
