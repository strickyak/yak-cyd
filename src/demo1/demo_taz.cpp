#include "cyd_shell.h"
#include <HTTPClient.h>
#include <WiFi.h>
#include <vector>

namespace {

static const char *TAZ_URL = "http://yak.net/taz.txt";
static const int DEFAULT_TAZ_SIZE = 269651;
static const int LINES_PER_PAGE = 11;

static bool showingWifiPrompt = false;
static int tazFileSize = DEFAULT_TAZ_SIZE;
static int lastByteOffset = 0;
static std::vector<String> wrappedLines;
static int scrollLine = 0;
static String statusFooter = "";

void drawReaderScreen();

void wrapParagraphText(const String &para) {
  wrappedLines.clear();
  scrollLine = 0;

  String clean = "";
  clean.reserve(para.length());
  bool lastWasSpace = true;
  for (size_t i = 0; i < para.length(); i++) {
    char c = para[i];
    if (c == '\r' || c == '\n' || c == '\t' || c == ' ') {
      if (!lastWasSpace) {
        clean += ' ';
        lastWasSpace = true;
      }
    } else if (c >= 32 && c <= 126) {
      clean += c;
      lastWasSpace = false;
    }
  }
  clean.trim();

  const int maxPxWidth = SCREEN_W - 14;
  String currentLine = "";
  int idx = 0;
  int len = clean.length();

  while (idx < len) {
    int nextSpace = clean.indexOf(' ', idx);
    if (nextSpace < 0) nextSpace = len;
    String word = clean.substring(idx, nextSpace);
    idx = nextSpace + 1;

    if (currentLine.length() == 0) {
      currentLine = word;
    } else {
      String candidate = currentLine + " " + word;
      if (tft.textWidth(candidate, 2) <= maxPxWidth) {
        currentLine = candidate;
      } else {
        wrappedLines.push_back(currentLine);
        currentLine = word;
      }
    }
  }
  if (currentLine.length() > 0) {
    wrappedLines.push_back(currentLine);
  }
}

bool isUsenetHeaderParagraph(const String &p) {
  if (p.length() < 45) return true;
  if (p.startsWith("From ") || p.startsWith("Path:") ||
      p.startsWith("Return-Path:") || p.startsWith("Newsgroups:") ||
      p.startsWith("Article:") || p.startsWith("Subject:")) {
    return true;
  }
  return false;
}

void fetchRandomParagraph() {
  if (!ensureWifiOrPrompt()) {
    showingWifiPrompt = true;
    return;
  }
  showingWifiPrompt = false;

  tft.fillRect(0, 222, SCREEN_W, 18, 0x0841);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_YELLOW, 0x0841);
  tft.drawString("Fetching random slice from http://yak.net/taz.txt ...", SCREEN_W / 2, 231, 1);
  setRgbLed(false, false, true, 100);

  const int windowBytes = 6144;
  int maxStart = max(1000, tazFileSize - windowBytes - 500);
  lastByteOffset = random(800, maxStart);

  HTTPClient http;
  http.begin(TAZ_URL);
  char rangeHdr[48];
  snprintf(rangeHdr, sizeof(rangeHdr), "bytes=%d-%d",
           lastByteOffset, lastByteOffset + windowBytes - 1);
  http.addHeader("Range", rangeHdr);

  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_PARTIAL_CONTENT || httpCode == HTTP_CODE_OK) {
    String chunk = http.getString();
    http.end();

    String chosenPara = "";
    int searchPos = 0;
    while (searchPos < (int)chunk.length()) {
      int pStart = chunk.indexOf("\n\n", searchPos);
      if (pStart < 0) break;
      pStart += 2;
      while (pStart < (int)chunk.length() && (chunk[pStart] == '\n' || chunk[pStart] == '\r')) {
        pStart++;
      }
      int pEnd = chunk.indexOf("\n\n", pStart);
      if (pEnd < 0) break;

      String candidate = chunk.substring(pStart, pEnd);
      candidate.trim();
      if (!isUsenetHeaderParagraph(candidate)) {
        chosenPara = candidate;
        break;
      }
      searchPos = pEnd;
    }

    if (chosenPara.length() == 0) {
      chosenPara = chunk.substring(min(200, (int)chunk.length()),
                                   min(1200, (int)chunk.length()));
    }

    wrapParagraphText(chosenPara);
    int pct = (lastByteOffset * 100) / max(1, tazFileSize);
    statusFooter = "yak.net/taz.txt @ byte " + String(lastByteOffset) +
                   " (" + String(pct) + "%) | " + WiFi.SSID() +
                   " (" + String(WiFi.RSSI()) + "dBm)";
    setRgbLed(false, true, false, 100);
  } else {
    String err = "HTTP Error " + String(httpCode) + " fetching " + String(TAZ_URL);
    http.end();
    wrapParagraphText(err);
    statusFooter = "Check Wi-Fi connection in WIFI CONFIG";
    setRgbLed(true, false, false, 200);
  }

  drawReaderScreen();
}

void drawReaderTextBody() {
  tft.fillRect(0, 26, SCREEN_W, 194, TFT_BLACK);

  int totalLines = (int)wrappedLines.size();
  int endLine = min(totalLines, scrollLine + LINES_PER_PAGE);

  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);

  for (int i = scrollLine; i < endLine; i++) {
    int y = 30 + (i - scrollLine) * 17;
    tft.drawString(wrappedLines[i], 6, y, 2);
  }

  if (totalLines > LINES_PER_PAGE) {
    tft.drawFastVLine(SCREEN_W - 3, 28, 190, 0x2945);
    int thumbH = max(18, (190 * LINES_PER_PAGE) / totalLines);
    int maxScroll = totalLines - LINES_PER_PAGE;
    int thumbY = 28 + ((190 - thumbH) * scrollLine) / max(1, maxScroll);
    tft.fillRect(SCREEN_W - 4, thumbY, 3, thumbH, TFT_CYAN);
  }
}

void drawReaderTopBar() {
  tft.fillRect(0, 0, SCREEN_W, 25, 0x0841);
  tft.drawFastHLine(0, 25, SCREEN_W, TFT_DARKGREY);
  tft.setTextDatum(MC_DATUM);

  // [MENU] (x: 2..54)
  tft.fillRoundRect(2, 2, 52, 20, 4, 0x2945);
  tft.drawRoundRect(2, 2, 52, 20, 4, TFT_CYAN);
  tft.setTextColor(TFT_CYAN, 0x2945);
  tft.drawString("MENU", 28, 12, 1);

  // [RANDOM PARA] (x: 60..186)
  tft.fillRoundRect(60, 2, 126, 20, 4, TFT_DARKGREEN);
  tft.drawRoundRect(60, 2, 126, 20, 4, TFT_GREEN);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREEN);
  tft.drawString("RANDOM PARAGRAPH", 123, 12, 1);

  // [UP] (x: 192..252)
  bool canUp = (scrollLine > 0);
  tft.fillRoundRect(192, 2, 60, 20, 4, canUp ? 0x2945 : 0x10A2);
  tft.drawRoundRect(192, 2, 60, 20, 4, canUp ? TFT_YELLOW : TFT_DARKGREY);
  tft.setTextColor(canUp ? TFT_YELLOW : TFT_DARKGREY, canUp ? 0x2945 : 0x10A2);
  tft.drawString("UP", 222, 12, 1);

  // [DOWN] (x: 258..317)
  bool canDn = (scrollLine + LINES_PER_PAGE < (int)wrappedLines.size());
  tft.fillRoundRect(258, 2, 59, 20, 4, canDn ? 0x2945 : 0x10A2);
  tft.drawRoundRect(258, 2, 59, 20, 4, canDn ? TFT_YELLOW : TFT_DARKGREY);
  tft.setTextColor(canDn ? TFT_YELLOW : TFT_DARKGREY, canDn ? 0x2945 : 0x10A2);
  tft.drawString("DOWN", 287, 12, 1);
}

void drawReaderScreen() {
  tft.fillScreen(TFT_BLACK);
  drawReaderTopBar();
  drawReaderTextBody();

  tft.fillRect(0, 222, SCREEN_W, 18, 0x0841);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_CYAN, 0x0841);
  tft.drawString(statusFooter, SCREEN_W / 2, 231, 1);
}

void tazEnter() {
  fetchRandomParagraph();
}

void tazOnTap(const TouchSample &t) {
  if (showingWifiPrompt) {
    handleWifiPromptTap(t);
    return;
  }

  if (t.y <= 26) {
    if (t.x <= 56) {
      returnToMenu();
      return;
    }
    if (t.x >= 58 && t.x <= 188) {
      fetchRandomParagraph();
      return;
    }
    if (t.x >= 190 && t.x <= 254) {
      if (scrollLine > 0) {
        scrollLine = max(0, scrollLine - 5);
        drawReaderTopBar();
        drawReaderTextBody();
      }
      return;
    }
    if (t.x >= 256) {
      if (scrollLine + LINES_PER_PAGE < (int)wrappedLines.size()) {
        scrollLine = min((int)wrappedLines.size() - LINES_PER_PAGE, scrollLine + 5);
        drawReaderTopBar();
        drawReaderTextBody();
      }
      return;
    }
    return;
  }

  if (scrollLine + LINES_PER_PAGE < (int)wrappedLines.size()) {
    scrollLine = min((int)wrappedLines.size() - LINES_PER_PAGE, scrollLine + 5);
    drawReaderTopBar();
    drawReaderTextBody();
  } else {
    fetchRandomParagraph();
  }
}

} // namespace

const DemoApp DEMO_TAZ = {
  "TAZ.TXT",
  "Random paragraph from yak.net",
  TFT_GREEN,
  tazEnter,
  nullptr,
  tazOnTap,
  nullptr,
  nullptr
};
