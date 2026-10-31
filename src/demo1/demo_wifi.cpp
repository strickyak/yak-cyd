#include "cyd_shell.h"
#include <Preferences.h>
#include <WiFi.h>

namespace {

enum WifiScreenMode {
  WIFI_MODE_SCAN,
  WIFI_MODE_KEYBOARD,
  WIFI_MODE_STATUS
};

static WifiScreenMode mode = WIFI_MODE_SCAN;
static Preferences wifiPrefs;
static bool backToSettings = true;

static void exitWifiScreen() {
  if (backToSettings) {
    launchDemo(&DEMO_SETTINGS);
  } else {
    returnToMenu();
  }
}

static const int MAX_SCAN_RESULTS = 5;
static int scanCount = 0;
static String scanSsids[MAX_SCAN_RESULTS];
static int32_t scanRssi[MAX_SCAN_RESULTS];
static bool scanOpen[MAX_SCAN_RESULTS];

static String targetSsid = "";
static String typedPass = "";
static int kbPage = 0; // 0 = lower, 1 = upper, 2 = symbols

static const char *const KB_ROWS[3][4] = {
  {
    "1234567890",
    "qwertyuiop",
    "asdfghjkl-",
    "zxcvbnm._!"
  },
  {
    "1234567890",
    "QWERTYUIOP",
    "ASDFGHJKL-",
    "ZXCVBNM._!"
  },
  {
    "!@#$%^&*()",
    "-_=+[]{}\\|",
    ";:'\",.<>/?",
    "`~$%&*+=-_"
  }
};

void startWifiScan();
void drawKeyboardScreen();
void connectSelectedNetwork();

bool loadSavedWifi(String &ssidOut, String &passOut) {
  wifiPrefs.begin("cyd_wifi", true);
  ssidOut = wifiPrefs.getString("ssid", "");
  passOut = wifiPrefs.getString("pass", "");
  wifiPrefs.end();
  return ssidOut.length() > 0;
}

void saveWifiToNvs(const String &ssid, const String &pass) {
  wifiPrefs.begin("cyd_wifi", false);
  wifiPrefs.putString("ssid", ssid);
  wifiPrefs.putString("pass", pass);
  wifiPrefs.end();
  Serial.printf("[WIFI] Saved SSID '%s' to NVS\n", ssid.c_str());
}

// ---------------------------------------------------------------------------
// Screen 1: 2.4 GHz Network Scanner
// ---------------------------------------------------------------------------
void drawScanScreen(bool scanning) {
  tft.fillScreen(0x0841);
  tft.fillRect(0, 0, SCREEN_W, 26, TFT_NAVY);
  tft.drawFastHLine(0, 26, SCREEN_W, TFT_CYAN);

  // [< SET] (x: 4..58, y: 3..23)
  tft.fillRoundRect(4, 3, 54, 20, 4, 0x2945);
  tft.drawRoundRect(4, 3, 54, 20, 4, TFT_CYAN);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_CYAN, 0x2945);
  tft.drawString(backToSettings ? "< SET" : "MENU", 31, 13, 1);

  tft.setTextColor(TFT_YELLOW, TFT_NAVY);
  tft.drawString("WIFI CONFIG (2.4GHz)", SCREEN_W / 2, 13, 2);

  // [RESCAN] (x: 256..316, y: 3..23)
  tft.fillRoundRect(256, 3, 60, 20, 4, 0x2945);
  tft.drawRoundRect(256, 3, 60, 20, 4, TFT_GREEN);
  tft.setTextColor(TFT_GREEN, 0x2945);
  tft.drawString("RESCAN", 286, 13, 1);

  // Footer showing current connection state
  tft.fillRect(0, 222, SCREEN_W, 18, 0x0000);
  tft.setTextDatum(MC_DATUM);
  if (WiFi.status() == WL_CONNECTED) {
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    String st = "Connected: " + WiFi.SSID() + " (" + WiFi.localIP().toString() + ")";
    tft.drawString(st, SCREEN_W / 2, 231, 1);
  } else {
    String savedSsid, savedPass;
    if (loadSavedWifi(savedSsid, savedPass)) {
      tft.setTextColor(TFT_YELLOW, TFT_BLACK);
      tft.drawString("Saved SSID: " + savedSsid + " (Not connected)", SCREEN_W / 2, 231, 1);
    } else {
      tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
      tft.drawString("Tap a network below to configure Wi-Fi", SCREEN_W / 2, 231, 1);
    }
  }

  if (scanning) {
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, 0x0841);
    tft.drawString("Scanning 2.4 GHz Wi-Fi networks...", SCREEN_W / 2, 120, 2);
    return;
  }

  if (scanCount == 0) {
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_ORANGE, 0x0841);
    tft.drawString("No 2.4 GHz networks found.", SCREEN_W / 2, 110, 2);
    tft.setTextColor(TFT_LIGHTGREY, 0x0841);
    tft.drawString("Tap RESCAN at top-right to try again.", SCREEN_W / 2, 136, 2);
    return;
  }

  for (int i = 0; i < scanCount; i++) {
    int rowY = 32 + i * 37;
    int rowH = 33;

    bool isCurrent = (WiFi.status() == WL_CONNECTED && WiFi.SSID() == scanSsids[i]);
    uint16_t borderCol = isCurrent ? TFT_GREEN : TFT_CYAN;

    tft.fillRoundRect(10, rowY, SCREEN_W - 20, rowH, 6, 0x18E3);
    tft.drawRoundRect(10, rowY, SCREEN_W - 20, rowH, 6, borderCol);

    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(isCurrent ? TFT_GREEN : TFT_WHITE, 0x18E3);
    String label = scanSsids[i];
    if (label.length() > 21) label = label.substring(0, 21) + "..";
    tft.drawString(label, 20, rowY + 16, 2);

    tft.setTextDatum(MR_DATUM);
    uint16_t sigCol = (scanRssi[i] > -65) ? TFT_GREEN :
                      (scanRssi[i] > -78) ? TFT_YELLOW : TFT_ORANGE;
    tft.setTextColor(sigCol, 0x18E3);
    String info = String(scanRssi[i]) + "dBm " + (scanOpen[i] ? "[OPEN]" : "[LOCK]");
    tft.drawString(info, SCREEN_W - 18, rowY + 16, 1);
  }
}

void startWifiScan() {
  mode = WIFI_MODE_SCAN;
  drawScanScreen(true);

  WiFi.mode(WIFI_STA);
  int n = WiFi.scanNetworks(false, false);
  scanCount = 0;
  for (int i = 0; i < n && scanCount < MAX_SCAN_RESULTS; i++) {
    String s = WiFi.SSID(i);
    if (s.length() == 0) continue;
    bool dup = false;
    for (int j = 0; j < scanCount; j++) {
      if (scanSsids[j] == s) { dup = true; break; }
    }
    if (dup) continue;

    scanSsids[scanCount] = s;
    scanRssi[scanCount]  = WiFi.RSSI(i);
    scanOpen[scanCount]  = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
    scanCount++;
  }
  WiFi.scanDelete();
  drawScanScreen(false);
}

void handleScanTap(const TouchSample &t) {
  if (t.y <= 28) {
    if (t.x <= 60) {
      exitWifiScreen();
      return;
    }
    if (t.x >= 250) {
      startWifiScan();
      return;
    }
    return;
  }

  for (int i = 0; i < scanCount; i++) {
    int rowY = 32 + i * 37;
    int rowH = 33;
    if (t.x >= 10 && t.x <= SCREEN_W - 10 && t.y >= rowY && t.y <= rowY + rowH) {
      targetSsid = scanSsids[i];
      if (scanOpen[i]) {
        typedPass = "";
        connectSelectedNetwork();
      } else {
        String savedSsid, savedPass;
        if (loadSavedWifi(savedSsid, savedPass) && savedSsid == targetSsid) {
          typedPass = savedPass;
        } else {
          typedPass = "";
        }
        kbPage = 0;
        mode = WIFI_MODE_KEYBOARD;
        drawKeyboardScreen();
      }
      return;
    }
  }
}

// ---------------------------------------------------------------------------
// Screen 2: On-Screen Stylus Keyboard
// ---------------------------------------------------------------------------
void drawPasswordBox() {
  tft.fillRoundRect(4, 28, 246, 30, 4, TFT_BLACK);
  tft.drawRoundRect(4, 28, 246, 30, 4, TFT_CYAN);

  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  String shown = typedPass + "_";
  if (shown.length() > 26) {
    shown = ".." + shown.substring(shown.length() - 25);
  }
  tft.drawString(shown, 10, 43, 2);
}

void drawKeyboardKeys() {
  tft.fillRect(0, 62, SCREEN_W, SCREEN_H - 62, 0x0841);

  for (int r = 0; r < 4; r++) {
    const char *rowStr = KB_ROWS[kbPage][r];
    int ky = 64 + r * 35;
    for (int c = 0; c < 10; c++) {
      int kx = 2 + c * 32;
      char ch[2] = { rowStr[c], '\0' };

      tft.fillRoundRect(kx, ky, 29, 32, 4, 0x18E3);
      tft.drawRoundRect(kx, ky, 29, 32, 4, 0x4A69);
      tft.setTextDatum(MC_DATUM);
      tft.setTextColor(TFT_WHITE, 0x18E3);
      tft.drawString(ch, kx + 14, ky + 16, 2);
    }
  }

  int by = 205;
  int bh = 32;

  // [ABC / abc] (x: 2..76)
  uint16_t shiftCol = (kbPage == 1) ? TFT_YELLOW : 0x2945;
  tft.fillRoundRect(2, by, 74, bh, 4, shiftCol);
  tft.drawRoundRect(2, by, 74, bh, 4, TFT_YELLOW);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor((kbPage == 1) ? TFT_BLACK : TFT_YELLOW, shiftCol);
  tft.drawString((kbPage == 1) ? "abc" : "ABC", 39, by + 16, 2);

  // [#+=] (x: 80..142)
  uint16_t symCol = (kbPage == 2) ? TFT_MAGENTA : 0x2945;
  tft.fillRoundRect(80, by, 62, bh, 4, symCol);
  tft.drawRoundRect(80, by, 62, bh, 4, TFT_MAGENTA);
  tft.setTextColor((kbPage == 2) ? TFT_BLACK : TFT_MAGENTA, symCol);
  tft.drawString((kbPage == 2) ? "abc" : "#+=", 111, by + 16, 2);

  // [SPACE] (x: 146..238)
  tft.fillRoundRect(146, by, 92, bh, 4, 0x18E3);
  tft.drawRoundRect(146, by, 92, bh, 4, TFT_WHITE);
  tft.setTextColor(TFT_WHITE, 0x18E3);
  tft.drawString("SPACE", 192, by + 16, 2);

  // [CLEAR] (x: 242..317)
  tft.fillRoundRect(242, by, 75, bh, 4, 0x3800);
  tft.drawRoundRect(242, by, 75, bh, 4, TFT_ORANGE);
  tft.setTextColor(TFT_ORANGE, 0x3800);
  tft.drawString("CLEAR", 279, by + 16, 2);
}

void drawKeyboardScreen() {
  tft.fillScreen(0x0841);
  tft.fillRect(0, 0, SCREEN_W, 25, TFT_NAVY);

  // [< SCAN] (x: 2..58, y: 2..22)
  tft.fillRoundRect(2, 2, 56, 20, 4, 0x2945);
  tft.drawRoundRect(2, 2, 56, 20, 4, TFT_CYAN);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_CYAN, 0x2945);
  tft.drawString("< SCAN", 30, 12, 1);

  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(TFT_YELLOW, TFT_NAVY);
  String ssidLabel = targetSsid;
  if (ssidLabel.length() > 16) ssidLabel = ssidLabel.substring(0, 16) + "..";
  tft.drawString(ssidLabel, 64, 12, 2);

  // [CONNECT] (x: 236..317, y: 2..22)
  tft.fillRoundRect(236, 2, 81, 20, 4, TFT_DARKGREEN);
  tft.drawRoundRect(236, 2, 81, 20, 4, TFT_GREEN);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREEN);
  tft.drawString("CONNECT", 276, 12, 1);

  // [DEL] (x: 254..317, y: 28..58)
  tft.fillRoundRect(254, 28, 63, 30, 4, 0x4800);
  tft.drawRoundRect(254, 28, 63, 30, 4, TFT_RED);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, 0x4800);
  tft.drawString("DEL", 285, 43, 2);

  drawPasswordBox();
  drawKeyboardKeys();
}

void handleKeyboardTap(const TouchSample &t) {
  if (t.y <= 25) {
    if (t.x <= 60) {
      startWifiScan();
      return;
    }
    if (t.x >= 232) {
      connectSelectedNetwork();
      return;
    }
    return;
  }

  if (t.y >= 26 && t.y <= 60) {
    if (t.x >= 250 && typedPass.length() > 0) {
      typedPass.remove(typedPass.length() - 1);
      drawPasswordBox();
    }
    return;
  }

  if (t.y >= 64 && t.y < 204) {
    int r = (t.y - 64) / 35;
    int c = t.x / 32;
    if (r >= 0 && r < 4 && c >= 0 && c < 10) {
      char ch = KB_ROWS[kbPage][r][c];
      if (typedPass.length() < 63) {
        typedPass += ch;
        int kx = 2 + c * 32;
        int ky = 64 + r * 35;
        tft.drawRoundRect(kx, ky, 29, 32, 4, TFT_CYAN);
        drawPasswordBox();
      }
    }
    return;
  }

  if (t.y >= 204) {
    if (t.x < 78) {
      kbPage = (kbPage == 1) ? 0 : 1;
      drawKeyboardKeys();
    } else if (t.x >= 78 && t.x < 144) {
      kbPage = (kbPage == 2) ? 0 : 2;
      drawKeyboardKeys();
    } else if (t.x >= 144 && t.x < 240) {
      if (typedPass.length() < 63) {
        typedPass += ' ';
        drawPasswordBox();
      }
    } else {
      typedPass = "";
      drawPasswordBox();
    }
  }
}

// ---------------------------------------------------------------------------
// Screen 3: Connection Result Status Card
// ---------------------------------------------------------------------------
void connectSelectedNetwork() {
  mode = WIFI_MODE_STATUS;
  tft.fillScreen(0x0841);
  tft.fillRect(0, 0, SCREEN_W, 26, TFT_NAVY);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_YELLOW, TFT_NAVY);
  tft.drawString("CONNECTING TO WI-FI...", SCREEN_W / 2, 13, 2);

  tft.setTextColor(TFT_WHITE, 0x0841);
  tft.drawString("SSID: " + targetSsid, SCREEN_W / 2, 90, 2);
  tft.setTextColor(TFT_CYAN, 0x0841);
  tft.drawString("Waiting for DHCP address...", SCREEN_W / 2, 118, 2);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  if (typedPass.length() > 0) {
    WiFi.begin(targetSsid.c_str(), typedPass.c_str());
  } else {
    WiFi.begin(targetSsid.c_str());
  }

  uint32_t startMs = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - startMs) < 10000) {
    delay(200);
  }

  tft.fillRect(0, 40, SCREEN_W, 190, 0x0841);
  if (WiFi.status() == WL_CONNECTED) {
    saveWifiToNvs(targetSsid, typedPass);
    syncNtpTimezone();
    setRgbLed(false, true, false, 300);

    tft.setTextColor(TFT_GREEN, 0x0841);
    tft.drawString("WI-FI CONNECTED!", SCREEN_W / 2, 70, 4);

    tft.setTextColor(TFT_WHITE, 0x0841);
    tft.drawString("SSID: " + WiFi.SSID(), SCREEN_W / 2, 106, 2);
    tft.drawString("IP: " + WiFi.localIP().toString() + "  (" + String(WiFi.RSSI()) + " dBm)",
                   SCREEN_W / 2, 130, 2);
    tft.setTextColor(TFT_CYAN, 0x0841);
    tft.drawString("Credentials saved to NVS flash.", SCREEN_W / 2, 154, 2);
  } else {
    setRgbLed(true, false, false, 300);
    tft.setTextColor(TFT_RED, 0x0841);
    tft.drawString("CONNECTION FAILED", SCREEN_W / 2, 85, 4);
    tft.setTextColor(TFT_LIGHTGREY, 0x0841);
    tft.drawString("Check password or signal for " + targetSsid, SCREEN_W / 2, 125, 2);
  }

  // Bottom buttons: [ < SETTINGS ] and [ SCAN AGAIN ]
  tft.fillRoundRect(16, 182, 138, 42, 6, TFT_DARKGREEN);
  tft.drawRoundRect(16, 182, 138, 42, 6, TFT_GREEN);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREEN);
  tft.drawString(backToSettings ? "< SETTINGS" : "MAIN MENU", 85, 203, 2);

  tft.fillRoundRect(166, 182, 138, 42, 6, 0x2945);
  tft.drawRoundRect(166, 182, 138, 42, 6, TFT_CYAN);
  tft.setTextColor(TFT_CYAN, 0x2945);
  tft.drawString("SCAN AGAIN", 235, 203, 2);
}

void handleStatusTap(const TouchSample &t) {
  if (t.y >= 175 && t.y <= 232) {
    if (t.x >= 12 && t.x <= 158) {
      exitWifiScreen();
    } else if (t.x >= 162 && t.x <= 308) {
      startWifiScan();
    }
  }
}

void wifiEnter() {
  startWifiScan();
}

void wifiOnTap(const TouchSample &t) {
  if (mode == WIFI_MODE_SCAN) {
    handleScanTap(t);
  } else if (mode == WIFI_MODE_KEYBOARD) {
    handleKeyboardTap(t);
  } else if (mode == WIFI_MODE_STATUS) {
    handleStatusTap(t);
  }
}

} // namespace

// ---------------------------------------------------------------------------
// Shared Helper for Apps Requiring Wi-Fi (TAZ, THUMB, etc.)
// ---------------------------------------------------------------------------
bool ensureWifiOrPrompt() {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

  String savedSsid, savedPass;
  if (loadSavedWifi(savedSsid, savedPass)) {
    tft.fillScreen(0x0841);
    tft.fillRect(0, 0, SCREEN_W, 26, TFT_NAVY);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_YELLOW, TFT_NAVY);
    tft.drawString("CONNECTING TO SAVED WI-FI...", SCREEN_W / 2, 13, 2);
    tft.setTextColor(TFT_WHITE, 0x0841);
    tft.drawString("SSID: " + savedSsid, SCREEN_W / 2, 110, 2);

    WiFi.mode(WIFI_STA);
    if (savedPass.length() > 0) {
      WiFi.begin(savedSsid.c_str(), savedPass.c_str());
    } else {
      WiFi.begin(savedSsid.c_str());
    }

    uint32_t startMs = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - startMs) < 8000) {
      delay(200);
    }

    if (WiFi.status() == WL_CONNECTED) {
      syncNtpTimezone();
      setRgbLed(false, true, false, 150);
      return true;
    }
  }

  // Draw prompt suggesting opening WIFI CONFIG in Settings
  setRgbLed(true, false, false, 200);
  tft.fillScreen(0x0841);
  tft.fillRect(0, 0, SCREEN_W, 26, TFT_NAVY);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_YELLOW, TFT_NAVY);
  tft.drawString("WI-FI CONNECTION REQUIRED", SCREEN_W / 2, 13, 2);

  tft.setTextColor(TFT_ORANGE, 0x0841);
  tft.drawString("Wi-Fi is not connected.", SCREEN_W / 2, 72, 4);

  tft.setTextColor(TFT_LIGHTGREY, 0x0841);
  tft.drawString("Open Wi-Fi Config in Settings to select", SCREEN_W / 2, 112, 2);
  tft.drawString("your 2.4 GHz network and password.", SCREEN_W / 2, 132, 2);

  // [ WIFI SETTINGS ] (x: 16..164, y: 174..222)
  tft.fillRoundRect(16, 174, 148, 46, 6, TFT_DARKGREEN);
  tft.drawRoundRect(16, 174, 148, 46, 6, TFT_GREEN);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREEN);
  tft.drawString("WIFI SETTINGS", 90, 197, 2);

  // [ MAIN MENU ] (x: 174..304, y: 174..222)
  tft.fillRoundRect(174, 174, 130, 46, 6, 0x2945);
  tft.drawRoundRect(174, 174, 130, 46, 6, TFT_CYAN);
  tft.setTextColor(TFT_CYAN, 0x2945);
  tft.drawString("MAIN MENU", 239, 197, 2);

  return false;
}

void openWifiConfigFromSettings(bool returnToSettings) {
  backToSettings = returnToSettings;
  launchDemo(&DEMO_WIFI);
}

bool handleWifiPromptTap(const TouchSample &t) {
  if (t.y >= 168 && t.y <= 228) {
    if (t.x >= 12 && t.x <= 168) {
      openWifiConfigFromSettings(true);
      return true;
    }
    if (t.x >= 170 && t.x <= 308) {
      returnToMenu();
      return true;
    }
  }
  return false;
}

const DemoApp DEMO_WIFI = {
  "WIFI CONFIG",
  "Scan 2.4GHz networks & enter password",
  TFT_CYAN,
  wifiEnter,
  nullptr,
  wifiOnTap,
  nullptr,
  nullptr
};
