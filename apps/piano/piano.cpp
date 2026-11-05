#include "lakatos_api.h"
#include <stdint.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// Direct ESP32 Hardware Registers for Digital Push-Pull Output on GPIO 26
// (CYD Speaker Amp Input) and Cycle Counter (240 MHz CCOUNT)
// ---------------------------------------------------------------------------
static constexpr uintptr_t kGpioOutW1tsReg        = 0x3FF44008u;
static constexpr uintptr_t kGpioOutW1tcReg        = 0x3FF4400Cu;
static constexpr uintptr_t kGpioEnableW1tsReg     = 0x3FF44024u;
static constexpr uintptr_t kGpioFunc26OutSelReg   = 0x3FF44598u;
static constexpr uintptr_t kRtcIoPadDac2Reg       = 0x3FF48488u;
static constexpr uintptr_t kIoMuxGpio26Reg        = 0x3FF49028u;
static constexpr uint32_t  kSpeakerPinMask        = (1u << 26);

static inline uint32_t readCcount() {
  uint32_t cc;
  __asm__ __volatile__("rsr %0, ccount" : "=a"(cc));
  return cc;
}

static inline void speakerPinHigh() {
  *(volatile uint32_t*)kGpioOutW1tsReg = kSpeakerPinMask;
}

static inline void speakerPinLow() {
  *(volatile uint32_t*)kGpioOutW1tcReg = kSpeakerPinMask;
}

static inline void speakerInit() {
  // 1. Route GPIO 26 pad mux to digital IO_MUX (clear bit 17 RTC_IO_PDAC2_MUX_SEL,
  //    and keep internal analog DAC powered down)
  *(volatile uint32_t*)kRtcIoPadDac2Reg = (2u << 30);

  // 2. Configure IO_MUX_GPIO26 for digital GPIO function (MCU_SEL = 2) with
  //    maximum 40 mA CMOS push-pull drive strength (FUN_DRV = 3)
  *(volatile uint32_t*)kIoMuxGpio26Reg = (2u << 12) | (3u << 10);

  // 3. Route simple digital GPIO_OUT register (0x100 = SIG_GPIO_OUT_IDX) to GPIO 26
  *(volatile uint32_t*)kGpioFunc26OutSelReg = 0x100u;

  // 4. Drive LOW and enable the digital output driver on GPIO 26
  speakerPinLow();
  *(volatile uint32_t*)kGpioEnableW1tsReg = kSpeakerPinMask;
}

static inline void speakerSilence() {
  speakerPinLow();
}

// ---------------------------------------------------------------------------
// 1.5 Octave Note Table: C4 (261.63 Hz) to G5 (783.99 Hz) - 20 Semitones
// phaseStepCc = round(freq_hz * 2^32 / 240,000,000)
// ---------------------------------------------------------------------------
struct NoteInfo {
  const char* name;
  const char* keyLabel;
  uint16_t    freqInt;
  uint16_t    freqDec;      // tenths of Hz
  uint32_t    phaseStepCc;  // 32-bit phase increment per 240 MHz clock cycle
  bool        isBlack;
  int8_t      keyIndex;     // 0..11 for white keys, 0..7 for black keys
  int8_t      leftWhiteIdx; // for black keys: white key index to its left
};

static const NoteInfo kNotes[20] = {
    {"C4",  "C4", 261, 6,  4682u, false,  0, -1},
    {"C#4", "C#", 277, 2,  4960u, true,   0,  0},
    {"D4",  "D4", 293, 7,  5255u, false,  1, -1},
    {"D#4", "D#", 311, 1,  5568u, true,   1,  1},
    {"E4",  "E4", 329, 6,  5899u, false,  2, -1},
    {"F4",  "F4", 349, 2,  6250u, false,  3, -1},
    {"F#4", "F#", 370, 0,  6621u, true,   2,  3},
    {"G4",  "G4", 392, 0,  7015u, false,  4, -1},
    {"G#4", "G#", 415, 3,  7432u, true,   3,  4},
    {"A4",  "A4", 440, 0,  7874u, false,  5, -1},
    {"A#4", "A#", 466, 2,  8342u, true,   4,  5},
    {"B4",  "B4", 493, 9,  8838u, false,  6, -1},
    {"C5",  "C5", 523, 3,  9364u, false,  7, -1},
    {"C#5", "C#", 554, 4,  9921u, true,   5,  7},
    {"D5",  "D5", 587, 3, 10511u, false,  8, -1},
    {"D#5", "D#", 622, 3, 11136u, true,   6,  8},
    {"E5",  "E5", 659, 3, 11798u, false,  9, -1},
    {"F5",  "F5", 698, 5, 12499u, false, 10, -1},
    {"F#5", "F#", 740, 0, 13243u, true,   7, 10},
    {"G5",  "G5", 784, 0, 14030u, false, 11, -1},
};

static const int8_t kWhiteToNote[12] = {
    0, 2, 4, 5, 7, 9, 11, 12, 14, 16, 17, 19
};

static const int8_t kBlackToNote[8] = {
    1, 3, 6, 8, 10, 13, 15, 18
};

// Volume levels (ramp wave peak amplitude out of 255)
static const uint8_t kVolumePeaks[5] = {64, 115, 166, 218, 255};

// ---------------------------------------------------------------------------
// Screen Layout Constants (320x240)
// ---------------------------------------------------------------------------
static constexpr int kHeaderH    = 32;
static constexpr int kFeltY      = 32;
static constexpr int kFeltH      = 3;
static constexpr int kKeysY      = 35;
static constexpr int kWhiteH     = 202;
static constexpr int kBlackH     = 118;
static constexpr int kWhitePitch = 26;
static constexpr int kWhiteW     = 25;
static constexpr int kBlackW     = 18;
static constexpr int kKeysX0     = 4;

static inline int whiteKeyX(int wIdx) {
  return kKeysX0 + wIdx * kWhitePitch;
}

static inline int blackKeyX(int bIdx) {
  int noteIdx = kBlackToNote[bIdx];
  int leftW   = kNotes[noteIdx].leftWhiteIdx;
  int boundaryX = kKeysX0 + (leftW + 1) * kWhitePitch;
  return boundaryX - (kBlackW / 2);
}

// ---------------------------------------------------------------------------
// UI Drawing Helpers
// ---------------------------------------------------------------------------
static void drawNoteStatus(const LakatosApi* os, int activeNote) {
  uint16_t boxBg = 0x0841;
  os->tft_fill_round_rect(108, 4, 112, 24, 4, boxBg);
  os->tft_draw_round_rect(108, 4, 112, 24, 4, 0x3186);
  os->tft_set_text_datum(LAKATOS_MC_DATUM);
  if (activeNote >= 0 && activeNote < 20) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%s %u.%uHz",
             kNotes[activeNote].name,
             (unsigned)kNotes[activeNote].freqInt,
             (unsigned)kNotes[activeNote].freqDec);
    os->tft_set_text_color(LAKATOS_YELLOW, boxBg);
    os->tft_draw_string(buf, 164, 16, 2);
  } else {
    os->tft_set_text_color(0xBDF7, boxBg);
    os->tft_draw_string("C4 - G5 RAMP", 164, 16, 2);
  }
  os->tft_set_text_datum(LAKATOS_TL_DATUM);
}

static void drawVolumeControls(const LakatosApi* os, int volIdx) {
  // [-] button
  os->tft_fill_round_rect(224, 4, 24, 24, 4, 0x2124);
  os->tft_draw_round_rect(224, 4, 24, 24, 4, LAKATOS_CYAN);
  // Volume box
  os->tft_fill_rect(250, 4, 42, 24, 0x0841);
  os->tft_draw_rect(250, 4, 42, 24, 0x3186);
  // [+] button
  os->tft_fill_round_rect(294, 4, 24, 24, 4, 0x2124);
  os->tft_draw_round_rect(294, 4, 24, 24, 4, LAKATOS_CYAN);

  os->tft_set_text_datum(LAKATOS_MC_DATUM);
  os->tft_set_text_color(LAKATOS_WHITE, 0x2124);
  os->tft_draw_string("-", 236, 16, 2);
  os->tft_draw_string("+", 306, 16, 2);

  char vbuf[16];
  snprintf(vbuf, sizeof(vbuf), "V:%d/5", (volIdx & 7) + 1);
  os->tft_set_text_color(LAKATOS_GREEN, 0x0841);
  os->tft_draw_string(vbuf, 271, 16, 2);
  os->tft_set_text_datum(LAKATOS_TL_DATUM);
}

static void drawHeader(const LakatosApi* os, int activeNote, int volIdx) {
  os->tft_fill_rect(0, 0, 320, kHeaderH, 0x10A2);

  // [HOME] button (top-left)
  os->tft_fill_round_rect(2, 4, 48, 24, 4, 0x8000);
  os->tft_draw_round_rect(2, 4, 48, 24, 4, LAKATOS_WHITE);
  os->tft_set_text_datum(LAKATOS_MC_DATUM);
  os->tft_set_text_color(LAKATOS_WHITE, 0x8000);
  os->tft_draw_string("HOME", 26, 16, 2);

  // Title
  os->tft_set_text_datum(LAKATOS_ML_DATUM);
  os->tft_set_text_color(LAKATOS_CYAN, 0x10A2);
  os->tft_draw_string("PIANO", 54, 16, 2);
  os->tft_set_text_datum(LAKATOS_TL_DATUM);

  drawNoteStatus(os, activeNote);
  drawVolumeControls(os, volIdx);

  // Red piano felt strip
  os->tft_fill_rect(0, kFeltY, 320, kFeltH, 0x9000);
}

static void drawSingleWhiteKey(const LakatosApi* os, int wIdx, bool pressed) {
  int x = whiteKeyX(wIdx);
  int noteIdx = kWhiteToNote[wIdx];
  uint16_t bodyCol = pressed ? 0x67FF : 0xFFFF;
  uint16_t bevCol  = pressed ? 0x051F : 0xDEFB;
  uint16_t txtCol  = pressed ? 0x0000 : ((wIdx == 0 || wIdx == 7) ? 0x001F : 0x4208);

  os->tft_fill_rect(x, kKeysY, kWhiteW, kWhiteH - 6, bodyCol);
  os->tft_fill_rect(x, kKeysY + kWhiteH - 6, kWhiteW, 6, bevCol);
  os->tft_draw_rect(x, kKeysY, kWhiteW, kWhiteH, 0x3186);

  os->tft_set_text_datum(LAKATOS_BC_DATUM);
  os->tft_set_text_color(txtCol, bodyCol);
  os->tft_draw_string(kNotes[noteIdx].keyLabel, x + (kWhiteW / 2), kKeysY + kWhiteH - 8, 2);
  os->tft_set_text_datum(LAKATOS_TL_DATUM);
}

static void drawSingleBlackKey(const LakatosApi* os, int bIdx, bool pressed) {
  int x = blackKeyX(bIdx);
  int noteIdx = kBlackToNote[bIdx];
  uint16_t outerCol = 0x0000;
  uint16_t innerCol = pressed ? LAKATOS_ORANGE : 0x18E3;
  uint16_t hiCol    = pressed ? LAKATOS_YELLOW : 0x4208;
  uint16_t txtCol   = pressed ? 0x0000 : 0xBDF7;

  os->tft_fill_rect(x, kKeysY, kBlackW, kBlackH, outerCol);
  os->tft_fill_rect(x + 2, kKeysY + 1, kBlackW - 4, kBlackH - 5, innerCol);
  os->tft_draw_rect(x + 2, kKeysY + 1, kBlackW - 4, kBlackH - 5, hiCol);

  os->tft_set_text_datum(LAKATOS_BC_DATUM);
  os->tft_set_text_color(txtCol, innerCol);
  os->tft_draw_string(kNotes[noteIdx].keyLabel, x + (kBlackW / 2), kKeysY + kBlackH - 8, 1);
  os->tft_set_text_datum(LAKATOS_TL_DATUM);
}

static void drawNoteKeyAndNeighbors(const LakatosApi* os, int noteIdx, int activeNote) {
  if (noteIdx < 0 || noteIdx >= 20) return;
  const NoteInfo& info = kNotes[noteIdx];
  if (info.isBlack) {
    drawSingleBlackKey(os, info.keyIndex, (noteIdx == activeNote));
  } else {
    int wIdx = info.keyIndex;
    drawSingleWhiteKey(os, wIdx, (noteIdx == activeNote));
    // Redraw any black keys that overlap the upper edges of this white key
    for (int b = 0; b < 8; b++) {
      int bNote = kBlackToNote[b];
      int lw = kNotes[bNote].leftWhiteIdx;
      if (lw == wIdx - 1 || lw == wIdx) {
        drawSingleBlackKey(os, b, (bNote == activeNote));
      }
    }
  }
}

static void drawAllKeys(const LakatosApi* os, int activeNote) {
  // Side margins and bottom margin
  os->tft_fill_rect(0, kKeysY, kKeysX0, 240 - kKeysY, LAKATOS_BLACK);
  os->tft_fill_rect(kKeysX0 + 12 * kWhitePitch, kKeysY, 320 - (kKeysX0 + 12 * kWhitePitch), 240 - kKeysY, LAKATOS_BLACK);
  os->tft_fill_rect(0, kKeysY + kWhiteH, 320, 240 - (kKeysY + kWhiteH), LAKATOS_BLACK);

  for (int w = 0; w < 12; w++) {
    int x = whiteKeyX(w);
    // 1px vertical separator to the right of each white key
    os->tft_draw_fast_v_line(x + kWhiteW, kKeysY, kWhiteH, LAKATOS_BLACK);
    int nIdx = kWhiteToNote[w];
    drawSingleWhiteKey(os, w, (nIdx == activeNote));
  }
  for (int b = 0; b < 8; b++) {
    int nIdx = kBlackToNote[b];
    drawSingleBlackKey(os, b, (nIdx == activeNote));
  }
}

// ---------------------------------------------------------------------------
// Keyboard Hit-Testing
// ---------------------------------------------------------------------------
static int hitTestPianoKey(int tx, int ty) {
  if (ty < kKeysY || ty >= kKeysY + kWhiteH) {
    return -1;
  }
  // 1. Check black keys first when touch is in upper portion of keyboard
  if (ty < kKeysY + kBlackH) {
    for (int b = 0; b < 8; b++) {
      int bx = blackKeyX(b);
      if (tx >= bx - 2 && tx < bx + kBlackW + 2) {
        return kBlackToNote[b];
      }
    }
  }
  // 2. Otherwise map to white key
  if (tx >= kKeysX0 && tx < kKeysX0 + 12 * kWhitePitch) {
    int w = (tx - kKeysX0) / kWhitePitch;
    if (w >= 0 && w < 12) {
      return kWhiteToNote[w];
    }
  }
  return -1;
}

// ---------------------------------------------------------------------------
// Play a short monophonic ramp-wave note (used for startup confirmation chime)
// ---------------------------------------------------------------------------
static void playRampToneMs(int noteIdx, int volIdx, uint32_t durationMs) {
  if (noteIdx < 0 || noteIdx >= 20 || durationMs == 0) return;
  static constexpr uint32_t kPwmPeriodCycles = 15360u; // 64 us (15.625 kHz carrier)
  uint32_t totalCycles = durationMs * 240000u;
  uint32_t startCc     = readCcount();
  uint32_t pwmStartCc  = startCc;
  uint32_t phase       = 0;
  uint32_t highCycles  = 0;
  bool     pinIsHigh   = false;

  while ((uint32_t)(readCcount() - startCc) < totalCycles) {
    uint32_t nowCc  = readCcount();
    uint32_t pwmPos = nowCc - pwmStartCc;
    if (pwmPos >= kPwmPeriodCycles) {
      pwmStartCc = nowCc;
      phase += pwmPos * kNotes[noteIdx].phaseStepCc;
      uint32_t ramp   = phase >> 24; // 0..255 sawtooth ramp
      uint32_t scaled = (ramp * kVolumePeaks[volIdx]) >> 8; // 0..255
      highCycles = scaled * 60u; // 0..15300 cycles out of 15360
      if (highCycles > 0) {
        speakerPinHigh();
        pinIsHigh = true;
      } else {
        speakerPinLow();
        pinIsHigh = false;
      }
    } else if (pinIsHigh && pwmPos >= highCycles) {
      speakerPinLow();
      pinIsHigh = false;
    }
  }
  speakerSilence();
}

// ---------------------------------------------------------------------------
// Application Entry Point
// ---------------------------------------------------------------------------
extern "C" void app_main(const LakatosApi* os) {
  speakerInit();

  int activeNote = -1;
  int volIdx     = 4;  // Default volume: 5/5 (100% full-scale ramp)
  bool headerBtnHeld = false;

  os->tft_fill_screen(LAKATOS_BLACK);
  drawHeader(os, activeNote, volIdx);
  drawAllKeys(os, activeNote);

  // Brief startup chime (C5 -> G5, 70 ms each) to confirm speaker output
  os->set_rgb_led(true, false, false, 0);
  playRampToneMs(12, volIdx, 70);
  os->delay_ms(20);
  playRampToneMs(19, volIdx, 90);
  os->set_rgb_led(false, false, false, 0);

  uint32_t phase            = 0;
  uint32_t pwmStartCc       = readCcount();
  uint32_t lastTouchPollCc  = pwmStartCc;
  uint32_t lastValidTouchMs = 0;
  uint32_t highCycles       = 0;
  bool     pinIsHigh        = false;

  // 240,000 cycles = 1 ms at 240 MHz; poll touch every 8 ms (1,920,000 cycles)
  static constexpr uint32_t kTouchPollCycles = 240000u * 8u;
  // Digital PWM carrier period: 15,360 cycles = 64 us (15.625 kHz carrier)
  // Duty cycle (0..15300 cycles) is modulated by the 8-bit ramp wave (0..255 * 60)
  static constexpr uint32_t kPwmPeriodCycles = 15360u;

  for (;;) {
    if (activeNote >= 0) {
      // ---------------------------------------------------------------------
      // Tight Monophonic Ramp-Wave Digital PWM Synthesis Loop (Core 1)
      // ---------------------------------------------------------------------
      uint32_t nowCc  = readCcount();
      uint32_t pwmPos = nowCc - pwmStartCc;
      if (pwmPos >= kPwmPeriodCycles) {
        pwmStartCc = nowCc;
        phase += pwmPos * kNotes[activeNote].phaseStepCc;
        uint32_t ramp   = phase >> 24; // 0..255 linear ramp wave
        uint32_t scaled = (ramp * kVolumePeaks[volIdx]) >> 8; // 0..255
        highCycles = scaled * 60u; // 0..15300 cycles out of 15360
        if (highCycles > 0) {
          speakerPinHigh();
          pinIsHigh = true;
        } else {
          speakerPinLow();
          pinIsHigh = false;
        }
      } else if (pinIsHigh && pwmPos >= highCycles) {
        speakerPinLow();
        pinIsHigh = false;
      }

      if ((uint32_t)(nowCc - lastTouchPollCc) < kTouchPollCycles) {
        continue;
      }
      lastTouchPollCc = nowCc;
    }

    // -----------------------------------------------------------------------
    // Touch Polling & UI State Machine
    // -----------------------------------------------------------------------
    LakatosTouchSample ts = os->read_touch();
    uint32_t nowMs = os->millis();

    if (ts.touched) {
      lastValidTouchMs = nowMs;

      // Check top header buttons ([HOME], [-], [+])
      if (ts.y < kHeaderH) {
        if (!headerBtnHeld) {
          headerBtnHeld = true;
          if (ts.x <= 52) {
            speakerSilence();
            os->set_rgb_led(false, false, false, 0);
            os->exit(0);
          } else if (ts.x >= 220 && ts.x <= 252) {
            if (volIdx > 0) {
              volIdx--;
              drawVolumeControls(os, volIdx);
            }
          } else if (ts.x >= 290 && ts.x < 320) {
            if (volIdx < 4) {
              volIdx++;
              drawVolumeControls(os, volIdx);
            }
          }
        }
        if (activeNote >= 0) {
          int oldNote = activeNote;
          activeNote = -1;
          pinIsHigh  = false;
          speakerSilence();
          os->set_rgb_led(false, false, false, 0);
          drawNoteKeyAndNeighbors(os, oldNote, -1);
          drawNoteStatus(os, -1);
        }
      } else {
        headerBtnHeld = false;
        int hitNote = hitTestPianoKey(ts.x, ts.y);
        if (hitNote != activeNote) {
          int oldNote = activeNote;
          activeNote = hitNote;
          if (activeNote < 0) {
            pinIsHigh = false;
            speakerSilence();
            os->set_rgb_led(false, false, false, 0);
          } else if (oldNote < 0) {
            os->set_rgb_led(true, false, false, 0);
          }
          if (oldNote >= 0) {
            drawNoteKeyAndNeighbors(os, oldNote, activeNote);
          }
          if (activeNote >= 0) {
            drawNoteKeyAndNeighbors(os, activeNote, activeNote);
          }
          drawNoteStatus(os, activeNote);
          // Reset cycle timers after TFT draw so phase starts cleanly
          pwmStartCc      = readCcount();
          lastTouchPollCc = pwmStartCc;
          if (oldNote < 0 && activeNote >= 0) {
            phase      = 0;
            highCycles = 0;
            pinIsHigh  = false;
          }
        }
      }
    } else {
      headerBtnHeld = false;
      // 35 ms release debounce prevents resistive touch bounce from stuttering held notes
      if (activeNote >= 0 && (uint32_t)(nowMs - lastValidTouchMs) >= 35u) {
        int oldNote = activeNote;
        activeNote = -1;
        pinIsHigh  = false;
        speakerSilence();
        os->set_rgb_led(false, false, false, 0);
        drawNoteKeyAndNeighbors(os, oldNote, -1);
        drawNoteStatus(os, -1);
      }
    }

    if (activeNote < 0) {
      while (os->serial_available && os->serial_available() > 0) {
        int ch = os->serial_read();
        if (ch == 'q' || ch == 'Q' || ch == 3) {
          speakerSilence();
          os->set_rgb_led(false, false, false, 0);
          os->exit(0);
        }
      }
      os->delay_ms(8);
    }
  }
}
