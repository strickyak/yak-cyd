#include "cyd_shell.h"
#include <HTTPClient.h>
#include <WiFi.h>
#include <esp32/rom/tjpgd.h>
#include <vector>

namespace {

static const char *THUMB_DIR_URL = "http://yak.net/strick/thumb/";
static const int MAX_IMG_W = 160;
static const int MAX_IMG_H = 120;

enum ThumbViewMode {
  VIEW_2X_ZOOM = 0, // 2x bilinear-smoothed upscale (264x198)
  VIEW_1X_NAT  = 1, // 1x native resolution (132x99)
  VIEW_4UP     = 2  // 2x2 grid of 4 random images
};

static ThumbViewMode viewMode = VIEW_2X_ZOOM;
static bool showingWifiPrompt = false;

static std::vector<String> jpgFiles;
static int currentImgIdx = -1;
static int gridImgIndices[4] = {-1, -1, -1, -1};

// Decoded image buffer (RGB565, native size typically 132x99)
static uint16_t decodedImg[MAX_IMG_H][MAX_IMG_W];
static int decodedW = 0;
static int decodedH = 0;
static int lastJpgBytes = 0;
static String statusFooter = "";

// TJpgDec ROM decoder state
struct JpgMemStream {
  const uint8_t *data;
  size_t size;
  size_t pos;
};

static uint8_t tjpgdWork[4096];

UINT jpgInCallback(JDEC *jd, BYTE *buff, UINT nbyte) {
  JpgMemStream *stream = (JpgMemStream *)jd->device;
  size_t avail = (stream->pos < stream->size) ? (stream->size - stream->pos) : 0;
  size_t toRead = (nbyte < avail) ? nbyte : avail;
  if (buff && toRead > 0) {
    memcpy(buff, stream->data + stream->pos, toRead);
  }
  stream->pos += toRead;
  return (UINT)toRead;
}

UINT jpgOutCallback(JDEC *jd, void *bitmap, JRECT *rect) {
  const uint8_t *src = (const uint8_t *)bitmap;
  for (int y = rect->top; y <= rect->bottom; y++) {
    for (int x = rect->left; x <= rect->right; x++) {
      uint8_t r = src[0];
      uint8_t g = src[1];
      uint8_t b = src[2];
      src += 3;
      if (x < MAX_IMG_W && y < MAX_IMG_H) {
        decodedImg[y][x] = ((uint16_t)(r & 0xF8) << 8) |
                           ((uint16_t)(g & 0xFC) << 3) |
                           (b >> 3);
      }
    }
  }
  return 1; // Continue decoding
}

bool decodeJpegBuffer(const uint8_t *buf, size_t len) {
  JpgMemStream stream = { buf, len, 0 };
  JDEC jdec;
  JRESULT res = jd_prepare(&jdec, jpgInCallback, tjpgdWork, sizeof(tjpgdWork), &stream);
  if (res != JDR_OK) {
    Serial.printf("[THUMB] jd_prepare error: %d\n", (int)res);
    return false;
  }
  decodedW = min((int)jdec.width, MAX_IMG_W);
  decodedH = min((int)jdec.height, MAX_IMG_H);
  memset(decodedImg, 0, sizeof(decodedImg));

  res = jd_decomp(&jdec, jpgOutCallback, 0);
  if (res != JDR_OK) {
    Serial.printf("[THUMB] jd_decomp error: %d\n", (int)res);
    return false;
  }
  return true;
}

// Fast RGB565 average of 2 colors (for smooth 2x bilinear upscale)
inline uint16_t avg565(uint16_t c0, uint16_t c1) {
  return (((c0 ^ c1) & 0xF7DEU) >> 1) + (c0 & c1);
}

void drawTopBar() {
  tft.fillRect(0, 0, SCREEN_W, 24, 0x0841);
  tft.drawFastHLine(0, 24, SCREEN_W, TFT_DARKGREY);
  tft.setTextDatum(MC_DATUM);

  // [MENU] (x: 2..50)
  tft.fillRoundRect(2, 2, 48, 20, 4, 0x2945);
  tft.drawRoundRect(2, 2, 48, 20, 4, TFT_CYAN);
  tft.setTextColor(TFT_CYAN, 0x2945);
  tft.drawString("MENU", 26, 12, 1);

  // [NEXT IMG] (x: 54..148)
  tft.fillRoundRect(54, 2, 94, 20, 4, TFT_DARKGREEN);
  tft.drawRoundRect(54, 2, 94, 20, 4, TFT_GREEN);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREEN);
  tft.drawString("RANDOM IMG", 101, 12, 1);

  // [2X ZOOM] (x: 152..210)
  bool is2x = (viewMode == VIEW_2X_ZOOM);
  tft.fillRoundRect(152, 2, 58, 20, 4, is2x ? TFT_YELLOW : 0x2945);
  tft.drawRoundRect(152, 2, 58, 20, 4, TFT_YELLOW);
  tft.setTextColor(is2x ? TFT_BLACK : TFT_YELLOW, is2x ? TFT_YELLOW : 0x2945);
  tft.drawString("2X ZOOM", 181, 12, 1);

  // [1X NAT] (x: 214..264)
  bool is1x = (viewMode == VIEW_1X_NAT);
  tft.fillRoundRect(214, 2, 50, 20, 4, is1x ? TFT_YELLOW : 0x2945);
  tft.drawRoundRect(214, 2, 50, 20, 4, TFT_YELLOW);
  tft.setTextColor(is1x ? TFT_BLACK : TFT_YELLOW, is1x ? TFT_YELLOW : 0x2945);
  tft.drawString("1X", 239, 12, 1);

  // [4-UP] (x: 268..317)
  bool is4up = (viewMode == VIEW_4UP);
  tft.fillRoundRect(268, 2, 49, 20, 4, is4up ? TFT_MAGENTA : 0x2945);
  tft.drawRoundRect(268, 2, 49, 20, 4, TFT_MAGENTA);
  tft.setTextColor(is4up ? TFT_BLACK : TFT_MAGENTA, is4up ? TFT_MAGENTA : 0x2945);
  tft.drawString("4-UP", 292, 12, 1);
}

void drawFooter(const String &msg, uint16_t color = TFT_CYAN) {
  tft.fillRect(0, 223, SCREEN_W, 17, 0x0841);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(color, 0x0841);
  tft.drawString(msg, SCREEN_W / 2, 231, 1);
}

// Render decodedImg at either 1x or smooth 2x
void renderDecodedSingle() {
  tft.fillRect(0, 25, SCREEN_W, 198, TFT_BLACK);
  if (decodedW <= 0 || decodedH <= 0) return;

  tft.setSwapBytes(true);

  if (viewMode == VIEW_1X_NAT) {
    int ox = (SCREEN_W - decodedW) / 2;
    int oy = 25 + (198 - decodedH) / 2;
    tft.drawRect(ox - 2, oy - 2, decodedW + 4, decodedH + 4, TFT_DARKGREY);
    for (int y = 0; y < decodedH; y++) {
      tft.pushImage(ox, oy + y, decodedW, 1, decodedImg[y]);
    }
  } else {
    // 2x bilinear-smoothed upscale (e.g. 132x99 -> 264x198)
    static uint16_t lineEven[MAX_IMG_W * 2];
    static uint16_t lineOdd[MAX_IMG_W * 2];
    int outW = min(SCREEN_W, decodedW * 2);
    int outH = min(198, decodedH * 2);
    int ox = (SCREEN_W - outW) / 2;
    int oy = 25 + (198 - outH) / 2;

    for (int y = 0; y < decodedH && (y * 2) < outH; y++) {
      int yn = min(decodedH - 1, y + 1);
      const uint16_t *row0 = decodedImg[y];
      const uint16_t *row1 = decodedImg[yn];

      for (int x = 0; x < decodedW; x++) {
        int xn = min(decodedW - 1, x + 1);
        uint16_t p00 = row0[x];
        uint16_t p01 = row0[xn];
        uint16_t p10 = row1[x];
        uint16_t p11 = row1[xn];

        uint16_t topMid = avg565(p00, p01);
        uint16_t botLeft = avg565(p00, p10);
        uint16_t botMid  = avg565(topMid, avg565(p10, p11));

        lineEven[x * 2]     = p00;
        lineEven[x * 2 + 1] = topMid;
        lineOdd[x * 2]      = botLeft;
        lineOdd[x * 2 + 1]  = botMid;
      }

      tft.pushImage(ox, oy + y * 2, outW, 1, lineEven);
      if (y * 2 + 1 < outH) {
        tft.pushImage(ox, oy + y * 2 + 1, outW, 1, lineOdd);
      }
    }
  }

  tft.setSwapBytes(false);
  drawFooter(statusFooter);
}

bool ensureDirectoryLoaded() {
  if (!jpgFiles.empty()) return true;

  drawFooter("Loading index from yak.net/strick/thumb/ ...", TFT_YELLOW);
  HTTPClient http;
  http.begin(THUMB_DIR_URL);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    drawFooter("HTTP Error " + String(code) + " loading thumb index", TFT_RED);
    return false;
  }

  String html = http.getString();
  http.end();

  int pos = 0;
  while (true) {
    int hIdx = html.indexOf("href=\"", pos);
    if (hIdx < 0) break;
    hIdx += 6;
    int qEnd = html.indexOf('"', hIdx);
    if (qEnd < 0) break;
    String fname = html.substring(hIdx, qEnd);
    if (fname.endsWith(".jpg") || fname.endsWith(".JPG") || fname.endsWith(".jpeg")) {
      jpgFiles.push_back(fname);
    }
    pos = qEnd + 1;
  }

  Serial.printf("[THUMB] Loaded %u JPEG filenames from %s\n",
                (unsigned)jpgFiles.size(), THUMB_DIR_URL);
  return !jpgFiles.empty();
}

bool downloadAndDecodeIndex(int idx) {
  if (idx < 0 || idx >= (int)jpgFiles.size()) return false;

  String fname = jpgFiles[idx];
  String url = String(THUMB_DIR_URL) + fname;
  drawFooter("Fetching " + fname + " ...", TFT_YELLOW);
  setRgbLed(false, false, true, 90);

  HTTPClient http;
  http.begin(url);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    drawFooter("HTTP " + String(code) + " on " + fname, TFT_RED);
    return false;
  }

  int len = http.getSize();
  WiFiClient *stream = http.getStreamPtr();
  std::vector<uint8_t> jpgBuf;
  if (len > 0 && len < 65536) {
    jpgBuf.resize(len);
    size_t got = stream->readBytes(jpgBuf.data(), len);
    jpgBuf.resize(got);
  } else {
    uint8_t tmp[512];
    while (http.connected() || stream->available()) {
      int avail = stream->available();
      if (avail > 0) {
        int r = stream->readBytes(tmp, min((int)sizeof(tmp), avail));
        jpgBuf.insert(jpgBuf.end(), tmp, tmp + r);
      } else {
        delay(5);
      }
    }
  }
  http.end();

  lastJpgBytes = (int)jpgBuf.size();
  if (jpgBuf.empty() || !decodeJpegBuffer(jpgBuf.data(), jpgBuf.size())) {
    drawFooter("Decode failed: " + fname, TFT_RED);
    return false;
  }

  currentImgIdx = idx;
  statusFooter = "thumb/" + fname + " (" + String(idx + 1) + "/" +
                 String(jpgFiles.size()) + ") " +
                 String(decodedW) + "x" + String(decodedH) + " (" +
                 String(lastJpgBytes) + "B)";
  setRgbLed(false, true, false, 100);
  return true;
}

void fetchAndShowRandomSingle() {
  if (!ensureWifiOrPrompt()) {
    showingWifiPrompt = true;
    return;
  }
  showingWifiPrompt = false;

  drawTopBar();
  if (!ensureDirectoryLoaded()) return;

  int nextIdx = random(0, (int)jpgFiles.size());
  if ((int)jpgFiles.size() > 1 && nextIdx == currentImgIdx) {
    nextIdx = (nextIdx + 1) % (int)jpgFiles.size();
  }

  if (downloadAndDecodeIndex(nextIdx)) {
    renderDecodedSingle();
  }
}

void fetchAndShow4UpGrid() {
  if (!ensureWifiOrPrompt()) {
    showingWifiPrompt = true;
    return;
  }
  showingWifiPrompt = false;

  viewMode = VIEW_4UP;
  drawTopBar();
  if (!ensureDirectoryLoaded()) return;

  tft.fillRect(0, 25, SCREEN_W, 198, TFT_BLACK);

  static const int GX[4] = { 18, 170,  18, 170 };
  static const int GY[4] = { 25,  25, 124, 124 };

  for (int slot = 0; slot < 4; slot++) {
    int idx = random(0, (int)jpgFiles.size());
    gridImgIndices[slot] = idx;
    if (downloadAndDecodeIndex(idx)) {
      tft.setSwapBytes(true);
      for (int y = 0; y < decodedH; y++) {
        tft.pushImage(GX[slot], GY[slot] + y, decodedW, 1, decodedImg[y]);
      }
      tft.setSwapBytes(false);
      tft.drawRect(GX[slot] - 1, GY[slot] - 1, decodedW + 2, decodedH + 2, 0x4A69);
    }
  }

  statusFooter = "Tap any thumbnail to zoom 2X, or RANDOM IMG for 4 more";
  drawFooter(statusFooter);
}

void thumbEnter() {
  tft.fillScreen(TFT_BLACK);
  if (viewMode == VIEW_4UP) {
    fetchAndShow4UpGrid();
  } else {
    fetchAndShowRandomSingle();
  }
}

void thumbOnTap(const TouchSample &t) {
  if (showingWifiPrompt) {
    handleWifiPromptTap(t);
    return;
  }

  if (t.y <= 25) {
    if (t.x <= 52) {
      tft.setSwapBytes(false);
      returnToMenu();
      return;
    }
    if (t.x >= 54 && t.x <= 150) {
      if (viewMode == VIEW_4UP) {
        fetchAndShow4UpGrid();
      } else {
        fetchAndShowRandomSingle();
      }
      return;
    }
    if (t.x >= 152 && t.x <= 212) {
      viewMode = VIEW_2X_ZOOM;
      drawTopBar();
      if (currentImgIdx >= 0) renderDecodedSingle();
      else fetchAndShowRandomSingle();
      return;
    }
    if (t.x >= 214 && t.x <= 266) {
      viewMode = VIEW_1X_NAT;
      drawTopBar();
      if (currentImgIdx >= 0) renderDecodedSingle();
      else fetchAndShowRandomSingle();
      return;
    }
    if (t.x >= 268) {
      fetchAndShow4UpGrid();
      return;
    }
    return;
  }

  // Tapping inside the image area:
  if (viewMode == VIEW_4UP) {
    // Check which of the 4 quadrants was tapped and zoom that image to 2X!
    int col = (t.x >= SCREEN_W / 2) ? 1 : 0;
    int row = (t.y >= 124) ? 1 : 0;
    int slot = row * 2 + col;
    if (gridImgIndices[slot] >= 0) {
      viewMode = VIEW_2X_ZOOM;
      drawTopBar();
      if (downloadAndDecodeIndex(gridImgIndices[slot])) {
        renderDecodedSingle();
      }
    }
  } else {
    fetchAndShowRandomSingle();
  }
}

} // namespace

const DemoApp DEMO_THUMB = {
  "THUMB IMAGES",
  "Random JPG from yak.net/strick/thumb/",
  TFT_YELLOW,
  thumbEnter,
  nullptr,
  thumbOnTap,
  nullptr,
  nullptr
};
