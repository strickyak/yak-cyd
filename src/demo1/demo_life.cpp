#include "cyd_shell.h"
#include <string.h>

namespace {

static const int TOP_BAR_H = 24;
static const int CELL_SIZE = 4;
static const int GRID_W = SCREEN_W / CELL_SIZE;                   // 80
static const int GRID_H = (SCREEN_H - TOP_BAR_H) / CELL_SIZE;     // 54

static uint8_t gridCur[GRID_H][GRID_W];
static uint8_t gridNext[GRID_H][GRID_W];

static bool paused = false;
static bool isDrawing = false;
static int lastDrawGx = -1;
static int lastDrawGy = -1;
static uint32_t lastDrawMs = 0;
static uint32_t lastStepMs = 0;
static uint32_t generation = 0;
static int population = 0;

static const uint32_t STEP_INTERVAL_MS = 90;     // ~11 generations/sec
static const uint32_t DRAW_HOLD_PAUSE_MS = 220;  // Let user finish stroke before stepping

void drawCell(int gx, int gy, bool alive) {
  int px = gx * CELL_SIZE;
  int py = TOP_BAR_H + gy * CELL_SIZE;
  // 3x3 cell inside 4x4 pitch leaves a crisp 1px black grid border
  tft.fillRect(px, py, CELL_SIZE - 1, CELL_SIZE - 1, alive ? TFT_WHITE : TFT_BLACK);
}

void drawStats() {
  tft.fillRect(240, 2, 79, 20, TFT_BLACK);
  tft.setTextDatum(MR_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("G:" + String(generation % 10000), 316, 7, 1);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString("P:" + String(population), 316, 17, 1);
}

void drawTopBar() {
  tft.fillRect(0, 0, SCREEN_W, TOP_BAR_H - 1, TFT_BLACK);
  tft.drawFastHLine(0, TOP_BAR_H - 1, SCREEN_W, TFT_DARKGREY);

  tft.setTextDatum(MC_DATUM);

  // [ MENU ] (x: 2..54)
  tft.drawRoundRect(2, 2, 52, 19, 4, TFT_WHITE);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("MENU", 28, 11, 1);

  // [ PAUSE / RUN ] (x: 58..120)
  uint16_t runFill = paused ? TFT_WHITE : TFT_BLACK;
  uint16_t runText = paused ? TFT_BLACK : TFT_WHITE;
  tft.fillRoundRect(58, 2, 62, 19, 4, runFill);
  tft.drawRoundRect(58, 2, 62, 19, 4, TFT_WHITE);
  tft.setTextColor(runText, runFill);
  tft.drawString(paused ? "RESUME" : "PAUSE", 89, 11, 1);

  // [ CLEAR ] (x: 124..180)
  tft.drawRoundRect(124, 2, 56, 19, 4, TFT_WHITE);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("CLEAR", 152, 11, 1);

  // [ SEED ] (x: 184..236)
  tft.drawRoundRect(184, 2, 52, 19, 4, TFT_WHITE);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("SEED", 210, 11, 1);

  drawStats();
}

void clearBoard() {
  memset(gridCur, 0, sizeof(gridCur));
  memset(gridNext, 0, sizeof(gridNext));
  generation = 0;
  population = 0;
  tft.fillRect(0, TOP_BAR_H, SCREEN_W, SCREEN_H - TOP_BAR_H, TFT_BLACK);
  drawStats();
}

void birthCell(int gx, int gy) {
  if (gx < 0 || gx >= GRID_W || gy < 0 || gy >= GRID_H) return;
  if (!gridCur[gy][gx]) {
    gridCur[gy][gx] = 1;
    population++;
    drawCell(gx, gy, true);
  }
}

// 4-connected line interpolation so any drawn line has orthogonal neighbors
// and enough local density to take off in Conway's Life
void drawCellLine(int x0, int y0, int x1, int y1) {
  int dx = abs(x1 - x0);
  int dy = abs(y1 - y0);
  int sx = (x0 < x1) ? 1 : -1;
  int sy = (y0 < y1) ? 1 : -1;
  int err = dx - dy;

  while (true) {
    birthCell(x0, y0);
    if (x0 == x1 && y0 == y1) break;

    int e2 = 2 * err;
    if (e2 > -dy && e2 < dx) {
      // Diagonal step: also birth an adjacent orthogonal bridge cell so the
      // stroke is 4-connected and has 3-neighbor elbows that ignite evolution
      birthCell(x0 + sx, y0);
    }
    if (e2 > -dy) {
      err -= dy;
      x0 += sx;
    }
    if (e2 < dx) {
      err += dx;
      y0 += sy;
    }
  }
}

void seedStarterPattern() {
  // Place an R-pentomino in the center and two gliders in corners
  int cx = GRID_W / 2;
  int cy = GRID_H / 2;
  birthCell(cx,     cy - 1);
  birthCell(cx + 1, cy - 1);
  birthCell(cx - 1, cy);
  birthCell(cx,     cy);
  birthCell(cx,     cy + 1);

  // Top-left glider
  int gx = 12, gy = 8;
  birthCell(gx + 1, gy);
  birthCell(gx + 2, gy + 1);
  birthCell(gx,     gy + 2);
  birthCell(gx + 1, gy + 2);
  birthCell(gx + 2, gy + 2);

  drawStats();
}

void stepGeneration() {
  int newPop = 0;

  for (int y = 0; y < GRID_H; y++) {
    int ym1 = (y == 0) ? (GRID_H - 1) : (y - 1);
    int yp1 = (y == GRID_H - 1) ? 0 : (y + 1);

    const uint8_t *rowM1 = gridCur[ym1];
    const uint8_t *row0  = gridCur[y];
    const uint8_t *rowP1 = gridCur[yp1];

    for (int x = 0; x < GRID_W; x++) {
      int xm1 = (x == 0) ? (GRID_W - 1) : (x - 1);
      int xp1 = (x == GRID_W - 1) ? 0 : (x + 1);

      int neighbors =
        rowM1[xm1] + rowM1[x] + rowM1[xp1] +
        row0[xm1]             + row0[xp1] +
        rowP1[xm1] + rowP1[x] + rowP1[xp1];

      uint8_t alive = row0[x];
      uint8_t nextAlive = (neighbors == 3 || (alive && neighbors == 2)) ? 1 : 0;
      gridNext[y][x] = nextAlive;
      newPop += nextAlive;
    }
  }

  // Only redraw cells whose state changed for flicker-free high-speed updates
  for (int y = 0; y < GRID_H; y++) {
    for (int x = 0; x < GRID_W; x++) {
      uint8_t nextAlive = gridNext[y][x];
      if (nextAlive != gridCur[y][x]) {
        gridCur[y][x] = nextAlive;
        drawCell(x, y, nextAlive != 0);
      }
    }
  }

  generation++;
  population = newPop;
  drawStats();
}

void plotTouchCell(const TouchSample &t, bool isContinuation) {
  if (t.y < TOP_BAR_H) return;

  int gx = constrain(t.x / CELL_SIZE, 0, GRID_W - 1);
  int gy = constrain((t.y - TOP_BAR_H) / CELL_SIZE, 0, GRID_H - 1);

  if (isContinuation && lastDrawGx >= 0 && lastDrawGy >= 0) {
    int dist = abs(gx - lastDrawGx) + abs(gy - lastDrawGy);
    if (dist > 0 && dist <= 28) {
      drawCellLine(lastDrawGx, lastDrawGy, gx, gy);
    } else if (dist > 28) {
      birthCell(gx, gy);
    }
  } else {
    birthCell(gx, gy);
  }

  lastDrawGx = gx;
  lastDrawGy = gy;
  lastDrawMs = millis();
  isDrawing = true;
}

void lifeEnter() {
  tft.fillScreen(TFT_BLACK);
  paused = false;
  isDrawing = false;
  lastDrawGx = -1;
  lastDrawGy = -1;
  clearBoard();
  seedStarterPattern();
  drawTopBar();
  lastStepMs = millis();
}

void lifeLoop() {
  uint32_t now = millis();
  if (paused) return;

  // Hold off stepping while stylus is actively drawing on the grid
  if (isDrawing || (now - lastDrawMs < DRAW_HOLD_PAUSE_MS)) {
    return;
  }

  if (now - lastStepMs >= STEP_INTERVAL_MS) {
    lastStepMs = now;
    stepGeneration();
  }
}

void lifeOnTap(const TouchSample &t) {
  if (t.y < TOP_BAR_H) {
    // [ MENU ] (x: 2..54)
    if (t.x <= 56) {
      returnToMenu();
      return;
    }
    // [ PAUSE / RESUME ] (x: 58..120)
    if (t.x >= 58 && t.x <= 122) {
      paused = !paused;
      drawTopBar();
      return;
    }
    // [ CLEAR ] (x: 124..180)
    if (t.x >= 124 && t.x <= 182) {
      clearBoard();
      return;
    }
    // [ SEED ] (x: 184..236)
    if (t.x >= 184 && t.x <= 238) {
      seedStarterPattern();
      return;
    }
    return;
  }

  plotTouchCell(t, false);
  drawStats();
}

void lifeOnDrag(const TouchSample &t) {
  if (t.y < TOP_BAR_H) return;
  plotTouchCell(t, true);
}

void lifeOnRelease() {
  if (isDrawing) {
    isDrawing = false;
    lastDrawGx = -1;
    lastDrawGy = -1;
    drawStats();
  }
}

} // namespace

const DemoApp DEMO_LIFE = {
  "CONWAY'S LIFE",
  "White on black - draw lines to spawn life",
  TFT_WHITE,
  lifeEnter,
  lifeLoop,
  lifeOnTap,
  lifeOnDrag,
  lifeOnRelease
};
