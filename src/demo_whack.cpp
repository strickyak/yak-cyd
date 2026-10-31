#include "cyd_shell.h"
#include <math.h>

namespace {

enum WhackState {
  WHACK_PLAYING,
  WHACK_GAME_OVER
};

enum TargetType {
  TARGET_MOLE = 0,
  TARGET_GOLD = 1,
  TARGET_BOMB = 2
};

struct Hole {
  int x;
  int y;
};

static const int NUM_HOLES = 9;
static const Hole HOLES[NUM_HOLES] = {
  { 60,  72}, {160,  72}, {260,  72},
  { 60, 132}, {160, 132}, {260, 132},
  { 60, 192}, {160, 192}, {260, 192}
};

static const int TARGET_RADIUS = 24;
static const uint32_t GAME_DURATION_MS = 30000;

static WhackState state = WHACK_PLAYING;
static int score = 0;
static int highScore = 0;
static int streak = 0;
static int bestStreak = 0;
static int hits = 0;
static int misses = 0;
static uint32_t totalReactionMs = 0;
static uint32_t totalTapErrorPx = 0;
static uint32_t gameStartMs = 0;
static int lastHudSec = -1;

static int activeHole = -1;
static TargetType activeType = TARGET_MOLE;
static uint32_t targetSpawnMs = 0;
static uint32_t targetLifetimeMs = 1400;
static int lastRingRadius = -1;
static String statusBanner = "TAP THE MOLE!";
static uint16_t statusColor = TFT_CYAN;

void drawEmptyHole(int idx) {
  int hx = HOLES[idx].x;
  int hy = HOLES[idx].y;
  tft.fillCircle(hx, hy, TARGET_RADIUS + 8, 0x10A2);
  tft.fillCircle(hx, hy + 2, TARGET_RADIUS - 3, 0x0841);
  tft.drawCircle(hx, hy, TARGET_RADIUS - 2, 0x2945);
}

void drawTarget(int idx, TargetType type) {
  int hx = HOLES[idx].x;
  int hy = HOLES[idx].y;

  if (type == TARGET_MOLE) {
    tft.fillCircle(hx, hy, TARGET_RADIUS - 2, 0xA280);
    tft.drawCircle(hx, hy, TARGET_RADIUS - 2, TFT_ORANGE);
    tft.fillCircle(hx - 7, hy - 6, 3, TFT_WHITE);
    tft.fillCircle(hx + 7, hy - 6, 3, TFT_WHITE);
    tft.fillCircle(hx - 7, hy - 6, 1, TFT_BLACK);
    tft.fillCircle(hx + 7, hy - 6, 1, TFT_BLACK);
    tft.fillCircle(hx, hy + 2, 5, TFT_MAGENTA);
    tft.drawFastHLine(hx - 6, hy + 10, 12, TFT_WHITE);
  } else if (type == TARGET_GOLD) {
    tft.fillCircle(hx, hy, TARGET_RADIUS - 2, TFT_GOLD);
    tft.drawCircle(hx, hy, TARGET_RADIUS - 2, TFT_WHITE);
    tft.drawCircle(hx, hy, TARGET_RADIUS - 6, TFT_ORANGE);
    tft.fillCircle(hx - 7, hy - 5, 3, TFT_BLACK);
    tft.fillCircle(hx + 7, hy - 5, 3, TFT_BLACK);
    tft.fillCircle(hx, hy + 2, 5, TFT_RED);
    tft.setTextColor(TFT_BLACK, TFT_GOLD);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("2X", hx, hy + 12, 1);
  } else if (type == TARGET_BOMB) {
    tft.fillCircle(hx, hy, TARGET_RADIUS - 2, TFT_RED);
    tft.drawCircle(hx, hy, TARGET_RADIUS - 2, TFT_YELLOW);
    for (int d = -2; d <= 2; d++) {
      tft.drawLine(hx - 10 + d, hy - 10, hx + 10 + d, hy + 10, TFT_WHITE);
      tft.drawLine(hx + 10 + d, hy - 10, hx - 10 + d, hy + 10, TFT_WHITE);
    }
  }
}

void drawHud(int remainingSec) {
  tft.fillRect(0, 0, SCREEN_W, 26, 0x0841);
  tft.drawFastHLine(0, 26, SCREEN_W, TFT_DARKGREY);

  // [MENU] pill at top-left (x: 4..54, y: 3..23)
  tft.fillRoundRect(4, 3, 50, 20, 4, 0x2945);
  tft.drawRoundRect(4, 3, 50, 20, 4, TFT_CYAN);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_CYAN, 0x2945);
  tft.drawString("MENU", 29, 13, 1);

  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(TFT_YELLOW, 0x0841);
  tft.drawString("SCORE:" + String(score), 62, 13, 2);

  tft.setTextDatum(MC_DATUM);
  uint16_t streakCol = (streak >= 3) ? TFT_GREEN : TFT_CYAN;
  tft.setTextColor(streakCol, 0x0841);
  tft.drawString("x" + String(max(1, streak)), 200, 13, 2);

  tft.setTextDatum(MR_DATUM);
  uint16_t timeCol = (remainingSec <= 5) ? TFT_RED : TFT_WHITE;
  tft.setTextColor(timeCol, 0x0841);
  tft.drawString(String(remainingSec) + "s", SCREEN_W - 8, 13, 2);
}

void drawStatusFooter() {
  tft.fillRect(0, 222, SCREEN_W, 18, 0x0841);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(statusColor, 0x0841);
  tft.drawString(statusBanner, SCREEN_W / 2, 231, 2);
}

void spawnNextTarget() {
  if (activeHole >= 0) {
    drawEmptyHole(activeHole);
  }

  int nextHole = random(0, NUM_HOLES);
  if (nextHole == activeHole) {
    nextHole = (nextHole + random(1, NUM_HOLES)) % NUM_HOLES;
  }
  activeHole = nextHole;

  int roll = random(0, 100);
  if (roll < 18) {
    activeType = TARGET_GOLD;
  } else if (roll < 32) {
    activeType = TARGET_BOMB;
  } else {
    activeType = TARGET_MOLE;
  }

  uint32_t baseLife = 1350;
  if (score > 250) baseLife = 1150;
  if (score > 500) baseLife = 980;
  if (activeType == TARGET_GOLD) baseLife = (baseLife * 80) / 100;
  if (activeType == TARGET_BOMB) baseLife = 950;

  targetSpawnMs = millis();
  targetLifetimeMs = baseLife;
  lastRingRadius = -1;

  drawTarget(activeHole, activeType);
}

void startRound() {
  state = WHACK_PLAYING;
  score = 0;
  streak = 0;
  bestStreak = 0;
  hits = 0;
  misses = 0;
  totalReactionMs = 0;
  totalTapErrorPx = 0;
  activeHole = -1;
  lastHudSec = -1;
  statusBanner = "WHACK THE MOLES! AVOID RED [X]!";
  statusColor = TFT_CYAN;

  tft.fillScreen(0x10A2);
  for (int i = 0; i < NUM_HOLES; i++) {
    drawEmptyHole(i);
  }
  drawHud(GAME_DURATION_MS / 1000);
  drawStatusFooter();

  gameStartMs = millis();
  spawnNextTarget();
  setRgbLed(false, false, true, 150);
}

void showGameOverScreen() {
  state = WHACK_GAME_OVER;
  if (score > highScore) {
    highScore = score;
  }
  setRgbLed(true, true, false, 400);

  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0, 0, SCREEN_W, 30, TFT_NAVY);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_YELLOW, TFT_NAVY);
  tft.drawString("ROUND COMPLETE!", SCREEN_W / 2, 15, 4);

  int totalAttempts = hits + misses;
  int accuracyPct = (totalAttempts > 0) ? ((hits * 100) / totalAttempts) : 0;
  int avgReaction = (hits > 0) ? (int)(totalReactionMs / hits) : 0;
  int avgErrorPx  = (hits > 0) ? (int)(totalTapErrorPx / hits) : 0;

  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("FINAL SCORE:", 24, 52, 2);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.drawString(String(score) + "  (BEST: " + String(highScore) + ")", 150, 52, 2);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("HIT ACCURACY:", 24, 78, 2);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString(String(hits) + "/" + String(totalAttempts) + " (" + String(accuracyPct) + "%)", 150, 78, 2);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("AVG REACTION:", 24, 104, 2);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawString(String(avgReaction) + " ms", 150, 104, 2);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("TOUCH PRECISION:", 24, 130, 2);
  tft.setTextColor(TFT_MAGENTA, TFT_BLACK);
  tft.drawString("avg " + String(avgErrorPx) + " px from center", 150, 130, 2);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("BEST STREAK:", 24, 156, 2);
  tft.setTextColor(TFT_ORANGE, TFT_BLACK);
  tft.drawString("x" + String(bestStreak), 150, 156, 2);

  // [ PLAY AGAIN ] (x: 16..154, y: 184..228)
  tft.fillRoundRect(16, 184, 138, 44, 8, TFT_DARKGREEN);
  tft.drawRoundRect(16, 184, 138, 44, 8, TFT_GREEN);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREEN);
  tft.drawString("PLAY AGAIN", 85, 206, 2);

  // [ MAIN MENU ] (x: 166..304, y: 184..228)
  tft.fillRoundRect(166, 184, 138, 44, 8, 0x3186);
  tft.drawRoundRect(166, 184, 138, 44, 8, TFT_CYAN);
  tft.setTextColor(TFT_CYAN, 0x3186);
  tft.drawString("MAIN MENU", 235, 206, 2);
}

void whackEnter() {
  startRound();
}

void whackLoop() {
  if (state != WHACK_PLAYING) return;

  uint32_t now = millis();
  uint32_t elapsedGame = now - gameStartMs;
  if (elapsedGame >= GAME_DURATION_MS) {
    showGameOverScreen();
    return;
  }

  int remSec = (int)((GAME_DURATION_MS - elapsedGame + 999) / 1000);
  if (remSec != lastHudSec) {
    lastHudSec = remSec;
    drawHud(remSec);
  }

  uint32_t elapsedTarget = now - targetSpawnMs;
  if (elapsedTarget >= targetLifetimeMs) {
    if (activeType != TARGET_BOMB) {
      streak = 0;
      statusBanner = "MOLE ESCAPED!";
      statusColor = TFT_ORANGE;
      drawStatusFooter();
    }
    spawnNextTarget();
    return;
  }

  int hx = HOLES[activeHole].x;
  int hy = HOLES[activeHole].y;
  float fracRemaining = 1.0f - ((float)elapsedTarget / (float)targetLifetimeMs);
  int ringRadius = TARGET_RADIUS + 1 + (int)lroundf(fracRemaining * 6.0f);

  if (ringRadius != lastRingRadius) {
    if (lastRingRadius > 0) {
      tft.drawCircle(hx, hy, lastRingRadius, 0x10A2);
    }
    uint16_t ringCol = (activeType == TARGET_BOMB) ? TFT_RED :
                       (activeType == TARGET_GOLD) ? TFT_YELLOW : TFT_CYAN;
    tft.drawCircle(hx, hy, ringRadius, ringCol);
    lastRingRadius = ringRadius;
  }
}

void whackOnTap(const TouchSample &t) {
  if (state == WHACK_GAME_OVER) {
    if (t.x >= 12 && t.x <= 158 && t.y >= 175 && t.y <= 236) {
      startRound();
    } else if (t.x >= 162 && t.x <= 308 && t.y >= 175 && t.y <= 236) {
      returnToMenu();
    }
    return;
  }

  // Top-left [MENU] button
  if (t.x <= 58 && t.y <= 28) {
    returnToMenu();
    return;
  }
  if (t.y < 28) return;

  tft.drawFastHLine(t.x - 5, t.y, 11, TFT_WHITE);
  tft.drawFastVLine(t.x, t.y - 5, 11, TFT_WHITE);

  int hx = HOLES[activeHole].x;
  int hy = HOLES[activeHole].y;
  int dx = t.x - hx;
  int dy = t.y - hy;
  int distPx = (int)lroundf(sqrtf((float)(dx * dx + dy * dy)));

  if (distPx <= TARGET_RADIUS + 8) {
    uint32_t reactionMs = millis() - targetSpawnMs;

    if (activeType == TARGET_BOMB) {
      score = max(0, score - 25);
      streak = 0;
      misses++;
      statusBanner = "OUCH! TAPPED BOMB (-25)";
      statusColor = TFT_RED;
      setRgbLed(true, false, false, 250);
    } else {
      hits++;
      streak++;
      if (streak > bestStreak) bestStreak = streak;
      totalReactionMs += reactionMs;
      totalTapErrorPx += distPx;

      int pts = 15;
      if (reactionMs < 400) pts += 15;
      else if (reactionMs < 700) pts += 10;
      else if (reactionMs < 1000) pts += 5;

      bool bullseye = (distPx <= 8);
      if (bullseye) pts += 10;
      if (activeType == TARGET_GOLD) pts *= 2;
      if (streak >= 5) pts += 10;

      score += pts;

      String tag = bullseye ? "BULLSEYE! " : "WHACK! ";
      statusBanner = tag + "+" + String(pts) + " (" + String(reactionMs) + "ms, " + String(distPx) + "px)";
      statusColor = bullseye ? TFT_GREEN : TFT_YELLOW;
      setRgbLed(false, true, bullseye, 140);

      tft.fillCircle(hx, hy, TARGET_RADIUS, bullseye ? TFT_GREEN : TFT_YELLOW);
      delay(40);
    }

    drawHud(max(0, (int)((GAME_DURATION_MS - (millis() - gameStartMs)) / 1000)));
    drawStatusFooter();
    spawnNextTarget();
  } else {
    misses++;
    streak = 0;
    statusBanner = "MISS! (" + String(distPx) + "px away)";
    statusColor = TFT_ORANGE;
    setRgbLed(true, false, false, 100);
    drawHud(max(0, (int)((GAME_DURATION_MS - (millis() - gameStartMs)) / 1000)));
    drawStatusFooter();
  }
}

} // namespace

const DemoApp DEMO_WHACK = {
  "WHACK-A-MOLE",
  "30s arcade target tap & reflex test",
  TFT_ORANGE,
  whackEnter,
  whackLoop,
  whackOnTap,
  nullptr,
  nullptr
};
