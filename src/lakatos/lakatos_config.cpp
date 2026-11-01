#include "lakatos_state.h"
#include <WiFi.h>
#include <math.h>
#include <string.h>
#include <time.h>

// ---------------------------------------------------------------------------
// Touch Calibration State (NVS namespace "cyd_cal", shared with demo1/demo2)
// ---------------------------------------------------------------------------
struct TouchCalibration {
  bool  swapXY = false;
  float uAtX1  = 450.0f;
  float uAtX2  = 3550.0f;
  float vAtY1  = 550.0f;
  float vAtY2  = 3450.0f;
  int   calX1  = 40;
  int   calX2  = 280;
  int   calY1  = 50;
  int   calY2  = 200;
};

static TouchCalibration gCal;

bool loadTouchCalibrationFromNvs() {
  prefs.begin("cyd_cal", true);
  bool valid = prefs.getBool("valid", false);
  if (valid) {
    gCal.swapXY = prefs.getBool("swapXY", false);
    gCal.uAtX1  = prefs.getFloat("u1", 450.0f);
    gCal.uAtX2  = prefs.getFloat("u2", 3550.0f);
    gCal.vAtY1  = prefs.getFloat("v1", 550.0f);
    gCal.vAtY2  = prefs.getFloat("v2", 3450.0f);
  }
  prefs.end();
  return valid;
}

static void saveTouchCalibrationToNvs() {
  prefs.begin("cyd_cal", false);
  prefs.putBool("valid", true);
  prefs.putBool("swapXY", gCal.swapXY);
  prefs.putFloat("u1", gCal.uAtX1);
  prefs.putFloat("u2", gCal.uAtX2);
  prefs.putFloat("v1", gCal.vAtY1);
  prefs.putFloat("v2", gCal.vAtY2);
  prefs.end();
}

void mapRawTouchToScreen(int rawX, int rawY, int& sx, int& sy) {
  float u = gCal.swapXY ? (float)rawY : (float)rawX;
  float v = gCal.swapXY ? (float)rawX : (float)rawY;
  float du = gCal.uAtX2 - gCal.uAtX1;
  float dv = gCal.vAtY2 - gCal.vAtY1;
  if (fabsf(du) < 1.0f) du = 1.0f;
  if (fabsf(dv) < 1.0f) dv = 1.0f;

  float fx = gCal.calX1 + (u - gCal.uAtX1) * (float)(gCal.calX2 - gCal.calX1) / du;
  float fy = gCal.calY1 + (v - gCal.vAtY1) * (float)(gCal.calY2 - gCal.calY1) / dv;
  sx = constrain((int)lroundf(fx), 0, LAKATOS_SCREEN_W - 1);
  sy = constrain((int)lroundf(fy), 0, LAKATOS_SCREEN_H - 1);
}

// ---------------------------------------------------------------------------
// Airport Database (Coordinates + POSIX Timezone)
// ---------------------------------------------------------------------------
struct KnownAirport {
  const char* iata;
  const char* city;
  const char* posixTz;
  float       lat;
  float       lon;
};

static const KnownAirport KNOWN_AIRPORTS[] = {
  { "ATL", "Atlanta, GA",        "EST5EDT,M3.2.0,M11.1.0",   33.6407f,  -84.4277f },
  { "SFO", "San Francisco, CA",  "PST8PDT,M3.2.0,M11.1.0",   37.6213f, -122.3790f },
  { "NYC", "New York (JFK), NY", "EST5EDT,M3.2.0,M11.1.0",   40.6413f,  -73.7781f },
  { "JFK", "New York (JFK), NY", "EST5EDT,M3.2.0,M11.1.0",   40.6413f,  -73.7781f },
  { "LAX", "Los Angeles, CA",    "PST8PDT,M3.2.0,M11.1.0",   33.9416f, -118.4085f },
  { "ORD", "Chicago O'Hare, IL", "CST6CDT,M3.2.0,M11.1.0",   41.9742f,  -87.9073f },
  { "DEN", "Denver, CO",         "MST7MDT,M3.2.0,M11.1.0",   39.8561f, -104.6737f },
  { "SEA", "Seattle-Tacoma, WA", "PST8PDT,M3.2.0,M11.1.0",   47.4502f, -122.3088f },
  { "BOS", "Boston, MA",         "EST5EDT,M3.2.0,M11.1.0",   42.3656f,  -71.0096f },
  { "DFW", "Dallas/Ft Worth, TX","CST6CDT,M3.2.0,M11.1.0",   32.8998f,  -97.0403f },
  { "MIA", "Miami, FL",          "EST5EDT,M3.2.0,M11.1.0",   25.7959f,  -80.2870f },
  { "IAH", "Houston, TX",        "CST6CDT,M3.2.0,M11.1.0",   29.9902f,  -95.3368f },
  { "PHX", "Phoenix, AZ",        "MST7",                     33.4352f, -112.0101f },
  { "LAS", "Las Vegas, NV",      "PST8PDT,M3.2.0,M11.1.0",   36.0840f, -115.1537f },
  { "MSP", "Minneapolis, MN",    "CST6CDT,M3.2.0,M11.1.0",   44.8848f,  -93.2223f },
  { "PDX", "Portland, OR",       "PST8PDT,M3.2.0,M11.1.0",   45.5898f, -122.5951f },
  { "HNL", "Honolulu, HI",       "HST10",                    21.3187f, -157.9225f },
  { "ANC", "Anchorage, AK",      "AKST9AKDT,M3.2.0,M11.1.0", 61.1743f, -149.9962f },
  { "DCA", "Washington, DC",     "EST5EDT,M3.2.0,M11.1.0",   38.8512f,  -77.0402f },
  { "IAD", "Dulles, VA",         "EST5EDT,M3.2.0,M11.1.0",   38.9531f,  -77.4565f },
  { "PHL", "Philadelphia, PA",   "EST5EDT,M3.2.0,M11.1.0",   39.8744f,  -75.2424f },
  { "CLT", "Charlotte, NC",      "EST5EDT,M3.2.0,M11.1.0",   35.2144f,  -80.9473f },
  { "MCO", "Orlando, FL",        "EST5EDT,M3.2.0,M11.1.0",   28.4312f,  -81.3081f },
  { "DTW", "Detroit, MI",        "EST5EDT,M3.2.0,M11.1.0",   42.2162f,  -83.3554f },
  { "AUS", "Austin, TX",         "CST6CDT,M3.2.0,M11.1.0",   30.1975f,  -97.6664f },
  { "SLC", "Salt Lake City, UT", "MST7MDT,M3.2.0,M11.1.0",   40.7899f, -111.9791f },
  { "SAN", "San Diego, CA",      "PST8PDT,M3.2.0,M11.1.0",   32.7338f, -117.1933f },
  { "SJC", "San Jose, CA",       "PST8PDT,M3.2.0,M11.1.0",   37.3639f, -121.9289f },
  { "OAK", "Oakland, CA",        "PST8PDT,M3.2.0,M11.1.0",   37.7126f, -122.2197f },
};
static const int NUM_KNOWN_AIRPORTS = sizeof(KNOWN_AIRPORTS) / sizeof(KNOWN_AIRPORTS[0]);

static const char* const QUICK_AIRPORTS[12] = {
  "ATL", "SFO", "NYC", "LAX",
  "ORD", "DEN", "SEA", "BOS",
  "DFW", "MIA", "PHX", "HNL"
};

// ---------------------------------------------------------------------------
// Persistent OS Configuration State (NVS "cyd_cfg" & "cyd_wifi")
// ---------------------------------------------------------------------------
static bool   gAutoBright      = true;
static int    gManualBrightPct = 85;
static int    gCurrentBrightPct = 85;
static float  gSmoothedLdr     = 1500.0f;
static uint32_t gLastLdrMs     = 0;
static int    gLastPwm         = -1;

static char   gAirportCode[5]  = "SFO";
static char   gAirportCity[24] = "San Francisco, CA";
static char   gAirportTz[32]   = "PST8PDT,M3.2.0,M11.1.0";
static float  gAirportLat      = 37.6213f;
static float  gAirportLon      = -122.3790f;
static bool   gNtpStarted      = false;

static char   gDeviceName[32]  = "test";
static char   gSerialNo[32]    = "ing";
static char   gShUrl[96]       = "http://pizga.net:1908/";
static char   gShPass[64]      = "";  // Never stored in source code; loaded from NVS

static void applyAirportCode(const char* code) {
  char up[4] = {'S', 'F', 'O', '\0'};
  for (int i = 0; i < 3 && code && code[i]; i++) {
    char c = code[i];
    if (c >= 'a' && c <= 'z') c -= 32;
    up[i] = c;
  }
  strncpy(gAirportCode, up, sizeof(gAirportCode) - 1);
  gAirportCode[3] = '\0';

  for (int i = 0; i < NUM_KNOWN_AIRPORTS; i++) {
    if (strcmp(KNOWN_AIRPORTS[i].iata, gAirportCode) == 0) {
      strncpy(gAirportCity, KNOWN_AIRPORTS[i].city, sizeof(gAirportCity) - 1);
      gAirportCity[sizeof(gAirportCity) - 1] = '\0';
      strncpy(gAirportTz, KNOWN_AIRPORTS[i].posixTz, sizeof(gAirportTz) - 1);
      gAirportTz[sizeof(gAirportTz) - 1] = '\0';
      gAirportLat = KNOWN_AIRPORTS[i].lat;
      gAirportLon = KNOWN_AIRPORTS[i].lon;
      setenv("TZ", gAirportTz, 1);
      tzset();
      return;
    }
  }
  snprintf(gAirportCity, sizeof(gAirportCity), "%s Airport", gAirportCode);
  setenv("TZ", gAirportTz, 1);
  tzset();
}

static void applyBacklightPercent(int pct) {
  pct = constrain(pct, 5, 100);
  gCurrentBrightPct = pct;
  float norm = (float)pct / 100.0f;
  int pwm = (int)lroundf(12.0f + 243.0f * powf(norm, 1.8f));
  pwm = constrain(pwm, 12, 255);
  if (pwm != gLastPwm) {
    gLastPwm = pwm;
    analogWrite(TFT_BL_PIN, pwm);
  }
}

static int computeAutoBrightPct(float ldr) {
  float clamped = constrain(ldr, 200.0f, 3500.0f);
  float t = (clamped - 200.0f) / (3500.0f - 200.0f);
  int pct = (int)lroundf(100.0f - t * 88.0f);
  return constrain(pct, 12, 100);
}

static void saveConfigToNvs() {
  prefs.begin("cyd_cfg", false);
  prefs.putBool("autoB", gAutoBright);
  prefs.putInt("manB", gManualBrightPct);
  prefs.putString("iata", gAirportCode);
  prefs.putString("devName", gDeviceName);
  prefs.putString("serialNo", gSerialNo);
  prefs.putString("shUrl", gShUrl);
  prefs.putString("shPass", gShPass);
  prefs.end();
}

void initLakatosConfig() {
  pinMode(LDR_PIN, INPUT);
  pinMode(TFT_BL_PIN, OUTPUT);

  prefs.begin("cyd_cfg", true);
  gAutoBright = prefs.getBool("autoB", true);
  gManualBrightPct = constrain((int)prefs.getInt("manB", 85), 5, 100);
  String savedIata = prefs.getString("iata", "SFO");
  String savedDev  = prefs.getString("devName", "test");
  String savedSer  = prefs.getString("serialNo", "ing");
  String savedUrl  = prefs.getString("shUrl", "http://pizga.net:1908/");
  String savedPass = prefs.getString("shPass", "");
  prefs.end();

  strncpy(gDeviceName, savedDev.c_str(), sizeof(gDeviceName) - 1);
  strncpy(gSerialNo, savedSer.c_str(), sizeof(gSerialNo) - 1);
  strncpy(gShUrl, savedUrl.c_str(), sizeof(gShUrl) - 1);
  strncpy(gShPass, savedPass.c_str(), sizeof(gShPass) - 1);

  applyAirportCode(savedIata.c_str());

  int raw = analogRead(LDR_PIN);
  gSmoothedLdr = (float)raw;
  if (gAutoBright) {
    applyBacklightPercent(computeAutoBrightPct(gSmoothedLdr));
  } else {
    applyBacklightPercent(gManualBrightPct);
  }

  // Auto-connect Wi-Fi in background if credentials saved in "cyd_wifi"
  prefs.begin("cyd_wifi", true);
  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");
  prefs.end();

  if (ssid.length() > 0) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());
    Serial.printf("[Lakatos] Auto-connecting Wi-Fi to '%s'...\n", ssid.c_str());
  }
}

void updateLakatosConfigBackground() {
  uint32_t now = millis();
  if (now - gLastLdrMs >= 100) {
    gLastLdrMs = now;
    int raw = analogRead(LDR_PIN);
    gSmoothedLdr = 0.90f * gSmoothedLdr + 0.10f * (float)raw;
    if (gAutoBright) {
      applyBacklightPercent(computeAutoBrightPct(gSmoothedLdr));
    }
  }

  if (WiFi.status() == WL_CONNECTED && !gNtpStarted) {
    gNtpStarted = true;
    configTzTime(gAirportTz, "pool.ntp.org", "time.nist.gov");
  }
}

const char* lakatosGetAirportCode() { return gAirportCode; }

bool lakatosGetAirportInfo(LakatosAirportInfo* out) {
  if (!out) return false;
  memset(out, 0, sizeof(*out));
  strncpy(out->code, gAirportCode, sizeof(out->code) - 1);
  strncpy(out->city, gAirportCity, sizeof(out->city) - 1);
  strncpy(out->tz, gAirportTz, sizeof(out->tz) - 1);
  out->lat = gAirportLat;
  out->lon = gAirportLon;
  return true;
}

const char* lakatosGetDeviceName() { return gDeviceName; }
const char* lakatosGetSerialNumber() { return gSerialNo; }
const char* lakatosGetSuperhighwayUrl() { return gShUrl; }
const char* lakatosGetSuperhighwayPassword() { return gShPass; }

void lakatosSetSuperhighwayPassword(const char* pass) {
  strncpy(gShPass, pass ? pass : "", sizeof(gShPass) - 1);
  gShPass[sizeof(gShPass) - 1] = '\0';
  saveConfigToNvs();
}

bool lakatosWifiIsConnected() { return WiFi.status() == WL_CONNECTED; }

int64_t lakatosGetUnixTime() {
  time_t now = time(nullptr);
  return (int64_t)now;
}

bool lakatosFormatLocalTime(char* buf, size_t len, const char* fmt) {
  if (!buf || len == 0) return false;
  time_t now = time(nullptr);
  struct tm ti;
  if (now < 1700000000 || !localtime_r(&now, &ti)) {
    snprintf(buf, len, "--:--");
    return false;
  }
  strftime(buf, len, (fmt && fmt[0]) ? fmt : "%H:%M:%S", &ti);
  return true;
}

// ---------------------------------------------------------------------------
// Reusable Modal Stylus Keyboard
// ---------------------------------------------------------------------------
static const char* const KB_ROWS[3][4] = {
  { "1234567890", "qwertyuiop", "asdfghjkl/", "zxcvbnm._-" },
  { "1234567890", "QWERTYUIOP", "ASDFGHJKL/", "ZXCVBNM._-" },
  { "!@#$%^&*()", "-_=+[]{}\\|", ";:'\",.<>/?", "`~$%&*+=-_" }
};

static void drawKbInputBox(const String& text, bool maskPassword) {
  tft.fillRoundRect(4, 28, 246, 30, 4, TFT_BLACK);
  tft.drawRoundRect(4, 28, 246, 30, 4, TFT_CYAN);

  String shown;
  if (maskPassword && text.length() > 0) {
    for (size_t i = 0; i + 1 < text.length(); i++) shown += '*';
    shown += text[text.length() - 1];
  } else {
    shown = text;
  }
  shown += "_";
  if (shown.length() > 26) {
    shown = ".." + shown.substring(shown.length() - 25);
  }
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(shown, 10, 43, 2);
}

static void drawKbGrid(int page) {
  tft.fillRect(0, 62, LAKATOS_SCREEN_W, LAKATOS_SCREEN_H - 62, 0x0841);

  for (int r = 0; r < 4; r++) {
    const char* rowStr = KB_ROWS[page][r];
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

  int by = 205, bh = 32;
  uint16_t shiftCol = (page == 1) ? TFT_YELLOW : 0x2945;
  tft.fillRoundRect(2, by, 66, bh, 4, shiftCol);
  tft.drawRoundRect(2, by, 66, bh, 4, TFT_YELLOW);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor((page == 1) ? TFT_BLACK : TFT_YELLOW, shiftCol);
  tft.drawString((page == 1) ? "abc" : "ABC", 35, by + 16, 2);

  uint16_t symCol = (page == 2) ? TFT_MAGENTA : 0x2945;
  tft.fillRoundRect(72, by, 62, bh, 4, symCol);
  tft.drawRoundRect(72, by, 62, bh, 4, TFT_MAGENTA);
  tft.setTextColor((page == 2) ? TFT_BLACK : TFT_MAGENTA, symCol);
  tft.drawString((page == 2) ? "abc" : "#+=", 103, by + 16, 2);

  tft.fillRoundRect(138, by, 90, bh, 4, 0x18E3);
  tft.drawRoundRect(138, by, 90, bh, 4, TFT_WHITE);
  tft.setTextColor(TFT_WHITE, 0x18E3);
  tft.drawString("SPACE", 183, by + 16, 2);

  tft.fillRoundRect(232, by, 85, bh, 4, TFT_DARKGREEN);
  tft.drawRoundRect(232, by, 85, bh, 4, TFT_GREEN);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREEN);
  tft.drawString("OK", 274, by + 16, 2);
}

int lakatosPromptKeyboard(const char* title, char* buf, size_t maxLen,
                          bool allowCtrlC, bool maskPassword) {
  if (!buf || maxLen == 0) return 0;
  String text = String(buf);
  int page = 0;

  tft.fillScreen(0x0841);
  tft.fillRect(0, 0, LAKATOS_SCREEN_W, 25, TFT_NAVY);

  tft.fillRoundRect(2, 2, 56, 20, 4, 0x2945);
  tft.drawRoundRect(2, 2, 56, 20, 4, TFT_CYAN);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_CYAN, 0x2945);
  tft.drawString("CANCEL", 30, 12, 1);

  if (allowCtrlC) {
    tft.fillRoundRect(62, 2, 48, 20, 4, 0xA800);
    tft.drawRoundRect(62, 2, 48, 20, 4, TFT_RED);
    tft.setTextColor(TFT_WHITE, 0xA800);
    tft.drawString("^C", 86, 12, 2);
  }

  tft.setTextColor(TFT_YELLOW, TFT_NAVY);
  tft.drawString(title ? title : "INPUT", allowCtrlC ? 172 : 148, 12, 2);

  tft.fillRoundRect(236, 2, 81, 20, 4, TFT_DARKGREEN);
  tft.drawRoundRect(236, 2, 81, 20, 4, TFT_GREEN);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREEN);
  tft.drawString("SAVE/OK", 276, 12, 1);

  tft.fillRoundRect(254, 28, 63, 30, 4, 0x4800);
  tft.drawRoundRect(254, 28, 63, 30, 4, TFT_RED);
  tft.setTextColor(TFT_WHITE, 0x4800);
  tft.drawString("DEL", 285, 43, 2);

  drawKbInputBox(text, maskPassword);
  drawKbGrid(page);

  // Wait for touch release before accepting new taps
  while (lakatosReadTouch().touched) delay(15);

  bool wasDown = false;
  uint32_t lastRel = millis();

  while (true) {
    updateLakatosConfigBackground();
    LakatosTouchSample t = lakatosReadTouch();
    uint32_t now = millis();

    if (t.touched) {
      if (!wasDown && (now - lastRel > 90)) {
        wasDown = true;
        if (t.y <= 25) {
          if (t.x <= 60) return 0;  // Cancelled
          if (allowCtrlC && t.x >= 62 && t.x <= 112) return -1;  // ^C
          if (t.x >= 232) {
            strncpy(buf, text.c_str(), maxLen - 1);
            buf[maxLen - 1] = '\0';
            return 1;
          }
        } else if (t.y >= 26 && t.y <= 60) {
          if (t.x >= 250 && text.length() > 0) {
            text.remove(text.length() - 1);
            drawKbInputBox(text, maskPassword);
          }
        } else if (t.y >= 64 && t.y < 204) {
          int r = (t.y - 64) / 35;
          int c = t.x / 32;
          if (r >= 0 && r < 4 && c >= 0 && c < 10) {
            if (text.length() + 1 < maxLen) {
              char ch = KB_ROWS[page][r][c];
              text += ch;
              drawKbInputBox(text, maskPassword);
            }
          }
        } else if (t.y >= 205) {
          if (t.x < 70) {
            page = (page == 1) ? 0 : 1;
            drawKbGrid(page);
          } else if (t.x < 136) {
            page = (page == 2) ? 0 : 2;
            drawKbGrid(page);
          } else if (t.x < 230) {
            if (text.length() + 1 < maxLen) {
              text += ' ';
              drawKbInputBox(text, maskPassword);
            }
          } else {
            strncpy(buf, text.c_str(), maxLen - 1);
            buf[maxLen - 1] = '\0';
            return 1;
          }
        }
      }
    } else {
      if (wasDown) {
        wasDown = false;
        lastRel = now;
      }
    }
    delay(12);
  }
}

// ---------------------------------------------------------------------------
// 3-Point Touchscreen Calibration Modal
// ---------------------------------------------------------------------------
static void drawCrosshair(int x, int y, uint16_t color, const char* label) {
  tft.drawCircle(x, y, 16, color);
  tft.drawCircle(x, y, 8, color);
  tft.fillCircle(x, y, 3, TFT_WHITE);
  tft.drawFastHLine(x - 22, y, 44, color);
  tft.drawFastVLine(x, y - 22, 44, color);
  tft.setTextColor(color, TFT_BLACK);
  tft.setTextDatum(TC_DATUM);
  tft.drawString(label, x, (y < 120) ? (y + 22) : (y - 34), 2);
}

static void runTouchCalibrationModal() {
  int rawX[3] = {0, 0, 0};
  int rawY[3] = {0, 0, 0};
  const int tx[3] = {gCal.calX1, gCal.calX2, gCal.calX2};
  const int ty[3] = {gCal.calY1, gCal.calY1, gCal.calY2};
  const char* labels[3] = {"TAP 1: TOP-LEFT", "TAP 2: TOP-RIGHT", "TAP 3: BOTTOM-RIGHT"};
  const uint16_t cols[3] = {TFT_CYAN, TFT_YELLOW, TFT_GREEN};

  for (int step = 0; step < 3; step++) {
    tft.fillScreen(TFT_BLACK);
    tft.fillRect(0, 0, LAKATOS_SCREEN_W, 24, TFT_NAVY);
    tft.setTextColor(TFT_WHITE, TFT_NAVY);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("LAKATOS TOUCH CALIBRATION", 160, 12, 2);
    drawCrosshair(tx[step], ty[step], cols[step], labels[step]);

    while (lakatosReadTouch().touched) delay(20);
    while (true) {
      LakatosTouchSample t = lakatosReadTouch();
      if (t.touched) {
        rawX[step] = t.rawX;
        rawY[step] = t.rawY;
        lakatosSetRgbLed(false, true, false, 80);
        delay(220);
        break;
      }
      delay(15);
    }
  }

  int dx12 = abs(rawX[1] - rawX[0]);
  int dy12 = abs(rawY[1] - rawY[0]);
  gCal.swapXY = (dy12 > dx12);
  gCal.uAtX1  = gCal.swapXY ? (float)rawY[0] : (float)rawX[0];
  gCal.uAtX2  = gCal.swapXY ? (float)rawY[1] : (float)rawX[1];
  gCal.vAtY1  = gCal.swapXY ? (float)rawX[0] : (float)rawY[0];
  gCal.vAtY2  = gCal.swapXY ? (float)rawX[2] : (float)rawY[2];
  saveTouchCalibrationToNvs();
}

// ---------------------------------------------------------------------------
// Wi-Fi Network Scanner & Config Modal
// ---------------------------------------------------------------------------
static bool runWifiConfigModal() {
  String ssids[5];
  int32_t rssis[5];
  int count = 0;

  auto scanNow = [&]() {
    tft.fillScreen(0x0841);
    tft.fillRect(0, 0, LAKATOS_SCREEN_W, 24, TFT_NAVY);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_NAVY);
    tft.drawString("SCANNING WI-FI...", 160, 12, 2);
    WiFi.mode(WIFI_STA);
    int n = WiFi.scanNetworks(false, false);
    count = 0;
    for (int i = 0; i < n && count < 5; i++) {
      String s = WiFi.SSID(i);
      if (s.length() == 0) continue;
      bool dup = false;
      for (int j = 0; j < count; j++) {
        if (ssids[j] == s) dup = true;
      }
      if (!dup) {
        ssids[count] = s;
        rssis[count] = WiFi.RSSI(i);
        count++;
      }
    }
    WiFi.scanDelete();
  };

  auto drawWifiList = [&]() {
    tft.fillScreen(0x0841);
    tft.fillRect(0, 0, LAKATOS_SCREEN_W, 25, TFT_NAVY);
    tft.fillRoundRect(2, 2, 58, 20, 4, 0x2945);
    tft.drawRoundRect(2, 2, 58, 20, 4, TFT_CYAN);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_CYAN, 0x2945);
    tft.drawString("HOME", 31, 12, 2);

    tft.setTextColor(TFT_WHITE, TFT_NAVY);
    tft.drawString("WI-FI NETWORKS", 155, 12, 2);

    tft.fillRoundRect(254, 2, 64, 20, 4, 0x2945);
    tft.drawRoundRect(254, 2, 64, 20, 4, TFT_YELLOW);
    tft.setTextColor(TFT_YELLOW, 0x2945);
    tft.drawString("RESCAN", 286, 12, 1);

    tft.setTextDatum(ML_DATUM);
    if (WiFi.status() == WL_CONNECTED) {
      tft.setTextColor(TFT_GREEN, 0x0841);
      tft.drawString("Connected: " + WiFi.SSID() + " (" + WiFi.localIP().toString() + ")", 6, 36, 1);
    } else {
      tft.setTextColor(TFT_ORANGE, 0x0841);
      tft.drawString("Not connected - tap a network below:", 6, 36, 1);
    }

    for (int i = 0; i < count; i++) {
      int ry = 48 + i * 36;
      tft.fillRoundRect(6, ry, 308, 32, 5, 0x18E3);
      tft.drawRoundRect(6, ry, 308, 32, 5, TFT_CYAN);
      tft.setTextDatum(ML_DATUM);
      tft.setTextColor(TFT_WHITE, 0x18E3);
      tft.drawString(ssids[i].substring(0, 24), 14, ry + 16, 2);
      tft.setTextDatum(MR_DATUM);
      tft.setTextColor(TFT_LIGHTGREY, 0x18E3);
      tft.drawString(String(rssis[i]) + "dBm", 306, ry + 16, 1);
    }
  };

  scanNow();
  drawWifiList();
  while (lakatosReadTouch().touched) delay(15);

  bool wasDown = false;
  uint32_t lastRel = millis();
  while (true) {
    LakatosTouchSample t = lakatosReadTouch();
    uint32_t now = millis();
    if (t.touched) {
      if (!wasDown && (now - lastRel > 100)) {
        wasDown = true;
        if (t.y <= 25) {
          if (t.x <= 62) return true;
          if (t.x >= 250) {
            scanNow();
            drawWifiList();
          }
        } else if (t.y >= 48) {
          int idx = (t.y - 48) / 36;
          if (idx >= 0 && idx < count) {
            char passBuf[64] = "";
            String title = "PW: " + ssids[idx].substring(0, 14);
            if (lakatosPromptKeyboard(title.c_str(), passBuf, sizeof(passBuf), false, false) == 1) {
              prefs.begin("cyd_wifi", false);
              prefs.putString("ssid", ssids[idx]);
              prefs.putString("pass", passBuf);
              prefs.end();
              tft.fillScreen(0x0841);
              tft.setTextDatum(MC_DATUM);
              tft.setTextColor(TFT_YELLOW, 0x0841);
              tft.drawString("Connecting to " + ssids[idx] + "...", 160, 120, 2);
              WiFi.disconnect();
              WiFi.begin(ssids[idx].c_str(), passBuf);
              uint32_t startMs = millis();
              while (WiFi.status() != WL_CONNECTED && (millis() - startMs < 9000)) {
                delay(200);
              }
              return false;
            }
            drawWifiList();
          }
        }
      }
    } else {
      if (wasDown) {
        wasDown = false;
        lastRel = now;
      }
    }
    delay(15);
  }
}

// ---------------------------------------------------------------------------
// Airport Selector Modal
// ---------------------------------------------------------------------------
static bool runAirportSelectorModal() {
  auto drawAirportModal = [&]() {
    tft.fillScreen(0x0841);
    tft.fillRect(0, 0, LAKATOS_SCREEN_W, 25, TFT_NAVY);

    tft.fillRoundRect(2, 2, 58, 20, 4, 0x2945);
    tft.drawRoundRect(2, 2, 58, 20, 4, TFT_CYAN);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_CYAN, 0x2945);
    tft.drawString("HOME", 31, 12, 2);

    tft.setTextColor(TFT_WHITE, TFT_NAVY);
    tft.drawString("SELECT AIRPORT", 145, 12, 2);

    tft.fillRoundRect(232, 2, 85, 20, 4, 0x2945);
    tft.drawRoundRect(232, 2, 85, 20, 4, TFT_YELLOW);
    tft.setTextColor(TFT_YELLOW, 0x2945);
    tft.drawString("TYPE CODE", 274, 12, 1);

    for (int i = 0; i < 12; i++) {
      int r = i / 4;
      int c = i % 4;
      int bx = 6 + c * 78;
      int by = 36 + r * 64;
      bool sel = (strcmp(QUICK_AIRPORTS[i], gAirportCode) == 0);
      uint16_t bg = sel ? TFT_DARKGREEN : 0x18E3;
      uint16_t fg = sel ? TFT_GREEN : TFT_CYAN;
      tft.fillRoundRect(bx, by, 72, 56, 6, bg);
      tft.drawRoundRect(bx, by, 72, 56, 6, fg);
      tft.setTextDatum(MC_DATUM);
      tft.setTextColor(TFT_WHITE, bg);
      tft.drawString(QUICK_AIRPORTS[i], bx + 36, by + 28, 4);
    }
  };

  drawAirportModal();
  while (lakatosReadTouch().touched) delay(15);

  bool wasDown = false;
  uint32_t lastRel = millis();
  while (true) {
    LakatosTouchSample t = lakatosReadTouch();
    uint32_t now = millis();
    if (t.touched) {
      if (!wasDown && (now - lastRel > 100)) {
        wasDown = true;
        if (t.y <= 25) {
          if (t.x <= 62) return true;
          if (t.x >= 230) {
            char codeBuf[8] = "";
            strncpy(codeBuf, gAirportCode, sizeof(codeBuf) - 1);
            if (lakatosPromptKeyboard("3-LETTER IATA", codeBuf, 4, false, false) == 1 &&
                strlen(codeBuf) == 3) {
              applyAirportCode(codeBuf);
              saveConfigToNvs();
              return false;
            }
            drawAirportModal();
          }
        } else if (t.y >= 36) {
          int r = (t.y - 36) / 64;
          int c = (t.x - 6) / 78;
          if (r >= 0 && r < 3 && c >= 0 && c < 4) {
            applyAirportCode(QUICK_AIRPORTS[r * 4 + c]);
            saveConfigToNvs();
            return false;
          }
        }
      }
    } else {
      if (wasDown) {
        wasDown = false;
        lastRel = now;
      }
    }
    delay(15);
  }
}

// ---------------------------------------------------------------------------
// ABOUT Lakatos Operating System Modal
// ---------------------------------------------------------------------------
static bool runAboutModal() {
  tft.fillScreen(0x0841);
  tft.fillRect(0, 0, LAKATOS_SCREEN_W, 24, TFT_NAVY);

  // [ HOME ] (top-left -> goes back to index page)
  tft.fillRoundRect(2, 2, 58, 20, 4, 0x2945);
  tft.drawRoundRect(2, 2, 58, 20, 4, TFT_CYAN);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_CYAN, 0x2945);
  tft.drawString("HOME", 31, 12, 2);

  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.drawString("ABOUT", 160, 12, 2);

  // [ CONFIG ] (top-right -> returns to CONFIG page)
  tft.fillRoundRect(256, 2, 62, 20, 4, TFT_MAROON);
  tft.drawRoundRect(256, 2, 62, 20, 4, TFT_WHITE);
  tft.setTextColor(TFT_WHITE, TFT_MAROON);
  tft.drawString("CONFIG", 287, 12, 1);

  // Main Card
  tft.fillRoundRect(6, 30, 308, 204, 6, 0x1082);
  tft.drawRoundRect(6, 30, 308, 204, 6, TFT_CYAN);

  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_CYAN, 0x1082);
  tft.drawString("Lakatos Operating System", 160, 38, 2);

  tft.setTextColor(TFT_YELLOW, 0x1082);
  tft.drawString("YAK-CYD  |  ESP32-2432S028R (240 MHz)", 160, 58, 1);

  tft.drawFastHLine(16, 72, 288, 0x39E7);

  tft.setTextDatum(TL_DATUM);
  int y = 80;
  auto drawRow = [&](const char* label, const String& val, uint16_t valCol) {
    tft.setTextColor(TFT_LIGHTGREY, 0x1082);
    tft.drawString(label, 16, y, 2);
    tft.setTextColor(valCol, 0x1082);
    tft.drawString(val, 108, y, 2);
    y += 22;
  };

  drawRow("OS / API:", "Lakatos OS v" + String(LAKATOS_API_VERSION), TFT_WHITE);
  drawRow("Runtime:", "LCode 32-Bit VM + FPU", TFT_GREEN);
  drawRow("Device ID:", String(gDeviceName) + String(gSerialNo), TFT_YELLOW);
  drawRow("Airport:", String(gAirportCode) + " (" + String(gAirportCity) + ")", TFT_CYAN);
  drawRow("Free RAM:", String(ESP.getFreeHeap() / 1024) + " KB SRAM", TFT_WHITE);
  drawRow("/sh URL:", String(gShUrl).substring(0, 24), TFT_MAGENTA);

  while (lakatosReadTouch().touched) delay(15);

  bool wasDown = false;
  uint32_t lastRel = millis();
  while (true) {
    updateLakatosConfigBackground();
    LakatosTouchSample t = lakatosReadTouch();
    uint32_t now = millis();
    if (t.touched) {
      if (!wasDown && (now - lastRel > 100)) {
        wasDown = true;
        if (t.y <= 25) {
          if (t.x <= 62) return true;    // HOME -> index page
          if (t.x >= 252) return false;  // CONFIG -> back to config page
        }
      }
    } else {
      if (wasDown) {
        wasDown = false;
        lastRel = now;
      }
    }
    delay(15);
  }
}

// ---------------------------------------------------------------------------
// Main CONFIG Screen
// ---------------------------------------------------------------------------
static void drawConfigScreen() {
  tft.fillScreen(0x0841);
  tft.fillRect(0, 0, LAKATOS_SCREEN_W, 24, TFT_NAVY);

  // [ HOME ] (top-left -> goes back to index page)
  tft.fillRoundRect(2, 2, 58, 20, 4, 0x2945);
  tft.drawRoundRect(2, 2, 58, 20, 4, TFT_CYAN);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_CYAN, 0x2945);
  tft.drawString("HOME", 31, 12, 2);

  tft.setTextColor(TFT_WHITE, TFT_NAVY);
  tft.drawString("CONFIG", 160, 12, 2);

  // [ ABOUT ] (top-right -> Lakatos Operating System info)
  tft.fillRoundRect(258, 2, 60, 20, 4, TFT_PURPLE);
  tft.drawRoundRect(258, 2, 60, 20, 4, TFT_WHITE);
  tft.setTextColor(TFT_WHITE, TFT_PURPLE);
  tft.drawString("ABOUT", 288, 12, 2);

  // Row 1 (y: 28..66): Brightness Slider + [AUTO] toggle
  tft.fillRoundRect(4, 28, 312, 38, 5, 0x1082);
  tft.drawRoundRect(4, 28, 312, 38, 5, 0x39E7);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(TFT_YELLOW, 0x1082);
  tft.drawString("BRIGHT:", 10, 47, 1);

  // Slider track (x: 58..232, y: 41..53)
  int trackX = 58, trackW = 174;
  tft.fillRoundRect(trackX, 41, trackW, 12, 4, TFT_BLACK);
  int fillW = (trackW * gCurrentBrightPct) / 100;
  tft.fillRoundRect(trackX, 41, fillW, 12, 4, gAutoBright ? TFT_CYAN : TFT_ORANGE);
  tft.drawRoundRect(trackX, 41, trackW, 12, 4, TFT_WHITE);

  // [AUTO] button (x: 240..310, y: 33..61)
  uint16_t autoBg = gAutoBright ? TFT_DARKGREEN : 0x2945;
  tft.fillRoundRect(240, 33, 70, 28, 4, autoBg);
  tft.drawRoundRect(240, 33, 70, 28, 4, gAutoBright ? TFT_GREEN : TFT_LIGHTGREY);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, autoBg);
  tft.drawString(gAutoBright ? "AUTO:ON" : "AUTO:OFF", 275, 47, 1);

  // Row 2 (y: 70..118): [DEV NAME] (x: 4..157) & [SERIAL #] (x: 163..316)
  tft.fillRoundRect(4, 70, 153, 46, 5, 0x18E3);
  tft.drawRoundRect(4, 70, 153, 46, 5, TFT_CYAN);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_CYAN, 0x18E3);
  tft.drawString("DEVICE NAME:", 10, 75, 1);
  tft.setTextColor(TFT_WHITE, 0x18E3);
  tft.drawString(gDeviceName, 10, 90, 2);

  tft.fillRoundRect(163, 70, 153, 46, 5, 0x18E3);
  tft.drawRoundRect(163, 70, 153, 46, 5, TFT_CYAN);
  tft.setTextColor(TFT_CYAN, 0x18E3);
  tft.drawString("SERIAL NUMBER:", 169, 75, 1);
  tft.setTextColor(TFT_WHITE, 0x18E3);
  tft.drawString(gSerialNo, 169, 90, 2);

  // Row 3 (y: 120..168): [/sh URL] (x: 4..198) & [/sh PASS] (x: 204..316)
  tft.fillRoundRect(4, 120, 194, 46, 5, 0x18E3);
  tft.drawRoundRect(4, 120, 194, 46, 5, TFT_MAGENTA);
  tft.setTextColor(TFT_MAGENTA, 0x18E3);
  tft.drawString("/sh SERVER URL:", 10, 125, 1);
  tft.setTextColor(TFT_WHITE, 0x18E3);
  String shortUrl = String(gShUrl);
  if (shortUrl.length() > 28) shortUrl = shortUrl.substring(0, 28);
  tft.drawString(shortUrl, 10, 142, 1);

  tft.fillRoundRect(204, 120, 112, 46, 5, 0x18E3);
  tft.drawRoundRect(204, 120, 112, 46, 5, TFT_MAGENTA);
  tft.setTextColor(TFT_MAGENTA, 0x18E3);
  tft.drawString("/sh PASSWORD:", 210, 125, 1);
  tft.setTextColor(gShPass[0] ? TFT_GREEN : TFT_ORANGE, 0x18E3);
  tft.drawString(gShPass[0] ? "******** (SET)" : "(NOT SET)", 210, 142, 1);

  // Row 4 (y: 172..234): [AIRPORT] (x: 4..104), [WI-FI] (x: 109..210), [TOUCH CAL] (x: 215..316)
  tft.fillRoundRect(4, 172, 100, 62, 6, 0x18E3);
  tft.drawRoundRect(4, 172, 100, 62, 6, TFT_YELLOW);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_YELLOW, 0x18E3);
  tft.drawString("AIRPORT", 54, 186, 1);
  tft.setTextColor(TFT_WHITE, 0x18E3);
  tft.drawString(gAirportCode, 54, 210, 4);

  tft.fillRoundRect(109, 172, 101, 62, 6, 0x18E3);
  tft.drawRoundRect(109, 172, 101, 62, 6, TFT_GREEN);
  tft.setTextColor(TFT_GREEN, 0x18E3);
  tft.drawString("WI-FI SETUP", 159, 190, 1);
  tft.setTextColor(TFT_WHITE, 0x18E3);
  tft.drawString((WiFi.status() == WL_CONNECTED) ? "ONLINE" : "CONFIGURE", 159, 212, 2);

  tft.fillRoundRect(215, 172, 101, 62, 6, 0x18E3);
  tft.drawRoundRect(215, 172, 101, 62, 6, TFT_ORANGE);
  tft.setTextColor(TFT_ORANGE, 0x18E3);
  tft.drawString("TOUCHSCREEN", 265, 190, 1);
  tft.setTextColor(TFT_WHITE, 0x18E3);
  tft.drawString("CALIBRATE", 265, 212, 2);
}

void runLakatosConfigScreen(bool startInCalibration) {
  if (startInCalibration) {
    runTouchCalibrationModal();
  }
  drawConfigScreen();
  while (lakatosReadTouch().touched) delay(15);

  bool wasDown = false;
  uint32_t lastRel = millis();

  while (true) {
    updateLakatosConfigBackground();
    LakatosTouchSample t = lakatosReadTouch();
    uint32_t now = millis();

    if (t.touched) {
      // Allow live dragging on the brightness slider
      if (t.y >= 28 && t.y <= 66 && t.x >= 54 && t.x <= 236) {
        gAutoBright = false;
        int pct = ((t.x - 58) * 100) / 174;
        gManualBrightPct = constrain(pct, 5, 100);
        applyBacklightPercent(gManualBrightPct);
        saveConfigToNvs();
        drawConfigScreen();
      } else if (!wasDown && (now - lastRel > 100)) {
        wasDown = true;
        if (t.y <= 25 && t.x <= 62) {
          return;  // HOME -> Exit Config back to YAK-CYD index page
        } else if (t.y <= 25 && t.x >= 254) {
          if (runAboutModal()) return;
          drawConfigScreen();
        } else if (t.y >= 28 && t.y <= 66 && t.x >= 238) {
          gAutoBright = !gAutoBright;
          if (gAutoBright) {
            applyBacklightPercent(computeAutoBrightPct(gSmoothedLdr));
          } else {
            applyBacklightPercent(gManualBrightPct);
          }
          saveConfigToNvs();
          drawConfigScreen();
        } else if (t.y >= 70 && t.y <= 116) {
          if (t.x < 160) {
            if (lakatosPromptKeyboard("DEVICE NAME", gDeviceName, sizeof(gDeviceName), false, false) == 1) {
              saveConfigToNvs();
            }
          } else {
            if (lakatosPromptKeyboard("SERIAL NUMBER", gSerialNo, sizeof(gSerialNo), false, false) == 1) {
              saveConfigToNvs();
            }
          }
          drawConfigScreen();
        } else if (t.y >= 120 && t.y <= 166) {
          if (t.x < 200) {
            if (lakatosPromptKeyboard("SUPERHIGHWAY URL", gShUrl, sizeof(gShUrl), false, false) == 1) {
              saveConfigToNvs();
            }
          } else {
            if (lakatosPromptKeyboard("/sh PASSWORD", gShPass, sizeof(gShPass), false, true) == 1) {
              saveConfigToNvs();
            }
          }
          drawConfigScreen();
        } else if (t.y >= 172) {
          if (t.x <= 106) {
            if (runAirportSelectorModal()) return;
          } else if (t.x <= 212) {
            if (runWifiConfigModal()) return;
          } else {
            runTouchCalibrationModal();
          }
          drawConfigScreen();
        }
      }
    } else {
      if (wasDown) {
        wasDown = false;
        lastRel = now;
      }
    }
    delay(15);
  }
}
