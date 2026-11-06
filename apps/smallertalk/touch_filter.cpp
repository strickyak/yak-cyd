#include "touch_filter.h"
#include <string.h>

static inline int iabs(int v) { return (v < 0) ? -v : v; }

void touchFilterInit(TouchFilterState* tf) {
  memset(tf, 0, sizeof(*tf));
  tf->clusterRadiusPx   = 14;
  tf->releaseDebounceMs = 35;
  tf->clickCooldownMs   = 110;
}

void touchFilterResetAfterModal(TouchFilterState* tf, const LakatosApi* os) {
  uint32_t now = (os && os->millis) ? os->millis() : 0;
  tf->isDown          = false;
  tf->hasCandidate    = false;
  tf->hasJumpCand     = false;
  tf->clickConsumed   = false;
  tf->lastClickFireMs = now;
  tf->lastValidDownMs = now;
}

FilteredTouchEvent touchFilterPoll(TouchFilterState* tf, const LakatosApi* os) {
  FilteredTouchEvent ev = {false, false, false, tf->curX, tf->curY};
  if (!tf || !os || !os->read_touch) return ev;

  LakatosTouchSample raw = os->read_touch();
  uint32_t now = os->millis ? os->millis() : 0;

  if (raw.touched) {
    // Clamp coordinates to valid 320x240 bounds
    int tx = raw.x;
    int ty = raw.y;
    if (tx < 0) tx = 0;
    if (tx >= LAKATOS_SCREEN_W) tx = LAKATOS_SCREEN_W - 1;
    if (ty < 0) ty = 0;
    if (ty >= LAKATOS_SCREEN_H) ty = LAKATOS_SCREEN_H - 1;

    if (!tf->isDown) {
      // Require 2 consecutive samples within `clusterRadiusPx` within 80 ms
      if (tf->hasCandidate && (uint32_t)(now - tf->candMs) <= 80u) {
        int dist = iabs(tx - tf->candX) + iabs(ty - tf->candY);
        if (dist <= (int)tf->clusterRadiusPx) {
          tf->isDown          = true;
          tf->hasCandidate    = false;
          tf->hasJumpCand     = false;
          tf->curX            = (tx + tf->candX) >> 1;
          tf->curY            = (ty + tf->candY) >> 1;
          tf->lastValidDownMs = now;

          if (!tf->clickConsumed &&
              (tf->lastClickFireMs == 0 || (uint32_t)(now - tf->lastClickFireMs) >= tf->clickCooldownMs)) {
            tf->clickConsumed   = true;
            tf->lastClickFireMs = now;
            ev.justClicked      = true;
          }
          ev.isDown = true;
          ev.x      = tf->curX;
          ev.y      = tf->curY;
          return ev;
        }
      }
      // First sample or outside cluster radius: update candidate
      tf->hasCandidate = true;
      tf->candX        = tx;
      tf->candY        = ty;
      tf->candMs       = now;
      return ev;
    }

    // Already in stabilized `isDown` state: filter out sudden single-frame resistive jumps (> 32 px)
    tf->lastValidDownMs = now;
    int jumpDist = iabs(tx - tf->curX) + iabs(ty - tf->curY);
    if (jumpDist <= 32) {
      tf->curX        = (tf->curX + tx) >> 1;
      tf->curY        = (tf->curY + ty) >> 1;
      tf->hasJumpCand = false;
    } else if (tf->hasJumpCand && (iabs(tx - tf->jumpCandX) + iabs(ty - tf->jumpCandY)) <= (int)tf->clusterRadiusPx) {
      // Second consecutive sample confirmed the new location
      tf->curX        = (tx + tf->jumpCandX) >> 1;
      tf->curY        = (ty + tf->jumpCandY) >> 1;
      tf->hasJumpCand = false;
    } else {
      tf->hasJumpCand = true;
      tf->jumpCandX   = tx;
      tf->jumpCandY   = ty;
    }

    ev.isDown = true;
    ev.x      = tf->curX;
    ev.y      = tf->curY;
    return ev;
  }

  // `raw.touched == false`
  if (tf->isDown) {
    if ((uint32_t)(now - tf->lastValidDownMs) >= tf->releaseDebounceMs) {
      tf->isDown        = false;
      tf->clickConsumed = false;
      tf->hasCandidate  = false;
      tf->hasJumpCand   = false;
      ev.justReleased   = true;
    } else {
      // Hold stabilized down state across brief resistive contact bounce
      ev.isDown = true;
    }
  } else if (tf->hasCandidate && (uint32_t)(now - tf->candMs) > 80u) {
    tf->hasCandidate = false;
  }
  ev.x = tf->curX;
  ev.y = tf->curY;
  return ev;
}
