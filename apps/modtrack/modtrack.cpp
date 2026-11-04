#include "lakatos_api.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Amiga ProTracker Period Table (16 Finetunes x 36 Notes: C-1 to B-3)
// Finetune index: 0..7 = 0..+7, 8..15 = -8..-1
// ---------------------------------------------------------------------------
static const uint16_t kPeriodTable[16][36] = {
    // Tuning 0 (Normal)
    {856,808,762,720,678,640,604,570,538,508,480,453,
     428,404,381,360,339,320,302,285,269,254,240,226,
     214,202,190,180,170,160,151,143,135,127,120,113},
    // Tuning +1
    {850,802,757,715,674,637,601,567,535,505,477,450,
     425,401,379,357,337,318,300,284,268,253,239,225,
     213,201,189,179,169,159,150,142,134,126,119,113},
    // Tuning +2
    {844,796,752,709,670,632,597,563,532,502,474,447,
     422,398,376,355,335,316,298,282,266,251,237,224,
     211,199,188,177,167,158,149,141,133,125,118,112},
    // Tuning +3
    {838,791,746,704,665,628,592,559,528,498,470,444,
     419,395,373,352,332,314,296,280,264,249,235,222,
     209,198,187,176,166,157,148,140,132,125,118,111},
    // Tuning +4
    {832,785,741,699,660,623,588,555,524,495,467,441,
     416,392,370,350,330,312,294,278,262,247,233,220,
     208,196,185,175,165,156,147,139,131,124,117,110},
    // Tuning +5
    {826,779,736,694,655,619,584,551,520,491,463,437,
     413,390,368,347,328,309,292,276,260,245,232,219,
     206,195,184,174,164,155,146,138,130,123,116,109},
    // Tuning +6
    {820,774,730,689,651,614,580,547,516,487,460,434,
     410,387,365,345,325,307,290,274,258,244,230,217,
     205,193,183,172,163,154,145,137,129,122,115,109},
    // Tuning +7
    {814,768,725,684,646,610,575,543,513,484,457,431,
     407,384,363,342,323,305,288,272,256,242,228,216,
     204,192,181,171,161,152,144,136,128,121,114,108},
    // Tuning -8
    {907,856,808,762,720,678,640,604,570,538,508,480,
     453,428,404,381,360,339,320,302,285,269,254,240,
     226,214,202,190,180,170,160,151,143,135,127,120},
    // Tuning -7
    {900,850,802,757,715,675,636,601,567,535,505,477,
     450,425,401,379,357,337,318,300,284,268,253,238,
     225,212,200,189,179,169,159,150,142,134,126,119},
    // Tuning -6
    {894,844,796,752,709,670,632,597,563,532,502,474,
     447,422,398,376,355,335,316,298,282,266,251,237,
     223,211,199,188,177,167,158,149,141,133,125,118},
    // Tuning -5
    {887,838,791,746,704,665,628,592,559,528,498,470,
     444,419,395,373,352,332,314,296,280,264,249,235,
     222,209,198,187,176,166,157,148,140,132,125,118},
    // Tuning -4
    {881,832,785,741,699,660,623,588,555,524,494,467,
     441,416,392,370,350,330,312,294,278,262,247,233,
     220,208,196,185,175,165,156,147,139,131,123,117},
    // Tuning -3
    {875,826,779,736,694,655,619,584,551,520,491,463,
     437,413,390,368,347,328,309,292,276,260,245,232,
     219,206,195,184,174,164,155,146,138,130,123,116},
    // Tuning -2
    {868,820,774,730,689,651,614,580,547,516,487,460,
     434,410,387,365,345,325,307,290,274,258,244,230,
     217,205,193,183,172,163,154,145,137,129,122,115},
    // Tuning -1
    {862,814,768,725,684,646,610,575,543,513,484,457,
     431,407,384,363,342,323,305,288,272,256,242,228,
     216,203,192,181,171,161,152,144,136,128,121,114}
};

static const uint8_t kVibratoSine[32] = {
      0,  24,  49,  74,  97, 120, 141, 161,
    180, 197, 212, 224, 235, 244, 250, 253,
    255, 253, 250, 244, 235, 224, 212, 197,
    180, 161, 141, 120,  97,  74,  49,  24
};

static const char* const kNoteNames[36] = {
    "C-1", "C#1", "D-1", "D#1", "E-1", "F-1", "F#1", "G-1", "G#1", "A-1", "A#1", "B-1",
    "C-2", "C#2", "D-2", "D#2", "E-2", "F-2", "F#2", "G-2", "G#2", "A-2", "A#2", "B-2",
    "C-3", "C#3", "D-3", "D#3", "E-3", "F-3", "F#3", "G-3", "G#3", "A-3", "A#3", "B-3"
};

// ---------------------------------------------------------------------------
// Data Structures (Placed in 32 KB Static Arena 0x3FFC8000)
// ---------------------------------------------------------------------------
static constexpr uint32_t kStaticPatPoolSize = 16384u; // 16 KB static pattern pool in .bss
static constexpr int      kMaxTotalPatterns  = 64;
static constexpr int      kMaxBrowserItems   = 64;
static constexpr uint32_t kPcmHeapBudget     = 49152u; // 48 KB heap budget for PCM samples

struct ModInst {
  char     name[23];
  uint32_t rawLen;
  uint32_t rawLoopStart;
  uint32_t rawLoopLen;
  uint32_t storedLen;
  uint32_t storedLoopStart;
  uint32_t storedLoopLen;
  uint8_t  finetune;     // 0..15
  uint8_t  vol;          // 0..64
  uint8_t  decShift;     // 0..4 (1:1 .. 16:1 decimation)
  int8_t*  pcm;
};

struct ModChan {
  const int8_t* pcm;
  uint32_t      len;
  uint32_t      loopStart;
  uint32_t      loopLen;
  uint16_t      period;
  uint16_t      targetPeriod;
  int8_t        noteIdx;      // 0..35 or -1
  uint8_t       instIdx;      // 1..31
  uint8_t       finetune;
  uint8_t       decShift;
  int8_t        vol;          // 0..64
  int8_t        tremoloVol;
  uint8_t       effect;
  uint8_t       param;
  uint8_t       portaSpeed;
  uint8_t       vibSpeed;
  uint8_t       vibDepth;
  uint8_t       vibPos;
  uint8_t       tremSpeed;
  uint8_t       tremDepth;
  uint8_t       tremPos;
  uint8_t       sampleOffset;
  uint8_t       loopRow;
  uint8_t       loopCount;
  uint8_t       vuLevel;      // 0..64 for UI VU bar
  bool          muted;
};

struct BrowserItem {
  char name[44];
  bool isDir;
};

struct AppState {
  const LakatosApi* os;

  // Bitmask-packed pattern pool (16 KB in .bss + overflow on OS heap)
  uint8_t  staticPatPool[kStaticPatPoolSize];
  uint32_t staticPatPoolUsed;
  uint8_t* patternPtrs[kMaxTotalPatterns];

  // Parsed MOD Metadata & Instruments
  char     songTitle[24];
  char     currentFileName[48];
  ModInst  insts[32];     // 1..31 used
  uint8_t  orderTable[128];
  uint8_t  songLen;
  uint8_t  restartPos;
  uint8_t  numPatterns;
  uint8_t  numInstSlots;  // 31 or 15
  uint8_t  fileChans;     // Channels stored per row in file (4..32)
  uint8_t  numChans;      // Active played channels (4..8)
  uint32_t totalRawPcm;
  uint32_t totalStoredPcm;

  // Sequencer Runtime State (updated on Core 0 at 50 Hz)
  ModChan       chans[8];
  volatile int  curOrder;
  volatile int  curRow;
  volatile int  curTick;
  volatile int  speed;
  volatile int  bpm;
  volatile int  patDelayCount;
  volatile bool playing;
  volatile bool songLoaded;
  volatile bool songEndedFlag;
  bool          autoNextSong;
  int           masterVolIdx; // 0..11 ("goes to 11")

  // File Browser State
  bool        inBrowserView;
  char        curDir[128];
  BrowserItem items[kMaxBrowserItems];
  int         itemCount;
  int         scrollTop;
  int         currentSongItemIdx;

  // Streaming Loader Scratch State
  uint8_t     hdrBuf[1084];
  uint8_t     curRowBuf[32];
  uint8_t     curPatBuf[2112];
  uint16_t    curPatBufLen;
  uint8_t     curLoadPat;
  uint8_t     curLoadRow;
  uint16_t    curLoadByteInRow;
  uint32_t    streamPos;
  uint32_t    patEndOffset;
  int         curLoadInst;
  uint32_t    curInstRawBytePos;
  bool        headerParsed;
  bool        parseOk;
};

static AppState gState;
// Volume 0..11 ("goes to 11"): 0 = silent, 8 = normal 1.0x (64), 9..11 = overmodulated
static const uint8_t kMasterVolScale[12] = {
    0, 6, 12, 18, 26, 35, 44, 54, 64, 96, 144, 224
};

// ---------------------------------------------------------------------------
// Utility Functions
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

static bool hasModExtension(const char* name) {
  size_t n = strlen(name);
  if (n >= 4 && strCaseCmp(name + n - 4, ".mod") == 0) return true;
  if (n >= 8 && strCaseCmp(name + n - 8, ".mod.zip") == 0) return true;
  if (n >= 4 && strCaseCmp(name, "mod.") == 0) return true;
  return false;
}

static int findClosestNoteIdx(uint16_t period, uint8_t finetune) {
  if (period == 0) return -1;
  const uint16_t* tbl = kPeriodTable[finetune & 15u];
  int bestIdx = 0;
  int bestDiff = 99999;
  for (int i = 0; i < 36; i++) {
    int d = (int)period - (int)tbl[i];
    if (d < 0) d = -d;
    if (d < bestDiff) {
      bestDiff = d;
      bestIdx = i;
    }
  }
  return bestIdx;
}

static inline uint32_t periodToStepFp16(uint16_t period, uint8_t decShift) {
  if (period < 56) period = 56;
  // (3546895 Hz Amiga PAL clock << 16) / 15625 Hz = 14,876,755
  uint32_t step = 14876755u / (uint32_t)period;
  return step >> decShift;
}

static inline uint8_t scaleChannelVol(int chVol) {
  if (chVol <= 0) return 0;
  if (chVol > 64) chVol = 64;
  int vIdx = gState.masterVolIdx;
  if (vIdx <= 0) return 0;
  if (vIdx > 11) vIdx = 11;
  uint32_t scaled = ((uint32_t)chVol * kMasterVolScale[vIdx]) >> 6;
  return (scaled > 255u) ? 255u : (uint8_t)scaled;
}

static void updateVoiceOutput(int chIdx, uint16_t period, int vol, bool resetPos, uint32_t byteOffset = 0) {
  ModChan& ch = gState.chans[chIdx];
  if (ch.muted || !ch.pcm || ch.len == 0 || period == 0) {
    gState.os->audio_set_voice(chIdx, ch.pcm, ch.len, ch.loopStart, ch.loopLen,
                               periodToStepFp16(period ? period : 428, ch.decShift),
                               0, resetPos);
    return;
  }
  const int8_t* pcmPtr = ch.pcm;
  uint32_t len         = ch.len;
  uint32_t lStart      = ch.loopStart;
  uint32_t lLen        = ch.loopLen;
  if (resetPos && byteOffset > 0 && byteOffset < len) {
    pcmPtr += byteOffset;
    len    -= byteOffset;
    lStart  = (lStart > byteOffset) ? (lStart - byteOffset) : 0;
  }
  uint32_t step = periodToStepFp16(period, ch.decShift);
  uint8_t  outVol = scaleChannelVol(vol);
  gState.os->audio_set_voice(chIdx, pcmPtr, len, lStart, lLen, step, outVol, resetPos);
}

static void applyMasterVolumeImmediate() {
  if (!gState.songLoaded || !gState.playing) return;
  int nCh = (gState.numChans > 0 && gState.numChans <= 8) ? gState.numChans : 4;
  for (int c = 0; c < nCh; c++) {
    updateVoiceOutput(c, gState.chans[c].period, gState.chans[c].vol, false);
  }
}

static bool unpackPatternRow(uint8_t patIdx, int row, uint8_t outRow[8][4]) {
  memset(outRow, 0, 32);
  if (patIdx >= kMaxTotalPatterns || row < 0 || row >= 64) return false;
  const uint8_t* p = gState.patternPtrs[patIdx];
  if (!p) return false;
  for (int r = 0; r < row; r++) {
    uint8_t m = *p++;
    while (m) {
      m &= (uint8_t)(m - 1u);
      p += 4;
    }
  }
  uint8_t mask = *p++;
  int nCh = (gState.numChans > 0 && gState.numChans <= 8) ? gState.numChans : 4;
  for (int c = 0; c < nCh; c++) {
    if (mask & (uint8_t)(1u << c)) {
      memcpy(outRow[c], p, 4);
      p += 4;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Core 0 50 Hz ProTracker Sequencer & Effect Engine
// ---------------------------------------------------------------------------
static void triggerRowChannel(int c, const uint8_t* cell) {
  ModChan& ch = gState.chans[c];
  uint8_t  instNum   = (cell[0] & 0xF0u) | ((cell[2] >> 4) & 0x0Fu);
  uint16_t rawPeriod = ((uint16_t)(cell[0] & 0x0Fu) << 8) | cell[1];
  uint8_t  effect    = cell[2] & 0x0Fu;
  uint8_t  param     = cell[3];

  ch.effect = effect;
  ch.param  = param;

  if (instNum >= 1 && instNum <= 31) {
    const ModInst& inst = gState.insts[instNum];
    ch.instIdx   = instNum;
    ch.vol       = (int8_t)inst.vol;
    ch.finetune  = inst.finetune;
    if (rawPeriod > 0 || ch.pcm == nullptr) {
      ch.decShift  = inst.decShift;
      ch.pcm       = inst.pcm;
      ch.len       = inst.storedLen;
      ch.loopStart = inst.storedLoopStart;
      ch.loopLen   = inst.storedLoopLen;
    }
  }

  int noteIdx = -1;
  uint16_t tunedPeriod = rawPeriod;
  if (rawPeriod > 0) {
    noteIdx = findClosestNoteIdx(rawPeriod, 0);
    if (noteIdx >= 0 && noteIdx < 36) {
      tunedPeriod = kPeriodTable[ch.finetune & 15u][noteIdx];
    }
    ch.noteIdx = (int8_t)noteIdx;
  }

  // Extended E9x..EFx handling on Tick 0
  if (effect == 0x0E) {
    uint8_t sub = param >> 4;
    uint8_t val = param & 0x0Fu;
    if (sub == 0x0D && val > 0) {
      // EDx Note Delay: wait until tick == val to trigger
      ch.targetPeriod = tunedPeriod;
      return;
    }
    if (sub == 0x01) { // E1x Fine Porta Up
      if (ch.period > val + 113) ch.period -= val;
      else ch.period = 113;
    } else if (sub == 0x02) { // E2x Fine Porta Down
      ch.period += val;
      if (ch.period > 856) ch.period = 856;
    } else if (sub == 0x05) { // E5x Set Finetune
      ch.finetune = val;
    } else if (sub == 0x0A) { // EAx Fine Vol Slide Up
      ch.vol += val;
      if (ch.vol > 64) ch.vol = 64;
    } else if (sub == 0x0B) { // EBx Fine Vol Slide Down
      ch.vol -= val;
      if (ch.vol < 0) ch.vol = 0;
    } else if (sub == 0x0C && val == 0) { // EC0 Immediate Note Cut
      ch.vol = 0;
    }
  }

  if (effect == 0x0C) { // Cxx Set Volume
    ch.vol = (param > 64) ? 64 : (int8_t)param;
  } else if (effect == 0x09) { // 9xx Sample Offset
    if (param > 0) ch.sampleOffset = param;
  }

  bool isTonePorta = (effect == 0x03 || effect == 0x05);
  if (effect == 0x03 && param > 0) {
    ch.portaSpeed = param;
  }
  if (effect == 0x04 || effect == 0x06) {
    if ((param >> 4) > 0) ch.vibSpeed = param >> 4;
    if ((param & 0x0F) > 0) ch.vibDepth = param & 0x0F;
  }
  if (effect == 0x07) {
    if ((param >> 4) > 0) ch.tremSpeed = param >> 4;
    if ((param & 0x0F) > 0) ch.tremDepth = param & 0x0F;
  }

  uint32_t offsetBytes = 0;
  if (effect == 0x09) {
    offsetBytes = ((uint32_t)ch.sampleOffset * 256u) >> ch.decShift;
  }

  if (tunedPeriod > 0) {
    if (isTonePorta && ch.period > 0) {
      ch.targetPeriod = tunedPeriod;
      updateVoiceOutput(c, ch.period, ch.vol, false);
    } else {
      ch.period       = tunedPeriod;
      ch.targetPeriod = tunedPeriod;
      ch.vibPos       = 0;
      ch.tremPos      = 0;
      ch.vuLevel      = (uint8_t)ch.vol;
      updateVoiceOutput(c, ch.period, ch.vol, true, offsetBytes);
    }
  } else {
    updateVoiceOutput(c, ch.period, ch.vol, false);
  }
}

static void updateTickEffects(int c, int tick) {
  ModChan& ch = gState.chans[c];
  uint8_t effect = ch.effect;
  uint8_t param  = ch.param;
  uint16_t outPeriod = ch.period;
  int outVol = ch.vol;

  auto doVolSlide = [&](uint8_t p) {
    uint8_t up = p >> 4;
    uint8_t dn = p & 0x0Fu;
    if (up > 0) {
      ch.vol += up;
      if (ch.vol > 64) ch.vol = 64;
    } else if (dn > 0) {
      ch.vol -= dn;
      if (ch.vol < 0) ch.vol = 0;
    }
    outVol = ch.vol;
  };

  auto doTonePorta = [&]() {
    if (ch.period == 0 || ch.targetPeriod == 0) return;
    if (ch.period < ch.targetPeriod) {
      ch.period += ch.portaSpeed;
      if (ch.period > ch.targetPeriod) ch.period = ch.targetPeriod;
    } else if (ch.period > ch.targetPeriod) {
      if (ch.period > ch.portaSpeed + ch.targetPeriod) ch.period -= ch.portaSpeed;
      else ch.period = ch.targetPeriod;
    }
    outPeriod = ch.period;
  };

  auto doVibrato = [&]() {
    uint8_t idx = ch.vibPos & 31u;
    int delta = ((int)kVibratoSine[idx] * (int)ch.vibDepth) >> 7;
    if (ch.vibPos & 32u) outPeriod = (ch.period > delta + 56) ? (ch.period - delta) : 56;
    else outPeriod = ch.period + delta;
    ch.vibPos = (ch.vibPos + ch.vibSpeed) & 63u;
  };

  switch (effect) {
    case 0x00: { // 0xy Arpeggio
      if (param != 0 && ch.noteIdx >= 0) {
        int m = tick % 3;
        int semi = (m == 1) ? (param >> 4) : ((m == 2) ? (param & 0x0Fu) : 0);
        int idx = ch.noteIdx + semi;
        if (idx >= 0 && idx < 36) {
          outPeriod = kPeriodTable[ch.finetune & 15u][idx];
        }
      }
      break;
    }
    case 0x01: // 1xx Porta Up
      if (ch.period > param + 113) ch.period -= param;
      else ch.period = 113;
      outPeriod = ch.period;
      break;
    case 0x02: // 2xx Porta Down
      ch.period += param;
      if (ch.period > 856) ch.period = 856;
      outPeriod = ch.period;
      break;
    case 0x03: // 3xx Tone Portamento
      doTonePorta();
      break;
    case 0x04: // 4xy Vibrato
      doVibrato();
      break;
    case 0x05: // 5xy Tone Porta + Vol Slide
      doTonePorta();
      doVolSlide(param);
      break;
    case 0x06: // 6xy Vibrato + Vol Slide
      doVibrato();
      doVolSlide(param);
      break;
    case 0x07: { // 7xy Tremolo
      uint8_t idx = ch.tremPos & 31u;
      int delta = ((int)kVibratoSine[idx] * (int)ch.tremDepth) >> 6;
      outVol = (ch.tremPos & 32u) ? (ch.vol - delta) : (ch.vol + delta);
      if (outVol < 0) outVol = 0;
      else if (outVol > 64) outVol = 64;
      ch.tremPos = (ch.tremPos + ch.tremSpeed) & 63u;
      break;
    }
    case 0x0A: // Axy Volume Slide
      doVolSlide(param);
      break;
    case 0x0E: {
      uint8_t sub = param >> 4;
      uint8_t val = param & 0x0Fu;
      if (sub == 0x09 && val > 0 && (tick % val) == 0) {
        // E9x Retrig Note
        updateVoiceOutput(c, outPeriod, outVol, true);
        return;
      } else if (sub == 0x0C && tick == val) {
        // ECx Note Cut
        ch.vol = 0;
        outVol = 0;
      } else if (sub == 0x0D && tick == val && ch.targetPeriod > 0) {
        // EDx Note Delay
        ch.period  = ch.targetPeriod;
        outPeriod  = ch.period;
        ch.vuLevel = (uint8_t)ch.vol;
        updateVoiceOutput(c, outPeriod, outVol, true);
        return;
      }
      break;
    }
    default:
      break;
  }

  updateVoiceOutput(c, outPeriod, outVol, false);
}

static void modTickCb(void* user) {
  (void)user;
  if (!gState.playing || !gState.songLoaded) return;

  int nCh = (gState.numChans > 0 && gState.numChans <= 8) ? gState.numChans : 4;

  // Decay VU meters slightly every 20 ms tick
  for (int c = 0; c < nCh; c++) {
    if (gState.chans[c].vuLevel > 2) gState.chans[c].vuLevel -= 2;
    else gState.chans[c].vuLevel = 0;
  }

  int tick = gState.curTick;
  if (tick == 0) {
    if (gState.patDelayCount > 0) {
      gState.patDelayCount--;
    } else {
      int ord = gState.curOrder;
      int row = gState.curRow;
      if (ord < 0 || ord >= gState.songLen) ord = 0;
      uint8_t patIdx = gState.orderTable[ord];

      int jumpOrder = -1;
      int breakRow  = -1;

      uint8_t rowCells[8][4];
      if (unpackPatternRow(patIdx, row, rowCells)) {
        for (int c = 0; c < nCh; c++) {
          const uint8_t* cell = rowCells[c];
          triggerRowChannel(c, cell);

          uint8_t effect = cell[2] & 0x0Fu;
          uint8_t param  = cell[3];
          if (effect == 0x0B) { // Bxx Position Jump
            jumpOrder = param;
            if (breakRow < 0) breakRow = 0;
          } else if (effect == 0x0D) { // Dxx Pattern Break (BCD)
            breakRow = ((param >> 4) * 10) + (param & 0x0Fu);
            if (breakRow >= 64) breakRow = 0;
          } else if (effect == 0x0F) { // Fxx Set Speed / BPM
            if (param > 0 && param < 0x20) {
              gState.speed = param;
            } else if (param >= 0x20) {
              gState.bpm = param;
              uint32_t tickSamples = (LAKATOS_AUDIO_RATE * 5u) / (2u * (uint32_t)param);
              gState.os->audio_set_tick_cb(modTickCb, nullptr, tickSamples);
            }
          } else if (effect == 0x0E) {
            uint8_t sub = param >> 4;
            uint8_t val = param & 0x0Fu;
            if (sub == 0x06) { // E6x Pattern Loop
              ModChan& ch = gState.chans[c];
              if (val == 0) {
                ch.loopRow = (uint8_t)row;
              } else {
                if (ch.loopCount == 0) {
                  ch.loopCount = val;
                  breakRow = ch.loopRow;
                  jumpOrder = ord;
                } else {
                  ch.loopCount--;
                  if (ch.loopCount > 0) {
                    breakRow = ch.loopRow;
                    jumpOrder = ord;
                  }
                }
              }
            } else if (sub == 0x0E) { // EEx Pattern Delay
              gState.patDelayCount = val;
            }
          }
        }
      }

      // Compute next row/order for after this row's ticks complete
      if (jumpOrder >= 0 || breakRow >= 0) {
        int nextOrd = (jumpOrder >= 0) ? jumpOrder : (ord + 1);
        int nextRow = (breakRow >= 0) ? breakRow : 0;
        if (nextOrd >= gState.songLen || (jumpOrder >= 0 && jumpOrder <= ord && gState.autoNextSong)) {
          if (gState.autoNextSong) {
            gState.songEndedFlag = true;
          }
          nextOrd = (gState.restartPos < gState.songLen) ? gState.restartPos : 0;
        }
        // Encode pending jump in negative curRow (-1000 - (nextOrd * 64 + nextRow))
        gState.curRow = -1000 - (nextOrd * 64 + nextRow);
      }
    }
  } else {
    for (int c = 0; c < nCh; c++) {
      updateTickEffects(c, tick);
    }
  }

  tick++;
  if (tick >= gState.speed) {
    tick = 0;
    if (gState.patDelayCount == 0) {
      int r = gState.curRow;
      if (r <= -1000) {
        int packed  = -(r + 1000);
        gState.curOrder = packed >> 6;
        gState.curRow   = packed & 63;
      } else {
        r++;
        if (r >= 64) {
          r = 0;
          int nextOrd = gState.curOrder + 1;
          if (nextOrd >= gState.songLen) {
            if (gState.autoNextSong) {
              gState.songEndedFlag = true;
            }
            nextOrd = (gState.restartPos < gState.songLen) ? gState.restartPos : 0;
          }
          gState.curOrder = nextOrd;
        }
        gState.curRow = r;
      }
    }
  }
  gState.curTick = tick;
}

static inline int getDisplayRow() {
  int r = gState.curRow;
  if (r <= -1000) return 0;
  return (r >= 0 && r < 64) ? r : 0;
}

// ---------------------------------------------------------------------------
// Streaming .MOD / .MOD.ZIP Loader
// ---------------------------------------------------------------------------
static void freeCurrentSongResources() {
  gState.playing = false;
  gState.songLoaded = false;
  gState.os->audio_set_tick_cb(nullptr, nullptr, 0);
  for (int c = 0; c < 8; c++) {
    gState.os->audio_set_voice(c, nullptr, 0, 0, 0, 0, 0, true);
  }
  gState.os->delay_ms(3);
  for (int i = 1; i <= 31; i++) {
    if (gState.insts[i].pcm) {
      gState.os->free(gState.insts[i].pcm);
      gState.insts[i].pcm = nullptr;
    }
  }
  const uint8_t* poolStart = gState.staticPatPool;
  const uint8_t* poolEnd   = gState.staticPatPool + kStaticPatPoolSize;
  for (int p = 0; p < kMaxTotalPatterns; p++) {
    uint8_t* ptr = gState.patternPtrs[p];
    if (ptr != nullptr) {
      if (ptr < poolStart || ptr >= poolEnd) {
        gState.os->free(ptr);
      }
      gState.patternPtrs[p] = nullptr;
    }
  }
  gState.staticPatPoolUsed = 0;
}

static void detectModFormat(const uint8_t m[4], uint8_t* outFileChans, bool* outIs31) {
  if (memcmp(m, "M.K.", 4) == 0 || memcmp(m, "M!K!", 4) == 0 ||
      memcmp(m, "FLT4", 4) == 0 || memcmp(m, "4CHN", 4) == 0 ||
      memcmp(m, "N.T.", 4) == 0 || memcmp(m, "PATT", 4) == 0 ||
      memcmp(m, "EXO4", 4) == 0) {
    *outFileChans = 4;
    *outIs31      = true;
    return;
  }
  if (memcmp(m, "6CHN", 4) == 0 || memcmp(m, "CD61", 4) == 0 ||
      memcmp(m, "FA06", 4) == 0) {
    *outFileChans = 6;
    *outIs31      = true;
    return;
  }
  if (memcmp(m, "8CHN", 4) == 0 || memcmp(m, "FLT8", 4) == 0 ||
      memcmp(m, "CD81", 4) == 0 || memcmp(m, "OCTA", 4) == 0 ||
      memcmp(m, "OKTA", 4) == 0 || memcmp(m, "FA08", 4) == 0 ||
      memcmp(m, "EXO8", 4) == 0) {
    *outFileChans = 8;
    *outIs31      = true;
    return;
  }
  if (m[0] >= '1' && m[0] <= '9' && memcmp(m + 1, "CHN", 3) == 0) {
    *outFileChans = (uint8_t)(m[0] - '0');
    *outIs31      = true;
    return;
  }
  if (m[0] >= '0' && m[0] <= '9' && m[1] >= '0' && m[1] <= '9' &&
      (memcmp(m + 2, "CH", 2) == 0 || memcmp(m + 2, "CN", 2) == 0)) {
    int ch = (m[0] - '0') * 10 + (m[1] - '0');
    if (ch >= 1 && ch <= 32) {
      *outFileChans = (uint8_t)ch;
      *outIs31      = true;
      return;
    }
  }
  if (memcmp(m, "TDZ", 3) == 0 && m[3] >= '1' && m[3] <= '9') {
    *outFileChans = (uint8_t)(m[3] - '0');
    *outIs31      = true;
    return;
  }
  // Legacy 15-instrument 4-channel SoundTracker
  *outFileChans = 4;
  *outIs31      = false;
}

static bool parseModHeader() {
  const uint8_t* h = gState.hdrBuf;

  bool is31 = false;
  uint8_t fileChans = 4;
  detectModFormat(h + 1080, &fileChans, &is31);
  gState.fileChans    = fileChans;
  gState.numChans     = (fileChans > 8) ? 8 : fileChans;
  gState.numInstSlots = is31 ? 31 : 15;

  memcpy(gState.songTitle, h, 20);
  gState.songTitle[20] = '\0';
  for (int i = 0; i < 20; i++) {
    if ((uint8_t)gState.songTitle[i] < 32 || (uint8_t)gState.songTitle[i] > 126) {
      if (gState.songTitle[i] != '\0') gState.songTitle[i] = ' ';
    }
  }

  gState.totalRawPcm = 0;
  for (int i = 1; i <= 31; i++) {
    ModInst& inst = gState.insts[i];
    memset(&inst, 0, sizeof(inst));
    if (i > gState.numInstSlots) continue;

    const uint8_t* ih = h + 20 + (i - 1) * 30;
    memcpy(inst.name, ih, 22);
    inst.name[22] = '\0';
    for (int k = 0; k < 22; k++) {
      if ((uint8_t)inst.name[k] < 32 || (uint8_t)inst.name[k] > 126) {
        if (inst.name[k] != '\0') inst.name[k] = ' ';
      }
    }
    inst.rawLen       = ((uint32_t)((ih[22] << 8) | ih[23])) * 2u;
    inst.finetune     = ih[24] & 0x0Fu;
    inst.vol          = (ih[25] > 64) ? 64 : ih[25];
    uint32_t lStartW  = (uint32_t)((ih[26] << 8) | ih[27]);
    uint32_t lLenW    = (uint32_t)((ih[28] << 8) | ih[29]);
    inst.rawLoopStart = is31 ? (lStartW * 2u) : lStartW;
    inst.rawLoopLen   = lLenW * 2u;
    if (inst.rawLoopLen <= 2u || inst.rawLoopStart >= inst.rawLen) {
      inst.rawLoopStart = 0;
      inst.rawLoopLen   = 0;
    } else if (inst.rawLoopStart + inst.rawLoopLen > inst.rawLen) {
      inst.rawLoopLen = inst.rawLen - inst.rawLoopStart;
    }
    gState.totalRawPcm += inst.rawLen;
  }

  int ordOffset = is31 ? 950 : 470;
  gState.songLen    = h[ordOffset];
  gState.restartPos = h[ordOffset + 1];
  if (gState.songLen == 0 || gState.songLen > 128) return false;
  if (gState.restartPos >= gState.songLen) gState.restartPos = 0;

  memcpy(gState.orderTable, h + ordOffset + 2, 128);
  int maxPat = 0;
  for (int i = 0; i < 128; i++) {
    if (gState.orderTable[i] < kMaxTotalPatterns && gState.orderTable[i] > maxPat) {
      maxPat = gState.orderTable[i];
    }
  }
  gState.numPatterns = (uint8_t)(maxPat + 1);

  // Pick decimation shift (0..7) for long samples (> 512 B) so total stored PCM fits budget
  uint8_t chosenShift = 0;
  for (uint8_t sh = 0; sh <= 7; sh++) {
    uint32_t sum = 0;
    for (int i = 1; i <= gState.numInstSlots; i++) {
      uint32_t rl = gState.insts[i].rawLen;
      if (rl == 0) continue;
      uint8_t s = (rl <= 512u) ? 0u : sh;
      uint32_t sl = rl >> s;
      if (sl < 4u) sl = 4u;
      sum += sl;
    }
    chosenShift = sh;
    if (sum <= kPcmHeapBudget) break;
  }

  gState.totalStoredPcm = 0;
  for (int i = 1; i <= gState.numInstSlots; i++) {
    ModInst& inst = gState.insts[i];
    if (inst.rawLen == 0) continue;
    inst.decShift = (inst.rawLen <= 512u) ? 0u : chosenShift;
    inst.storedLen = inst.rawLen >> inst.decShift;
    if (inst.storedLen < 2u) inst.storedLen = 2u;
    if (inst.rawLoopLen > 2u) {
      inst.storedLoopStart = inst.rawLoopStart >> inst.decShift;
      inst.storedLoopLen   = inst.rawLoopLen >> inst.decShift;
      if (inst.storedLoopLen < 2u) inst.storedLoopLen = 2u;
      if (inst.storedLoopStart + inst.storedLoopLen > inst.storedLen) {
        if (inst.storedLen >= 2u) {
          inst.storedLoopStart = inst.storedLen - 2u;
          inst.storedLoopLen   = 2u;
        }
      }
    }
    gState.totalStoredPcm += inst.storedLen;
  }

  gState.staticPatPoolUsed = 0;
  for (int p = 0; p < kMaxTotalPatterns; p++) {
    gState.patternPtrs[p] = nullptr;
  }
  gState.curLoadPat       = 0;
  gState.curLoadRow       = 0;
  gState.curLoadByteInRow = 0;
  gState.curPatBufLen     = 0;
  memset(gState.curRowBuf, 0, sizeof(gState.curRowBuf));

  uint32_t hdrLen = is31 ? 1084u : 600u;
  uint32_t filePatSize = 64u * (uint32_t)gState.fileChans * 4u;
  gState.patEndOffset = hdrLen + (uint32_t)gState.numPatterns * filePatSize;
  gState.curLoadInst = 1;
  gState.curInstRawBytePos = 0;
  while (gState.curLoadInst <= gState.numInstSlots &&
         gState.insts[gState.curLoadInst].rawLen == 0) {
    gState.curLoadInst++;
  }
  return true;
}

static bool feedPostHeaderByte(uint8_t b, uint32_t fileOffset) {
  uint32_t hdrLen = (gState.numInstSlots == 31) ? 1084u : 600u;
  if (fileOffset < hdrLen) return true;

  // 1. Pattern Data Region (packed per row with a 1-byte channel bitmask)
  if (fileOffset < gState.patEndOffset) {
    uint16_t activeRowBytes = (uint16_t)gState.numChans * 4u;
    uint16_t fileRowBytes   = (uint16_t)gState.fileChans * 4u;
    if (gState.curLoadByteInRow < activeRowBytes) {
      gState.curRowBuf[gState.curLoadByteInRow] = b;
    }
    gState.curLoadByteInRow++;
    if (gState.curLoadByteInRow >= fileRowBytes) {
      uint8_t mask = 0;
      for (int c = 0; c < gState.numChans; c++) {
        const uint8_t* cell = &gState.curRowBuf[c * 4];
        if (cell[0] | cell[1] | cell[2] | cell[3]) {
          mask |= (uint8_t)(1u << c);
        }
      }
      if (gState.curPatBufLen + 1u + (uint16_t)(gState.numChans * 4u) <= sizeof(gState.curPatBuf)) {
        gState.curPatBuf[gState.curPatBufLen++] = mask;
        for (int c = 0; c < gState.numChans; c++) {
          if (mask & (uint8_t)(1u << c)) {
            memcpy(&gState.curPatBuf[gState.curPatBufLen], &gState.curRowBuf[c * 4], 4);
            gState.curPatBufLen += 4;
          }
        }
      }
      gState.curLoadByteInRow = 0;
      memset(gState.curRowBuf, 0, sizeof(gState.curRowBuf));
      gState.curLoadRow++;
      if (gState.curLoadRow >= 64) {
        if (gState.curLoadPat < kMaxTotalPatterns && gState.curPatBufLen > 0) {
          uint32_t sz = gState.curPatBufLen;
          uint8_t* dst = nullptr;
          if (gState.staticPatPoolUsed + sz <= kStaticPatPoolSize) {
            dst = &gState.staticPatPool[gState.staticPatPoolUsed];
            gState.staticPatPoolUsed += sz;
          } else {
            dst = (uint8_t*)gState.os->malloc(sz);
          }
          if (dst != nullptr) {
            memcpy(dst, gState.curPatBuf, sz);
            gState.patternPtrs[gState.curLoadPat] = dst;
          }
        }
        gState.curLoadRow   = 0;
        gState.curPatBufLen = 0;
        gState.curLoadPat++;
      }
    }
    return true;
  }

  // 2. Instrument PCM Sample Data Region
  while (gState.curLoadInst <= gState.numInstSlots &&
         gState.insts[gState.curLoadInst].rawLen == 0) {
    gState.curLoadInst++;
    gState.curInstRawBytePos = 0;
  }
  if (gState.curLoadInst > gState.numInstSlots) {
    return false; // All instruments loaded! Stop stream early if extra trailing bytes exist.
  }

  ModInst& inst = gState.insts[gState.curLoadInst];
  if (gState.curInstRawBytePos == 0) {
    inst.pcm = (int8_t*)gState.os->malloc(inst.storedLen);
    if (inst.pcm) {
      memset(inst.pcm, 0, inst.storedLen);
    }
  }

  uint32_t mask = (1u << inst.decShift) - 1u;
  if ((gState.curInstRawBytePos & mask) == 0 && inst.pcm != nullptr) {
    uint32_t dstIdx = gState.curInstRawBytePos >> inst.decShift;
    if (dstIdx < inst.storedLen) {
      // First 2 bytes of non-looped Amiga sample are cleared to silence
      if (dstIdx < 2u && inst.rawLoopLen == 0) {
        inst.pcm[dstIdx] = 0;
      } else {
        inst.pcm[dstIdx] = (int8_t)b;
      }
    }
  }

  gState.curInstRawBytePos++;
  if (gState.curInstRawBytePos >= inst.rawLen) {
    gState.curLoadInst++;
    gState.curInstRawBytePos = 0;
  }
  return true;
}

static bool onModStreamChunk(const uint8_t* data, size_t len, void* user) {
  (void)user;
  for (size_t i = 0; i < len; i++) {
    uint8_t b = data[i];
    uint32_t pos = gState.streamPos++;

    if (!gState.headerParsed) {
      if (pos < 1084u) {
        gState.hdrBuf[pos] = b;
      }
      if (pos == 1083u) {
        gState.headerParsed = true;
        if (!parseModHeader()) {
          gState.parseOk = false;
          return false;
        }
        // If 15-instrument SoundTracker (600-byte header), replay bytes 600..1083 into pattern 0!
        if (gState.numInstSlots == 15) {
          for (uint32_t k = 600u; k < 1084u; k++) {
            feedPostHeaderByte(gState.hdrBuf[k], k);
          }
        }
      }
    } else {
      if (!feedPostHeaderByte(b, pos)) {
        return false;
      }
    }
  }
  return true;
}

static bool loadAndPlayModFile(const char* fullPath, const char* displayName) {
  freeCurrentSongResources();

  strncpy(gState.currentFileName, displayName ? displayName : fullPath, sizeof(gState.currentFileName) - 1);
  gState.currentFileName[sizeof(gState.currentFileName) - 1] = '\0';

  gState.streamPos         = 0;
  gState.headerParsed      = false;
  gState.parseOk           = true;
  gState.curLoadInst       = 1;
  gState.curInstRawBytePos = 0;

  int32_t got = gState.os->fs_read_stream(fullPath, onModStreamChunk, nullptr);
  if (got < 600 || !gState.headerParsed || !gState.parseOk) {
    char lbuf[128];
    int n = snprintf(lbuf, sizeof(lbuf), "[MOD] load failed: %s (got=%ld)\n", fullPath, (long)got);
    if (n > 0) gState.os->serial_write(lbuf, (size_t)n);
    freeCurrentSongResources();
    return false;
  }

  {
    char lbuf[160];
    int n = snprintf(lbuf, sizeof(lbuf), "[MOD] playing: %s ['%s', %uch, %u orders, %u pats, %lu/%lu B PCM, heap=%lu]\n",
             gState.currentFileName, gState.songTitle,
             (unsigned)gState.numChans,
             (unsigned)gState.songLen, (unsigned)gState.numPatterns,
             (unsigned long)gState.totalStoredPcm, (unsigned long)gState.totalRawPcm,
             (unsigned long)gState.os->free_heap_bytes());
    if (n > 0) gState.os->serial_write(lbuf, (size_t)n);
  }

  // Initialize playback state
  for (int c = 0; c < 8; c++) {
    bool wasMuted = gState.chans[c].muted;
    memset(&gState.chans[c], 0, sizeof(ModChan));
    gState.chans[c].noteIdx = -1;
    gState.chans[c].muted   = wasMuted;
  }
  gState.curOrder      = 0;
  gState.curRow        = 0;
  gState.curTick       = 0;
  gState.speed         = 6;
  gState.bpm           = 125;
  gState.patDelayCount = 0;
  gState.songEndedFlag = false;
  gState.songLoaded    = true;
  gState.playing       = true;

  uint32_t tickSamples = (LAKATOS_AUDIO_RATE * 5u) / (2u * 125u); // 312 samples @ 125 BPM (50 Hz)
  gState.os->audio_set_tick_cb(modTickCb, nullptr, tickSamples);
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

  // Collect directories first, then .mod / .mod.zip files
  for (int pass = 0; pass < 2; pass++) {
    for (int i = 0; i < n && gState.itemCount < kMaxBrowserItems; i++) {
      const LakatosFsEntry& e = rawEntries[i];
      if (e.name[0] == '.' || e.name[0] == '\0') continue;
      if (pass == 0 && e.isDir) {
        BrowserItem& it = gState.items[gState.itemCount++];
        strncpy(it.name, e.name, sizeof(it.name) - 1);
        it.name[sizeof(it.name) - 1] = '\0';
        it.isDir = true;
      } else if (pass == 1 && !e.isDir && hasModExtension(e.name)) {
        BrowserItem& it = gState.items[gState.itemCount++];
        strncpy(it.name, e.name, sizeof(it.name) - 1);
        it.name[sizeof(it.name) - 1] = '\0';
        it.isDir = false;
      }
    }
  }

  // Simple insertion sort within directories and within files
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
      if (loadAndPlayModFile(fullPath, gState.items[idx].name)) {
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

  // [HOME] button
  os->tft_fill_round_rect(2, 2, 46, 24, 4, 0x8000);
  os->tft_draw_round_rect(2, 2, 46, 24, 4, LAKATOS_WHITE);
  os->tft_set_text_datum(LAKATOS_MC_DATUM);
  os->tft_set_text_color(LAKATOS_WHITE, 0x8000);
  os->tft_draw_string("HOME", 25, 14, 2);

  // Title / Song name
  os->tft_set_text_datum(LAKATOS_ML_DATUM);
  os->tft_set_text_color(LAKATOS_CYAN, 0x10A2);
  if (gState.songLoaded && gState.songTitle[0] != '\0') {
    os->tft_draw_string(gState.songTitle, 52, 14, 2);
  } else {
    os->tft_draw_string("MOD TRACKER", 52, 14, 2);
  }

  // [FILES] / [TRACK] view toggle button
  uint16_t viewBg = gState.inBrowserView ? 0x03E0 : 0x2124;
  os->tft_fill_round_rect(206, 2, 56, 24, 4, viewBg);
  os->tft_draw_round_rect(206, 2, 56, 24, 4, LAKATOS_CYAN);
  os->tft_set_text_datum(LAKATOS_MC_DATUM);
  os->tft_set_text_color(LAKATOS_WHITE, viewBg);
  os->tft_draw_string(gState.inBrowserView ? "TRACK" : "FILES", 234, 14, 2);

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

static void drawTrackerStatusBar() {
  const LakatosApi* os = gState.os;
  os->tft_fill_rect(0, 28, 320, 22, 0x0841);
  os->tft_draw_fast_h_line(0, 49, 320, 0x3186);

  char buf[64];
  if (gState.songLoaded) {
    int ord = gState.curOrder;
    int pat = (ord >= 0 && ord < 128) ? gState.orderTable[ord] : 0;
    int row = getDisplayRow();
    int nCh = (gState.numChans > 0) ? gState.numChans : 4;
    snprintf(buf, sizeof(buf), "Ord %02d/%02d Pat %02d Row %02X %dch %d/%d %luK",
             ord + 1, (int)gState.songLen, pat, row, nCh,
             gState.speed, gState.bpm,
             (unsigned long)((gState.totalStoredPcm + 512u) / 1024u));
  } else {
    snprintf(buf, sizeof(buf), "No MOD loaded - tap FILES to pick a song");
  }
  os->tft_set_text_datum(LAKATOS_ML_DATUM);
  os->tft_set_text_color(LAKATOS_YELLOW, 0x0841);
  os->tft_draw_string(buf, 6, 39, 1);
  os->tft_set_text_datum(LAKATOS_TL_DATUM);
}

static void drawChannelHeadersAndVu(bool drawFrame) {
  const LakatosApi* os = gState.os;
  int nCols = (gState.songLoaded && gState.numChans > 4) ? gState.numChans : 4;
  if (nCols > 8) nCols = 8;
  int colW = 320 / nCols;

  for (int c = 0; c < nCols; c++) {
    int x = c * colW;
    int w = (c == nCols - 1) ? (320 - x) : (colW - 1);
    const ModChan& ch = gState.chans[c];
    if (drawFrame) {
      uint16_t hdrBg = ch.muted ? 0x3800 : 0x18E3;
      os->tft_fill_rect(x, 50, w, 28, hdrBg);
      os->tft_draw_rect(x, 50, w, 28, ch.muted ? LAKATOS_RED : 0x4208);

      char hbuf[16];
      if (nCols <= 4) {
        if (ch.muted) {
          snprintf(hbuf, sizeof(hbuf), "CH%d MUTE", c + 1);
        } else {
          snprintf(hbuf, sizeof(hbuf), "CH%d I:%02u", c + 1, (unsigned)ch.instIdx);
        }
      } else {
        if (ch.muted) {
          snprintf(hbuf, sizeof(hbuf), "%d:OFF", c + 1);
        } else {
          snprintf(hbuf, sizeof(hbuf), "%d:%02u", c + 1, (unsigned)ch.instIdx);
        }
      }
      os->tft_set_text_datum(LAKATOS_TL_DATUM);
      os->tft_set_text_color(ch.muted ? LAKATOS_ORANGE : LAKATOS_CYAN, hdrBg);
      os->tft_draw_string(hbuf, x + (nCols <= 4 ? 4 : 3), 53, 1);
    } else {
      // Update instrument number text cleanly
      uint16_t hdrBg = ch.muted ? 0x3800 : 0x18E3;
      char ibuf[8];
      if (!ch.muted) {
        snprintf(ibuf, sizeof(ibuf), "%02u", (unsigned)ch.instIdx);
        os->tft_set_text_datum(LAKATOS_TL_DATUM);
        os->tft_set_text_color(LAKATOS_CYAN, hdrBg);
        os->tft_draw_string(ibuf, x + (nCols <= 4 ? 40 : 15), 53, 1);
      }
    }

    // VU meter bar (y = 66..74)
    int pad = (nCols <= 4) ? 4 : 3;
    int maxVuW = w - 2 * pad;
    if (maxVuW < 4) maxVuW = 4;
    int vuW = ch.muted ? 0 : ((int)ch.vuLevel * maxVuW) / 64;
    if (vuW > maxVuW) vuW = maxVuW;
    if (vuW > 0) {
      uint16_t vuCol = (vuW * 64 > maxVuW * 52) ? LAKATOS_ORANGE : LAKATOS_GREEN;
      os->tft_fill_rect(x + pad, 66, vuW, 8, vuCol);
    }
    if (vuW < maxVuW) {
      os->tft_fill_rect(x + pad + vuW, 66, maxVuW - vuW, 8, LAKATOS_BLACK);
    }
  }
}

static void formatPatternCell(const uint8_t* cell, bool compact, char out[12]) {
  uint8_t  instNum   = (cell[0] & 0xF0u) | ((cell[2] >> 4) & 0x0Fu);
  uint16_t rawPeriod = ((uint16_t)(cell[0] & 0x0Fu) << 8) | cell[1];
  uint8_t  effect    = cell[2] & 0x0Fu;
  uint8_t  param     = cell[3];

  const char* noteStr = "---";
  if (rawPeriod > 0) {
    int nIdx = findClosestNoteIdx(rawPeriod, 0);
    if (nIdx >= 0 && nIdx < 36) noteStr = kNoteNames[nIdx];
  }

  char i0 = (instNum > 0) ? (char)('0' + (instNum / 10) % 10) : '-';
  char i1 = (instNum > 0) ? (char)('0' + (instNum % 10)) : '-';

  if (compact) {
    snprintf(out, 12, "%s %c%c", noteStr, i0, i1);
    return;
  }

  const char* hex = "0123456789ABCDEF";
  char e0 = (effect != 0 || param != 0) ? hex[effect & 15] : '-';
  char e1 = (effect != 0 || param != 0) ? hex[(param >> 4) & 15] : '-';
  char e2 = (effect != 0 || param != 0) ? hex[param & 15] : '-';

  snprintf(out, 12, "%s %c%c %c%c%c", noteStr, i0, i1, e0, e1, e2);
}

static void drawPatternGrid() {
  const LakatosApi* os = gState.os;
  int ord = gState.curOrder;
  int centerRow = getDisplayRow();
  uint8_t patIdx = (gState.songLoaded && ord >= 0 && ord < 128) ? gState.orderTable[ord] : 0;
  int nCols = (gState.songLoaded && gState.numChans > 4) ? gState.numChans : 4;
  if (nCols > 8) nCols = 8;
  int colW = 320 / nCols;
  bool compact = (nCols > 4);

  os->tft_set_text_datum(LAKATOS_TL_DATUM);
  for (int dr = -3; dr <= 3; dr++) {
    int r = centerRow + dr;
    int y = 78 + (dr + 3) * 14;
    bool isCenter = (dr == 0);
    uint16_t rowBg = isCenter ? 0x2124 : LAKATOS_BLACK;
    os->tft_fill_rect(0, y, 320, 14, rowBg);

    uint8_t rowCells[8][4];
    if (!gState.songLoaded || !unpackPatternRow(patIdx, r, rowCells)) continue;

    for (int c = 0; c < nCols; c++) {
      int x = c * colW;
      char cbuf[12];
      formatPatternCell(rowCells[c], compact, cbuf);
      uint16_t fg = gState.chans[c].muted
                        ? 0x4208
                        : (isCenter ? LAKATOS_YELLOW : ((dr < 0) ? 0x7BEF : 0xBDF7));
      os->tft_set_text_color(fg, rowBg);
      os->tft_draw_string(cbuf, x + (compact ? 2 : 8), y + 3, 1);
      if (c > 0) {
        os->tft_draw_fast_v_line(x, y, 14, 0x2104);
      }
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

  // Left label: "VOL" + numeric 0..11
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

  // Slider geometry: x = 62 .. 304 (11 intervals of 22 px = 242 px)
  static constexpr int kTrackX0 = 62;
  static constexpr int kStepPx  = 22;
  static constexpr int kTrackW  = 11 * kStepPx; // 242

  // Scale labels & ticks along top of slider (y = 178..186)
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

  // Slider trough (y = 190..199)
  os->tft_fill_round_rect(kTrackX0 - 4, 190, kTrackW + 8, 10, 4, LAKATOS_BLACK);
  os->tft_draw_round_rect(kTrackX0 - 4, 190, kTrackW + 8, 10, 4, 0x4208);

  // Segmented color fill up to current volume
  if (vol > 0) {
    int cleanEnd = (vol <= 8) ? vol : 8;
    int cleanW = cleanEnd * kStepPx;
    os->tft_fill_rect(kTrackX0, 192, cleanW, 6, LAKATOS_GREEN);
    if (vol >= 9) {
      int warnEnd = (vol <= 10) ? vol : 10;
      int warnW = (warnEnd - 8) * kStepPx;
      os->tft_fill_rect(kTrackX0 + 8 * kStepPx, 192, warnW, 6, LAKATOS_ORANGE);
    }
    if (vol >= 11) {
      os->tft_fill_rect(kTrackX0 + 10 * kStepPx, 192, kStepPx, 6, LAKATOS_RED);
    }
  }

  // Unity gain (8) reference notch on trough
  os->tft_draw_fast_v_line(kTrackX0 + 8 * kStepPx, 189, 12, LAKATOS_WHITE);

  // Draggable thumb knob centered at thumbX
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

  // Root & Up navigation bar (y = 30..54)
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

  // Current directory path label (y = 54..68)
  os->tft_fill_rect(0, 54, 320, 14, 0x0841);
  os->tft_set_text_datum(LAKATOS_ML_DATUM);
  os->tft_set_text_color(LAKATOS_YELLOW, 0x0841);
  os->tft_draw_string(gState.curDir, 6, 61, 1);

  // File list (8 visible rows, y = 70..230, height 20 px each, width 268 px)
  static constexpr int kRowsPerPage = 8;
  os->tft_set_text_datum(LAKATOS_ML_DATUM);
  if (gState.itemCount == 0) {
    os->tft_set_text_color(0xBDF7, LAKATOS_BLACK);
    os->tft_draw_string("No .mod / .mod.zip files or folders here", 12, 110, 2);
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

  // Right-side Page Up / Page Down buttons (x = 272..318)
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
    drawTrackerStatusBar();
    drawChannelHeadersAndVu(true);
    drawPatternGrid();
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
  gState.numChans           = 4;
  gState.fileChans          = 4;
  gState.masterVolIdx       = 7; // Default level 7 on 0..11 scale (8 = unity, 9..11 = overdrive)
  gState.autoNextSong       = true;
  gState.currentSongItemIdx = -1;

  os->tft_fill_screen(LAKATOS_BLACK);
  os->audio_start();
  os->set_rgb_led(true, false, false, 0);

  // Choose initial directory (/sd/mods -> /net/mods -> /sd -> /net)
  const char* candidateDirs[4] = {"/sd/mods", "/net/mods", "/sd", "/net"};
  for (int i = 0; i < 4; i++) {
    loadDirectory(candidateDirs[i]);
    if (gState.itemCount > 0) break;
  }
  if (gState.curDir[0] == '\0') {
    loadDirectory("/sd");
  }

  // Auto-load the first available .mod / .mod.zip in the initial folder if present
  bool startedSong = playAdjacentSong(1);
  gState.inBrowserView = !startedSong;
  drawFullScreen();

  int lastDrawnOrd = -1;
  int lastDrawnRow = -1;
  uint32_t lastVuDrawMs = 0;
  bool touchHeld = false;
  bool draggingVolSlider = false;
  int savedVolBeforeMute = 7;

  for (;;) {
    while (os->serial_available() > 0) {
      int ch = os->serial_read();
      if (ch == 'q' || ch == 3) {
        freeCurrentSongResources();
        os->audio_stop();
        os->set_rgb_led(false, false, false, 0);
        os->exit(0);
      }
    }
    if (gState.songEndedFlag) {
      gState.songEndedFlag = false;
      if (playAdjacentSong(1)) {
        lastDrawnOrd = -1;
        lastDrawnRow = -1;
        if (!gState.inBrowserView) {
          drawFullScreen();
        } else {
          drawTopHeader();
          drawBrowserView();
        }
      }
    }

    uint32_t nowMs = os->millis();
    if (!gState.inBrowserView && gState.songLoaded) {
      int ord = gState.curOrder;
      int row = getDisplayRow();
      if (ord != lastDrawnOrd || row != lastDrawnRow) {
        lastDrawnOrd = ord;
        lastDrawnRow = row;
        drawTrackerStatusBar();
        drawPatternGrid();
      }
      if ((uint32_t)(nowMs - lastVuDrawMs) >= 35u) {
        lastVuDrawMs = nowMs;
        drawChannelHeadersAndVu(false);
      }
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
            freeCurrentSongResources();
            os->audio_stop();
            os->set_rgb_led(false, false, false, 0);
            os->exit(0);
          } else if (tx >= 204 && tx <= 262) {
            gState.inBrowserView = !gState.inBrowserView;
            lastDrawnOrd = -1;
            lastDrawnRow = -1;
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
                // Show loading feedback in top bar
                os->tft_fill_rect(50, 0, 154, 28, 0x10A2);
                os->tft_set_text_datum(LAKATOS_ML_DATUM);
                os->tft_set_text_color(LAKATOS_YELLOW, 0x10A2);
                os->tft_draw_string("LOADING...", 52, 14, 2);
                os->tft_set_text_datum(LAKATOS_TL_DATUM);

                if (loadAndPlayModFile(nextPath, it.name)) {
                  gState.currentSongItemIdx = itemIdx;
                  gState.inBrowserView = false;
                  lastDrawnOrd = -1;
                  lastDrawnRow = -1;
                  drawFullScreen();
                } else {
                  drawTopHeader();
                  drawBrowserView();
                }
              }
            }
          }
        } else {
          // 3. Tracker View Touch Handling
          if (ty >= 50 && ty < 78) {
            // Channel Header Mute Toggle (CH1..CH8)
            int nCols = (gState.songLoaded && gState.numChans > 4) ? gState.numChans : 4;
            if (nCols > 8) nCols = 8;
            int colW = 320 / nCols;
            int chIdx = tx / colW;
            if (chIdx >= 0 && chIdx < nCols) {
              gState.chans[chIdx].muted = !gState.chans[chIdx].muted;
              if (gState.chans[chIdx].muted) {
                os->audio_set_voice(chIdx, nullptr, 0, 0, 0, 0, 0, false);
              }
              drawChannelHeadersAndVu(true);
              drawPatternGrid();
            }
          } else if (ty >= 208) {
            // Bottom Transport Controls
            if (tx < 74) {
              if (playAdjacentSong(-1)) {
                lastDrawnOrd = -1;
                lastDrawnRow = -1;
                drawFullScreen();
              }
            } else if (tx < 162) {
              gState.playing = !gState.playing;
              if (!gState.playing) {
                for (int c = 0; c < 8; c++) {
                  os->audio_set_voice(c, nullptr, 0, 0, 0, 0, 0, false);
                }
              }
              drawTransportBar();
            } else if (tx < 234) {
              if (playAdjacentSong(1)) {
                lastDrawnOrd = -1;
                lastDrawnRow = -1;
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
