// ambient: Native slideshow & ambient display app for Lakatos OS (320x240)
//
// Starts with full feature parity with gallery:
//   - Scans up to 700 directory entries in /net/pix/ and remembers up to 500
//     .jpg basenames (<= 8 bytes each without the ".jpg" extension).
//   - Shuffles the catalog using the ESP32 hardware TRNG (os->rand).
//   - Displays each image centered on the 320x240 screen using Core 0
//     background prefetching into a 64 KB heap buffer.
//   - Supports left/right half-screen tap navigation, NE Menu Wedge
//     (Shuffle / Config slider for 5, 12, 30, 60, 300, 3600 sec),
//     60-second Ambient quiet auto-hide of corner wedges, and
//     incremental retry backoff (3, 4, 5, ... sec) with a live countdown.

#include "lakatos_api.h"

#include <stdio.h>
#include <string.h>

namespace {

constexpr size_t   kBufMax        = 65536;
constexpr int      kMaxScan       = 700;  // 700 * 88 = 61,600 B fits inside 64 KB imgBuf
constexpr int      kMaxImages     = 500;
constexpr int      kNameStride    = 8;
constexpr int      kMaxSubApps    = 128;
constexpr int      kSubNameStride = 12;
constexpr uint32_t kSubAppPercent = 100;  // Probability (0..100) of running a sub-app between JPEGs
constexpr uint32_t kPollStepMs    = 40;
constexpr uint32_t kAmbientHideMs = 60000;

struct AmbientState {
  const LakatosApi* os;
  uint8_t*          imgBuf;  // 64 KB heap buffer for fs_ls scan & Core 0 JPEG prefetch
  char              imgNames[kMaxImages][kNameStride];
  int               imgCount;
  char              subNames[kMaxSubApps][kSubNameStride];
  int               subCount;
  int               subIdx;
  int               prefetchIdx;

  int               slideStopIdx;
  int               slideSec;

  uint32_t          appStartMs;
  uint32_t          lastTouchClickMs;
  uint32_t          lastSlideClickMs;
  bool              wedgesVisible;

  char              pathBuf[32];
  char              cfgBuf[24];
  char              retryMsgBuf[40];
};

static AmbientState S;

static int stopSeconds(int idx) {
  static const int kStops[6] = {5, 12, 30, 60, 300, 3600};
  if (idx <= 0) return kStops[0];
  if (idx >= 5) return kStops[5];
  return kStops[idx];
}

static const char* stopLabel(int idx) {
  static const char* const kLabels[6] = {"5", "12", "30", "60", "300", "3600"};
  if (idx <= 0) return kLabels[0];
  if (idx >= 5) return kLabels[5];
  return kLabels[idx];
}

static const char* stopBadgeLabel(int idx) {
  static const char* const kBadges[6] = {
      "5 sec", "12 sec", "30 sec", "60s (1m)", "300s (5m)", "3600s (1h)"};
  if (idx <= 0) return kBadges[0];
  if (idx >= 5) return kBadges[5];
  return kBadges[idx];
}

static int nearestStopIdx(int sec) {
  int bestIdx  = 1;
  int bestDiff = 999999;
  for (int i = 0; i < 6; i++) {
    int d = sec - stopSeconds(i);
    if (d < 0) d = -d;
    if (d < bestDiff) {
      bestDiff = d;
      bestIdx  = i;
    }
  }
  return bestIdx;
}

static int parsePositiveInt(const char* s, int maxLen) {
  int val    = 0;
  int digits = 0;
  for (int i = 0; i < maxLen && s[i] != '\0'; i++) {
    char ch = s[i];
    if (ch >= '0' && ch <= '9') {
      val = val * 10 + (ch - '0');
      digits++;
    } else if (digits > 0) {
      break;
    }
  }
  return (digits > 0 && val > 0) ? val : 0;
}

static void loadAmbientConfig() {
  S.slideStopIdx = 1;
  S.slideSec     = 12;

  int32_t n = S.os->fs_read("/flash/etc/ambient.cfg", 0, S.cfgBuf, 15);
  if (n <= 0) {
    n = S.os->fs_read("/flash/etc/gallery.cfg", 0, S.cfgBuf, 15);
  }
  if (n > 0) {
    S.cfgBuf[n] = '\0';
    int val = parsePositiveInt(S.cfgBuf, n);
    if (val > 0) {
      S.slideStopIdx = nearestStopIdx(val);
      S.slideSec     = stopSeconds(S.slideStopIdx);
    }
  }

  // Check if an explicit seconds argument was passed after "ambient" (e.g. "ambient 15")
  const char* arg = lakatos_app_arg();
  if (arg) {
    const char* p = arg;
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;
    if (*p) {
      int argVal = parsePositiveInt(p, 16);
      if (argVal > 0) {
        S.slideStopIdx = nearestStopIdx(argVal);
        S.slideSec     = argVal;
      }
    }
  }
}

static void saveAmbientConfig() {
  S.os->fs_mkdir("/flash/etc", true);
  int v = (S.slideSec > 0) ? S.slideSec : 12;
  int len = snprintf(S.cfgBuf, sizeof(S.cfgBuf), "%d\n", v);
  if (len > 0) {
    S.os->fs_write("/flash/etc/ambient.cfg", S.cfgBuf, (size_t)len);
    S.os->fs_write("/flash/etc/gallery.cfg", S.cfgBuf, (size_t)len);
  }
}

static char toLowerChar(char c) {
  if (c >= 'A' && c <= 'Z') return (char)(c + 32);
  return c;
}

static int jpgBasenameLen(const char* name) {
  int ln = 0;
  while (ln < 63 && name[ln] != '\0') {
    ln++;
  }
  if (ln < 5 || ln > 12) return 0;
  if (name[ln - 4] == '.' &&
      toLowerChar(name[ln - 3]) == 'j' &&
      toLowerChar(name[ln - 2]) == 'p' &&
      toLowerChar(name[ln - 1]) == 'g') {
    return ln - 4;
  }
  return 0;
}

static int sappBasenameLen(const char* name) {
  int ln = 0;
  while (ln < 63 && name[ln] != '\0') {
    ln++;
  }
  if (ln < 6 || ln > (kSubNameStride + 5)) return 0;
  if (name[ln - 5] == '.' &&
      toLowerChar(name[ln - 4]) == 's' &&
      toLowerChar(name[ln - 3]) == 'a' &&
      toLowerChar(name[ln - 2]) == 'p' &&
      toLowerChar(name[ln - 1]) == 'p') {
    return ln - 5;
  }
  return 0;
}

static void exitCleanly() {
  S.os->prefetch_cancel();
  if (S.imgBuf) {
    S.os->free(S.imgBuf);
    S.imgBuf = nullptr;
  }
  S.os->exit(0);
}

static bool isAmbientQuiet(uint32_t now) {
  if ((uint32_t)(now - S.appStartMs) < kAmbientHideMs) {
    return false;
  }
  if (S.lastTouchClickMs != 0 && (uint32_t)(now - S.lastTouchClickMs) < kAmbientHideMs) {
    return false;
  }
  return true;
}

static void drawAmbientWedges() {
  lakatos_draw_exit_wedge(S.os);
  lakatos_draw_menu_wedge(S.os);
  S.wedgesVisible = true;
}

static void hideAmbientWedges() {
  int sz = lakatos_corner_size(S.os);
  S.os->tft_fill_rect(0, 0, sz, sz, LAKATOS_BLACK);
  S.os->tft_fill_rect(320 - sz, 0, sz, sz, LAKATOS_BLACK);
  S.wedgesVisible = false;
}

static void swapCatalogEntries(int i, int j) {
  if (i == j) return;
  for (int k = 0; k < kNameStride; k++) {
    char tmp          = S.imgNames[i][k];
    S.imgNames[i][k]  = S.imgNames[j][k];
    S.imgNames[j][k]  = tmp;
  }
}

static void swapSubAppEntries(int i, int j) {
  if (i == j) return;
  for (int k = 0; k < kSubNameStride; k++) {
    char tmp          = S.subNames[i][k];
    S.subNames[i][k]  = S.subNames[j][k];
    S.subNames[j][k]  = tmp;
  }
}

static void shuffleSubApps() {
  for (int i = S.subCount - 1; i > 0; i--) {
    int j = (int)S.os->rand((uint32_t)(i + 1));
    swapSubAppEntries(i, j);
  }
  S.subIdx = 0;
}

static void shuffleCatalog() {
  for (int i = S.imgCount - 1; i > 0; i--) {
    int j = (int)S.os->rand((uint32_t)(i + 1));
    swapCatalogEntries(i, j);
  }
  shuffleSubApps();
}

static void drawAmbientConfigPanel() {
  const LakatosApi* os = S.os;
  drawAmbientWedges();
  os->tft_fill_round_rect(12, 34, 296, 172, 6, 0x1082);
  os->tft_draw_round_rect(12, 34, 296, 172, 6, LAKATOS_CYAN);
  os->tft_fill_round_rect(16, 38, 288, 24, 4, LAKATOS_NAVY);

  os->tft_set_text_datum(LAKATOS_MC_DATUM);
  os->tft_set_text_color(LAKATOS_WHITE, LAKATOS_NAVY);
  os->tft_draw_string("AMBIENT CONFIG", 160, 50, 2);

  os->tft_set_text_datum(LAKATOS_ML_DATUM);
  os->tft_set_text_color(LAKATOS_LIGHTGREY, 0x1082);
  os->tft_draw_string("Seconds per photo:", 24, 78, 2);

  os->tft_fill_round_rect(182, 66, 114, 24, 4, 0x18E3);
  os->tft_draw_round_rect(182, 66, 114, 24, 4, LAKATOS_CYAN);
  os->tft_set_text_datum(LAKATOS_MC_DATUM);
  os->tft_set_text_color(LAKATOS_YELLOW, 0x18E3);
  os->tft_draw_string(stopBadgeLabel(S.slideStopIdx), 239, 78, 2);

  int trackX0 = 35;
  int stepX   = 50;
  int trackW  = stepX * 5;
  os->tft_fill_round_rect(trackX0, 113, trackW, 6, 3, LAKATOS_BLACK);
  if (S.slideStopIdx > 0) {
    os->tft_fill_round_rect(trackX0, 113, S.slideStopIdx * stepX, 6, 3, LAKATOS_CYAN);
  }
  os->tft_draw_round_rect(trackX0, 113, trackW, 6, 3, LAKATOS_WHITE);

  os->tft_set_text_datum(LAKATOS_TC_DATUM);
  for (int i = 0; i < 6; i++) {
    int sx = trackX0 + i * stepX;
    if (i == S.slideStopIdx) {
      os->tft_draw_line(sx, 108, sx, 124, LAKATOS_YELLOW);
      os->tft_fill_circle(sx, 116, 6, LAKATOS_YELLOW);
      os->tft_draw_circle(sx, 116, 6, LAKATOS_WHITE);
      os->tft_set_text_color(LAKATOS_YELLOW, 0x1082);
    } else {
      os->tft_draw_line(sx, 110, sx, 122, LAKATOS_LIGHTGREY);
      os->tft_fill_circle(sx, 116, 3, 0x2945);
      os->tft_draw_circle(sx, 116, 3, LAKATOS_CYAN);
      os->tft_set_text_color(LAKATOS_LIGHTGREY, 0x1082);
    }
    os->tft_draw_string(stopLabel(i), sx, 130, 1);
  }

  os->tft_fill_round_rect(100, 162, 120, 32, 5, 0x1A26);
  os->tft_draw_round_rect(100, 162, 120, 32, 5, LAKATOS_GREEN);
  os->tft_set_text_datum(LAKATOS_MC_DATUM);
  os->tft_set_text_color(LAKATOS_WHITE, 0x1A26);
  os->tft_draw_string("DONE", 160, 178, 2);
  os->tft_set_text_datum(LAKATOS_TL_DATUM);
}

static int runAmbientConfigPanel() {
  const LakatosApi* os = S.os;
  drawAmbientConfigPanel();

  bool sliderDragging = false;
  for (;;) {
    if (os->serial_available() > 0) {
      int ch = os->serial_read();
      if (ch == 'q' || ch == 'Q' || ch == 3) {
        exitCleanly();
        return 99;
      }
    }
    if (sliderDragging) {
      LakatosTouchSample ts = os->read_touch();
      if (!ts.touched) {
        sliderDragging = false;
      } else if (ts.y >= 94 && ts.y <= 150 && ts.x >= 15 && ts.x <= 305) {
        uint32_t now       = os->millis();
        S.lastSlideClickMs = now;
        S.lastTouchClickMs = now;
        int idx = (ts.x - 35 + 25) / 50;
        if (idx < 0) idx = 0;
        if (idx > 5) idx = 5;
        if (idx != S.slideStopIdx) {
          S.slideStopIdx = idx;
          S.slideSec     = stopSeconds(idx);
          saveAmbientConfig();
          drawAmbientConfigPanel();
        }
      }
    } else {
      LakatosTouchSample click = os->read_click();
      if (click.touched) {
        uint32_t now       = os->millis();
        S.lastSlideClickMs = now;
        S.lastTouchClickMs = now;
        int tx = click.x;
        int ty = click.y;

        if (ty >= 94 && ty <= 150 && tx >= 15 && tx <= 305) {
          int idx = (tx - 35 + 25) / 50;
          if (idx < 0) idx = 0;
          if (idx > 5) idx = 5;
          if (idx != S.slideStopIdx) {
            S.slideStopIdx = idx;
            S.slideSec     = stopSeconds(idx);
            saveAmbientConfig();
            drawAmbientConfigPanel();
          }
          sliderDragging = true;
        } else if ((tx >= 100 && tx <= 220 && ty >= 162 && ty <= 194) ||
                   tx < 12 || tx > 308 || ty < 34 || ty > 206) {
          saveAmbientConfig();
          return 2;
        }
      }
    }
    os->delay_ms(20);
  }
}

static int runAmbientMenu() {
  const LakatosApi* os = S.os;
  drawAmbientWedges();
  int s     = lakatos_corner_size(os);
  int menuY = (s < 24) ? 24 : s;

  os->tft_fill_round_rect(204, menuY, 114, 61, 5, 0x10A2);
  os->tft_draw_round_rect(204, menuY, 114, 61, 5, LAKATOS_CYAN);

  os->tft_set_text_datum(LAKATOS_MC_DATUM);
  for (int i = 0; i < 2; i++) {
    int iy = menuY + 3 + i * 29;
    os->tft_fill_round_rect(208, iy, 106, 25, 4, 0x18E3);
    os->tft_draw_round_rect(208, iy, 106, 25, 4, LAKATOS_CYAN);
    os->tft_set_text_color(LAKATOS_WHITE, 0x18E3);
    os->tft_draw_string((i == 0) ? "Shuffle" : "Config", 261, iy + 12, 2);
  }
  os->tft_set_text_datum(LAKATOS_TL_DATUM);

  for (;;) {
    if (os->serial_available() > 0) {
      int ch = os->serial_read();
      if (ch == 'q' || ch == 'Q' || ch == 3) {
        exitCleanly();
        return 99;
      }
    }
    LakatosTouchSample click = os->read_click();
    if (click.touched) {
      uint32_t now       = os->millis();
      S.lastSlideClickMs = now;
      S.lastTouchClickMs = now;
      int cx = click.x;
      int cy = click.y;
      if (cx >= 204 && cx <= 318 && cy >= menuY + 2 && cy < menuY + 60) {
        int itemIdx = (cy - (menuY + 3)) / 29;
        if (itemIdx == 0) {
          shuffleCatalog();
          return 1;
        }
        if (itemIdx == 1) {
          return runAmbientConfigPanel();
        }
      }
      return 2;
    }
    os->delay_ms(kPollStepMs);
  }
}

static int pollUserAction() {
  const LakatosApi* os = S.os;
  if (os->serial_available() > 0) {
    int ch = os->serial_read();
    if (ch == 'q' || ch == 'Q' || ch == 3) {
      exitCleanly();
      return 99;
    }
  }
  LakatosTouchSample click = os->read_click();
  if (click.touched) {
    uint32_t now = os->millis();
    if (S.lastSlideClickMs != 0 && (uint32_t)(now - S.lastSlideClickMs) < 250) {
      return 0;
    }
    S.lastSlideClickMs = now;
    S.lastTouchClickMs = now;
    if (!S.wedgesVisible) {
      drawAmbientWedges();
    }
    if (lakatos_is_menu_wedge(os, click.x, click.y)) {
      return runAmbientMenu();
    }
    return (click.x >= 160) ? 1 : -1;
  }
  return 0;
}

static bool checkHomeExit() {
  return pollUserAction() == 99;
}

static int waitSlideAction(uint32_t durationMs) {
  const LakatosApi* os = S.os;
  uint32_t startMs = os->millis();
  while ((uint32_t)(os->millis() - startMs) < durationMs) {
    uint32_t now = os->millis();
    if (S.wedgesVisible && isAmbientQuiet(now)) {
      hideAmbientWedges();
    }
    int act = pollUserAction();
    if (act != 0) {
      return act;
    }
    os->delay_ms(kPollStepMs);
  }
  return pollUserAction();
}

static const char* buildNetJpgPath(int idx) {
  char base[kNameStride + 1];
  for (int i = 0; i < kNameStride; i++) {
    base[i] = S.imgNames[idx][i];
  }
  base[kNameStride] = '\0';
  snprintf(S.pathBuf, sizeof(S.pathBuf), "/net/pix/%s.jpg", base);
  return S.pathBuf;
}

static bool waitRetryCountdown(int totalSec) {
  const LakatosApi* os = S.os;
  os->tft_fill_screen(LAKATOS_BLACK);
  if (!isAmbientQuiet(os->millis())) {
    drawAmbientWedges();
  } else {
    S.wedgesVisible = false;
  }
  os->tft_set_text_color(LAKATOS_CYAN, LAKATOS_BLACK);
  os->tft_draw_string("No images in /net/pix/", 40, 100, 2);

  for (int rem = totalSec; rem > 0; rem--) {
    snprintf(S.retryMsgBuf, sizeof(S.retryMsgBuf),
             "Retrying in %d second%s...", rem, (rem == 1) ? "" : "s");
    os->tft_fill_rect(40, 122, 260, 20, LAKATOS_BLACK);
    os->tft_set_text_color(LAKATOS_LIGHTGREY, LAKATOS_BLACK);
    os->tft_draw_string(S.retryMsgBuf, 40, 124, 2);

    int act = waitSlideAction(1000);
    if (act == 99) return true;
    if (act == 2) {
      os->tft_fill_screen(LAKATOS_BLACK);
      if (!isAmbientQuiet(os->millis())) {
        drawAmbientWedges();
      } else {
        S.wedgesVisible = false;
      }
      os->tft_set_text_color(LAKATOS_CYAN, LAKATOS_BLACK);
      os->tft_draw_string("No images in /net/pix/", 40, 100, 2);
    }
  }
  return false;
}

static void startPrefetch(int idx) {
  if (idx < 0 || idx >= S.imgCount || !S.imgBuf) {
    S.prefetchIdx = -1;
    return;
  }
  const char* fullPath = buildNetJpgPath(idx);
  if (S.os->prefetch_start(fullPath, S.imgBuf, kBufMax)) {
    S.prefetchIdx = idx;
  } else {
    S.prefetchIdx = -1;
  }
}

static bool hasSubAppName(const char* name, int baseLen) {
  for (int i = 0; i < S.subCount; i++) {
    bool match = true;
    for (int k = 0; k < kSubNameStride; k++) {
      char expected = (k < baseLen) ? name[k] : '\0';
      if (S.subNames[i][k] != expected) {
        match = false;
        break;
      }
    }
    if (match) return true;
  }
  return false;
}

static void scanSubAppsDir(const char* dirPath) {
  if (!S.imgBuf || S.subCount >= kMaxSubApps) return;
  LakatosFsEntry* entries = (LakatosFsEntry*)S.imgBuf;
  int n = S.os->fs_ls(dirPath, entries, kMaxSubApps);
  if (n <= 0) return;

  for (int i = 0; i < n; i++) {
    if (S.subCount >= kMaxSubApps) break;
    const LakatosFsEntry& ent = entries[i];
    if (ent.isDir || ent.name[0] == '\0' || ent.name[0] == '.') {
      continue;
    }
    int baseLen = sappBasenameLen(ent.name);
    if (baseLen <= 0 || hasSubAppName(ent.name, baseLen)) continue;

    memset(S.subNames[S.subCount], 0, kSubNameStride);
    memcpy(S.subNames[S.subCount], ent.name, (size_t)baseLen);
    S.subCount++;
  }
}

static void scanSubApps() {
  S.subCount = 0;
  S.subIdx   = 0;
  scanSubAppsDir("/sd/subapps");
  scanSubAppsDir("/net/subapps");
  scanSubAppsDir("/flash/subapps");
  shuffleSubApps();
}

static int scanCatalog() {
  S.os->prefetch_cancel();
  S.imgCount    = 0;
  S.prefetchIdx = -1;
  if (!S.imgBuf) return 0;

  scanSubApps();

  LakatosFsEntry* entries = (LakatosFsEntry*)S.imgBuf;
  int n = S.os->fs_ls("/net/pix", entries, kMaxScan);
  if (n <= 0) return 0;

  for (int i = 0; i < n; i++) {
    if (S.imgCount >= kMaxImages) break;
    const LakatosFsEntry& ent = entries[i];
    if (ent.isDir || ent.name[0] == '\0' || ent.name[0] == '.') {
      continue;
    }
    int baseLen = jpgBasenameLen(ent.name);
    if (baseLen <= 0) continue;

    memset(S.imgNames[S.imgCount], 0, kNameStride);
    memcpy(S.imgNames[S.imgCount], ent.name, (size_t)baseLen);
    S.imgCount++;
  }

  shuffleCatalog();
  return S.imgCount;
}

static bool displaySlide(int idx) {
  const LakatosApi* os = S.os;
  bool drawn = false;

  if (S.prefetchIdx != idx) {
    startPrefetch(idx);
  }
  uint32_t got = 0;
  int st = os->prefetch_status(&got);
  while (st == 1) {
    if (checkHomeExit()) return false;
    os->delay_ms(15);
    st = os->prefetch_status(&got);
  }
  if (st == 2 && got > 0 && S.imgBuf) {
    os->tft_fill_screen(LAKATOS_BLACK);
    drawn = os->tft_draw_jpg_mem(S.imgBuf, got, -1, 0, 0);
  }

  if (!drawn) {
    const char* fullPath = buildNetJpgPath(idx);
    os->tft_fill_screen(LAKATOS_BLACK);
    drawn = os->tft_draw_jpg_file(fullPath, -1, 0, 0);
  }

  if (drawn) {
    if (!isAmbientQuiet(os->millis())) {
      drawAmbientWedges();
    } else {
      S.wedgesVisible = false;
    }
  }

  if (S.imgCount > 0) {
    int nextIdx = idx + 1;
    if (nextIdx >= S.imgCount) nextIdx = 0;
    startPrefetch(nextIdx);
  }

  return drawn;
}

static void runSubAppSlide(const char* subappName) {
  const LakatosApi* os = S.os;
  if (!os->run_subapp) return;

  os->prefetch_cancel();
  S.prefetchIdx = -1;
  if (S.imgBuf) {
    os->free(S.imgBuf);
    S.imgBuf = nullptr;
  }

  os->run_subapp(subappName, "");

  S.imgBuf = (uint8_t*)os->malloc(kBufMax);
}

static void maybeRunNextSubApp() {
  if (S.subCount <= 0 || !S.os->run_subapp) return;
  if (kSubAppPercent < 100 && S.os->rand(100) >= kSubAppPercent) return;

  if (S.subIdx < 0 || S.subIdx >= S.subCount) {
    shuffleSubApps();
  }
  char subName[kSubNameStride + 1];
  memcpy(subName, S.subNames[S.subIdx], kSubNameStride);
  subName[kSubNameStride] = '\0';

  S.subIdx++;
  if (S.subIdx >= S.subCount) {
    shuffleSubApps();
  }

  runSubAppSlide(subName);
}

}  // namespace

extern "C" void app_main(const LakatosApi* os) {
  memset(&S, 0, sizeof(S));
  S.os               = os;
  S.prefetchIdx      = -1;
  S.appStartMs       = os->millis();
  S.lastTouchClickMs = 0;
  S.imgBuf           = (uint8_t*)os->malloc(kBufMax);

  loadAmbientConfig();
  os->tft_fill_screen(LAKATOS_BLACK);
  drawAmbientWedges();
  os->tft_set_text_color(LAKATOS_CYAN, LAKATOS_BLACK);
  os->tft_draw_string("Loading /net/pix/ ...", 64, 110, 2);

  int retrySec = 3;
  for (;;) {
    int count = scanCatalog();
    if (count > 0) {
      int idx      = 0;
      int failures = 0;
      while (failures < count) {
        if (displaySlide(idx)) {
          failures = 0;
          retrySec = 3;
          int act = waitSlideAction((uint32_t)S.slideSec * 1000u);
          if (act == 99) return;
          if (act == 2) continue;
          if (act == -1) {
            idx--;
            if (idx < 0) idx = count - 1;
          } else {
            maybeRunNextSubApp();
            idx++;
            if (idx >= count) idx = 0;
          }
        } else {
          if (checkHomeExit()) return;
          failures++;
          idx++;
          if (idx >= count) idx = 0;
        }
      }
    }

    if (waitRetryCountdown(retrySec)) {
      return;
    }
    retrySec++;

    os->tft_fill_screen(LAKATOS_BLACK);
    if (!isAmbientQuiet(os->millis())) {
      drawAmbientWedges();
    } else {
      S.wedgesVisible = false;
    }
    os->tft_set_text_color(LAKATOS_CYAN, LAKATOS_BLACK);
    os->tft_draw_string("Loading /net/pix/ ...", 64, 110, 2);
  }
}
