#include "lakatos_api.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// 128-Note MIDI Pitch Step Table for 256-Sample Wavetable @ 15,625 Hz
// stepFp16(n) = round(440.0 * 2^((n - 69)/12) * 256 * 65536 / 15625)
// ---------------------------------------------------------------------------
static const uint32_t kMidiStepFp16[128] = {
       8769,    9291,    9843,   10428,   11048,   11705,   12401,   13139,
      13920,   14748,   15625,   16554,   17538,   18581,   19686,   20857,
      22097,   23411,   24803,   26278,   27840,   29496,   31250,   33108,
      35077,   37162,   39372,   41713,   44194,   46822,   49606,   52556,
      55681,   58992,   62500,   66216,   70154,   74325,   78745,   83427,
      88388,   93644,   99213,  105112,  111362,  117984,  125000,  132433,
     140308,  148651,  157490,  166855,  176777,  187288,  198425,  210224,
     222725,  235969,  250000,  264866,  280616,  297302,  314980,  333710,
     353553,  374577,  396850,  420448,  445449,  471937,  500000,  529732,
     561231,  594604,  629961,  667420,  707107,  749154,  793701,  840896,
     890899,  943874, 1000000, 1059463, 1122462, 1189207, 1259921, 1334840,
    1414214, 1498307, 1587401, 1681793, 1781797, 1887749, 2000000, 2118926,
    2244924, 2378414, 2519842, 2669680, 2828427, 2996614, 3174802, 3363586,
    3563595, 3775497, 4000000, 4237852, 4489848, 4756828, 5039684, 5339359,
    5656854, 5993228, 6349604, 6727171, 7127190, 7550995, 8000000, 8475705,
    8979696, 9513657,10079368,10678719,11313708,11986457,12699208,13454343
};

// Quarter-wave 64-entry sine table (0..255) for harmonic wavetable construction & vibrato
static const uint8_t kQuarterSine64[64] = {
      0,   6,  13,  19,  25,  31,  37,  44,  50,  56,  62,  68,  74,  80,  86,  92,
     98, 103, 109, 115, 120, 126, 131, 136, 142, 147, 152, 157, 162, 167, 171, 176,
    180, 185, 189, 193, 197, 201, 205, 208, 212, 215, 219, 222, 225, 228, 231, 233,
    236, 238, 240, 242, 244, 246, 247, 249, 250, 251, 252, 253, 254, 254, 255, 255
};

static inline int32_t fastSin256(uint8_t phase) {
  uint8_t q = phase & 0x3Fu;
  int32_t mag = (phase & 0x40u) ? kQuarterSine64[63u - q] : kQuarterSine64[q];
  return (phase & 0x80u) ? -mag : mag;
}

// ---------------------------------------------------------------------------
// Windchimes General MIDI Harmonic Wavetable + ADSR Presets (16 Families)
// Ported from github.com/strickyak/windchimes/gm_presets.go
// ---------------------------------------------------------------------------
struct GmPresetDef {
  const char* shortName;
  uint8_t     harmonics[8]; // Relative harmonic amplitudes (H1..H8)
  uint16_t    attackTicks;  // In 4ms (250 Hz) ticks
  uint16_t    decayTicks;   // In 4ms (250 Hz) ticks
  uint8_t     sustainLvl;   // 0..255
  uint16_t    releaseTicks; // In 4ms (250 Hz) ticks
  uint8_t     gainQ7;       // 128 = 1.0x
  uint8_t     vibDepth;     // Pitch vibrato depth (0..8)
  bool        chorus;       // Ensemble voice detuning
};

enum GmFamilyIdx : uint8_t {
  GM_PIANO       = 0,
  GM_HARPSICHORD = 1,
  GM_BELLS       = 2,
  GM_ORGAN       = 3,
  GM_CHURCH_ORG  = 4,
  GM_GUITAR      = 5,
  GM_BASS        = 6,
  GM_SOLO_STR    = 7,
  GM_PIZZICATO   = 8,
  GM_HARP        = 9,
  GM_TIMPANI     = 10,
  GM_STR_ENS     = 11,
  GM_CHOIR       = 12,
  GM_BRASS       = 13,
  GM_REEDS       = 14,
  GM_FLUTE       = 15,
  GM_FAMILY_COUNT = 16
};

static const GmPresetDef kGmPresets[GM_FAMILY_COUNT] = {
    // 0: Acoustic Piano (0..5)
    {"Piano",     {100, 60, 35, 20, 10,  5,  2,  0},   1, 375,  14, 100, 128, 0, false},
    // 1: Harpsichord / Clavinet (6..7)
    {"Harpsichd", {100, 80, 65, 50, 40, 30, 20, 15},   1, 200,   0,  38, 110, 0, false},
    // 2: Bells / Celesta / Marimba (8..15)
    {"Bells",     {100, 15, 60,  8, 35,  4, 20,  0},   1, 300,   0, 150, 115, 0, false},
    // 3: Organ / Accordion (16..18, 20..23)
    {"Organ",     {100, 70, 50, 40, 20, 30, 15,  0},   3,  14, 230,  50, 102, 0, false},
    // 4: Church Organ (19)
    {"ChurchOrg", {100, 80, 60, 50, 30, 40, 20, 10},   4,  14, 242,  64,  98, 0, true },
    // 5: Acoustic / Electric Guitar (24..31)
    {"Guitar",    {100, 70, 40, 25, 15,  8,  4,  0},   1, 250,   8,  64, 122, 0, false},
    // 6: Acoustic / Fingered Bass (32..39)
    {"Bass",      {100, 85, 50, 30, 15,  5,  0,  0},   2, 100, 166,  75, 150, 0, false},
    // 7: Solo Violin / Viola / Cello / Contrabass (40..43)
    {"SoloStr",   {100, 75, 55, 42, 32, 24, 18, 14},   8,  20, 217,  88, 115, 4, false},
    // 8: Pizzicato Strings (45)
    {"Pizzicato", {100, 60, 30, 15,  5,  0,  0,  0},   1, 112,   0,  38, 128, 0, false},
    // 9: Orchestral Harp (46)
    {"Harp",      {100, 50, 25, 12,  6,  0,  0,  0},   1, 450,   0, 125, 115, 0, false},
    // 10: Timpani (47)
    {"Timpani",   {100, 45, 70, 25, 35, 15,  0,  0},   1, 165,   0,  80, 145, 0, false},
    // 11: String Ensemble (44, 48..51)
    {"Strings",   {100, 75, 58, 45, 35, 26, 20, 15},  10,  25, 224, 112,  98, 2, true },
    // 12: Choir / Voice (52..55)
    {"Choir",     {100, 25, 70, 15, 40,  8, 25,  0},  20,  38, 230, 125,  98, 2, true },
    // 13: Trumpet / Horn / Trombone / Tuba / Brass (56..63)
    {"Brass",     {100, 90, 75, 60, 45, 30, 20, 10},   5,  20, 204,  64, 110, 1, false},
    // 14: Oboe / Bassoon / Clarinet / Reeds (64..71)
    {"Reed/Oboe", { 70,100, 80, 65, 45, 30, 18, 10},   6,  15, 217,  50, 110, 3, false},
    // 15: Flute / Piccolo / Pipe / Synth (72..127)
    {"Flute",     {100, 20,  8,  3,  0,  0,  0,  0},   9,  14, 230,  64, 115, 3, false},
};

static uint8_t programToFamily(uint8_t prog) {
  if (prog <= 5)  return GM_PIANO;
  if (prog <= 7)  return GM_HARPSICHORD;
  if (prog <= 15) return GM_BELLS;
  if (prog == 19) return GM_CHURCH_ORG;
  if (prog <= 23) return GM_ORGAN;
  if (prog <= 31) return GM_GUITAR;
  if (prog <= 39) return GM_BASS;
  if (prog <= 43) return GM_SOLO_STR;
  if (prog == 45) return GM_PIZZICATO;
  if (prog == 46) return GM_HARP;
  if (prog == 47) return GM_TIMPANI;
  if (prog <= 51) return GM_STR_ENS;
  if (prog <= 55) return GM_CHOIR;
  if (prog <= 63) return GM_BRASS;
  if (prog <= 71) return GM_REEDS;
  return GM_FLUTE;
}

// ---------------------------------------------------------------------------
// Data Structures (Placed in 32 KB Static Arena 0x3FFC8000)
// ---------------------------------------------------------------------------
static constexpr int      kMaxTracks        = 32;
static constexpr uint32_t kTrkWinSize       = 512u;
static constexpr int      kMaxVoices        = 16;
static constexpr int      kMaxBrowserItems  = 64;
static constexpr uint32_t kMaxInRamMidiSize = 81920u; // 80 KB in-RAM threshold

enum VoiceEnvState : uint8_t {
  ENV_IDLE    = 0,
  ENV_ATTACK  = 1,
  ENV_DECAY   = 2,
  ENV_SUSTAIN = 3,
  ENV_RELEASE = 4,
};

struct SynthVoice {
  uint8_t  envState;
  uint8_t  midiCh;
  uint8_t  midiNote;
  uint8_t  familyIdx;
  uint8_t  velocity;
  uint8_t  peakVol;       // Target volume at peak of attack (0..64)
  uint8_t  sustainVol;    // Target volume during sustain (0..64)
  uint8_t  releaseStart;  // Volume when release began
  uint8_t  curEnvVol;     // Current unscaled envelope volume (0..64)
  uint8_t  vibPhase;
  uint8_t  drumType;      // 0 = melodic, 1 = kick/tom, 2 = snare/hat
  bool     sustainHeld;   // Deferred note-off due to CC64 sustain pedal
  uint16_t stateTicks;
  uint16_t ageTicks;
  uint32_t baseStepFp16;
};

struct MidiChanState {
  uint8_t  program;
  uint8_t  familyIdx;
  uint8_t  ccVol;         // CC7 (default 100)
  uint8_t  ccExpr;        // CC11 (default 127)
  int16_t  pitchBend;     // -8192 .. +8191
  bool     sustainPedal;  // CC64 >= 64
  bool     muted;
  bool     hasExplicitProg;
  uint8_t  vuLevel;       // 0..64
  uint8_t  lastNote;      // 0..127
  uint16_t activeNotes;   // Count of currently active voices on this channel
  uint32_t totalNoteOns;  // Total Note Ons seen on this channel
};

struct MidiTrack {
  uint32_t startOff;
  uint32_t endOff;
  uint32_t curOff;
  uint32_t winBaseOff;
  uint16_t winValidLen;
  uint8_t  runningStatus;
  bool     ended;
  bool     organHint;
  int64_t  nextTick;
};

struct BrowserItem {
  char name[44];
  bool isDir;
};

struct AppState {
  const LakatosApi* os;

  // 16 single-cycle 256-byte harmonic wavetables in internal SRAM (4,096 B)
  int8_t        wavetables[GM_FAMILY_COUNT][256];

  // Per-track 512-byte sliding windows for streaming large .mid files (16,384 B)
  uint8_t       trkWin[kMaxTracks][kTrkWinSize];
  MidiTrack     tracks[kMaxTracks];
  SynthVoice    voices[kMaxVoices];
  MidiChanState chans[16];

  // Optional in-RAM full file buffer for .mid / .mid.zip <= 80 KB
  uint8_t*      inRamBuf;
  uint32_t      inRamSize;
  uint32_t      inRamCap;
  bool          streamSpilledToSd;
  char          pageFilePath[192];

  // Song Metadata & Timing
  char          songTitle[32];
  char          currentFileName[48];
  uint16_t      midiFormat;
  uint16_t      numTracks;
  uint16_t      ticksPerQuarter;
  volatile uint32_t usPerQuarter;   // Default 500,000 us (120 BPM)
  volatile int64_t  curMidiTick;
  uint64_t          usTickRem;
  volatile uint32_t elapsedTicks250Hz;
  volatile uint32_t totalNoteCount;
  volatile bool     playing;
  volatile bool     songLoaded;
  volatile bool     songEndedFlag;
  bool              autoNextSong;
  int               masterVolIdx;   // 0..11 ("goes to 11")

  // File Browser State
  bool          inBrowserView;
  char          curDir[128];
  BrowserItem   items[kMaxBrowserItems];
  int           itemCount;
  int           scrollTop;
  int           currentSongItemIdx;
};

static AppState gState;

// Volume 0..11 ("goes to 11"): 0 = silent, 8 = normal 1.0x (64), 9..11 = overmodulated
static const uint8_t kMasterVolScale[12] = {
    0, 6, 12, 18, 26, 35, 44, 54, 64, 96, 144, 224
};

// ---------------------------------------------------------------------------
// Utility & String Helpers
// ---------------------------------------------------------------------------
static char toLowerAscii(char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static int strCaseCmp(const char* a, const char* b) {
  while (*a && *b) {
    char ca = toLowerAscii(*a++);
    char cb = toLowerAscii(*b++);
    if (ca != cb) return (int)(uint8_t)ca - (int)(uint8_t)cb;
  }
  return (int)(uint8_t)toLowerAscii(*a) - (int)(uint8_t)toLowerAscii(*b);
}

static bool strContainsIgnoreCase(const char* hay, const char* needle) {
  size_t nlen = strlen(needle);
  if (nlen == 0) return true;
  for (size_t i = 0; hay[i] != '\0'; i++) {
    size_t k = 0;
    while (k < nlen && hay[i + k] != '\0' &&
           toLowerAscii(hay[i + k]) == toLowerAscii(needle[k])) {
      k++;
    }
    if (k == nlen) return true;
  }
  return false;
}

static bool hasMidiExtension(const char* name) {
  size_t len = strlen(name);
  if (len >= 4 && strCaseCmp(name + len - 4, ".mid") == 0) return true;
  if (len >= 5 && strCaseCmp(name + len - 5, ".midi") == 0) return true;
  if (len >= 8 && strCaseCmp(name + len - 8, ".mid.zip") == 0) return true;
  if (len >= 4 && strCaseCmp(name + len - 4, ".kar") == 0) return true;
  return false;
}

static uint16_t readBe16(const uint8_t* p) {
  return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t readBe32(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

// ---------------------------------------------------------------------------
// Harmonic Wavetable Initialization (Ported from windchimes buildWavetable)
// ---------------------------------------------------------------------------
static void initGmWavetables() {
  for (int f = 0; f < GM_FAMILY_COUNT; f++) {
    const GmPresetDef& def = kGmPresets[f];
    int32_t maxAbs = 1;
    int32_t temp[256];
    for (int i = 0; i < 256; i++) {
      int32_t sum = 0;
      for (int h = 0; h < 8; h++) {
        uint8_t amp = def.harmonics[h];
        if (amp == 0) continue;
        uint8_t phase = (uint8_t)((i * (h + 1)) & 0xFFu);
        sum += (int32_t)amp * fastSin256(phase);
      }
      temp[i] = sum;
      int32_t a = (sum < 0) ? -sum : sum;
      if (a > maxAbs) maxAbs = a;
    }
    for (int i = 0; i < 256; i++) {
      int32_t v = (temp[i] * 122) / maxAbs;
      if (v < -126) v = -126;
      if (v > 126)  v = 126;
      gState.wavetables[f][i] = (int8_t)v;
    }
  }
}

// ---------------------------------------------------------------------------
// Voice Scaling & Hardware Mixer Output
// ---------------------------------------------------------------------------
static inline uint8_t scaleVoiceVol(int envVol) {
  if (envVol <= 0) return 0;
  if (envVol > 64) envVol = 64;
  int vIdx = gState.masterVolIdx;
  if (vIdx <= 0) return 0;
  if (vIdx > 11) vIdx = 11;
  uint32_t scaled = ((uint32_t)envVol * kMasterVolScale[vIdx]) >> 6;
  return (scaled > 255u) ? 255u : (uint8_t)scaled;
}

static void pushVoiceToMixer(int vIdx, uint32_t stepFp16, bool resetPos) {
  const SynthVoice& sv = gState.voices[vIdx];
  if (sv.envState == ENV_IDLE || gState.chans[sv.midiCh & 15u].muted) {
    gState.os->audio_set_voice(vIdx, nullptr, LAKATOS_WAVE_NONE, 0, 0, 0, 0, false);
    return;
  }
  uint8_t outVol = scaleVoiceVol(sv.curEnvVol);
  if (sv.drumType == 2) {
    // Unpitched percussion (Snare / Hi-Hat / Cymbal): use mixer LFSR noise generator
    gState.os->audio_set_voice(vIdx, nullptr, LAKATOS_WAVE_NOISE, 0, 256, stepFp16, outVol, resetPos);
  } else {
    const int8_t* wt = gState.wavetables[sv.familyIdx % GM_FAMILY_COUNT];
    gState.os->audio_set_voice(vIdx, wt, 256u, 0u, 256u, stepFp16, outVol, resetPos);
  }
}

static void applyMasterVolumeImmediate() {
  if (!gState.songLoaded || !gState.playing) return;
  for (int v = 0; v < kMaxVoices; v++) {
    if (gState.voices[v].envState != ENV_IDLE) {
      pushVoiceToMixer(v, gState.voices[v].baseStepFp16, false);
    }
  }
}

// ---------------------------------------------------------------------------
// 16-Voice Polyphonic Allocator & ADSR + Vibrato + Chorus Engine
// ---------------------------------------------------------------------------
static void releaseVoice(int vIdx) {
  SynthVoice& sv = gState.voices[vIdx];
  if (sv.envState == ENV_IDLE || sv.envState == ENV_RELEASE) return;
  sv.releaseStart = sv.curEnvVol;
  sv.envState     = ENV_RELEASE;
  sv.stateTicks   = 0;
}

static void handleMidiNoteOff(uint8_t ch, uint8_t note) {
  ch &= 15u;
  bool pedal = gState.chans[ch].sustainPedal;
  for (int v = 0; v < kMaxVoices; v++) {
    SynthVoice& sv = gState.voices[v];
    if (sv.envState != ENV_IDLE && sv.envState != ENV_RELEASE &&
        sv.midiCh == ch && sv.midiNote == note) {
      if (pedal) {
        sv.sustainHeld = true;
      } else {
        releaseVoice(v);
      }
    }
  }
}

static int allocateVoiceSlot(uint8_t ch, uint8_t note) {
  // 1. Re-use any voice already playing the exact same (ch, note)
  for (int v = 0; v < kMaxVoices; v++) {
    if (gState.voices[v].envState != ENV_IDLE &&
        gState.voices[v].midiCh == ch && gState.voices[v].midiNote == note) {
      return v;
    }
  }
  // 2. Any idle voice
  for (int v = 0; v < kMaxVoices; v++) {
    if (gState.voices[v].envState == ENV_IDLE) {
      return v;
    }
  }
  // 3. Quietest voice already in RELEASE state
  int bestRel = -1;
  int bestRelVol = 999;
  for (int v = 0; v < kMaxVoices; v++) {
    if (gState.voices[v].envState == ENV_RELEASE) {
      if ((int)gState.voices[v].curEnvVol < bestRelVol) {
        bestRelVol = gState.voices[v].curEnvVol;
        bestRel = v;
      }
    }
  }
  if (bestRel >= 0) return bestRel;

  // 4. Protect highest melody note and lowest bass note; steal oldest/quietest inner voice
  int highestNote = -1, highestIdx = -1;
  int lowestNote  = 256, lowestIdx  = -1;
  for (int v = 0; v < kMaxVoices; v++) {
    const SynthVoice& sv = gState.voices[v];
    if (sv.drumType == 0) {
      if ((int)sv.midiNote > highestNote) {
        highestNote = sv.midiNote;
        highestIdx  = v;
      }
      if ((int)sv.midiNote < lowestNote) {
        lowestNote = sv.midiNote;
        lowestIdx  = v;
      }
    }
  }

  int bestSteal = 0;
  int bestScore = 0x7FFFFFFF;
  for (int v = 0; v < kMaxVoices; v++) {
    if (v == highestIdx || v == lowestIdx) continue;
    const SynthVoice& sv = gState.voices[v];
    int score = (int)sv.curEnvVol * 64 - (int)(sv.ageTicks > 500 ? 500 : sv.ageTicks);
    if (score < bestScore) {
      bestScore = score;
      bestSteal = v;
    }
  }
  return bestSteal;
}

static void handleMidiNoteOn(uint8_t ch, uint8_t note, uint8_t vel) {
  if (vel == 0) {
    handleMidiNoteOff(ch, note);
    return;
  }
  ch &= 15u;
  note &= 127u;

  MidiChanState& cs = gState.chans[ch];
  cs.lastNote = note;
  cs.totalNoteOns++;
  gState.totalNoteCount++;

  // Compute channel + velocity gain (0..64)
  uint8_t fam = (ch == 9) ? GM_TIMPANI : cs.familyIdx;
  const GmPresetDef& preset = kGmPresets[fam];

  uint32_t gain = (uint32_t)vel * (uint32_t)cs.ccVol * (uint32_t)cs.ccExpr; // up to 127*127*127 = 2,048,383
  gain = (gain >> 14); // 0..125
  gain = (gain * (uint32_t)preset.gainQ7) >> 8; // 0..64
  if (gain < 4u) gain = 4u;
  if (gain > 64u) gain = 64u;

  if (gain > cs.vuLevel) {
    cs.vuLevel = (uint8_t)gain;
  }

  int vIdx = allocateVoiceSlot(ch, note);
  SynthVoice& sv = gState.voices[vIdx];
  sv.midiCh       = ch;
  sv.midiNote     = note;
  sv.familyIdx    = fam;
  sv.velocity     = vel;
  sv.peakVol      = (uint8_t)gain;
  sv.sustainVol   = (uint8_t)(((uint32_t)gain * preset.sustainLvl) >> 8);
  sv.releaseStart = (uint8_t)gain;
  sv.sustainHeld  = false;
  sv.stateTicks   = 0;
  sv.ageTicks     = 0;
  sv.vibPhase     = (uint8_t)(vIdx * 37u);

  if (ch == 9) {
    // General MIDI Channel 10 Percussion
    if (note == 35 || note == 36) {
      // Acoustic / Bass Drum Kick
      sv.drumType     = 1;
      sv.familyIdx    = GM_TIMPANI;
      sv.baseStepFp16 = kMidiStepFp16[41]; // Low F1 pitch sweep
      sv.sustainVol   = 0;
    } else if (note == 41 || note == 43 || note == 45 || note == 47 || note == 48 || note == 50) {
      // Toms
      sv.drumType     = 1;
      sv.familyIdx    = GM_TIMPANI;
      sv.baseStepFp16 = kMidiStepFp16[(note < 60) ? (note + 5) : note];
      sv.sustainVol   = 0;
    } else {
      // Snare (38, 40), Hi-Hats (42, 44, 46), Cymbals (49, 51..59), Perc
      sv.drumType     = 2;
      sv.baseStepFp16 = (note == 38 || note == 40) ? 180000u : 420000u;
      sv.sustainVol   = 0;
    }
    sv.curEnvVol = sv.peakVol;
    sv.envState  = ENV_DECAY;
  } else {
    sv.drumType = 0;
    uint32_t step = kMidiStepFp16[note];
    // Apply slight ensemble chorus detuning per voice slot (~1.5 cents)
    if (preset.chorus) {
      int detune = ((vIdx & 3) == 1) ? 2 : (((vIdx & 3) == 2) ? -2 : (((vIdx & 3) == 3) ? 1 : -1));
      step += (int32_t)(step >> 10) * detune;
    }
    sv.baseStepFp16 = step;
    if (preset.attackTicks <= 1) {
      sv.curEnvVol = sv.peakVol;
      sv.envState  = ENV_DECAY;
    } else {
      sv.curEnvVol = (uint8_t)((sv.peakVol / preset.attackTicks) + 1u);
      sv.envState  = ENV_ATTACK;
    }
  }

  pushVoiceToMixer(vIdx, sv.baseStepFp16, true);
}

static void updateVoicesAdsr250Hz() {
  uint16_t chActive[16] = {0};

  for (int v = 0; v < kMaxVoices; v++) {
    SynthVoice& sv = gState.voices[v];
    if (sv.envState == ENV_IDLE) continue;

    sv.ageTicks++;
    sv.stateTicks++;
    const GmPresetDef& preset = kGmPresets[sv.familyIdx % GM_FAMILY_COUNT];

    if (sv.drumType != 0) {
      // Percussion fast decay (Kick = 28 ticks, Snare/Hat = 22..45 ticks)
      uint16_t dTicks = (sv.drumType == 1) ? 30u : ((sv.midiNote == 42 || sv.midiNote == 44) ? 16u : 38u);
      if (sv.drumType == 1 && sv.baseStepFp16 > kMidiStepFp16[28]) {
        sv.baseStepFp16 -= (sv.baseStepFp16 >> 4); // Downward kick/tom pitch sweep
      }
      if (sv.stateTicks >= dTicks) {
        sv.envState  = ENV_IDLE;
        sv.curEnvVol = 0;
        pushVoiceToMixer(v, sv.baseStepFp16, false);
        continue;
      }
      sv.curEnvVol = (uint8_t)(((uint32_t)sv.peakVol * (dTicks - sv.stateTicks)) / dTicks);
      pushVoiceToMixer(v, sv.baseStepFp16, false);
      chActive[sv.midiCh & 15u]++;
      continue;
    }

    switch (sv.envState) {
      case ENV_ATTACK: {
        if (sv.stateTicks >= preset.attackTicks) {
          sv.curEnvVol  = sv.peakVol;
          sv.envState   = ENV_DECAY;
          sv.stateTicks = 0;
        } else {
          sv.curEnvVol = (uint8_t)(((uint32_t)sv.peakVol * sv.stateTicks) / preset.attackTicks);
          if (sv.curEnvVol == 0) sv.curEnvVol = 1;
        }
        break;
      }
      case ENV_DECAY: {
        if (sv.stateTicks >= preset.decayTicks) {
          if (sv.sustainVol == 0) {
            sv.envState  = ENV_IDLE;
            sv.curEnvVol = 0;
            pushVoiceToMixer(v, sv.baseStepFp16, false);
            continue;
          }
          sv.curEnvVol  = sv.sustainVol;
          sv.envState   = ENV_SUSTAIN;
          sv.stateTicks = 0;
        } else {
          uint32_t span = sv.peakVol - sv.sustainVol;
          sv.curEnvVol = (uint8_t)(sv.peakVol - (span * sv.stateTicks) / preset.decayTicks);
        }
        break;
      }
      case ENV_SUSTAIN: {
        sv.curEnvVol = sv.sustainVol;
        // Safety auto-release if a MIDI file omits Note Off on sustained instruments (> 12 seconds)
        if (sv.ageTicks > 3000u) {
          releaseVoice(v);
        }
        break;
      }
      case ENV_RELEASE: {
        if (sv.stateTicks >= preset.releaseTicks || sv.releaseStart == 0) {
          sv.envState  = ENV_IDLE;
          sv.curEnvVol = 0;
          pushVoiceToMixer(v, sv.baseStepFp16, false);
          continue;
        }
        sv.curEnvVol = (uint8_t)(((uint32_t)sv.releaseStart * (preset.releaseTicks - sv.stateTicks)) /
                                 preset.releaseTicks);
        break;
      }
      default:
        break;
    }

    // Apply delayed vibrato Modulation (after ~120 ms = 30 ticks, matching windchimes)
    uint32_t step = sv.baseStepFp16;
    if (preset.vibDepth > 0 && sv.ageTicks > 30u) {
      sv.vibPhase = (uint8_t)(sv.vibPhase + 5u); // ~5.0 Hz at 250 Hz tick rate
      int32_t s = fastSin256(sv.vibPhase);       // -255..+255
      int32_t mod = ((int32_t)(step >> 10) * s * (int32_t)preset.vibDepth) >> 8;
      step = (uint32_t)((int32_t)step + mod);
    }

    pushVoiceToMixer(v, step, false);
    chActive[sv.midiCh & 15u]++;
  }

  for (int c = 0; c < 16; c++) {
    gState.chans[c].activeNotes = chActive[c];
  }
}

// ---------------------------------------------------------------------------
// Multi-Track Standard MIDI File Reader (In-RAM & Sliding-Window Paged)
// ---------------------------------------------------------------------------
static inline uint8_t peekTrackByte(int t, uint32_t off) {
  if (gState.inRamBuf != nullptr) {
    return (off < gState.inRamSize) ? gState.inRamBuf[off] : 0;
  }
  const MidiTrack& tr = gState.tracks[t];
  if (off >= tr.winBaseOff && off < tr.winBaseOff + tr.winValidLen) {
    return gState.trkWin[t][off - tr.winBaseOff];
  }
  return 0;
}

static inline uint8_t readTrackByte(int t) {
  MidiTrack& tr = gState.tracks[t];
  if (tr.curOff >= tr.endOff) {
    tr.ended = true;
    return 0;
  }
  uint8_t b = peekTrackByte(t, tr.curOff);
  tr.curOff++;
  if (tr.curOff >= tr.endOff) {
    tr.ended = true;
  }
  return b;
}

static uint32_t readTrackVlq(int t) {
  MidiTrack& tr = gState.tracks[t];
  uint32_t val = 0;
  for (int i = 0; i < 4 && !tr.ended; i++) {
    uint8_t b = readTrackByte(t);
    val = (val << 7) | (uint32_t)(b & 0x7Fu);
    if ((b & 0x80u) == 0) break;
  }
  return val;
}

static void skipTrackBytes(int t, uint32_t len) {
  MidiTrack& tr = gState.tracks[t];
  if (tr.curOff + len >= tr.endOff) {
    tr.curOff = tr.endOff;
    tr.ended  = true;
  } else {
    tr.curOff += len;
  }
}

static void processTrackEvent(int t) {
  MidiTrack& tr = gState.tracks[t];
  if (tr.ended || tr.curOff >= tr.endOff) {
    tr.ended = true;
    return;
  }

  uint8_t b = peekTrackByte(t, tr.curOff);
  uint8_t status = 0;
  if (b >= 0x80u) {
    status = readTrackByte(t);
    if (status < 0xF0u) {
      tr.runningStatus = status;
    }
  } else {
    status = tr.runningStatus;
  }

  if (status == 0xFFu) {
    // Meta Event
    uint8_t mtype = readTrackByte(t);
    uint32_t mlen = readTrackVlq(t);
    if (mtype == 0x2Fu) {
      // End of Track
      skipTrackBytes(t, mlen);
      tr.ended = true;
      return;
    } else if (mtype == 0x51u && mlen == 3u) {
      // Set Tempo (microseconds per quarter note)
      uint32_t t0 = readTrackByte(t);
      uint32_t t1 = readTrackByte(t);
      uint32_t t2 = readTrackByte(t);
      uint32_t us = (t0 << 16) | (t1 << 8) | t2;
      if (us >= 20000u && us <= 4000000u) {
        gState.usPerQuarter = us;
      }
    } else {
      skipTrackBytes(t, mlen);
    }
  } else if (status == 0xF0u || status == 0xF7u) {
    // SysEx Event
    uint32_t slen = readTrackVlq(t);
    skipTrackBytes(t, slen);
  } else if (status >= 0x80u && status < 0xF0u) {
    uint8_t hi = status & 0xF0u;
    uint8_t ch = status & 0x0Fu;
    if (hi == 0xC0u) {
      uint8_t prog = readTrackByte(t) & 127u;
      gState.chans[ch].program         = prog;
      gState.chans[ch].familyIdx       = programToFamily(prog);
      gState.chans[ch].hasExplicitProg = true;
    } else if (hi == 0xD0u) {
      (void)readTrackByte(t); // Channel Pressure
    } else {
      uint8_t d1 = readTrackByte(t) & 127u;
      uint8_t d2 = readTrackByte(t) & 127u;
      if (hi == 0x80u) {
        handleMidiNoteOff(ch, d1);
      } else if (hi == 0x90u) {
        if (tr.organHint && !gState.chans[ch].hasExplicitProg) {
          gState.chans[ch].program   = 19;
          gState.chans[ch].familyIdx = GM_CHURCH_ORG;
        }
        handleMidiNoteOn(ch, d1, d2);
      } else if (hi == 0xB0u) {
        if (d1 == 7u) {
          gState.chans[ch].ccVol = d2;
        } else if (d1 == 11u) {
          gState.chans[ch].ccExpr = d2;
        } else if (d1 == 64u) {
          bool pedal = (d2 >= 64u);
          bool wasPedal = gState.chans[ch].sustainPedal;
          gState.chans[ch].sustainPedal = pedal;
          if (wasPedal && !pedal) {
            for (int v = 0; v < kMaxVoices; v++) {
              if (gState.voices[v].envState != ENV_IDLE &&
                  gState.voices[v].midiCh == ch && gState.voices[v].sustainHeld) {
                gState.voices[v].sustainHeld = false;
                releaseVoice(v);
              }
            }
          }
        } else if (d1 == 120u || d1 == 123u) {
          // All Sound Off / All Notes Off
          for (int v = 0; v < kMaxVoices; v++) {
            if (gState.voices[v].envState != ENV_IDLE && gState.voices[v].midiCh == ch) {
              releaseVoice(v);
            }
          }
        }
      } else if (hi == 0xE0u) {
        gState.chans[ch].pitchBend = (int16_t)(((int)d2 << 7) | (int)d1) - 8192;
      }
    }
  } else {
    // Unexpected byte; advance safely
    (void)readTrackByte(t);
  }

  if (!tr.ended && tr.curOff < tr.endOff) {
    uint32_t dt = readTrackVlq(t);
    tr.nextTick += (int64_t)dt;
  } else {
    tr.ended = true;
  }
}

// Core 0 252.016 Hz (62 samples = 3,968 us exact) MIDI Sequencer & Synth Tick
static void midiTickCb(void* user) {
  (void)user;
  if (!gState.playing || !gState.songLoaded) return;

  gState.elapsedTicks250Hz++;

  // Decay channel VU meters smoothly (~50 Hz equivalent rate)
  if ((gState.elapsedTicks250Hz & 3u) == 0) {
    for (int c = 0; c < 16; c++) {
      if (gState.chans[c].vuLevel > 2) gState.chans[c].vuLevel -= 2;
      else gState.chans[c].vuLevel = 0;
    }
  }

  // Advance MIDI tick accumulator by 3,968 microseconds
  uint32_t usPerQ = gState.usPerQuarter;
  if (usPerQ < 20000u) usPerQ = 500000u;
  gState.usTickRem += 3968ULL * (uint64_t)gState.ticksPerQuarter;
  uint64_t deltaTicks = gState.usTickRem / (uint64_t)usPerQ;
  gState.usTickRem   %= (uint64_t)usPerQ;
  gState.curMidiTick += (int64_t)deltaTicks;

  // Dispatch all track events due at or before curMidiTick
  int safetyLimit = 96;
  bool anyTrackActive = false;
  while (safetyLimit-- > 0) {
    int bestTrk = -1;
    int64_t minTick = 0x7FFFFFFFFFFFFFFFLL;
    anyTrackActive = false;
    for (int t = 0; t < gState.numTracks; t++) {
      const MidiTrack& tr = gState.tracks[t];
      if (!tr.ended) {
        anyTrackActive = true;
        // In paged mode, if curOff moved beyond currently buffered window, wait for Core 1 refill
        if (gState.inRamBuf == nullptr && tr.curOff >= tr.winBaseOff + tr.winValidLen) {
          continue;
        }
        if (tr.nextTick <= gState.curMidiTick && tr.nextTick < minTick) {
          minTick = tr.nextTick;
          bestTrk = t;
        }
      }
    }
    if (bestTrk < 0) break;
    processTrackEvent(bestTrk);
  }

  updateVoicesAdsr250Hz();

  if (!anyTrackActive) {
    // Check if all voices have finished their release tails
    bool anySounding = false;
    for (int v = 0; v < kMaxVoices; v++) {
      if (gState.voices[v].envState != ENV_IDLE) {
        anySounding = true;
        break;
      }
    }
    if (!anySounding) {
      if (gState.autoNextSong) {
        gState.songEndedFlag = true;
      } else {
        // Loop current MIDI file from the beginning
        for (int t = 0; t < gState.numTracks; t++) {
          MidiTrack& tr = gState.tracks[t];
          tr.curOff        = tr.startOff;
          tr.runningStatus = 0;
          tr.ended         = (tr.startOff >= tr.endOff);
          tr.nextTick      = 0;
          if (!tr.ended && (gState.inRamBuf != nullptr || tr.winBaseOff == tr.startOff)) {
            tr.nextTick = (int64_t)readTrackVlq(t);
          }
        }
        gState.curMidiTick       = 0;
        gState.usTickRem         = 0;
        gState.elapsedTicks250Hz = 0;
        gState.usPerQuarter      = 500000u;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Core 1 Track Window Refiller & MIDI File Loader
// ---------------------------------------------------------------------------
static void refillTrackWindow(int t, uint32_t targetOff) {
  if (gState.inRamBuf != nullptr) return;
  MidiTrack& tr = gState.tracks[t];
  if (targetOff >= tr.endOff) {
    tr.winBaseOff  = tr.endOff;
    tr.winValidLen = 0;
    return;
  }
  uint32_t avail = tr.endOff - targetOff;
  if (avail > kTrkWinSize) avail = kTrkWinSize;
  int32_t got = gState.os->fs_read(gState.pageFilePath, targetOff, gState.trkWin[t], avail);
  if (got > 0) {
    tr.winBaseOff  = targetOff;
    tr.winValidLen = (uint16_t)got;
  } else {
    tr.winBaseOff  = targetOff;
    tr.winValidLen = 0;
    tr.ended       = true;
  }
}

static void serviceTrackWindowsCore1() {
  if (!gState.songLoaded || gState.inRamBuf != nullptr) return;
  for (int t = 0; t < gState.numTracks; t++) {
    MidiTrack& tr = gState.tracks[t];
    if (tr.ended) continue;
    uint32_t cur = tr.curOff;
    if (cur < tr.winBaseOff ||
        (cur - tr.winBaseOff >= (kTrkWinSize / 2u) &&
         tr.winBaseOff + tr.winValidLen < tr.endOff)) {
      refillTrackWindow(t, cur);
    }
  }
}

static void freeCurrentMidiResources() {
  gState.playing    = false;
  gState.songLoaded = false;
  gState.os->audio_set_tick_cb(nullptr, nullptr, 0);
  for (int v = 0; v < kMaxVoices; v++) {
    gState.voices[v].envState = ENV_IDLE;
    gState.os->audio_set_voice(v, nullptr, LAKATOS_WAVE_NONE, 0, 0, 0, 0, true);
  }
  gState.os->delay_ms(3);
  if (gState.inRamBuf != nullptr) {
    gState.os->free(gState.inRamBuf);
    gState.inRamBuf = nullptr;
  }
  gState.inRamSize = 0;
  gState.inRamCap  = 0;
}

static void inspectInitialTrackMeta(int t) {
  MidiTrack& tr = gState.tracks[t];
  uint32_t scanOff = tr.startOff;
  uint32_t limitOff = tr.startOff + 160u;
  if (limitOff > tr.endOff) limitOff = tr.endOff;

  while (scanOff + 4u < limitOff) {
    // Only scan delta-time 0 initial meta events
    if (peekTrackByte(t, scanOff) != 0x00u) break;
    if (peekTrackByte(t, scanOff + 1u) != 0xFFu) break;
    uint8_t mtype = peekTrackByte(t, scanOff + 2u);
    uint8_t mlen  = peekTrackByte(t, scanOff + 3u);
    if (mlen >= 0x80u || scanOff + 4u + mlen > limitOff) break;

    if (mtype == 0x03u && mlen > 0) {
      char tname[48];
      uint8_t copyLen = (mlen < sizeof(tname) - 1) ? mlen : (uint8_t)(sizeof(tname) - 1);
      for (uint8_t i = 0; i < copyLen; i++) {
        char c = (char)peekTrackByte(t, scanOff + 4u + i);
        tname[i] = (c >= 32 && c <= 126) ? c : ' ';
      }
      tname[copyLen] = '\0';
      if (strContainsIgnoreCase(tname, "organ") || strContainsIgnoreCase(tname, "pedal")) {
        tr.organHint = true;
      }
      if (gState.songTitle[0] == '\0' && copyLen >= 3 &&
          !strContainsIgnoreCase(tname, "swell") &&
          !strContainsIgnoreCase(tname, "great") &&
          !strContainsIgnoreCase(tname, "pedal") &&
          !strContainsIgnoreCase(tname, "stops") &&
          !strContainsIgnoreCase(tname, "violin") &&
          !strContainsIgnoreCase(tname, "viola") &&
          !strContainsIgnoreCase(tname, "cello") &&
          !strContainsIgnoreCase(tname, "oboe") &&
          !strContainsIgnoreCase(tname, "flute") &&
          !strContainsIgnoreCase(tname, "horn") &&
          !strContainsIgnoreCase(tname, "trump") &&
          !strContainsIgnoreCase(tname, "bass")) {
        strncpy(gState.songTitle, tname, sizeof(gState.songTitle) - 1);
        gState.songTitle[sizeof(gState.songTitle) - 1] = '\0';
      }
    }
    scanOff += 4u + mlen;
  }
}

static bool onMidiStreamChunk(const uint8_t* data, size_t len, void* user) {
  (void)user;
  if (!gState.streamSpilledToSd) {
    if (gState.inRamSize + len <= kMaxInRamMidiSize) {
      if (gState.inRamSize + len > gState.inRamCap) {
        uint32_t nextCap = (gState.inRamCap == 0) ? 16384u : (gState.inRamCap * 2u);
        while (nextCap < gState.inRamSize + len) nextCap += 16384u;
        if (nextCap > kMaxInRamMidiSize) nextCap = kMaxInRamMidiSize;
        uint8_t* nb = (uint8_t*)gState.os->realloc(gState.inRamBuf, nextCap);
        if (!nb) return false;
        gState.inRamBuf = nb;
        gState.inRamCap = nextCap;
      }
      memcpy(gState.inRamBuf + gState.inRamSize, data, len);
      gState.inRamSize += (uint32_t)len;
      return true;
    }
    // Exceeded 80 KB in-RAM threshold (e.g. a 296 KB .mid.zip): spill to /sd/_midcache.mid
    strncpy(gState.pageFilePath, "/sd/_midcache.mid", sizeof(gState.pageFilePath) - 1);
    if (!gState.os->fs_write(gState.pageFilePath, gState.inRamBuf, gState.inRamSize)) {
      return false;
    }
    gState.os->free(gState.inRamBuf);
    gState.inRamBuf  = nullptr;
    gState.inRamCap  = 0;
    gState.streamSpilledToSd = true;
  }
  if (!gState.os->fs_write_at(gState.pageFilePath, gState.inRamSize, data, len)) {
    return false;
  }
  gState.inRamSize += (uint32_t)len;
  return true;
}

static bool loadAndPlayMidiFile(const char* fullPath, const char* displayName) {
  freeCurrentMidiResources();

  strncpy(gState.currentFileName, displayName ? displayName : fullPath, sizeof(gState.currentFileName) - 1);
  gState.currentFileName[sizeof(gState.currentFileName) - 1] = '\0';
  gState.songTitle[0] = '\0';

  // Determine whether we can page directly from fullPath or should stream into RAM / cache
  size_t pathLen = strlen(fullPath);
  bool isZip = (pathLen >= 4 && strCaseCmp(fullPath + pathLen - 4, ".zip") == 0);
  bool isLocalSdOrFlash = (strncmp(fullPath, "/sd/", 4) == 0 || strncmp(fullPath, "/flash/", 7) == 0);

  uint32_t totalFileSize = 0;
  if (isLocalSdOrFlash && !isZip) {
    LakatosFsEntry st;
    if (gState.os->fs_stat(fullPath, &st) && st.size > 14) {
      totalFileSize = (uint32_t)st.size;
      strncpy(gState.pageFilePath, fullPath, sizeof(gState.pageFilePath) - 1);
      gState.pageFilePath[sizeof(gState.pageFilePath) - 1] = '\0';
      // If small enough (<= 64 KB), load into RAM for zero-SD-access playback; otherwise page directly!
      if (totalFileSize <= 65536u) {
        gState.inRamBuf = (uint8_t*)gState.os->malloc(totalFileSize);
        if (gState.inRamBuf) {
          int32_t got = gState.os->fs_read(fullPath, 0, gState.inRamBuf, totalFileSize);
          if (got == (int32_t)totalFileSize) {
            gState.inRamSize = totalFileSize;
            gState.inRamCap  = totalFileSize;
          } else {
            gState.os->free(gState.inRamBuf);
            gState.inRamBuf = nullptr;
          }
        }
      }
    }
  }

  if (totalFileSize == 0) {
    gState.streamSpilledToSd = false;
    int32_t got = gState.os->fs_read_stream(fullPath, onMidiStreamChunk, nullptr);
    if (got <= 14) {
      freeCurrentMidiResources();
      return false;
    }
    totalFileSize = gState.inRamSize;
  }

  // Parse 14-byte MThd Header
  uint8_t mthd[14];
  if (gState.inRamBuf != nullptr) {
    memcpy(mthd, gState.inRamBuf, 14);
  } else {
    if (gState.os->fs_read(gState.pageFilePath, 0, mthd, 14) != 14) {
      freeCurrentMidiResources();
      return false;
    }
  }

  if (memcmp(mthd, "MThd", 4) != 0) {
    freeCurrentMidiResources();
    return false;
  }

  uint32_t hdrLen   = readBe32(mthd + 4);
  uint16_t fmt      = readBe16(mthd + 8);
  uint16_t ntrksHdr = readBe16(mthd + 10);
  uint16_t division = readBe16(mthd + 12);
  if (hdrLen < 6u || division == 0 || (division & 0x8000u) != 0) {
    freeCurrentMidiResources();
    return false;
  }

  gState.midiFormat      = fmt;
  gState.ticksPerQuarter = division;
  gState.usPerQuarter    = 500000u; // Default 120 BPM
  gState.numTracks       = 0;

  // Scan MTrk chunks
  uint32_t pos = 8u + hdrLen;
  while (pos + 8u <= totalFileSize && gState.numTracks < kMaxTracks && gState.numTracks < ntrksHdr) {
    uint8_t chdr[8];
    if (gState.inRamBuf != nullptr) {
      memcpy(chdr, gState.inRamBuf + pos, 8);
    } else {
      if (gState.os->fs_read(gState.pageFilePath, pos, chdr, 8) != 8) break;
    }
    uint32_t clen = readBe32(chdr + 4);
    uint32_t cend = pos + 8u + clen;
    if (cend > totalFileSize) cend = totalFileSize;

    if (memcmp(chdr, "MTrk", 4) == 0 && clen > 0) {
      int t = gState.numTracks++;
      MidiTrack& tr = gState.tracks[t];
      memset(&tr, 0, sizeof(tr));
      tr.startOff = pos + 8u;
      tr.endOff   = cend;
      tr.curOff   = tr.startOff;
      if (gState.inRamBuf == nullptr) {
        refillTrackWindow(t, tr.startOff);
      }
      inspectInitialTrackMeta(t);
      tr.nextTick = (int64_t)readTrackVlq(t);
    }
    pos = cend;
  }

  if (gState.numTracks == 0) {
    freeCurrentMidiResources();
    return false;
  }

  // Fallback songTitle from cleaned filename if no track name metadata found
  if (gState.songTitle[0] == '\0') {
    strncpy(gState.songTitle, gState.currentFileName, sizeof(gState.songTitle) - 1);
    gState.songTitle[sizeof(gState.songTitle) - 1] = '\0';
    for (int i = 0; gState.songTitle[i] != '\0'; i++) {
      if (gState.songTitle[i] == '.') {
        gState.songTitle[i] = '\0';
        break;
      }
      if (gState.songTitle[i] == '_') gState.songTitle[i] = ' ';
    }
  }

  // Reset 16 MIDI channels
  for (int c = 0; c < 16; c++) {
    bool wasMuted = gState.chans[c].muted;
    memset(&gState.chans[c], 0, sizeof(MidiChanState));
    gState.chans[c].program   = 0;
    gState.chans[c].familyIdx = (c == 9) ? GM_TIMPANI : GM_PIANO;
    gState.chans[c].ccVol     = 100;
    gState.chans[c].ccExpr    = 127;
    gState.chans[c].muted     = wasMuted;
  }

  gState.curMidiTick       = 0;
  gState.usTickRem         = 0;
  gState.elapsedTicks250Hz = 0;
  gState.totalNoteCount    = 0;
  gState.songEndedFlag     = false;
  gState.songLoaded        = true;
  gState.playing           = true;

  {
    char lbuf[160];
    int n = snprintf(lbuf, sizeof(lbuf),
                     "[MIDI] playing: %s ['%s', %u trks, div=%u, %lu B (%s), heap=%lu]\n",
                     gState.currentFileName, gState.songTitle,
                     (unsigned)gState.numTracks, (unsigned)gState.ticksPerQuarter,
                     (unsigned long)totalFileSize,
                     (gState.inRamBuf != nullptr) ? "RAM" : "PAGED",
                     (unsigned long)gState.os->free_heap_bytes());
    if (n > 0) gState.os->serial_write(lbuf, (size_t)n);
  }

  // 62 samples @ 15,625 Hz = 3,968 us (252.016 Hz)
  gState.os->audio_set_tick_cb(midiTickCb, nullptr, 62u);
  return true;
}

// ---------------------------------------------------------------------------
// Directory & File Browser Logic
// ---------------------------------------------------------------------------
static void loadDirectory(const char* path) {
  strncpy(gState.curDir, path, sizeof(gState.curDir) - 1);
  gState.curDir[sizeof(gState.curDir) - 1] = '\0';
  gState.itemCount = 0;
  gState.scrollTop = 0;

  static LakatosFsEntry rawEntries[64];
  int n = gState.os->fs_ls(gState.curDir, rawEntries, 64);
  if (n <= 0) return;

  for (int pass = 0; pass < 2; pass++) {
    for (int i = 0; i < n && gState.itemCount < kMaxBrowserItems; i++) {
      const LakatosFsEntry& e = rawEntries[i];
      if (e.name[0] == '.' || e.name[0] == '\0') continue;
      if (strcmp(e.name, "_midcache.mid") == 0) continue;
      if (pass == 0 && e.isDir) {
        BrowserItem& it = gState.items[gState.itemCount++];
        strncpy(it.name, e.name, sizeof(it.name) - 1);
        it.name[sizeof(it.name) - 1] = '\0';
        it.isDir = true;
      } else if (pass == 1 && !e.isDir && hasMidiExtension(e.name)) {
        BrowserItem& it = gState.items[gState.itemCount++];
        strncpy(it.name, e.name, sizeof(it.name) - 1);
        it.name[sizeof(it.name) - 1] = '\0';
        it.isDir = false;
      }
    }
  }

  for (int i = 1; i < gState.itemCount; i++) {
    BrowserItem key = gState.items[i];
    int j = i - 1;
    while (j >= 0 && gState.items[j].isDir == key.isDir &&
           strCaseCmp(gState.items[j].name, key.name) > 0) {
      gState.items[j + 1] = gState.items[j];
      j--;
    }
    gState.items[j + 1] = key;
  }
}

static void navigateParentDir() {
  size_t len = strlen(gState.curDir);
  while (len > 1 && gState.curDir[len - 1] == '/') {
    gState.curDir[--len] = '\0';
  }
  char* lastSlash = nullptr;
  for (size_t i = 1; i < len; i++) {
    if (gState.curDir[i] == '/') lastSlash = &gState.curDir[i];
  }
  if (lastSlash) {
    *lastSlash = '\0';
    loadDirectory(gState.curDir);
  }
}

static bool playAdjacentSong(int dir) {
  if (gState.itemCount <= 0) return false;
  int start = gState.currentSongItemIdx;
  if (start < 0 || start >= gState.itemCount) {
    start = (dir >= 0) ? (gState.itemCount - 1) : 1;
  }
  for (int step = 1; step <= gState.itemCount; step++) {
    int idx = (start + step * dir + gState.itemCount * 2) % gState.itemCount;
    if (!gState.items[idx].isDir) {
      char fullPath[192];
      snprintf(fullPath, sizeof(fullPath), "%s/%s", gState.curDir, gState.items[idx].name);
      if (loadAndPlayMidiFile(fullPath, gState.items[idx].name)) {
        gState.currentSongItemIdx = idx;
        return true;
      }
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// UI Rendering (320x240 TFT)
// ---------------------------------------------------------------------------
static void drawTopHeader() {
  const LakatosApi* os = gState.os;
  os->tft_fill_rect(0, 0, 320, 28, 0x10A2);

  // [EXIT] button
  os->tft_fill_round_rect(2, 2, 46, 24, 4, 0x8000);
  os->tft_draw_round_rect(2, 2, 46, 24, 4, LAKATOS_WHITE);
  os->tft_set_text_datum(LAKATOS_MC_DATUM);
  os->tft_set_text_color(LAKATOS_WHITE, 0x8000);
  os->tft_draw_string("EXIT", 25, 14, 2);

  // Song title / filename
  os->tft_set_text_datum(LAKATOS_ML_DATUM);
  os->tft_set_text_color(LAKATOS_CYAN, 0x10A2);
  if (gState.songLoaded && gState.songTitle[0] != '\0') {
    char tbuf[24];
    strncpy(tbuf, gState.songTitle, 20);
    tbuf[20] = '\0';
    os->tft_draw_string(tbuf, 52, 14, 2);
  } else {
    os->tft_draw_string("MIDI SYNTH", 52, 14, 2);
  }

  // [FILES] / [SYNTH] view toggle button
  uint16_t viewBg = gState.inBrowserView ? 0x03E0 : 0x2124;
  os->tft_fill_round_rect(206, 2, 56, 24, 4, viewBg);
  os->tft_draw_round_rect(206, 2, 56, 24, 4, LAKATOS_CYAN);
  os->tft_set_text_datum(LAKATOS_MC_DATUM);
  os->tft_set_text_color(LAKATOS_WHITE, viewBg);
  os->tft_draw_string(gState.inBrowserView ? "SYNTH" : "FILES", 234, 14, 2);

  // Master Volume indicator / mute-toggle badge [VOL:0..11]
  int vol = gState.masterVolIdx;
  if (vol < 0) vol = 0;
  if (vol > 11) vol = 11;
  uint16_t volCol = (vol == 0) ? 0x7BEF
                               : ((vol <= 8) ? LAKATOS_GREEN
                                             : ((vol <= 10) ? LAKATOS_ORANGE : LAKATOS_RED));
  os->tft_fill_round_rect(264, 2, 54, 24, 4, 0x0841);
  os->tft_draw_round_rect(264, 2, 54, 24, 4, volCol);
  char vbuf[16];
  snprintf(vbuf, sizeof(vbuf), "VOL:%d", vol);
  os->tft_set_text_color(volCol, 0x0841);
  os->tft_draw_string(vbuf, 291, 14, 2);
  os->tft_set_text_datum(LAKATOS_TL_DATUM);
}

static void drawMidiStatusBar() {
  const LakatosApi* os = gState.os;
  os->tft_fill_rect(0, 28, 320, 22, 0x0841);
  os->tft_draw_fast_h_line(0, 49, 320, 0x3186);

  char buf[68];
  if (gState.songLoaded) {
    uint32_t secs = gState.elapsedTicks250Hz / 252u;
    uint32_t bpm  = (gState.usPerQuarter > 0) ? (60000000u / gState.usPerQuarter) : 120u;
    int activeVoices = 0;
    for (int v = 0; v < kMaxVoices; v++) {
      if (gState.voices[v].envState != ENV_IDLE) activeVoices++;
    }
    snprintf(buf, sizeof(buf), "Time %02lu:%02lu  BPM %3lu  Trk %2u  Voices %2d/16  Notes %lu",
             (unsigned long)(secs / 60u), (unsigned long)(secs % 60u),
             (unsigned long)bpm, (unsigned)gState.numTracks,
             activeVoices, (unsigned long)gState.totalNoteCount);
  } else {
    snprintf(buf, sizeof(buf), "No MIDI loaded - tap FILES to pick a song");
  }
  os->tft_set_text_datum(LAKATOS_ML_DATUM);
  os->tft_set_text_color(LAKATOS_YELLOW, 0x0841);
  os->tft_draw_string(buf, 6, 39, 1);
  os->tft_set_text_datum(LAKATOS_TL_DATUM);
}

// 8x2 grid of all 16 MIDI channels (y = 50..175, 8 rows of 15 px each, 2 columns of 160 px)
static void drawMidiChannelsGrid(bool drawStaticFrame) {
  const LakatosApi* os = gState.os;
  if (drawStaticFrame) {
    os->tft_fill_rect(0, 50, 320, 126, LAKATOS_BLACK);
    os->tft_draw_fast_v_line(160, 50, 126, 0x3186);
  }

  os->tft_set_text_datum(LAKATOS_TL_DATUM);
  for (int c = 0; c < 16; c++) {
    int col = c / 8;
    int row = c % 8;
    int x   = col * 160;
    int y   = 51 + row * 15;
    const MidiChanState& cs = gState.chans[c];

    uint16_t rowBg = cs.muted ? 0x2800 : ((row & 1) ? 0x0841 : LAKATOS_BLACK);
    os->tft_fill_rect(x + 1, y, 158, 14, rowBg);

    const char* instName = (c == 9) ? "Percussn" : kGmPresets[cs.familyIdx % GM_FAMILY_COUNT].shortName;
    char lbuf[20];
    if (cs.muted) {
      snprintf(lbuf, sizeof(lbuf), "%2d MUTE", c + 1);
    } else {
      snprintf(lbuf, sizeof(lbuf), "%2d %-9s", c + 1, instName);
    }
    uint16_t fg = cs.muted ? LAKATOS_ORANGE
                           : ((cs.activeNotes > 0) ? LAKATOS_CYAN
                                                   : ((cs.totalNoteOns > 0) ? 0xBDF7 : 0x4208));
    os->tft_set_text_color(fg, rowBg);
    os->tft_draw_string(lbuf, x + 3, y + 3, 1);

    // Pitch & VU activity bar (x + 78 .. x + 156, width 78 px)
    int barX = x + 78;
    int barW = 78;
    os->tft_draw_rect(barX, y + 2, barW, 10, 0x2124);
    if (!cs.muted && cs.vuLevel > 0) {
      int vuW = ((int)cs.vuLevel * (barW - 2)) / 64;
      if (vuW > barW - 2) vuW = barW - 2;
      if (vuW > 0) {
        uint16_t vuCol = (cs.vuLevel > 50) ? LAKATOS_ORANGE : LAKATOS_GREEN;
        os->tft_fill_rect(barX + 1, y + 3, vuW, 8, vuCol);
      }
      // Highlight pitch marker inside bar for lastNote (24..108)
      int noteClamped = cs.lastNote;
      if (noteClamped < 24) noteClamped = 24;
      if (noteClamped > 108) noteClamped = 108;
      int pipX = barX + 2 + ((noteClamped - 24) * (barW - 6)) / 84;
      os->tft_fill_rect(pipX, y + 3, 3, 8, LAKATOS_YELLOW);
    }
  }
}

static void drawVolumeSliderBar() {
  const LakatosApi* os = gState.os;
  int vol = gState.masterVolIdx;
  if (vol < 0) vol = 0;
  if (vol > 11) vol = 11;

  os->tft_fill_rect(0, 176, 320, 32, 0x0841);
  os->tft_draw_fast_h_line(0, 176, 320, 0x3186);

  uint16_t valCol = (vol == 0) ? 0x7BEF
                               : ((vol <= 8) ? LAKATOS_GREEN
                                             : ((vol <= 10) ? LAKATOS_ORANGE : LAKATOS_RED));

  os->tft_set_text_datum(LAKATOS_ML_DATUM);
  os->tft_set_text_color(LAKATOS_WHITE, 0x0841);
  os->tft_draw_string("VOL", 6, 186, 1);
  char vbuf[8];
  snprintf(vbuf, sizeof(vbuf), "%2d", vol);
  os->tft_set_text_color(valCol, 0x0841);
  os->tft_draw_string(vbuf, 28, 192, 2);
  if (vol == 0) {
    os->tft_set_text_color(0x7BEF, 0x0841);
    os->tft_draw_string("MUTE", 6, 200, 1);
  } else if (vol >= 9) {
    os->tft_set_text_color(valCol, 0x0841);
    os->tft_draw_string("OVER", 6, 200, 1);
  }

  static constexpr int kTrackX0 = 62;
  static constexpr int kStepPx  = 22;
  static constexpr int kTrackW  = 11 * kStepPx;

  os->tft_set_text_datum(LAKATOS_MC_DATUM);
  for (int i = 0; i <= 11; i++) {
    int tx = kTrackX0 + i * kStepPx;
    uint16_t tickCol = (i <= 8) ? 0x7BEF : ((i <= 10) ? LAKATOS_ORANGE : LAKATOS_RED);
    os->tft_draw_fast_v_line(tx, 202, (i == 0 || i == 8 || i == 11) ? 5 : 3, tickCol);
  }
  os->tft_set_text_color(0xBDF7, 0x0841);
  os->tft_draw_string("0", kTrackX0, 181, 1);
  os->tft_draw_string("4", kTrackX0 + 4 * kStepPx, 181, 1);
  os->tft_set_text_color(LAKATOS_GREEN, 0x0841);
  os->tft_draw_string("8", kTrackX0 + 8 * kStepPx, 181, 1);
  os->tft_set_text_color(LAKATOS_RED, 0x0841);
  os->tft_draw_string("11", kTrackX0 + 11 * kStepPx, 181, 1);

  os->tft_fill_round_rect(kTrackX0 - 4, 190, kTrackW + 8, 10, 4, LAKATOS_BLACK);
  os->tft_draw_round_rect(kTrackX0 - 4, 190, kTrackW + 8, 10, 4, 0x4208);

  if (vol > 0) {
    int cleanEnd = (vol <= 8) ? vol : 8;
    os->tft_fill_rect(kTrackX0, 192, cleanEnd * kStepPx, 6, LAKATOS_GREEN);
    if (vol >= 9) {
      int warnEnd = (vol <= 10) ? vol : 10;
      os->tft_fill_rect(kTrackX0 + 8 * kStepPx, 192, (warnEnd - 8) * kStepPx, 6, LAKATOS_ORANGE);
    }
    if (vol >= 11) {
      os->tft_fill_rect(kTrackX0 + 10 * kStepPx, 192, kStepPx, 6, LAKATOS_RED);
    }
  }

  os->tft_draw_fast_v_line(kTrackX0 + 8 * kStepPx, 189, 12, LAKATOS_WHITE);

  int thumbX = kTrackX0 + vol * kStepPx;
  os->tft_fill_round_rect(thumbX - 7, 186, 15, 18, 4, valCol);
  os->tft_draw_round_rect(thumbX - 7, 186, 15, 18, 4, LAKATOS_WHITE);
  os->tft_draw_fast_v_line(thumbX, 189, 12, LAKATOS_BLACK);

  os->tft_set_text_datum(LAKATOS_TL_DATUM);
}

static void drawTransportBar() {
  const LakatosApi* os = gState.os;
  os->tft_fill_rect(0, 208, 320, 32, 0x10A2);
  os->tft_draw_fast_h_line(0, 208, 320, 0x3186);

  struct Btn {
    int x, w;
    const char* label;
    uint16_t bg;
  };
  Btn btns[4] = {
      {  4, 68, "|< PREV", 0x2124},
      { 76, 84, gState.playing ? "PAUSE" : "PLAY", (uint16_t)(gState.playing ? 0x03E0u : 0x8400u)},
      {164, 68, "NEXT >|", 0x2124},
      {236, 80, gState.autoNextSong ? "MODE:NEXT" : "MODE:LOOP", 0x18E3},
  };

  os->tft_set_text_datum(LAKATOS_MC_DATUM);
  for (int i = 0; i < 4; i++) {
    os->tft_fill_round_rect(btns[i].x, 211, btns[i].w, 26, 4, btns[i].bg);
    os->tft_draw_round_rect(btns[i].x, 211, btns[i].w, 26, 4, LAKATOS_CYAN);
    os->tft_set_text_color(LAKATOS_WHITE, btns[i].bg);
    os->tft_draw_string(btns[i].label, btns[i].x + btns[i].w / 2, 224, 2);
  }
  os->tft_set_text_datum(LAKATOS_TL_DATUM);
}

static void drawBrowserView() {
  const LakatosApi* os = gState.os;
  os->tft_fill_rect(0, 28, 320, 212, LAKATOS_BLACK);

  struct RootBtn {
    int x, w;
    const char* label;
  };
  static const RootBtn kRoots[4] = {
      {  4, 58, "/sd"},
      { 66, 58, "/net"},
      {128, 64, "/flash"},
      {196, 54, ".. UP"},
  };
  os->tft_set_text_datum(LAKATOS_MC_DATUM);
  for (int i = 0; i < 4; i++) {
    bool active = (i < 3) && (strncmp(gState.curDir, kRoots[i].label, strlen(kRoots[i].label)) == 0);
    uint16_t bg = active ? 0x03EF : 0x2124;
    os->tft_fill_round_rect(kRoots[i].x, 30, kRoots[i].w, 22, 3, bg);
    os->tft_draw_round_rect(kRoots[i].x, 30, kRoots[i].w, 22, 3, LAKATOS_CYAN);
    os->tft_set_text_color(LAKATOS_WHITE, bg);
    os->tft_draw_string(kRoots[i].label, kRoots[i].x + kRoots[i].w / 2, 41, 2);
  }

  os->tft_fill_rect(0, 54, 320, 14, 0x0841);
  os->tft_set_text_datum(LAKATOS_ML_DATUM);
  os->tft_set_text_color(LAKATOS_YELLOW, 0x0841);
  os->tft_draw_string(gState.curDir, 6, 61, 1);

  static constexpr int kRowsPerPage = 8;
  os->tft_set_text_datum(LAKATOS_ML_DATUM);
  if (gState.itemCount == 0) {
    os->tft_set_text_color(0xBDF7, LAKATOS_BLACK);
    os->tft_draw_string("No .mid / .mid.zip files or folders here", 12, 110, 2);
  } else {
    for (int r = 0; r < kRowsPerPage; r++) {
      int idx = gState.scrollTop + r;
      int y = 70 + r * 20;
      if (idx >= gState.itemCount) break;
      const BrowserItem& it = gState.items[idx];
      bool isCur = (!it.isDir && idx == gState.currentSongItemIdx);
      uint16_t rowBg = isCur ? 0x18E3 : ((r & 1) ? 0x0841 : LAKATOS_BLACK);
      os->tft_fill_rect(2, y, 266, 19, rowBg);

      char line[60];
      if (it.isDir) {
        snprintf(line, sizeof(line), "[DIR] %s/", it.name);
        os->tft_set_text_color(LAKATOS_CYAN, rowBg);
      } else {
        snprintf(line, sizeof(line), "%s", it.name);
        os->tft_set_text_color(isCur ? LAKATOS_GREEN : LAKATOS_WHITE, rowBg);
      }
      os->tft_draw_string(line, 8, y + 9, 2);
    }
  }

  os->tft_set_text_datum(LAKATOS_MC_DATUM);
  os->tft_fill_round_rect(272, 72, 46, 74, 4, 0x2124);
  os->tft_draw_round_rect(272, 72, 46, 74, 4, LAKATOS_CYAN);
  os->tft_set_text_color(LAKATOS_WHITE, 0x2124);
  os->tft_draw_string("PG UP", 295, 109, 2);

  os->tft_fill_round_rect(272, 156, 46, 74, 4, 0x2124);
  os->tft_draw_round_rect(272, 156, 46, 74, 4, LAKATOS_CYAN);
  os->tft_set_text_color(LAKATOS_WHITE, 0x2124);
  os->tft_draw_string("PG DN", 295, 193, 2);
  os->tft_set_text_datum(LAKATOS_TL_DATUM);
}

static void drawFullScreen() {
  drawTopHeader();
  if (gState.inBrowserView) {
    drawBrowserView();
  } else {
    drawMidiStatusBar();
    drawMidiChannelsGrid(true);
    drawVolumeSliderBar();
    drawTransportBar();
  }
}

// ---------------------------------------------------------------------------
// Application Entry Point
// ---------------------------------------------------------------------------
extern "C" void app_main(const LakatosApi* os) {
  memset(&gState, 0, sizeof(gState));
  gState.os                 = os;
  gState.masterVolIdx       = 7; // Default level 7 on 0..11 scale (8 = unity, 9..11 = overdrive)
  gState.autoNextSong       = true;
  gState.currentSongItemIdx = -1;

  initGmWavetables();

  os->tft_fill_screen(LAKATOS_BLACK);
  os->audio_start();
  os->set_rgb_led(true, false, false, 0);

  // Choose initial directory (/sd/midi -> /net/midi -> /sd -> /net)
  const char* candidateDirs[4] = {"/sd/midi", "/net/midi", "/sd", "/net"};
  for (int i = 0; i < 4; i++) {
    loadDirectory(candidateDirs[i]);
    if (gState.itemCount > 0) break;
  }
  if (gState.curDir[0] == '\0') {
    loadDirectory("/sd");
  }

  bool startedSong = playAdjacentSong(1);
  gState.inBrowserView = !startedSong;
  drawFullScreen();

  uint32_t lastUiDrawMs = 0;
  bool touchHeld = false;
  bool draggingVolSlider = false;
  int savedVolBeforeMute = 7;

  for (;;) {
    while (os->serial_available() > 0) {
      int ch = os->serial_read();
      if (ch == 'q' || ch == 3) {
        freeCurrentMidiResources();
        os->audio_stop();
        os->set_rgb_led(false, false, false, 0);
        os->exit(0);
      }
    }

    // Refill any paged MIDI track windows on Core 1
    serviceTrackWindowsCore1();

    if (gState.songEndedFlag) {
      gState.songEndedFlag = false;
      if (playAdjacentSong(1)) {
        if (!gState.inBrowserView) {
          drawFullScreen();
        } else {
          drawTopHeader();
          drawBrowserView();
        }
      }
    }

    uint32_t nowMs = os->millis();
    if (!gState.inBrowserView && gState.songLoaded && (uint32_t)(nowMs - lastUiDrawMs) >= 60u) {
      lastUiDrawMs = nowMs;
      drawMidiStatusBar();
      drawMidiChannelsGrid(false);
    }

    LakatosTouchSample ts = os->read_touch();
    if (ts.touched) {
      int tx = ts.x;
      int ty = ts.y;

      if (!touchHeld && !gState.inBrowserView && ty >= 174 && ty < 208) {
        touchHeld = true;
        draggingVolSlider = true;
      }

      if (draggingVolSlider && !gState.inBrowserView) {
        int newVol = (tx < 50) ? 0 : ((tx - 62 + 11) / 22);
        if (newVol < 0) newVol = 0;
        if (newVol > 11) newVol = 11;
        if (newVol != gState.masterVolIdx) {
          gState.masterVolIdx = newVol;
          if (newVol > 0) savedVolBeforeMute = newVol;
          applyMasterVolumeImmediate();
          drawVolumeSliderBar();
          drawTopHeader();
        }
      } else if (!touchHeld) {
        touchHeld = true;

        // 1. Top Header Bar (y < 28)
        if (ty < 28) {
          if (tx <= 50) {
            freeCurrentMidiResources();
            os->audio_stop();
            os->set_rgb_led(false, false, false, 0);
            os->exit(0);
          } else if (tx >= 204 && tx <= 262) {
            gState.inBrowserView = !gState.inBrowserView;
            drawFullScreen();
          } else if (tx >= 263) {
            if (gState.masterVolIdx > 0) {
              savedVolBeforeMute = gState.masterVolIdx;
              gState.masterVolIdx = 0;
            } else {
              gState.masterVolIdx = (savedVolBeforeMute > 0) ? savedVolBeforeMute : 7;
            }
            applyMasterVolumeImmediate();
            drawTopHeader();
            if (!gState.inBrowserView) {
              drawVolumeSliderBar();
            }
          }
        } else if (gState.inBrowserView) {
          // 2. Browser View Touch Handling
          if (ty >= 28 && ty <= 54) {
            if (tx < 64) {
              loadDirectory("/sd");
              gState.currentSongItemIdx = -1;
              drawBrowserView();
            } else if (tx < 126) {
              loadDirectory("/net");
              gState.currentSongItemIdx = -1;
              drawBrowserView();
            } else if (tx < 194) {
              loadDirectory("/flash");
              gState.currentSongItemIdx = -1;
              drawBrowserView();
            } else if (tx <= 256) {
              navigateParentDir();
              gState.currentSongItemIdx = -1;
              drawBrowserView();
            }
          } else if (ty >= 70 && tx >= 270) {
            if (ty < 150) {
              if (gState.scrollTop >= 8) gState.scrollTop -= 8;
              else gState.scrollTop = 0;
              drawBrowserView();
            } else {
              if (gState.scrollTop + 8 < gState.itemCount) gState.scrollTop += 8;
              drawBrowserView();
            }
          } else if (ty >= 70 && ty < 230 && tx < 268) {
            int rowIdx = (ty - 70) / 20;
            int itemIdx = gState.scrollTop + rowIdx;
            if (itemIdx >= 0 && itemIdx < gState.itemCount) {
              const BrowserItem& it = gState.items[itemIdx];
              char nextPath[192];
              if (strcmp(gState.curDir, "/") == 0) {
                snprintf(nextPath, sizeof(nextPath), "/%s", it.name);
              } else {
                snprintf(nextPath, sizeof(nextPath), "%s/%s", gState.curDir, it.name);
              }
              if (it.isDir) {
                loadDirectory(nextPath);
                gState.currentSongItemIdx = -1;
                drawBrowserView();
              } else {
                os->tft_fill_rect(50, 0, 154, 28, 0x10A2);
                os->tft_set_text_datum(LAKATOS_ML_DATUM);
                os->tft_set_text_color(LAKATOS_YELLOW, 0x10A2);
                os->tft_draw_string("LOADING...", 52, 14, 2);
                os->tft_set_text_datum(LAKATOS_TL_DATUM);

                if (loadAndPlayMidiFile(nextPath, it.name)) {
                  gState.currentSongItemIdx = itemIdx;
                  gState.inBrowserView = false;
                  drawFullScreen();
                } else {
                  drawTopHeader();
                  drawBrowserView();
                }
              }
            }
          }
        } else {
          // 3. Synth View Touch Handling
          if (ty >= 51 && ty < 171) {
            // Tap channel row to toggle mute on MIDI channel 1..16
            int col = (tx >= 160) ? 1 : 0;
            int row = (ty - 51) / 15;
            if (row >= 0 && row < 8) {
              int chIdx = col * 8 + row;
              gState.chans[chIdx].muted = !gState.chans[chIdx].muted;
              if (gState.chans[chIdx].muted) {
                for (int v = 0; v < kMaxVoices; v++) {
                  if (gState.voices[v].midiCh == chIdx) {
                    os->audio_set_voice(v, nullptr, LAKATOS_WAVE_NONE, 0, 0, 0, 0, false);
                  }
                }
              }
              drawMidiChannelsGrid(false);
            }
          } else if (ty >= 208) {
            if (tx < 74) {
              if (playAdjacentSong(-1)) {
                drawFullScreen();
              }
            } else if (tx < 162) {
              gState.playing = !gState.playing;
              if (!gState.playing) {
                for (int v = 0; v < kMaxVoices; v++) {
                  os->audio_set_voice(v, nullptr, LAKATOS_WAVE_NONE, 0, 0, 0, 0, false);
                }
              }
              drawTransportBar();
            } else if (tx < 234) {
              if (playAdjacentSong(1)) {
                drawFullScreen();
              }
            } else {
              gState.autoNextSong = !gState.autoNextSong;
              drawTransportBar();
            }
          }
        }
      }
    } else {
      touchHeld = false;
      draggingVolSlider = false;
    }

    os->delay_ms(12);
  }
}
