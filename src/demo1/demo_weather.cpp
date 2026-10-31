#include "cyd_shell.h"
#include <WiFi.h>
#include <NetworkClientSecure.h>
#include <HTTPClient.h>
#include <math.h>

// ---------------------------------------------------------------------------
// DEMO 6: NOAA Weather (NYC, LAX, ATL, ORD, SFO, Denver, Seattle)
// Fetches live observations from NOAA Aviation Weather / NWS APIs:
//   1. https://aviationweather.gov/api/data/metar?ids=KJFK,KLAX,KATL,KORD,KSFO,KDEN,KSEA&format=json
//   2. https://tgftp.nws.noaa.gov/data/observations/metar/decoded/<ICAO>.TXT
// ---------------------------------------------------------------------------

static const int NUM_CITIES = 7;

struct CityStation {
  String code;     // Short label on table
  String icao;     // NOAA / ICAO station ID
  String fullName; // City & Airport name
  bool isHome;
};

struct CoreHub {
  const char *code;
  const char *icao;
  const char *fullName;
};

static const CoreHub DEFAULT_HUBS[NUM_CITIES] = {
  { "NYC", "KJFK", "New York (JFK), NY"       },
  { "LAX", "KLAX", "Los Angeles (LAX), CA"    },
  { "ATL", "KATL", "Atlanta (ATL), GA"        },
  { "ORD", "KORD", "Chicago O'Hare (ORD), IL" },
  { "SFO", "KSFO", "San Francisco (SFO), CA"  },
  { "DEN", "KDEN", "Denver (DEN), CO"         },
  { "SEA", "KSEA", "Seattle-Tacoma (SEA), WA" },
};

static CityStation STATIONS[NUM_CITIES];
static String loadedHomeIcao = "";

static void rebuildStationList() {
  const char *hIata = getHomeAirportIata();
  const char *hIcao = getHomeAirportIcao();
  const char *hName = getHomeAirportName();

  STATIONS[0].code     = String(hIata);
  STATIONS[0].icao     = String(hIcao);
  STATIONS[0].fullName = String(hName) + " (" + hIcao + ")";
  STATIONS[0].isHome   = true;

  int dst = 1;
  for (int i = 0; i < NUM_CITIES && dst < NUM_CITIES; i++) {
    if (strcasecmp(DEFAULT_HUBS[i].icao, hIcao) == 0 ||
        strcasecmp(DEFAULT_HUBS[i].code, hIata) == 0) {
      continue;
    }
    STATIONS[dst].code     = DEFAULT_HUBS[i].code;
    STATIONS[dst].icao     = DEFAULT_HUBS[i].icao;
    STATIONS[dst].fullName = DEFAULT_HUBS[i].fullName;
    STATIONS[dst].isHome   = false;
    dst++;
  }
}

struct WeatherObs {
  bool valid;
  float tempC;
  float dewpC;
  int relHum;
  int wdir;        // -1 = VRB/Calm
  int wspdKt;
  int wgstKt;
  float altimHpa;
  float presTendHpa;
  String visib;
  String cover;
  String wxString;
  String fltCat;
  String obsUtc;
  String rawMetar;
};

static WeatherObs obs[NUM_CITIES];
static bool hasLoadedOnce = false;
static bool useCelsius = false;
static String lastFetchUtc = "";
static String lastError = "";

enum WeatherMode {
  WX_NEED_WIFI,
  WX_TABLE,
  WX_CITY_DETAIL,
  WX_CITY_NWS_TEXT
};

static WeatherMode wxMode = WX_TABLE;
static int selectedCity = 0;
static String nwsDecodedText = "";

// ---------------------------------------------------------------------------
// JSON & Weather Math Helpers
// ---------------------------------------------------------------------------
static float cToF(float c) {
  return c * 1.8f + 32.0f;
}

static int computeRelHumidity(float tempC, float dewpC) {
  if (isnan(tempC) || isnan(dewpC)) return 0;
  // Magnus approximation
  float num = expf((17.625f * dewpC) / (243.04f + dewpC));
  float den = expf((17.625f * tempC) / (243.04f + tempC));
  if (den <= 0.0f) return 0;
  int rh = (int)lroundf(100.0f * (num / den));
  return constrain(rh, 1, 100);
}

static const char *degToCompass(int deg) {
  if (deg < 0) return "VRB";
  static const char *const DIRS[16] = {
    "N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
    "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"
  };
  int idx = ((int)lroundf(((float)(deg % 360)) / 22.5f)) & 15;
  return DIRS[idx];
}

static String extractJsonString(const String &obj, const char *key) {
  String pat = String("\"") + key + "\":";
  int idx = obj.indexOf(pat);
  if (idx < 0) return "";
  int p = idx + pat.length();
  while (p < (int)obj.length() && obj[p] == ' ') p++;
  if (p >= (int)obj.length()) return "";

  if (obj[p] == '"') {
    p++;
    String out = "";
    while (p < (int)obj.length() && obj[p] != '"') {
      if (obj[p] == '\\' && p + 1 < (int)obj.length()) p++;
      out += obj[p++];
    }
    return out;
  } else {
    // Non-quoted primitive (e.g. number or null)
    int end = p;
    while (end < (int)obj.length() && obj[end] != ',' && obj[end] != '}') end++;
    String tok = obj.substring(p, end);
    tok.trim();
    if (tok == "null") return "";
    return tok;
  }
}

static float extractJsonFloat(const String &obj, const char *key, float defVal) {
  String s = extractJsonString(obj, key);
  if (s.length() == 0) return defVal;
  if (s == "VRB") return -1.0f;
  return s.toFloat();
}

static String extractUtcFromMetar(const String &raw, String &localTimeOut) {
  // Look for DDHHMMZ token inside METAR (e.g., "310551Z")
  int len = raw.length();
  for (int i = 0; i <= len - 7; i++) {
    if (raw[i + 6] == 'Z') {
      bool allDigits = true;
      for (int k = 0; k < 6; k++) {
        if (!isDigit(raw[i + k])) {
          allDigits = false;
          break;
        }
      }
      if (allDigits) {
        int hh = raw.substring(i + 2, i + 4).toInt();
        int mm = raw.substring(i + 4, i + 6).toInt();
        localTimeOut = formatUtcToHomeTz(hh, mm);
        return raw.substring(i + 2, i + 4) + ":" + raw.substring(i + 4, i + 6) + "Z";
      }
    }
  }
  localTimeOut = "";
  return "";
}

static void conditionLabelAndColor(const WeatherObs &w, String &label, uint16_t &bgCol, uint16_t &fgCol) {
  String wx = w.wxString;
  wx.toUpperCase();

  if (wx.indexOf("TS") >= 0) {
    label = "STORM";
    bgCol = TFT_RED;
    fgCol = TFT_WHITE;
    return;
  }
  if (wx.indexOf("SN") >= 0 || wx.indexOf("FZ") >= 0 || wx.indexOf("PL") >= 0) {
    label = "SNOW";
    bgCol = TFT_WHITE;
    fgCol = TFT_NAVY;
    return;
  }
  if (wx.indexOf("RA") >= 0 || wx.indexOf("DZ") >= 0 || wx.indexOf("SH") >= 0) {
    label = "RAIN";
    bgCol = 0x1C9F; // Bright blue
    fgCol = TFT_WHITE;
    return;
  }
  if (wx.indexOf("FG") >= 0 || wx.indexOf("BR") >= 0 || wx.indexOf("HZ") >= 0) {
    label = (wx.indexOf("FG") >= 0) ? "FOG" : "MIST";
    bgCol = 0x52AA;
    fgCol = TFT_YELLOW;
    return;
  }

  String cov = w.cover;
  cov.toUpperCase();
  if (cov == "CLR" || cov == "SKC" || cov == "CAVOK") {
    label = "CLEAR";
    bgCol = 0x03E0; // Deep green/teal
    fgCol = TFT_YELLOW;
  } else if (cov == "FEW") {
    label = "FEW CLD";
    bgCol = 0x0333;
    fgCol = TFT_CYAN;
  } else if (cov == "SCT") {
    label = "SCT CLD";
    bgCol = 0x1A6F;
    fgCol = TFT_WHITE;
  } else if (cov == "BKN") {
    label = "M.CLOUDY";
    bgCol = 0x39E7;
    fgCol = TFT_LIGHTGREY;
  } else if (cov == "OVC" || cov == "OVX") {
    label = "OVERCAST";
    bgCol = 0x4208;
    fgCol = TFT_WHITE;
  } else {
    label = cov.length() ? cov : "N/A";
    bgCol = 0x2104;
    fgCol = TFT_LIGHTGREY;
  }
}

static uint16_t tempColorF(float tempF) {
  if (tempF < 32.0f) return TFT_CYAN;
  if (tempF < 50.0f) return 0x5DFF; // Sky blue
  if (tempF < 68.0f) return TFT_GREEN;
  if (tempF < 80.0f) return TFT_YELLOW;
  if (tempF < 90.0f) return TFT_ORANGE;
  return 0xF986; // Warm red
}

// ---------------------------------------------------------------------------
// UI Rendering: 7-City Dashboard Table
// ---------------------------------------------------------------------------
static void drawTopBar(const char *titleText) {
  tft.fillRect(0, 0, SCREEN_W, 22, TFT_NAVY);
  tft.drawFastHLine(0, 22, SCREEN_W, TFT_CYAN);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(TFT_YELLOW, TFT_NAVY);
  tft.drawString(titleText, 6, 11, 2);

  if (lastFetchUtc.length() > 0) {
    tft.setTextDatum(MR_DATUM);
    tft.setTextColor(TFT_CYAN, TFT_NAVY);
    tft.drawString(lastFetchUtc, 260, 11, 1);
  }

  tft.fillRoundRect(266, 2, 51, 18, 4, TFT_MAROON);
  tft.drawRoundRect(266, 2, 51, 18, 4, TFT_WHITE);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_MAROON);
  tft.drawString("MENU", 291, 11, 1);
}

static void drawWeatherTable() {
  wxMode = WX_TABLE;
  tft.fillScreen(0x0841);
  drawTopBar("NOAA WEATHER");

  // Column Header Row (y = 23..35)
  tft.fillRect(0, 23, SCREEN_W, 13, 0x10A2);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(TFT_CYAN, 0x10A2);
  tft.drawString("CITY", 6, 29, 1);
  tft.drawString("CONDITION", 48, 29, 1);
  tft.drawString(useCelsius ? "TEMP(C)" : "TEMP(F)", 118, 29, 1);
  tft.drawString("RH", 174, 29, 1);
  tft.drawString("WIND", 204, 29, 1);
  tft.drawString("BARO", 274, 29, 1);
  tft.drawFastHLine(0, 36, SCREEN_W, 0x2945);

  // 7 City Rows (y = 37..211, 25px per row)
  for (int i = 0; i < NUM_CITIES; i++) {
    int rowY = 37 + i * 25;
    uint16_t rowBg = STATIONS[i].isHome ? 0x1924 : ((i & 1) ? 0x1082 : 0x0841);
    tft.fillRect(0, rowY, SCREEN_W, 24, rowBg);
    tft.drawFastHLine(0, rowY + 24, SCREEN_W, 0x18E3);

    const WeatherObs &w = obs[i];
    float tempF = cToF(w.tempC);
    uint16_t tCol = w.valid ? tempColorF(tempF) : TFT_DARKGREY;

    // Left temperature color accent bar (gold for Home airport)
    tft.fillRect(0, rowY + 2, 3, 20, STATIONS[i].isHome ? TFT_YELLOW : tCol);

    // 1. City Code (Home airport in Row 0 highlighted in yellow)
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(STATIONS[i].isHome ? TFT_YELLOW : TFT_WHITE, rowBg);
    tft.drawString(STATIONS[i].code, 6, rowY + 12, 2);

    if (!w.valid) {
      tft.setTextColor(TFT_LIGHTGREY, rowBg);
      tft.drawString(lastError.length() ? lastError : "No observation", 52, rowY + 12, 1);
      continue;
    }

    // 2. Sky / Weather Badge (x = 46..110)
    String condLabel;
    uint16_t badgeBg, badgeFg;
    conditionLabelAndColor(w, condLabel, badgeBg, badgeFg);
    tft.fillRoundRect(46, rowY + 4, 62, 16, 3, badgeBg);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(badgeFg, badgeBg);
    tft.drawString(condLabel, 77, rowY + 12, 1);

    // 3. Temperature (x = 114..168)
    char tempPrimary[16];
    char tempSecondary[16];
    if (useCelsius) {
      snprintf(tempPrimary, sizeof(tempPrimary), "%.0fC", w.tempC);
      snprintf(tempSecondary, sizeof(tempSecondary), "%.0fF", tempF);
    } else {
      snprintf(tempPrimary, sizeof(tempPrimary), "%.0fF", tempF);
      snprintf(tempSecondary, sizeof(tempSecondary), "%.0fC", w.tempC);
    }
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(tCol, rowBg);
    tft.drawString(tempPrimary, 114, rowY + 12, 2);
    tft.setTextColor(TFT_LIGHTGREY, rowBg);
    tft.drawString(tempSecondary, 146, rowY + 12, 1);

    // 4. Relative Humidity (x = 172..198)
    char rhBuf[12];
    snprintf(rhBuf, sizeof(rhBuf), "%d%%", w.relHum);
    tft.setTextColor(0x5DFF, rowBg);
    tft.drawString(rhBuf, 172, rowY + 12, 1);

    // 5. Wind Direction + Speed in mph (x = 202..270)
    char windBuf[20];
    int mph = (int)lroundf(w.wspdKt * 1.15078f);
    if (w.wspdKt <= 0) {
      snprintf(windBuf, sizeof(windBuf), "CALM");
    } else {
      snprintf(windBuf, sizeof(windBuf), "%s %dm", degToCompass(w.wdir), mph);
    }
    tft.setTextColor((mph >= 18) ? TFT_YELLOW : TFT_WHITE, rowBg);
    tft.drawString(windBuf, 202, rowY + 12, 1);

    // 6. Barometric Altimeter in inHg (x = 272..318)
    char baroBuf[16];
    if (w.altimHpa > 800.0f) {
      float inHg = w.altimHpa * 0.02953f;
      char trend = (w.presTendHpa > 0.2f) ? '+' : ((w.presTendHpa < -0.2f) ? '-' : ' ');
      snprintf(baroBuf, sizeof(baroBuf), "%.2f%c", inHg, trend);
    } else {
      snprintf(baroBuf, sizeof(baroBuf), "--");
    }
    tft.setTextColor(TFT_LIGHTGREY, rowBg);
    tft.drawString(baroBuf, 272, rowY + 12, 1);
  }

  // Bottom Toolbar (y = 213..239)
  tft.fillRect(0, 213, SCREEN_W, 27, 0x1082);
  tft.drawFastHLine(0, 213, SCREEN_W, TFT_CYAN);

  // [REFRESH NOAA]
  tft.fillRoundRect(4, 216, 98, 21, 4, TFT_DARKGREEN);
  tft.drawRoundRect(4, 216, 98, 21, 4, TFT_GREEN);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREEN);
  tft.drawString("REFRESH NOAA", 53, 226, 1);

  // [UNIT: F / C]
  tft.fillRoundRect(108, 216, 82, 21, 4, 0x2945);
  tft.drawRoundRect(108, 216, 82, 21, 4, TFT_CYAN);
  tft.setTextColor(TFT_CYAN, 0x2945);
  tft.drawString(useCelsius ? "UNIT: C -> F" : "UNIT: F -> C", 149, 226, 1);

  // [HOME AIRPORT / SETTINGS]
  tft.fillRoundRect(196, 216, 120, 21, 4, 0x18E3);
  tft.drawRoundRect(196, 216, 120, 21, 4, TFT_YELLOW);
  tft.setTextColor(TFT_YELLOW, 0x18E3);
  String homeBtn = String("HOME: ") + getHomeAirportIata() + " (" + getHomeTzAbbrev() + ")";
  tft.drawString(homeBtn, 256, 226, 1);
}

// ---------------------------------------------------------------------------
// UI Rendering: Single-City Detailed View & Raw METAR / Decoded NWS Report
// ---------------------------------------------------------------------------
static void drawCityDetail(int idx) {
  wxMode = WX_CITY_DETAIL;
  selectedCity = constrain(idx, 0, NUM_CITIES - 1);
  const CityStation &st = STATIONS[selectedCity];
  const WeatherObs &w = obs[selectedCity];

  tft.fillScreen(0x0841);
  drawTopBar(st.fullName.c_str());

  if (!w.valid) {
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_ORANGE, 0x0841);
    tft.drawString("No data loaded for this station.", SCREEN_W / 2, 110, 2);
  } else {
    float tempF = cToF(w.tempC);
    float dewpF = cToF(w.dewpC);
    uint16_t tCol = tempColorF(tempF);

    String condLabel;
    uint16_t badgeBg, badgeFg;
    conditionLabelAndColor(w, condLabel, badgeBg, badgeFg);

    // Top summary card (y = 26..86)
    tft.fillRoundRect(6, 26, 308, 58, 6, 0x10A2);
    tft.drawRoundRect(6, 26, 308, 58, 6, tCol);

    // Big Temp
    char bigTemp[24];
    snprintf(bigTemp, sizeof(bigTemp), "%.1f F  (%.1f C)", tempF, w.tempC);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(tCol, 0x10A2);
    tft.drawString(bigTemp, 14, 32, 4);

    // Condition badge + Flight category badge on right
    tft.fillRoundRect(232, 32, 74, 20, 4, badgeBg);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(badgeFg, badgeBg);
    tft.drawString(condLabel, 269, 42, 2);

    uint16_t fltCol = (w.fltCat == "VFR") ? TFT_GREEN : ((w.fltCat == "MVFR") ? TFT_CYAN : TFT_RED);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_LIGHTGREY, 0x10A2);
    char subLine[64];
    snprintf(subLine, sizeof(subLine), "Dewpoint: %.1fF (%.1fC)  |  Humidity: %d%%",
             dewpF, w.dewpC, w.relHum);
    tft.drawString(subLine, 14, 62, 1);

    tft.setTextDatum(MR_DATUM);
    tft.setTextColor(fltCol, 0x10A2);
    tft.drawString(w.fltCat + " " + w.obsUtc, 306, 66, 1);

    // Telemetry grid (y = 88..146)
    tft.fillRoundRect(6, 88, 308, 56, 6, 0x1082);
    tft.drawRoundRect(6, 88, 308, 56, 6, 0x2945);

    int mph = (int)lroundf(w.wspdKt * 1.15078f);
    int gustMph = (int)lroundf(w.wgstKt * 1.15078f);
    char windLine[64];
    if (w.wspdKt <= 0) {
      snprintf(windLine, sizeof(windLine), "Wind: Calm (0 mph)");
    } else if (w.wgstKt > 0) {
      snprintf(windLine, sizeof(windLine), "Wind: %s (%d deg) at %d mph (%d kt), Gust %d mph",
               degToCompass(w.wdir), w.wdir, mph, w.wspdKt, gustMph);
    } else {
      snprintf(windLine, sizeof(windLine), "Wind: %s (%d deg) at %d mph (%d kt)",
               degToCompass(w.wdir), w.wdir, mph, w.wspdKt);
    }

    float inHg = w.altimHpa * 0.02953f;
    char baroLine[64];
    snprintf(baroLine, sizeof(baroLine), "Barometer: %.2f inHg (%.1f hPa) | 3hr Tend: %+.1f hPa",
             inHg, w.altimHpa, w.presTendHpa);

    String visLine = "Visibility: " + (w.visib.length() ? (w.visib + " statute miles") : String("N/A"));
    if (w.wxString.length() > 0) {
      visLine += "  |  Wx: " + w.wxString;
    }

    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_WHITE, 0x1082);
    tft.drawString(windLine, 12, 94, 1);
    tft.setTextColor(TFT_CYAN, 0x1082);
    tft.drawString(baroLine, 12, 110, 1);
    tft.setTextColor(TFT_YELLOW, 0x1082);
    tft.drawString(visLine, 12, 126, 1);

    // Raw NOAA METAR box (y = 148..209)
    tft.fillRoundRect(6, 148, 308, 61, 5, TFT_BLACK);
    tft.drawRoundRect(6, 148, 308, 61, 5, 0x3186);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.drawString("NOAA RAW METAR:", 12, 153, 1);

    // Word-wrap rawMetar into 3 lines (~48 chars/line)
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    String m = w.rawMetar;
    int yCursor = 166;
    while (m.length() > 0 && yCursor <= 194) {
      int take = min((int)m.length(), 48);
      if (take < (int)m.length()) {
        int sp = m.lastIndexOf(' ', take);
        if (sp > 20) take = sp;
      }
      String line = m.substring(0, take);
      line.trim();
      tft.drawString(line, 12, yCursor, 1);
      m = m.substring(take);
      m.trim();
      yCursor += 12;
    }
  }

  // Bottom Navigation Toolbar (y = 213..239)
  tft.fillRect(0, 213, SCREEN_W, 27, 0x1082);
  tft.drawFastHLine(0, 213, SCREEN_W, TFT_CYAN);

  // [< ALL 7]
  tft.fillRoundRect(4, 216, 74, 21, 4, 0x2945);
  tft.drawRoundRect(4, 216, 74, 21, 4, TFT_CYAN);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_CYAN, 0x2945);
  tft.drawString("< ALL 7", 41, 226, 1);

  // [PREV]
  tft.fillRoundRect(84, 216, 56, 21, 4, 0x18E3);
  tft.drawRoundRect(84, 216, 56, 21, 4, TFT_WHITE);
  tft.setTextColor(TFT_WHITE, 0x18E3);
  tft.drawString("< PREV", 112, 226, 1);

  // [NEXT]
  tft.fillRoundRect(146, 216, 56, 21, 4, 0x18E3);
  tft.drawRoundRect(146, 216, 56, 21, 4, TFT_WHITE);
  tft.setTextColor(TFT_WHITE, 0x18E3);
  tft.drawString("NEXT >", 174, 226, 1);

  // [NWS DECODED]
  tft.fillRoundRect(208, 216, 108, 21, 4, TFT_DARKGREEN);
  tft.drawRoundRect(208, 216, 108, 21, 4, TFT_GREEN);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREEN);
  tft.drawString("NWS DECODED TXT", 262, 226, 1);
}

// ---------------------------------------------------------------------------
// Fetch Decoded NWS Text Bulletin from tgftp.nws.noaa.gov
// ---------------------------------------------------------------------------
static void fetchAndShowNwsDecoded(int idx) {
  wxMode = WX_CITY_NWS_TEXT;
  const CityStation &st = STATIONS[idx];

  tft.fillRect(0, 23, SCREEN_W, 190, 0x0841);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_CYAN, 0x0841);
  String url = String("https://tgftp.nws.noaa.gov/data/observations/metar/decoded/") + st.icao + ".TXT";
  tft.drawString("Fetching NWS Decoded Bulletin...", SCREEN_W / 2, 105, 2);
  tft.setTextColor(TFT_LIGHTGREY, 0x0841);
  tft.drawString(url, SCREEN_W / 2, 128, 1);

  setRgbLed(false, false, true, 0);
  NetworkClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(8000);
  http.setUserAgent("CYD-One-ESP32-Weather/1.0");

  nwsDecodedText = "";
  if (http.begin(client, url)) {
    int code = http.GET();
    if (code == HTTP_CODE_OK) {
      nwsDecodedText = http.getString();
      setRgbLed(false, true, false, 150);
    } else {
      nwsDecodedText = "HTTP error " + String(code) + " fetching decoded text.";
      setRgbLed(true, false, false, 250);
    }
    http.end();
  } else {
    nwsDecodedText = "Connection failed to tgftp.nws.noaa.gov";
    setRgbLed(true, false, false, 250);
  }

  tft.fillRect(0, 23, SCREEN_W, 190, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);

  // Render lines of the NWS decoded text
  int pos = 0;
  int y = 27;
  int lineNum = 0;
  while (pos < (int)nwsDecodedText.length() && y <= 198) {
    int nl = nwsDecodedText.indexOf('\n', pos);
    if (nl < 0) nl = nwsDecodedText.length();
    String line = nwsDecodedText.substring(pos, nl);
    line.trim();
    pos = nl + 1;
    if (line.length() == 0 || line.startsWith("cycle:")) continue;

    uint16_t col = (lineNum == 0) ? TFT_YELLOW : ((lineNum == 1) ? TFT_CYAN : TFT_WHITE);
    if (line.startsWith("ob:")) col = TFT_GREEN;
    tft.setTextColor(col, TFT_BLACK);

    // Wrap long lines at 51 chars
    while (line.length() > 0 && y <= 198) {
      int take = min((int)line.length(), 51);
      if (take < (int)line.length()) {
        int sp = line.lastIndexOf(' ', take);
        if (sp > 20) take = sp;
      }
      String part = line.substring(0, take);
      part.trim();
      tft.drawString(part, 6, y, 1);
      line = line.substring(take);
      line.trim();
      y += 12;
    }
    lineNum++;
  }

  // Bottom bar
  tft.fillRect(0, 213, SCREEN_W, 27, 0x1082);
  tft.drawFastHLine(0, 213, SCREEN_W, TFT_CYAN);

  tft.fillRoundRect(4, 216, 96, 21, 4, 0x2945);
  tft.drawRoundRect(4, 216, 96, 21, 4, TFT_CYAN);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_CYAN, 0x2945);
  tft.drawString("< ALL 7 CITIES", 52, 226, 1);

  tft.fillRoundRect(208, 216, 108, 21, 4, 0x18E3);
  tft.drawRoundRect(208, 216, 108, 21, 4, TFT_WHITE);
  tft.setTextColor(TFT_WHITE, 0x18E3);
  tft.drawString("BACK TO SUMMARY", 262, 226, 1);
}

// ---------------------------------------------------------------------------
// Fetch All 7 Cities from NOAA Aviation Weather JSON API
// ---------------------------------------------------------------------------
static void fetchNoaaWeather() {
  if (!ensureWifiOrPrompt()) {
    wxMode = WX_NEED_WIFI;
    return;
  }

  rebuildStationList();
  loadedHomeIcao = getHomeAirportIcao();

  String idsParam = "";
  for (int i = 0; i < NUM_CITIES; i++) {
    if (i > 0) idsParam += ",";
    idsParam += STATIONS[i].icao;
  }
  String url = "https://aviationweather.gov/api/data/metar?ids=" + idsParam + "&format=json";

  tft.fillScreen(0x0841);
  drawTopBar("NOAA WEATHER (FETCHING...)");
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_CYAN, 0x0841);
  tft.drawString("Querying NOAA Aviation Weather API...", SCREEN_W / 2, 105, 2);
  tft.setTextColor(TFT_LIGHTGREY, 0x0841);
  tft.drawString(idsParam, SCREEN_W / 2, 130, 1);

  setRgbLed(false, false, true, 0);

  NetworkClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(10000);
  http.setUserAgent("CYD-One-ESP32-Weather/1.0");

  lastError = "";
  if (!http.begin(client, url)) {
    setRgbLed(true, false, false, 300);
    lastError = "HTTPS init failed";
    drawWeatherTable();
    return;
  }

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    http.end();
    setRgbLed(true, false, false, 300);
    lastError = "HTTP " + String(httpCode);
    drawWeatherTable();
    return;
  }

  String payload = http.getString();
  http.end();

  // Parse each of the 7 stations out of the JSON array
  int parsedCount = 0;
  for (int i = 0; i < NUM_CITIES; i++) {
    obs[i].valid = false;
    String key = String("\"icaoId\":\"") + STATIONS[i].icao + "\"";
    int start = payload.indexOf(key);
    if (start < 0) continue;

    int nextStation = payload.indexOf("\"icaoId\":\"", start + key.length());
    int end = (nextStation > start) ? nextStation : payload.length();
    String obj = payload.substring(start, end);

    obs[i].tempC       = extractJsonFloat(obj, "temp", NAN);
    obs[i].dewpC       = extractJsonFloat(obj, "dewp", NAN);
    obs[i].relHum      = computeRelHumidity(obs[i].tempC, obs[i].dewpC);
    obs[i].wdir        = (int)lroundf(extractJsonFloat(obj, "wdir", -1.0f));
    obs[i].wspdKt      = (int)lroundf(extractJsonFloat(obj, "wspd", 0.0f));
    obs[i].wgstKt      = (int)lroundf(extractJsonFloat(obj, "wgst", 0.0f));
    obs[i].altimHpa    = extractJsonFloat(obj, "altim", 0.0f);
    obs[i].presTendHpa = extractJsonFloat(obj, "presTend", 0.0f);
    obs[i].visib       = extractJsonString(obj, "visib");
    obs[i].cover       = extractJsonString(obj, "cover");
    obs[i].wxString    = extractJsonString(obj, "wxString");
    obs[i].fltCat      = extractJsonString(obj, "fltCat");
    obs[i].rawMetar    = extractJsonString(obj, "rawOb");
    String locTime;
    String utcStr      = extractUtcFromMetar(obs[i].rawMetar, locTime);
    obs[i].obsUtc      = locTime.length() ? (locTime + " (" + utcStr + ")") : utcStr;
    obs[i].valid       = !isnan(obs[i].tempC);

    if (locTime.length() > 0) {
      lastFetchUtc = locTime;
    } else if (utcStr.length() > 0) {
      lastFetchUtc = utcStr;
    }
    if (obs[i].valid) parsedCount++;
  }

  hasLoadedOnce = (parsedCount > 0);
  setRgbLed(false, true, false, 180);
  Serial.printf("[WEATHER] Parsed %d/%d stations from NOAA (%u bytes)\n",
                parsedCount, NUM_CITIES, (unsigned)payload.length());

  drawWeatherTable();
}

// ---------------------------------------------------------------------------
// Demo Callbacks
// ---------------------------------------------------------------------------
static void weatherEnter() {
  if (!ensureWifiOrPrompt()) {
    wxMode = WX_NEED_WIFI;
    return;
  }
  if (!hasLoadedOnce || loadedHomeIcao != String(getHomeAirportIcao())) {
    fetchNoaaWeather();
  } else {
    drawWeatherTable();
  }
}

static void weatherLoop() {
  // Interactive touch-driven app
}

static void weatherTap(const TouchSample &t) {
  if (wxMode == WX_NEED_WIFI) {
    handleWifiPromptTap(t);
    return;
  }

  // Top-right [MENU] button
  if (t.x >= 262 && t.y <= 24) {
    returnToMenu();
    return;
  }

  if (wxMode == WX_TABLE) {
    // Check 7 City Rows (y = 37..211, 25px each)
    if (t.y >= 37 && t.y < 37 + NUM_CITIES * 25) {
      int idx = (t.y - 37) / 25;
      if (idx >= 0 && idx < NUM_CITIES) {
        setRgbLed(false, false, true, 80);
        drawCityDetail(idx);
        return;
      }
    }

    // Bottom toolbar buttons (y >= 212)
    if (t.y >= 212) {
      if (t.x <= 104) {
        // [REFRESH NOAA]
        fetchNoaaWeather();
        return;
      }
      if (t.x >= 106 && t.x <= 192) {
        // [UNIT: F / C]
        useCelsius = !useCelsius;
        setRgbLed(false, true, true, 80);
        drawWeatherTable();
        return;
      }
      if (t.x >= 194) {
        // [HOME: <IATA>] -> open Settings to change Airport/Timezone
        launchDemo(&DEMO_SETTINGS);
        return;
      }
    }
    return;
  }

  if (wxMode == WX_CITY_DETAIL) {
    if (t.y >= 212) {
      if (t.x < 80) {
        // [< ALL 7]
        drawWeatherTable();
        return;
      } else if (t.x >= 82 && t.x <= 142) {
        // [< PREV]
        drawCityDetail((selectedCity + NUM_CITIES - 1) % NUM_CITIES);
        return;
      } else if (t.x >= 144 && t.x <= 204) {
        // [NEXT >]
        drawCityDetail((selectedCity + 1) % NUM_CITIES);
        return;
      } else if (t.x >= 206) {
        // [NWS DECODED TXT]
        fetchAndShowNwsDecoded(selectedCity);
        return;
      }
    } else if (t.y > 25) {
      // Tapping main card cycles next city
      drawCityDetail((selectedCity + 1) % NUM_CITIES);
    }
    return;
  }

  if (wxMode == WX_CITY_NWS_TEXT) {
    if (t.y >= 212 && t.x <= 104) {
      drawWeatherTable();
    } else {
      drawCityDetail(selectedCity);
    }
    return;
  }
}

const DemoApp DEMO_WEATHER = {
  "NOAA WEATHER",
  "NYC LAX ATL ORD SFO DEN SEA",
  0x5DFF, // Sky blue
  weatherEnter,
  weatherLoop,
  weatherTap,
  nullptr,
  nullptr
};
