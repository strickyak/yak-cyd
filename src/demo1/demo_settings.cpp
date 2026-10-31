#include "cyd_shell.h"
#include <Preferences.h>
#include <WiFi.h>
#include <time.h>
#include <math.h>

// ---------------------------------------------------------------------------
// SETTINGS SUB-MENU (Brightness + LDR Auto, Home Airport + Timezone,
// Wi-Fi Config, and Touchscreen Calibration)
// ---------------------------------------------------------------------------

struct AirportInfo {
  const char *iata;     // 3-letter code ("ATL", "SFO", etc.)
  const char *icao;     // 4-letter NOAA METAR code ("KATL", "KSFO", "PHNL")
  const char *city;     // City & State
  const char *tzShort;  // Short TZ name ("ET", "CT", "MT", "AZ", "PT", "AKT", "HST")
  const char *posixTz;  // POSIX TZ string for ESP32 configTzTime()
  int stdOffsetMin;     // Standard time offset from UTC in minutes (e.g. -300 for EST)
  int dstOffsetMin;     // Daylight saving offset from UTC in minutes (e.g. -240 for EDT)
};

static const AirportInfo KNOWN_AIRPORTS[] = {
  // Featured 7 + Major US Hubs
  { "ATL", "KATL", "Atlanta, GA",          "ET",  "EST5EDT,M3.2.0,M11.1.0",   -300, -240 },
  { "SFO", "KSFO", "San Francisco, CA",    "PT",  "PST8PDT,M3.2.0,M11.1.0",   -480, -420 },
  { "NYC", "KJFK", "New York (JFK), NY",   "ET",  "EST5EDT,M3.2.0,M11.1.0",   -300, -240 },
  { "JFK", "KJFK", "New York (JFK), NY",   "ET",  "EST5EDT,M3.2.0,M11.1.0",   -300, -240 },
  { "LAX", "KLAX", "Los Angeles, CA",      "PT",  "PST8PDT,M3.2.0,M11.1.0",   -480, -420 },
  { "ORD", "KORD", "Chicago O'Hare, IL",   "CT",  "CST6CDT,M3.2.0,M11.1.0",   -360, -300 },
  { "DEN", "KDEN", "Denver, CO",           "MT",  "MST7MDT,M3.2.0,M11.1.0",   -420, -360 },
  { "SEA", "KSEA", "Seattle-Tacoma, WA",   "PT",  "PST8PDT,M3.2.0,M11.1.0",   -480, -420 },
  { "BOS", "KBOS", "Boston, MA",           "ET",  "EST5EDT,M3.2.0,M11.1.0",   -300, -240 },
  { "DFW", "KDFW", "Dallas/Ft Worth, TX",  "CT",  "CST6CDT,M3.2.0,M11.1.0",   -360, -300 },
  { "MIA", "KMIA", "Miami, FL",            "ET",  "EST5EDT,M3.2.0,M11.1.0",   -300, -240 },
  { "IAH", "KIAH", "Houston, TX",          "CT",  "CST6CDT,M3.2.0,M11.1.0",   -360, -300 },
  { "PHX", "KPHX", "Phoenix, AZ",          "AZ",  "MST7",                     -420, -420 },
  { "LAS", "KLAS", "Las Vegas, NV",        "PT",  "PST8PDT,M3.2.0,M11.1.0",   -480, -420 },
  { "PDX", "KPDX", "Portland, OR",         "PT",  "PST8PDT,M3.2.0,M11.1.0",   -480, -420 },
  { "HNL", "PHNL", "Honolulu, HI",         "HST", "HST10",                    -600, -600 },
  // Additional US airports recognized when typed in custom 3-letter keypad
  { "LGA", "KLGA", "New York (LGA), NY",   "ET",  "EST5EDT,M3.2.0,M11.1.0",   -300, -240 },
  { "EWR", "KEWR", "Newark, NJ",           "ET",  "EST5EDT,M3.2.0,M11.1.0",   -300, -240 },
  { "DCA", "KDCA", "Washington (DCA), DC", "ET",  "EST5EDT,M3.2.0,M11.1.0",   -300, -240 },
  { "IAD", "KIAD", "Washington (IAD), VA", "ET",  "EST5EDT,M3.2.0,M11.1.0",   -300, -240 },
  { "BWI", "KBWI", "Baltimore, MD",        "ET",  "EST5EDT,M3.2.0,M11.1.0",   -300, -240 },
  { "PHL", "KPHL", "Philadelphia, PA",     "ET",  "EST5EDT,M3.2.0,M11.1.0",   -300, -240 },
  { "CLT", "KCLT", "Charlotte, NC",        "ET",  "EST5EDT,M3.2.0,M11.1.0",   -300, -240 },
  { "RDU", "KRDU", "Raleigh-Durham, NC",   "ET",  "EST5EDT,M3.2.0,M11.1.0",   -300, -240 },
  { "MCO", "KMCO", "Orlando, FL",          "ET",  "EST5EDT,M3.2.0,M11.1.0",   -300, -240 },
  { "TPA", "KTPA", "Tampa, FL",            "ET",  "EST5EDT,M3.2.0,M11.1.0",   -300, -240 },
  { "DTW", "KDTW", "Detroit, MI",          "ET",  "EST5EDT,M3.2.0,M11.1.0",   -300, -240 },
  { "PIT", "KPIT", "Pittsburgh, PA",       "ET",  "EST5EDT,M3.2.0,M11.1.0",   -300, -240 },
  { "CLE", "KCLE", "Cleveland, OH",        "ET",  "EST5EDT,M3.2.0,M11.1.0",   -300, -240 },
  { "CMH", "KCMH", "Columbus, OH",         "ET",  "EST5EDT,M3.2.0,M11.1.0",   -300, -240 },
  { "CVG", "KCVG", "Cincinnati, OH",       "ET",  "EST5EDT,M3.2.0,M11.1.0",   -300, -240 },
  { "IND", "KIND", "Indianapolis, IN",     "ET",  "EST5EDT,M3.2.0,M11.1.0",   -300, -240 },
  { "MSP", "KMSP", "Minneapolis, MN",      "CT",  "CST6CDT,M3.2.0,M11.1.0",   -360, -300 },
  { "MDW", "KMDW", "Chicago Midway, IL",   "CT",  "CST6CDT,M3.2.0,M11.1.0",   -360, -300 },
  { "STL", "KSTL", "St. Louis, MO",        "CT",  "CST6CDT,M3.2.0,M11.1.0",   -360, -300 },
  { "MCI", "KMCI", "Kansas City, MO",      "CT",  "CST6CDT,M3.2.0,M11.1.0",   -360, -300 },
  { "BNA", "KBNA", "Nashville, TN",        "CT",  "CST6CDT,M3.2.0,M11.1.0",   -360, -300 },
  { "MEM", "KMEM", "Memphis, TN",          "CT",  "CST6CDT,M3.2.0,M11.1.0",   -360, -300 },
  { "MSY", "KMSY", "New Orleans, LA",      "CT",  "CST6CDT,M3.2.0,M11.1.0",   -360, -300 },
  { "AUS", "KAUS", "Austin, TX",           "CT",  "CST6CDT,M3.2.0,M11.1.0",   -360, -300 },
  { "SAT", "KSAT", "San Antonio, TX",      "CT",  "CST6CDT,M3.2.0,M11.1.0",   -360, -300 },
  { "DAL", "KDAL", "Dallas Love, TX",      "CT",  "CST6CDT,M3.2.0,M11.1.0",   -360, -300 },
  { "HOU", "KHOU", "Houston Hobby, TX",    "CT",  "CST6CDT,M3.2.0,M11.1.0",   -360, -300 },
  { "SLC", "KSLC", "Salt Lake City, UT",   "MT",  "MST7MDT,M3.2.0,M11.1.0",   -420, -360 },
  { "ABQ", "KABQ", "Albuquerque, NM",      "MT",  "MST7MDT,M3.2.0,M11.1.0",   -420, -360 },
  { "BOI", "KBOI", "Boise, ID",            "MT",  "MST7MDT,M3.2.0,M11.1.0",   -420, -360 },
  { "TUS", "KTUS", "Tucson, AZ",           "AZ",  "MST7",                     -420, -420 },
  { "SAN", "KSAN", "San Diego, CA",        "PT",  "PST8PDT,M3.2.0,M11.1.0",   -480, -420 },
  { "SJC", "KSJC", "San Jose, CA",         "PT",  "PST8PDT,M3.2.0,M11.1.0",   -480, -420 },
  { "OAK", "KOAK", "Oakland, CA",          "PT",  "PST8PDT,M3.2.0,M11.1.0",   -480, -420 },
  { "SMF", "KSMF", "Sacramento, CA",       "PT",  "PST8PDT,M3.2.0,M11.1.0",   -480, -420 },
  { "SNA", "KSNA", "Orange County, CA",    "PT",  "PST8PDT,M3.2.0,M11.1.0",   -480, -420 },
  { "BUR", "KBUR", "Burbank, CA",          "PT",  "PST8PDT,M3.2.0,M11.1.0",   -480, -420 },
  { "ONT", "KONT", "Ontario, CA",          "PT",  "PST8PDT,M3.2.0,M11.1.0",   -480, -420 },
  { "RNO", "KRNO", "Reno, NV",             "PT",  "PST8PDT,M3.2.0,M11.1.0",   -480, -420 },
  { "ANC", "PANC", "Anchorage, AK",        "AKT", "AKST9AKDT,M3.2.0,M11.1.0", -540, -480 },
};
static const int NUM_KNOWN_AIRPORTS = sizeof(KNOWN_AIRPORTS) / sizeof(KNOWN_AIRPORTS[0]);

// 16 Quick-pick airports shown in the 4x4 grid
static const char *const QUICK_AIRPORTS[16] = {
  "ATL", "SFO", "NYC", "LAX",
  "ORD", "DEN", "SEA", "BOS",
  "DFW", "MIA", "IAH", "PHX",
  "LAS", "MSP", "PDX", "HNL"
};

static Preferences cfgPrefs;

// Brightness state
static bool autoBright = true; // ON by default per user request
static int manualBrightPct = 85;
static int currentBrightPct = 85;
static float smoothedLdr = 1800.0f;
static uint32_t lastLdrSampleMs = 0;
static int lastAppliedPwm = -1;

// Home Airport & Timezone state
static char homeIata[5] = "SFO";
static char homeIcao[6] = "KSFO";
static char homeCity[32] = "San Francisco, CA";
static char homeTzShort[8] = "PT";
static char homePosixTz[36] = "PST8PDT,M3.2.0,M11.1.0";
static int  homeStdOffsetMin = -480;
static int  homeDstOffsetMin = -420;
static bool ntpConfigured = false;

// UI State inside Settings app
enum SettingsScreenMode {
  SET_MODE_MAIN,
  SET_MODE_AIRPORT_GRID,
  SET_MODE_AIRPORT_TYPING
};

static SettingsScreenMode setMode = SET_MODE_MAIN;
static bool draggingSlider = false;
static bool brightnessDirty = false;
static int lastDrawnSliderPct = -1;
static int lastDrawnLdr = -1;
static uint32_t lastUiTickMs = 0;
static String typedIata = "";
static int customTzIndex = 3; // 0=ET, 1=CT, 2=MT, 3=PT

struct TzChoice {
  const char *shortName;
  const char *posix;
  int stdOff;
  int dstOff;
};

static const TzChoice TZ_CHOICES[4] = {
  { "ET", "EST5EDT,M3.2.0,M11.1.0", -300, -240 },
  { "CT", "CST6CDT,M3.2.0,M11.1.0", -360, -300 },
  { "MT", "MST7MDT,M3.2.0,M11.1.0", -420, -360 },
  { "PT", "PST8PDT,M3.2.0,M11.1.0", -480, -420 },
};

// ---------------------------------------------------------------------------
// Lookup & Persistence Helpers
// ---------------------------------------------------------------------------
static const AirportInfo *findKnownAirport(const char *iata) {
  for (int i = 0; i < NUM_KNOWN_AIRPORTS; i++) {
    if (strcasecmp(KNOWN_AIRPORTS[i].iata, iata) == 0) {
      return &KNOWN_AIRPORTS[i];
    }
  }
  return nullptr;
}

static void applyAirportCode(const char *code, int fallbackTzIdx, bool saveToNvs) {
  char clean[4] = {0, 0, 0, 0};
  for (int i = 0; i < 3 && code[i]; i++) {
    clean[i] = toupper((unsigned char)code[i]);
  }
  if (strlen(clean) != 3) return;

  strncpy(homeIata, clean, sizeof(homeIata) - 1);
  const AirportInfo *info = findKnownAirport(clean);
  if (info) {
    strncpy(homeIcao, info->icao, sizeof(homeIcao) - 1);
    strncpy(homeCity, info->city, sizeof(homeCity) - 1);
    strncpy(homeTzShort, info->tzShort, sizeof(homeTzShort) - 1);
    strncpy(homePosixTz, info->posixTz, sizeof(homePosixTz) - 1);
    homeStdOffsetMin = info->stdOffsetMin;
    homeDstOffsetMin = info->dstOffsetMin;
  } else {
    snprintf(homeIcao, sizeof(homeIcao), "K%s", clean);
    snprintf(homeCity, sizeof(homeCity), "%s Airport, US", clean);
    int tzIdx = constrain(fallbackTzIdx, 0, 3);
    strncpy(homeTzShort, TZ_CHOICES[tzIdx].shortName, sizeof(homeTzShort) - 1);
    strncpy(homePosixTz, TZ_CHOICES[tzIdx].posix, sizeof(homePosixTz) - 1);
    homeStdOffsetMin = TZ_CHOICES[tzIdx].stdOff;
    homeDstOffsetMin = TZ_CHOICES[tzIdx].dstOff;
  }

  if (saveToNvs) {
    cfgPrefs.begin("cyd_cfg", false);
    cfgPrefs.putString("iata", homeIata);
    cfgPrefs.putInt("tzIdx", fallbackTzIdx);
    cfgPrefs.end();
    Serial.printf("[SETTINGS] Saved Home Airport: %s (%s, TZ=%s)\n",
                  homeIata, homeIcao, homeTzShort);
  }

  setenv("TZ", homePosixTz, 1);
  tzset();
  if (WiFi.status() == WL_CONNECTED) {
    syncNtpTimezone();
  }
}

static void saveBrightnessToNvs() {
  cfgPrefs.begin("cyd_cfg", false);
  cfgPrefs.putBool("autoB", autoBright);
  cfgPrefs.putInt("manB", manualBrightPct);
  cfgPrefs.end();
  brightnessDirty = false;
}

static void applyBacklightPercent(int pct) {
  currentBrightPct = constrain(pct, 5, 100);
  // Perceptual gamma (~1.8) mapping from 5..100% -> PWM 14..255
  float norm = (float)currentBrightPct / 100.0f;
  int pwm = (int)lroundf(12.0f + 243.0f * powf(norm, 1.8f));
  pwm = constrain(pwm, 12, 255);
  if (pwm != lastAppliedPwm) {
    lastAppliedPwm = pwm;
    analogWrite(TFT_BL_PIN, pwm);
  }
}

static int computeAutoBrightPercent(float ldrVal) {
  // On CYD: LDR = 0..4095 where 0 is very bright light and 4095 is pitch dark.
  // Map LDR 200..3500 -> 100%..12% brightness
  float clamped = constrain(ldrVal, 200.0f, 3500.0f);
  float t = (clamped - 200.0f) / (3500.0f - 200.0f); // 0.0 (bright) .. 1.0 (dark)
  int pct = (int)lroundf(100.0f - t * 88.0f);
  return constrain(pct, 12, 100);
}

void initSettings() {
  smoothedLdr = (float)analogRead(LDR_PIN);

  cfgPrefs.begin("cyd_cfg", true);
  autoBright      = cfgPrefs.getBool("autoB", true); // Default ON
  manualBrightPct = cfgPrefs.getInt("manB", 85);
  String savedIata = cfgPrefs.getString("iata", "SFO");
  customTzIndex    = cfgPrefs.getInt("tzIdx", 3);
  cfgPrefs.end();

  applyAirportCode(savedIata.c_str(), customTzIndex, false);

  if (autoBright) {
    applyBacklightPercent(computeAutoBrightPercent(smoothedLdr));
  } else {
    applyBacklightPercent(manualBrightPct);
  }
}

void updateBrightnessLoop() {
  uint32_t now = millis();
  if (now - lastLdrSampleMs < 40) return;
  lastLdrSampleMs = now;

  int raw = analogRead(LDR_PIN);
  smoothedLdr = 0.90f * smoothedLdr + 0.10f * (float)raw;

  if (autoBright) {
    int targetPct = computeAutoBrightPercent(smoothedLdr);
    applyBacklightPercent(targetPct);
  } else {
    applyBacklightPercent(manualBrightPct);
  }
}

bool isAutoBrightness() {
  return autoBright;
}

void setAutoBrightness(bool enabled, bool save) {
  autoBright = enabled;
  if (autoBright) {
    applyBacklightPercent(computeAutoBrightPercent(smoothedLdr));
  } else {
    applyBacklightPercent(manualBrightPct);
  }
  if (save) saveBrightnessToNvs();
}

int getBrightnessPercent() {
  return currentBrightPct;
}

void setManualBrightnessPercent(int pct, bool save) {
  manualBrightPct = constrain(pct, 5, 100);
  autoBright = false;
  applyBacklightPercent(manualBrightPct);
  if (save) {
    saveBrightnessToNvs();
  } else {
    brightnessDirty = true;
  }
}

int getSmoothedLdr() {
  return (int)lroundf(smoothedLdr);
}

const char *getHomeAirportIata() {
  return homeIata;
}

const char *getHomeAirportIcao() {
  return homeIcao;
}

const char *getHomeAirportName() {
  return homeCity;
}

const char *getHomeTzAbbrev() {
  return homeTzShort;
}

void syncNtpTimezone() {
  configTzTime(homePosixTz, "pool.ntp.org", "time.nist.gov");
  ntpConfigured = true;
}

String formatUtcToHomeTz(int utcHour, int utcMin) {
  // Determine whether DST is currently in effect if NTP time is valid;
  // otherwise default to DST offset during Mar..Nov.
  int offsetMin = homeDstOffsetMin;
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 5)) {
    offsetMin = (timeinfo.tm_isdst > 0) ? homeDstOffsetMin : homeStdOffsetMin;
  }

  int totalMin = utcHour * 60 + utcMin + offsetMin;
  while (totalMin < 0) totalMin += 1440;
  totalMin %= 1440;

  int locH24 = totalMin / 60;
  int locM   = totalMin % 60;
  bool pm    = (locH24 >= 12);
  int locH12 = locH24 % 12;
  if (locH12 == 0) locH12 = 12;

  char buf[24];
  snprintf(buf, sizeof(buf), "%d:%02d%s %s", locH12, locM, pm ? "p" : "a", homeTzShort);
  return String(buf);
}

String getCurrentLocalTimeString() {
  if (WiFi.status() == WL_CONNECTED && !ntpConfigured) {
    syncNtpTimezone();
  }
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 5)) {
    return String("TZ: ") + homeTzShort + " (NTP pending)";
  }
  int h24 = timeinfo.tm_hour;
  bool pm = (h24 >= 12);
  int h12 = h24 % 12;
  if (h12 == 0) h12 = 12;
  char buf[36];
  snprintf(buf, sizeof(buf), "%d:%02d:%02d %s %s",
           h12, timeinfo.tm_min, timeinfo.tm_sec, pm ? "PM" : "AM", homeTzShort);
  return String(buf);
}

// ---------------------------------------------------------------------------
// Settings Main Screen UI
// ---------------------------------------------------------------------------
static const int SLIDER_X1 = 22;
static const int SLIDER_X2 = 298;
static const int SLIDER_Y  = 78;

static void drawBrightnessSliderOnly() {
  int pct = getBrightnessPercent();
  int ldr = getSmoothedLdr();
  lastDrawnSliderPct = pct;
  lastDrawnLdr = ldr;

  // Update text label (x: 16..204, y: 34..54)
  tft.fillRect(14, 33, 192, 20, 0x10A2);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(TFT_WHITE, 0x10A2);
  char label[40];
  snprintf(label, sizeof(label), "BRIGHTNESS: %d%%", pct);
  tft.drawString(label, 16, 42, 2);

  tft.setTextColor(TFT_LIGHTGREY, 0x10A2);
  char ldrStr[24];
  snprintf(ldrStr, sizeof(ldrStr), "(LDR:%d)", ldr);
  tft.drawString(ldrStr, 142, 42, 1);

  // [AUTO: ON / OFF] button (x: 210..306, y: 32..54)
  uint16_t autoBg = autoBright ? TFT_DARKGREEN : 0x2945;
  uint16_t autoFg = autoBright ? TFT_WHITE : TFT_CYAN;
  uint16_t autoBorder = autoBright ? TFT_GREEN : TFT_CYAN;
  tft.fillRoundRect(210, 32, 96, 22, 4, autoBg);
  tft.drawRoundRect(210, 32, 96, 22, 4, autoBorder);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(autoFg, autoBg);
  tft.drawString(autoBright ? "AUTO: ON" : "AUTO: OFF", 258, 43, 2);

  // Slider area (x: 12..308, y: 58..98)
  tft.fillRect(12, 58, 296, 40, 0x10A2);

  int trackW = SLIDER_X2 - SLIDER_X1;
  int knobX = SLIDER_X1 + ((pct - 5) * trackW) / 95;
  knobX = constrain(knobX, SLIDER_X1, SLIDER_X2);

  // Unfilled track
  tft.fillRoundRect(SLIDER_X1, SLIDER_Y - 5, trackW, 10, 5, 0x0841);
  tft.drawRoundRect(SLIDER_X1, SLIDER_Y - 5, trackW, 10, 5, 0x4A69);

  // Filled portion
  uint16_t fillCol = autoBright ? TFT_GREEN : TFT_CYAN;
  if (knobX > SLIDER_X1 + 2) {
    tft.fillRoundRect(SLIDER_X1, SLIDER_Y - 4, knobX - SLIDER_X1, 8, 4, fillCol);
  }

  // Thumb knob
  tft.fillCircle(knobX, SLIDER_Y, 10, TFT_WHITE);
  tft.fillCircle(knobX, SLIDER_Y, 6, fillCol);
}

static void drawSettingsMainScreen() {
  setMode = SET_MODE_MAIN;
  tft.fillScreen(0x0841);

  // Top Header (y: 0..24)
  tft.fillRect(0, 0, SCREEN_W, 24, TFT_NAVY);
  tft.drawFastHLine(0, 24, SCREEN_W, TFT_CYAN);

  // [< MENU] button
  tft.fillRoundRect(4, 2, 58, 20, 4, 0x2945);
  tft.drawRoundRect(4, 2, 58, 20, 4, TFT_CYAN);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_CYAN, 0x2945);
  tft.drawString("< MENU", 33, 12, 1);

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_YELLOW, TFT_NAVY);
  tft.drawString("SYSTEM SETTINGS", SCREEN_W / 2, 12, 2);

  // 1. Brightness Card (y: 28..102)
  tft.fillRoundRect(8, 28, 304, 74, 6, 0x10A2);
  tft.drawRoundRect(8, 28, 304, 74, 6, TFT_CYAN);
  drawBrightnessSliderOnly();

  // 2. Airport & Timezone Card (y: 106..152)
  tft.fillRoundRect(8, 106, 304, 46, 6, 0x10A2);
  tft.drawRoundRect(8, 106, 304, 46, 6, TFT_YELLOW);

  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_YELLOW, 0x10A2);
  String apTitle = String("AIRPORT: ") + homeIata + " (" + homeIcao + ")";
  tft.drawString(apTitle, 16, 111, 2);

  tft.setTextColor(TFT_LIGHTGREY, 0x10A2);
  String apSub = String(homeCity) + " | TZ: " + homeTzShort;
  tft.drawString(apSub, 16, 132, 1);

  // [CHANGE] button (x: 210..306, y: 114..144)
  tft.fillRoundRect(210, 114, 96, 30, 5, 0x2945);
  tft.drawRoundRect(210, 114, 96, 30, 5, TFT_YELLOW);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_YELLOW, 0x2945);
  tft.drawString("AIRPORT >", 258, 129, 2);

  // 3. Bottom Action Cards: [WI-FI CONFIG] and [TOUCH CALIBRATION] (y: 156..212)
  // Left Card: WI-FI CONFIG (x: 8..156, y: 156..212)
  bool wifiUp = (WiFi.status() == WL_CONNECTED);
  uint16_t wifiBorder = wifiUp ? TFT_GREEN : TFT_CYAN;
  tft.fillRoundRect(8, 156, 148, 56, 6, 0x18E3);
  tft.drawRoundRect(8, 156, 148, 56, 6, wifiBorder);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_WHITE, 0x18E3);
  tft.drawString("WI-FI CONFIG", 82, 163, 2);

  tft.setTextColor(wifiUp ? TFT_GREEN : TFT_ORANGE, 0x18E3);
  String wifiSub = wifiUp ? WiFi.SSID() : String("Tap to scan/connect");
  if (wifiSub.length() > 21) wifiSub = wifiSub.substring(0, 21) + "..";
  tft.drawString(wifiSub, 82, 184, 1);
  if (wifiUp) {
    tft.setTextColor(TFT_LIGHTGREY, 0x18E3);
    tft.drawString(WiFi.localIP().toString(), 82, 197, 1);
  }

  // Right Card: TOUCH CALIBRATE (x: 164..312, y: 156..212)
  tft.fillRoundRect(164, 156, 148, 56, 6, 0x18E3);
  tft.drawRoundRect(164, 156, 148, 56, 6, TFT_MAGENTA);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_WHITE, 0x18E3);
  tft.drawString("CALIBRATE TOUCH", 238, 163, 2);
  tft.setTextColor(TFT_LIGHTGREY, 0x18E3);
  tft.drawString("3-Tap Stylus Setup", 238, 186, 1);
  tft.setTextColor(TFT_MAGENTA, 0x18E3);
  tft.drawString("Saved in NVS", 238, 198, 1);

  // Footer: Local Time / Timezone info (y: 218..239)
  tft.fillRect(0, 218, SCREEN_W, 22, 0x0841);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_CYAN, 0x0841);
  tft.drawString(String("Home: ") + homeIata + " | " + getCurrentLocalTimeString(),
                 SCREEN_W / 2, 229, 1);
}

// ---------------------------------------------------------------------------
// Airport Picker Sub-Screen (16 Quick-Pick Grid + Custom 3-Letter Button)
// ---------------------------------------------------------------------------
static void drawAirportGridScreen() {
  setMode = SET_MODE_AIRPORT_GRID;
  tft.fillScreen(0x0841);

  tft.fillRect(0, 0, SCREEN_W, 24, TFT_NAVY);
  tft.drawFastHLine(0, 24, SCREEN_W, TFT_CYAN);

  // [< BACK]
  tft.fillRoundRect(4, 2, 58, 20, 4, 0x2945);
  tft.drawRoundRect(4, 2, 58, 20, 4, TFT_CYAN);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_CYAN, 0x2945);
  tft.drawString("< BACK", 33, 12, 1);

  tft.setTextColor(TFT_YELLOW, TFT_NAVY);
  tft.drawString("SELECT AIRPORT", 142, 12, 2);

  // [CUSTOM A-Z] button in top-right (x: 222..316, y: 2..22)
  tft.fillRoundRect(222, 2, 94, 20, 4, TFT_DARKGREEN);
  tft.drawRoundRect(222, 2, 94, 20, 4, TFT_GREEN);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREEN);
  tft.drawString("TYPE CODE", 269, 12, 1);

  // 4x4 Grid of Quick-Pick Airports (y: 30..206)
  for (int i = 0; i < 16; i++) {
    int col = i % 4;
    int row = i / 4;
    int bx = 8 + col * 77;
    int by = 30 + row * 45;
    int bw = 73;
    int bh = 40;

    const char *code = QUICK_AIRPORTS[i];
    bool isSelected = (strcasecmp(code, homeIata) == 0);
    const AirportInfo *info = findKnownAirport(code);

    uint16_t bg = isSelected ? 0x03E0 : 0x18E3;
    uint16_t border = isSelected ? TFT_YELLOW : TFT_CYAN;

    tft.fillRoundRect(bx, by, bw, bh, 5, bg);
    tft.drawRoundRect(bx, by, bw, bh, 5, border);

    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(isSelected ? TFT_YELLOW : TFT_WHITE, bg);
    tft.drawString(code, bx + bw / 2, by + 5, 2);

    if (info) {
      tft.setTextColor(TFT_LIGHTGREY, bg);
      tft.drawString(info->tzShort, bx + bw / 2, by + 25, 1);
    }
  }

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_LIGHTGREY, 0x0841);
  tft.drawString("Tap an airport above or [TYPE CODE] for any 3-letter IATA", SCREEN_W / 2, 224, 1);
}

// ---------------------------------------------------------------------------
// Custom 3-Letter Airport Keypad Screen (A-Z + Timezone Selector)
// ---------------------------------------------------------------------------
static const char *const ALPHA_ROWS[3] = {
  "QWERTYUIOP",
  "ASDFGHJKL",
  "ZXCVBNM"
};

static void drawAirportTypeBox() {
  tft.fillRoundRect(8, 28, 198, 32, 4, TFT_BLACK);
  tft.drawRoundRect(8, 28, 198, 32, 4, TFT_YELLOW);

  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  String disp = "CODE: " + typedIata + "_";
  tft.drawString(disp, 16, 44, 2);

  if (typedIata.length() == 3) {
    const AirportInfo *known = findKnownAirport(typedIata.c_str());
    if (known) {
      tft.setTextDatum(MR_DATUM);
      tft.setTextColor(TFT_GREEN, TFT_BLACK);
      tft.drawString(known->tzShort, 198, 44, 2);
    }
  }
}

static void drawAirportTypingScreen() {
  setMode = SET_MODE_AIRPORT_TYPING;
  tft.fillScreen(0x0841);

  tft.fillRect(0, 0, SCREEN_W, 24, TFT_NAVY);
  tft.drawFastHLine(0, 24, SCREEN_W, TFT_CYAN);

  // [< BACK]
  tft.fillRoundRect(4, 2, 58, 20, 4, 0x2945);
  tft.drawRoundRect(4, 2, 58, 20, 4, TFT_CYAN);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_CYAN, 0x2945);
  tft.drawString("< BACK", 33, 12, 1);

  tft.setTextColor(TFT_YELLOW, TFT_NAVY);
  tft.drawString("TYPE 3-LETTER AIRPORT", 148, 12, 2);

  // [SAVE] button in top-right (x: 244..316, y: 2..22)
  tft.fillRoundRect(244, 2, 72, 20, 4, TFT_DARKGREEN);
  tft.drawRoundRect(244, 2, 72, 20, 4, TFT_GREEN);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREEN);
  tft.drawString("SAVE", 280, 12, 1);

  drawAirportTypeBox();

  // [DEL] button (x: 212..312, y: 28..60)
  tft.fillRoundRect(212, 28, 100, 32, 4, 0x4800);
  tft.drawRoundRect(212, 28, 100, 32, 4, TFT_RED);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, 0x4800);
  tft.drawString("BACKSPACE", 262, 44, 2);

  // 3 Rows of A-Z keys (y: 66..182)
  for (int r = 0; r < 3; r++) {
    const char *rowStr = ALPHA_ROWS[r];
    int len = strlen(rowStr);
    int rowW = len * 31;
    int startX = (SCREEN_W - rowW) / 2;
    int ky = 66 + r * 39;

    for (int c = 0; c < len; c++) {
      int kx = startX + c * 31;
      char ch[2] = { rowStr[c], '\0' };
      tft.fillRoundRect(kx, ky, 28, 35, 4, 0x18E3);
      tft.drawRoundRect(kx, ky, 28, 35, 4, 0x4A69);
      tft.setTextDatum(MC_DATUM);
      tft.setTextColor(TFT_WHITE, 0x18E3);
      tft.drawString(ch, kx + 14, ky + 17, 2);
    }
  }

  // Fallback Timezone Bar for custom airports (y: 188..234)
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(TFT_LIGHTGREY, 0x0841);
  tft.drawString("TZ:", 8, 210, 2);

  for (int i = 0; i < 4; i++) {
    int bx = 46 + i * 67;
    bool sel = (customTzIndex == i);
    uint16_t bg = sel ? 0x03E0 : 0x18E3;
    tft.fillRoundRect(bx, 194, 62, 34, 4, bg);
    tft.drawRoundRect(bx, 194, 62, 34, 4, sel ? TFT_YELLOW : TFT_CYAN);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(sel ? TFT_YELLOW : TFT_WHITE, bg);
    tft.drawString(TZ_CHOICES[i].shortName, bx + 31, 211, 2);
  }
}

// ---------------------------------------------------------------------------
// Input Handlers for Settings
// ---------------------------------------------------------------------------
static void updateSliderFromTouchX(int tx) {
  int trackW = SLIDER_X2 - SLIDER_X1;
  int clampedX = constrain(tx, SLIDER_X1, SLIDER_X2);
  int pct = 5 + ((clampedX - SLIDER_X1) * 95) / trackW;
  setManualBrightnessPercent(pct, false);
  drawBrightnessSliderOnly();
}

static void settingsEnter() {
  draggingSlider = false;
  drawSettingsMainScreen();
}

static void settingsLoop() {
  if (setMode == SET_MODE_MAIN && !draggingSlider) {
    uint32_t now = millis();
    if (now - lastUiTickMs >= 350) {
      lastUiTickMs = now;
      int pct = getBrightnessPercent();
      int ldr = getSmoothedLdr();
      if (abs(pct - lastDrawnSliderPct) >= 1 || abs(ldr - lastDrawnLdr) >= 25) {
        drawBrightnessSliderOnly();
      }
    }
  }
}

static void settingsOnTap(const TouchSample &t) {
  if (setMode == SET_MODE_MAIN) {
    // Top-left [< MENU]
    if (t.y <= 26 && t.x <= 70) {
      if (brightnessDirty) saveBrightnessToNvs();
      returnToMenu();
      return;
    }

    // [AUTO: ON / OFF] button (x: 206..312, y: 28..56)
    if (t.x >= 206 && t.x <= 312 && t.y >= 28 && t.y <= 56) {
      setAutoBrightness(!isAutoBrightness(), true);
      setRgbLed(false, true, false, 80);
      drawBrightnessSliderOnly();
      return;
    }

    // Brightness Slider (x: 12..308, y: 56..102)
    if (t.x >= 12 && t.x <= 308 && t.y >= 56 && t.y <= 102) {
      draggingSlider = true;
      updateSliderFromTouchX(t.x);
      return;
    }

    // Airport Card / [AIRPORT >] (y: 106..152)
    if (t.y >= 106 && t.y <= 152) {
      setRgbLed(false, false, true, 80);
      drawAirportGridScreen();
      return;
    }

    // Bottom Cards (y: 156..214)
    if (t.y >= 156 && t.y <= 214) {
      if (t.x <= 158) {
        // [WI-FI CONFIG]
        openWifiConfigFromSettings(true);
        return;
      } else if (t.x >= 162) {
        // [CALIBRATE TOUCH]
        startCalibration(false); // false = return to Settings after calibration
        return;
      }
    }
    return;
  }

  if (setMode == SET_MODE_AIRPORT_GRID) {
    if (t.y <= 26) {
      if (t.x <= 70) {
        drawSettingsMainScreen();
        return;
      }
      if (t.x >= 218) {
        typedIata = homeIata;
        drawAirportTypingScreen();
        return;
      }
      return;
    }

    // 4x4 Grid (y: 30..206)
    if (t.y >= 30 && t.y <= 208) {
      int col = (t.x - 8) / 77;
      int row = (t.y - 30) / 45;
      if (col >= 0 && col < 4 && row >= 0 && row < 4) {
        int idx = row * 4 + col;
        applyAirportCode(QUICK_AIRPORTS[idx], customTzIndex, true);
        setRgbLed(false, true, false, 120);
        drawSettingsMainScreen();
        return;
      }
    }
    return;
  }

  if (setMode == SET_MODE_AIRPORT_TYPING) {
    if (t.y <= 26) {
      if (t.x <= 70) {
        drawAirportGridScreen();
        return;
      }
      if (t.x >= 240 && typedIata.length() == 3) {
        applyAirportCode(typedIata.c_str(), customTzIndex, true);
        setRgbLed(false, true, false, 120);
        drawSettingsMainScreen();
        return;
      }
      return;
    }

    // [BACKSPACE]
    if (t.y >= 26 && t.y <= 62 && t.x >= 208) {
      if (typedIata.length() > 0) {
        typedIata.remove(typedIata.length() - 1);
        drawAirportTypeBox();
      }
      return;
    }

    // A-Z Keys (y: 66..182)
    if (t.y >= 66 && t.y < 183) {
      int r = (t.y - 66) / 39;
      if (r >= 0 && r < 3) {
        const char *rowStr = ALPHA_ROWS[r];
        int len = strlen(rowStr);
        int rowW = len * 31;
        int startX = (SCREEN_W - rowW) / 2;
        if (t.x >= startX && t.x < startX + rowW) {
          int c = (t.x - startX) / 31;
          if (c >= 0 && c < len) {
            if (typedIata.length() >= 3) typedIata = "";
            typedIata += rowStr[c];
            const AirportInfo *known = findKnownAirport(typedIata.c_str());
            if (known) {
              for (int k = 0; k < 4; k++) {
                if (strcmp(known->tzShort, TZ_CHOICES[k].shortName) == 0) {
                  customTzIndex = k;
                }
              }
              drawAirportTypingScreen();
            } else {
              drawAirportTypeBox();
            }
          }
        }
      }
      return;
    }

    // TZ row (y >= 190)
    if (t.y >= 190 && t.x >= 44) {
      int idx = (t.x - 46) / 67;
      if (idx >= 0 && idx < 4) {
        customTzIndex = idx;
        drawAirportTypingScreen();
      }
      return;
    }
  }
}

static void settingsOnDrag(const TouchSample &t) {
  if (setMode == SET_MODE_MAIN && (draggingSlider || (t.y >= 54 && t.y <= 104 && t.x >= 10 && t.x <= 310))) {
    draggingSlider = true;
    updateSliderFromTouchX(t.x);
  }
}

static void settingsOnRelease() {
  if (draggingSlider) {
    draggingSlider = false;
    if (brightnessDirty) {
      saveBrightnessToNvs();
    }
  }
}

const DemoApp DEMO_SETTINGS = {
  "SETTINGS",
  "Wi-Fi, Touch, Bright, Airport",
  TFT_CYAN,
  settingsEnter,
  settingsLoop,
  settingsOnTap,
  settingsOnDrag,
  settingsOnRelease
};
