#pragma once

#include "lakatos_api.h"
#include <stdbool.h>
#include <stdint.h>

struct FilteredTouchEvent {
  bool isDown;       // Continuous stabilized press state
  bool justClicked;  // Fires once per verified press after cluster gate + cooldown
  bool justReleased; // Fires once when stabilized press ends
  int  x;            // Stabilized screen X (0..319)
  int  y;            // Stabilized screen Y (0..239)
};

struct TouchFilterState {
  bool     isDown;
  bool     hasCandidate;
  bool     clickConsumed;
  int      candX;
  int      candY;
  uint32_t candMs;
  int      curX;
  int      curY;
  int      jumpCandX;
  int      jumpCandY;
  bool     hasJumpCand;
  uint32_t lastValidDownMs;
  uint32_t lastClickFireMs;
  uint16_t clusterRadiusPx;   // Manhattan radius for 2-sample cluster gate (default 14)
  uint16_t releaseDebounceMs; // Release dropout hysteresis (default 35 ms)
  uint16_t clickCooldownMs;   // Minimum time between discrete clicks (default 110 ms)
};

void               touchFilterInit(TouchFilterState* tf);
FilteredTouchEvent touchFilterPoll(TouchFilterState* tf, const LakatosApi* os);
void               touchFilterResetAfterModal(TouchFilterState* tf, const LakatosApi* os);
