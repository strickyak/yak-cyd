#include "lakatos_api.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// RGB565 Palette (HF Transceiver / KiwiSDR Dark Theme)
// ---------------------------------------------------------------------------
#define RGB565(r, g, b) \
  ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | (((b) & 0xF8) >> 3)))

static constexpr uint16_t kColBg           = RGB565(8, 12, 22);
static constexpr uint16_t kColHeader       = RGB565(16, 26, 48);
static constexpr uint16_t kColPanel        = RGB565(14, 20, 34);
static constexpr uint16_t kColGridBorder   = RGB565(42, 58, 90);
static constexpr uint16_t kColGtGold       = RGB565(238, 178, 17);
static constexpr uint16_t kColGtGoldBright = RGB565(255, 220, 80);
static constexpr uint16_t kColText         = RGB565(235, 242, 255);
static constexpr uint16_t kColSub          = RGB565(135, 155, 185);
static constexpr uint16_t kColAccent       = RGB565(0, 215, 255);
static constexpr uint16_t kColGreen        = RGB565(40, 230, 115);
static constexpr uint16_t kColYellow       = RGB565(255, 205, 50);
static constexpr uint16_t kColOrange       = RGB565(255, 130, 35);
static constexpr uint16_t kColRed          = RGB565(255, 65, 75);
static constexpr uint16_t kColScopeTrace   = RGB565(60, 255, 160);

// ---------------------------------------------------------------------------
// Amateur Radio Bands (75m, 40m, 20m) & Demodulation Modes (LSB, USB, AM)
// ---------------------------------------------------------------------------
enum DemodMode {
  MODE_LSB = 0,
  MODE_USB = 1,
  MODE_AM  = 2,
};

struct ModeSpec {
  const char* label;
  const char* kiwiMod;
  int         lowCut;
  int         highCut;
};

static const ModeSpec kModes[3] = {
  { "LSB", "lsb", -2700, -300 },
  { "USB", "usb",   300, 2700 },
  { "AM",  "am",  -4900, 4900 },
};

struct HamBand {
  const char* label;
  const char* name;
  int         minKhz;
  int         maxKhz;
  int         ssbMinKhz;
  int         ssbMaxKhz;
  int         defaultKhz;
  int         defaultMode;
};

static const HamBand kBands[3] = {
  { "75m", "75m / 80m HF", 3500,  4000,  3600,  4000,  3850, MODE_LSB },
  { "40m", "40m HF Band",  7000,  7300,  7125,  7300,  7200, MODE_LSB },
  { "20m", "20m HF Band", 14000, 14350, 14150, 14350, 14250, MODE_USB },
};
static constexpr int kNumBands = 3;

// Per-band saved VFO state
static int gBandFreqKhz[kNumBands] = { 3850, 7200, 14250 };
static int gBandMode[kNumBands]    = { MODE_LSB, MODE_LSB, MODE_USB };
static int gBandIdx                = 1; // Default to 40m (7200 kHz LSB)
static int gFreqKhz                = 7200;
static int gModeIdx                = MODE_LSB;

// ---------------------------------------------------------------------------
// IMA-ADPCM Decoder (matching go-kiwisdr-client/client/ima_adpcm_decoder.go)
// ---------------------------------------------------------------------------
static const int16_t kStepSizeTable[89] = {
      7,     8,     9,    10,    11,    12,    13,    14,    16,    17,
     19,    21,    23,    25,    28,    31,    34,    37,    41,    45,
     50,    55,    60,    66,    73,    80,    88,    97,   107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,
    337,   371,   408,   449,   494,   544,   598,   658,   724,   796,
    876,   963,  1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
   2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
   5894,  6484,  7132,  7845,  8630,  9493, 10442, 11487, 12635, 13899,
  15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

static const int8_t kIndexAdjustTable[16] = {
  -1, -1, -1, -1, 2, 4, 6, 8,
  -1, -1, -1, -1, 2, 4, 6, 8
};

static int32_t gAdpcmIndex = 0;
static int32_t gAdpcmPrev  = 0;

static inline int16_t decodeAdpcmSample(uint8_t code) {
  int32_t step = kStepSizeTable[gAdpcmIndex];
  int32_t idx  = gAdpcmIndex + kIndexAdjustTable[code & 0x0Fu];
  if (idx < 0) idx = 0;
  else if (idx > 88) idx = 88;
  gAdpcmIndex = idx;

  int32_t diff = step >> 3;
  if (code & 1u) diff += step >> 2;
  if (code & 2u) diff += step >> 1;
  if (code & 4u) diff += step;
  if (code & 8u) diff = -diff;

  int32_t samp = gAdpcmPrev + diff;
  if (samp < -32768) samp = -32768;
  else if (samp > 32767) samp = 32767;
  gAdpcmPrev = samp;
  return (int16_t)samp;
}

// ---------------------------------------------------------------------------
// Audio Ring Buffer & WebSocket State
// ---------------------------------------------------------------------------
static constexpr uint32_t kPcmRingSize    = 16384u; // 1.048s @ 15,625 Hz (power of 2)
static constexpr uint32_t kPcmRingMask    = kPcmRingSize - 1u;
static constexpr uint32_t kPrebufferSamps = 3072u;  // ~0.19s before starting playback
static constexpr size_t   kMaxWsFrameBuf  = 2048u;  // SND frames are 1034 bytes

static const LakatosApi* gOs = nullptr;

// Static .bss buffers (overlaid in 32 KB DRAM arena)
static int8_t  gPcmRing[kPcmRingSize];
static uint8_t gWsFrameBuf[kMaxWsFrameBuf];
static int16_t gDecodedPcm[2048]; // Up to 2048 samples per 1024-byte ADPCM SND packet

// Core 0 <-> Core 1 lockless PCM ring indices
static volatile uint32_t gPcmReadIdx   = 0;
static volatile uint32_t gPcmWriteIdx  = 0;
static volatile bool     gAudioPlaying = false;
static volatile bool     gFirstTick    = true;

// Streaming linear-interpolating resampler state (12,000 Hz -> 15,625 Hz)
static uint32_t gResampPhaseFp16 = 0;
static int16_t  gResampPrev      = 0;
static int16_t  gResampCurr      = 0;
static int32_t  gDitherErr       = 0;

enum StreamState {
  ST_DISCONNECTED = 0,
  ST_HANDSHAKE    = 1,
  ST_BUFFERING    = 2,
  ST_PLAYING      = 3,
  ST_ERROR        = 4,
};

enum WsParseState {
  WS_HDR_BASE = 0,
  WS_HDR_EXT  = 1,
  WS_PAYLOAD  = 2,
};

static StreamState  gState             = ST_DISCONNECTED;
static char         gKiwiHost[64]      = "sybil.yak.net:8073";
static char         gStatusNote[36]    = "Connecting...";
static uint8_t      gMasterVolStep     = 8; // 0..11 (8 = 100%, 9..11 = overdrive)
static uint32_t     gBytesReceived     = 0;
static uint32_t     gPktsReceived      = 0;
static uint32_t     gConnectStartMs    = 0;
static uint32_t     gLastReconnectMs   = 0;
static uint32_t     gLastKeepaliveMs   = 0;
static uint32_t     gLastTuneSendMs    = 0;
static bool         gTunePending       = false;
static int          gKiwiSamprate      = 12000;
static int          gSmeterDbm         = -115;
static uint16_t     gSmeterRaw         = 120;
static bool         gIsCompressed      = true;

// HTTP Upgrade header accumulator + WebSocket frame parser state
static char         gHttpHdrBuf[512];
static size_t       gHttpHdrLen        = 0;
static WsParseState gWsState           = WS_HDR_BASE;
static uint8_t      gWsHdr[14];
static size_t       gWsHdrPos          = 0;
static size_t       gWsHdrNeed         = 2;
static uint8_t      gWsOpcode          = 0;
static bool         gWsMasked          = false;
static uint8_t      gWsMaskKey[4]      = {0, 0, 0, 0};
static uint64_t     gWsPayloadLen      = 0;
static uint64_t     gWsPayloadPos      = 0;

// Real-time Visualizer State (16 spectrum bars + 144-point scope)
static constexpr int kNumBars   = 16;
static constexpr int kScopeCols = 144;
static uint8_t gBarLevel[kNumBars];
static uint8_t gBarPeak[kNumBars];
static int16_t gPrevBarH[kNumBars];
static int16_t gPrevPeakY[kNumBars];
static int16_t gPrevScopeY[kScopeCols];

static void serialLogf(const char* fmt, ...) {
  if (!gOs || !gOs->vsnprintf || !gOs->serial_write) return;
  char buf[160];
  va_list ap;
  va_start(ap, fmt);
  int n = gOs->vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n > 0) {
    size_t len = (size_t)n < sizeof(buf) ? (size_t)n : (sizeof(buf) - 1);
    gOs->serial_write(buf, len);
  }
}

static void setStatusNote(const char* s) {
  int i = 0;
  while (s && s[i] && i < (int)sizeof(gStatusNote) - 1) {
    gStatusNote[i] = s[i];
    i++;
  }
  gStatusNote[i] = '\0';
}

// ---------------------------------------------------------------------------
// Volume Mapping & Core 0 Hardware Timer Callback
// ---------------------------------------------------------------------------
static uint8_t getVoiceVolumeForStep(uint8_t step) {
  if (step == 0) return 0;
  if (step <= 8) return (uint8_t)((step * 44u) / 8u);
  if (step == 9)  return 58u;
  if (step == 10) return 76u;
  return 96u;
}

static void applyHardwareVoiceVolumes(bool resetPos) {
  if (!gOs) return;
  uint8_t v = (gAudioPlaying && gState == ST_PLAYING) ? getVoiceVolumeForStep(gMasterVolStep) : 0u;
  for (int ch = 0; ch < 4; ch++) {
    gOs->audio_set_voice(ch, gPcmRing, kPcmRingSize, 0, kPcmRingSize, 65536u, v, resetPos);
  }
}

static void muteHardwareVoices() {
  gAudioPlaying = false;
  if (!gOs) return;
  for (int ch = 0; ch < 4; ch++) {
    gOs->audio_set_voice(ch, gPcmRing, kPcmRingSize, 0, kPcmRingSize, 65536u, 0, false);
  }
}

static void audioTickCb(void* user) {
  (void)user;
  if (!gAudioPlaying) return;
  if (gFirstTick) {
    gFirstTick = false;
    return;
  }
  uint32_t r = gPcmReadIdx;
  uint32_t base = r & kPcmRingMask;
  for (uint32_t i = 0; i < 64u; i++) {
    gPcmRing[(base + i) & kPcmRingMask] = 0;
  }
  gPcmReadIdx = r + 64u;
}

static void startLockstepPlayback() {
  if (!gOs) return;
  gFirstTick    = true;
  gAudioPlaying = true;
  gOs->audio_set_tick_cb(audioTickCb, nullptr, 64u);
  applyHardwareVoiceVolumes(true);
}

// ---------------------------------------------------------------------------
// WebSocket Client Frame Sender & KiwiSDR Commands
// ---------------------------------------------------------------------------
static bool wsSendFrame(uint8_t opcode, const uint8_t* payload, size_t len) {
  if (!gOs || !gOs->http_stream_write || len > 180) return false;
  uint8_t frame[192];
  size_t pos = 0;
  frame[pos++] = (uint8_t)(0x80u | (opcode & 0x0Fu));
  if (len < 126) {
    frame[pos++] = (uint8_t)(0x80u | len);
  } else {
    frame[pos++] = (uint8_t)(0x80u | 126u);
    frame[pos++] = (uint8_t)((len >> 8) & 0xFFu);
    frame[pos++] = (uint8_t)(len & 0xFFu);
  }
  const uint8_t mask[4] = { 0x37u, 0xFAu, 0x21u, 0x3Du };
  frame[pos++] = mask[0];
  frame[pos++] = mask[1];
  frame[pos++] = mask[2];
  frame[pos++] = mask[3];
  for (size_t i = 0; i < len; i++) {
    frame[pos++] = payload[i] ^ mask[i & 3u];
  }
  return gOs->http_stream_write(frame, pos) == (int32_t)pos;
}

static bool wsSendText(const char* text) {
  size_t n = 0;
  while (text[n]) n++;
  return wsSendFrame(0x01u, (const uint8_t*)text, n);
}

static void sendKiwiTuning() {
  if (gState != ST_BUFFERING && gState != ST_PLAYING) return;
  const ModeSpec& m = kModes[gModeIdx];
  char cmd[96];
  snprintf(cmd, sizeof(cmd), "SET mod=%s low_cut=%d high_cut=%d freq=%d.000",
           m.kiwiMod, m.lowCut, m.highCut, gFreqKhz);
  wsSendText(cmd);
  gLastTuneSendMs = gOs->millis();
  gTunePending    = false;
}

static void sendKiwiLoginSequence() {
  wsSendText("SET auth t=kiwi p=");
  wsSendText("SET AR OK in=12000 out=44100");
  wsSendText("SET squelch=0 max=0");
  wsSendText("SET lms_autonotch=0");
  wsSendText("SET genattn=0");
  wsSendText("SET gen=0 mix=-1");
  wsSendText("SET ident_user=LakatosHam");
  sendKiwiTuning();
  wsSendText("SET agc=1 hang=0 thresh=-100 slope=6 decay=1000 manGain=50");
  wsSendText("SET compression=1");
  wsSendText("SET OVERRIDE inactivity_timeout=0");
  gLastKeepaliveMs = gOs->millis();
}

// ---------------------------------------------------------------------------
// Visualizer & Resampler (12,000 Hz -> 15,625 Hz)
// ---------------------------------------------------------------------------
static void updateVisualizerFromSamples(const int16_t* pcm, int numSamples) {
  if (numSamples <= 0) return;
  int segLen = numSamples / kNumBars;
  if (segLen < 4) segLen = 4;

  int32_t prevSample = 0;
  for (int b = 0; b < kNumBars; b++) {
    int start = (b * numSamples) / kNumBars;
    int32_t sumAbs  = 0;
    int32_t sumDiff = 0;
    for (int i = 0; i < segLen && (start + i) < numSamples; i++) {
      int32_t s = pcm[start + i];
      int32_t a = (s < 0) ? -s : s;
      int32_t d = s - prevSample;
      if (d < 0) d = -d;
      prevSample = s;
      sumAbs  += a;
      sumDiff += d;
    }
    int32_t avgLow  = sumAbs / segLen;
    int32_t avgHigh = sumDiff / segLen;
    int32_t energy  = ((avgLow * (kNumBars - b)) + (avgHigh * b * 2)) / kNumBars;
    int32_t lvl     = energy >> 5;
    if (lvl > 255) lvl = 255;
    if (lvl > gBarLevel[b]) {
      gBarLevel[b] = (uint8_t)lvl;
    } else {
      gBarLevel[b] = (uint8_t)(((int)gBarLevel[b] * 3 + lvl) >> 2);
    }
    if (gBarLevel[b] > gBarPeak[b]) {
      gBarPeak[b] = gBarLevel[b];
    }
  }
}

static void pushSamplesToRing(const int16_t* pcm, int numSamples) {
  if (numSamples <= 0) return;

  updateVisualizerFromSamples(pcm, numSamples);

  int32_t preGainNum = 4; // 1.0x
  if (gMasterVolStep == 9)       preGainNum = 5;
  else if (gMasterVolStep == 10) preGainNum = 6;
  else if (gMasterVolStep >= 11) preGainNum = 8;

  // Step size in 16.16 fixed-point input samples per 15,625 Hz output sample
  // For 12,000 Hz -> 15,625 Hz: stepFp16 = (12000 * 65536) / 15625 = 50332
  uint32_t stepFp16 = (uint32_t)(((uint64_t)gKiwiSamprate << 16) / (uint64_t)LAKATOS_AUDIO_RATE);
  if (stepFp16 < 16384u) stepFp16 = 50332u;

  uint32_t phase = gResampPhaseFp16;
  int16_t  prev  = gResampPrev;
  int16_t  curr  = gResampCurr;
  uint32_t w     = gPcmWriteIdx;
  int32_t  err   = gDitherErr;

  for (int i = 0; i < numSamples; i++) {
    prev = curr;
    curr = pcm[i];
    while (phase < 65536u) {
      // Prevent ring overwrite if buffer is completely full
      if ((uint32_t)(w - gPcmReadIdx) >= kPcmRingSize - 64u) {
        phase += stepFp16;
        continue;
      }
      int32_t frac   = (int32_t)phase;
      int32_t mono16 = (int32_t)prev + ((((int32_t)curr - (int32_t)prev) * frac) >> 16);
      if (preGainNum > 4) {
        mono16 = (mono16 * preGainNum) >> 2;
      }

      // First-order error-feedback noise shaping to 8-bit PCM
      int32_t shaped = mono16 + (err >> 1);
      if (shaped > 32511) shaped = 32511;
      else if (shaped < -32512) shaped = -32512;

      int32_t q8 = (shaped + 128) >> 8;
      if (q8 > 127) q8 = 127;
      else if (q8 < -127) q8 = -127;
      err = shaped - (q8 << 8);

      gPcmRing[w & kPcmRingMask] = (int8_t)q8;
      w++;
      phase += stepFp16;
    }
    phase -= 65536u;
  }

  gPcmWriteIdx     = w;
  gResampPhaseFp16 = phase;
  gResampPrev      = prev;
  gResampCurr      = curr;
  gDitherErr       = err;
}

// ---------------------------------------------------------------------------
// KiwiSDR MSG & SND Packet Handler
// ---------------------------------------------------------------------------
static int parseIntSimple(const char* s) {
  int sign = 1;
  if (*s == '-') { sign = -1; s++; }
  else if (*s == '+') { s++; }
  int val = 0;
  while (*s >= '0' && *s <= '9') {
    val = val * 10 + (*s - '0');
    s++;
  }
  return val * sign;
}

static void handleKiwiMsg(const uint8_t* payload, size_t len) {
  char buf[256];
  size_t copyLen = (len < sizeof(buf) - 1) ? len : (sizeof(buf) - 1);
  for (size_t i = 0; i < copyLen; i++) buf[i] = (char)payload[i];
  buf[copyLen] = '\0';

  // Scan space-separated key=value pairs
  char* p = buf;
  while (*p) {
    while (*p == ' ') p++;
    if (!*p) break;
    char* tok = p;
    while (*p && *p != ' ') p++;
    if (*p) *p++ = '\0';

    if (strncmp(tok, "audio_adpcm_state=", 18) == 0) {
      const char* v = tok + 18;
      gAdpcmIndex = parseIntSimple(v);
      if (gAdpcmIndex < 0) gAdpcmIndex = 0;
      if (gAdpcmIndex > 88) gAdpcmIndex = 88;
      const char* comma = v;
      while (*comma && *comma != ',') comma++;
      if (*comma == ',') {
        gAdpcmPrev = parseIntSimple(comma + 1);
      }
    } else if (strncmp(tok, "sample_rate=", 12) == 0 || strncmp(tok, "audio_rate=", 11) == 0) {
      const char* eq = tok;
      while (*eq && *eq != '=') eq++;
      if (*eq == '=') {
        int sr = parseIntSimple(eq + 1);
        if (sr >= 8000 && sr <= 24000) {
          gKiwiSamprate = sr;
        }
      }
    } else if (strncmp(tok, "too_busy=", 9) == 0 && tok[9] != '0') {
      setStatusNote("Server Busy (All Chans Used)");
    } else if (strncmp(tok, "badp=1", 6) == 0) {
      setStatusNote("Bad Password");
    } else if (strncmp(tok, "down", 4) == 0) {
      setStatusNote("KiwiSDR Server Down");
    }
  }
}

static void handleKiwiSnd(const uint8_t* data, size_t len) {
  // Header: [0..2]="SND", [3]=flags, [4..7]=seq(LE), [8..9]=smeter(BE)
  if (len <= 10) return;
  uint8_t  flags     = data[3];
  uint16_t smeterRaw = (uint16_t)(((uint16_t)data[8] << 8) | (uint16_t)data[9]);
  gSmeterRaw = smeterRaw;
  gSmeterDbm = ((int)smeterRaw / 10) - 127;

  size_t offset = (flags & 0x08u) ? 20u : 10u; // Stereo uses 20-byte header
  if (len <= offset) return;

  const uint8_t* audioBytes = data + offset;
  size_t audioLen = len - offset;

  if (flags & 0x20u) { // SND_FLAG_RESTART
    gAdpcmIndex = 0;
    gAdpcmPrev  = 0;
  }

  // If SND_FLAG_NEW_FREQ (0x04) is set, trim excess old audio so retuning is snappy
  if ((flags & 0x04u) && gState == ST_PLAYING) {
    uint32_t buffered = gPcmWriteIdx - gPcmReadIdx;
    if (buffered > 2048u) {
      gPcmWriteIdx = gPcmReadIdx + 1536u;
    }
  }

  int numSamples = 0;
  if (flags & 0x10u) { // SND_FLAG_COMPRESSED (4-bit IMA-ADPCM)
    gIsCompressed = true;
    size_t maxBytes = (audioLen > 1024u) ? 1024u : audioLen;
    for (size_t i = 0; i < maxBytes; i++) {
      uint8_t b = audioBytes[i];
      gDecodedPcm[numSamples++] = decodeAdpcmSample(b & 0x0Fu);
      gDecodedPcm[numSamples++] = decodeAdpcmSample((b >> 4) & 0x0Fu);
    }
  } else { // Uncompressed 16-bit signed PCM
    gIsCompressed = false;
    size_t maxPairs = audioLen / 2u;
    if (maxPairs > 2048u) maxPairs = 2048u;
    bool littleEnd = (flags & 0x80u) != 0;
    for (size_t i = 0; i < maxPairs; i++) {
      uint16_t u = littleEnd
                       ? (uint16_t)((uint16_t)audioBytes[2 * i] | ((uint16_t)audioBytes[2 * i + 1] << 8))
                       : (uint16_t)(((uint16_t)audioBytes[2 * i] << 8) | (uint16_t)audioBytes[2 * i + 1]);
      gDecodedPcm[numSamples++] = (int16_t)u;
    }
  }

  pushSamplesToRing(gDecodedPcm, numSamples);
  gPktsReceived++;

  // Transition from BUFFERING -> PLAYING once prebuffer is satisfied
  uint32_t pcmBuffered = gPcmWriteIdx - gPcmReadIdx;
  if (gState == ST_BUFFERING) {
    if (pcmBuffered >= kPrebufferSamps) {
      gState = ST_PLAYING;
      setStatusNote("Receiving Live Audio");
      startLockstepPlayback();
      serialLogf("[HAM] Playing %d kHz %s (ADPCM=%d, sr=%d Hz, smeter=%d dBm)\r\n",
                 gFreqKhz, kModes[gModeIdx].label, (int)gIsCompressed, gKiwiSamprate, gSmeterDbm);
    }
  } else if (gState == ST_PLAYING) {
    if ((int32_t)(gPcmWriteIdx - gPcmReadIdx) < 64) {
      muteHardwareVoices();
      gPcmReadIdx  = 0;
      gPcmWriteIdx = 0;
      gState       = ST_BUFFERING;
    }
  }
}

static void handleKiwiFrame(const uint8_t* data, size_t len) {
  if (len < 4) return;
  if (data[0] == 'S' && data[1] == 'N' && data[2] == 'D') {
    handleKiwiSnd(data, len);
  } else if (data[0] == 'M' && data[1] == 'S' && data[2] == 'G') {
    handleKiwiMsg(data + 3, len - 3);
  }
}

// ---------------------------------------------------------------------------
// Streaming WebSocket Frame Parser
// ---------------------------------------------------------------------------
static void feedWebSocketBytes(const uint8_t* buf, size_t len) {
  size_t pos = 0;
  while (pos < len) {
    if (gWsState == WS_HDR_BASE) {
      while (pos < len && gWsHdrPos < 2) {
        gWsHdr[gWsHdrPos++] = buf[pos++];
      }
      if (gWsHdrPos < 2) return;

      gWsOpcode = gWsHdr[0] & 0x0Fu;
      gWsMasked = (gWsHdr[1] & 0x80u) != 0;
      uint8_t len7 = gWsHdr[1] & 0x7Fu;
      size_t extBytes = (len7 == 126u) ? 2u : ((len7 == 127u) ? 8u : 0u);
      size_t maskBytes = gWsMasked ? 4u : 0u;
      gWsHdrNeed = 2u + extBytes + maskBytes;

      if (gWsHdrNeed == 2u) {
        gWsPayloadLen = len7;
        gWsPayloadPos = 0;
        if (gWsPayloadLen == 0) {
          gWsHdrPos = 0;
          gWsState  = WS_HDR_BASE;
        } else {
          gWsState = WS_PAYLOAD;
        }
      } else {
        gWsState = WS_HDR_EXT;
      }
    } else if (gWsState == WS_HDR_EXT) {
      while (pos < len && gWsHdrPos < gWsHdrNeed) {
        gWsHdr[gWsHdrPos++] = buf[pos++];
      }
      if (gWsHdrPos < gWsHdrNeed) return;

      uint8_t len7 = gWsHdr[1] & 0x7Fu;
      size_t maskOff = 2u;
      if (len7 == 126u) {
        gWsPayloadLen = ((uint64_t)gWsHdr[2] << 8) | (uint64_t)gWsHdr[3];
        maskOff = 4u;
      } else if (len7 == 127u) {
        gWsPayloadLen = 0;
        for (int i = 0; i < 8; i++) {
          gWsPayloadLen = (gWsPayloadLen << 8) | (uint64_t)gWsHdr[2 + i];
        }
        maskOff = 10u;
      } else {
        gWsPayloadLen = len7;
      }
      if (gWsMasked) {
        for (int i = 0; i < 4; i++) gWsMaskKey[i] = gWsHdr[maskOff + i];
      }
      gWsPayloadPos = 0;
      if (gWsPayloadLen == 0) {
        gWsHdrPos = 0;
        gWsState  = WS_HDR_BASE;
      } else {
        gWsState = WS_PAYLOAD;
      }
    } else if (gWsState == WS_PAYLOAD) {
      uint64_t remFrame = gWsPayloadLen - gWsPayloadPos;
      size_t   remInput = len - pos;
      size_t   chunk    = (remInput < remFrame) ? remInput : (size_t)remFrame;

      if (gWsPayloadLen <= kMaxWsFrameBuf) {
        for (size_t i = 0; i < chunk; i++) {
          uint8_t b = buf[pos + i];
          if (gWsMasked) b ^= gWsMaskKey[(gWsPayloadPos + i) & 3u];
          gWsFrameBuf[gWsPayloadPos + i] = b;
        }
      } else if (gWsPayloadPos < 256u) {
        // Capture prefix of oversize MSG frames (e.g. load_cfg) and discard remainder
        size_t cap = (256u - (size_t)gWsPayloadPos);
        if (cap > chunk) cap = chunk;
        for (size_t i = 0; i < cap; i++) {
          uint8_t b = buf[pos + i];
          if (gWsMasked) b ^= gWsMaskKey[(gWsPayloadPos + i) & 3u];
          gWsFrameBuf[gWsPayloadPos + i] = b;
        }
      }

      pos           += chunk;
      gWsPayloadPos += chunk;

      if (gWsPayloadPos >= gWsPayloadLen) {
        if (gWsOpcode == 0x08u) {
          // Server sent WebSocket Close frame
          muteHardwareVoices();
          gState = ST_ERROR;
          gLastReconnectMs = gOs->millis();
          setStatusNote("Connection Closed");
        } else if (gWsOpcode == 0x09u) {
          // Ping -> reply with Pong
          size_t pongLen = (gWsPayloadLen <= 64u) ? (size_t)gWsPayloadLen : 0u;
          wsSendFrame(0x0Au, gWsFrameBuf, pongLen);
        } else if (gWsPayloadLen <= kMaxWsFrameBuf) {
          handleKiwiFrame(gWsFrameBuf, (size_t)gWsPayloadLen);
        }
        gWsHdrPos = 0;
        gWsState  = WS_HDR_BASE;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Connection Management & Network Pump
// ---------------------------------------------------------------------------
static void closeKiwiConnection() {
  muteHardwareVoices();
  if (gOs && gOs->http_stream_close) {
    gOs->http_stream_close();
  }
  gHttpHdrLen      = 0;
  gWsState         = WS_HDR_BASE;
  gWsHdrPos        = 0;
  gWsPayloadLen    = 0;
  gWsPayloadPos    = 0;
  gAdpcmIndex      = 0;
  gAdpcmPrev       = 0;
  gPcmReadIdx      = 0;
  gPcmWriteIdx     = 0;
  gResampPhaseFp16 = 0;
  gResampPrev      = 0;
  gResampCurr      = 0;
  gDitherErr       = 0;
  for (uint32_t i = 0; i < kPcmRingSize; i++) gPcmRing[i] = 0;
}

static bool connectToKiwi() {
  closeKiwiConnection();
  if (!gOs->wifi_is_connected()) {
    if (gState != ST_ERROR) {
      serialLogf("[HAM] Waiting for Wi-Fi connection...\r\n");
    }
    gState = ST_ERROR;
    setStatusNote("No WiFi Connection");
    gLastReconnectMs = gOs->millis();
    return false;
  }

  // Ensure host includes port (default :8073)
  bool hasColon = false;
  for (int i = 0; gKiwiHost[i]; i++) {
    if (gKiwiHost[i] == ':') { hasColon = true; break; }
  }
  char tcpUrl[96];
  if (hasColon) {
    snprintf(tcpUrl, sizeof(tcpUrl), "tcp://%s", gKiwiHost);
  } else {
    snprintf(tcpUrl, sizeof(tcpUrl), "tcp://%s:8073", gKiwiHost);
  }

  setStatusNote("Connecting to KiwiSDR...");
  serialLogf("[HAM] Opening %s ...\r\n", tcpUrl);
  if (!gOs->http_stream_open(tcpUrl)) {
    serialLogf("[HAM] TCP connect failed: %s\r\n", tcpUrl);
    gState = ST_ERROR;
    setStatusNote("Connect Failed");
    gLastReconnectMs = gOs->millis();
    return false;
  }

  // Send HTTP/1.1 WebSocket Upgrade request
  uint32_t clientId = (uint32_t)gOs->get_unix_time();
  if (clientId < 1000000u) clientId = 1730850000u + (gOs->millis() / 100u);
  char req[320];
  int reqLen = snprintf(
      req, sizeof(req),
      "GET /%lu/SND HTTP/1.1\r\n"
      "Host: %s\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
      "Sec-WebSocket-Version: 13\r\n"
      "Origin: http://%s\r\n"
      "\r\n",
      (unsigned long)clientId, gKiwiHost, gKiwiHost);

  if (reqLen <= 0 || gOs->http_stream_write(req, (size_t)reqLen) != reqLen) {
    closeKiwiConnection();
    gState = ST_ERROR;
    setStatusNote("Handshake Write Error");
    gLastReconnectMs = gOs->millis();
    return false;
  }

  gOs->set_rgb_led(true, false, false, 0);
  gState          = ST_HANDSHAKE;
  gConnectStartMs = gOs->millis();
  setStatusNote("WebSocket Handshake...");
  return true;
}

static void pumpKiwiNetwork() {
  if (gState != ST_HANDSHAKE && gState != ST_BUFFERING && gState != ST_PLAYING) return;

  uint8_t tmp[1024];
  for (int iter = 0; iter < 6; iter++) {
    int32_t got = gOs->http_stream_read(tmp, sizeof(tmp));
    if (got < 0) {
      muteHardwareVoices();
      gState = ST_ERROR;
      gLastReconnectMs = gOs->millis();
      setStatusNote("Stream Disconnected");
      return;
    }
    if (got == 0) break;

    gBytesReceived += (uint32_t)got;

    if (gState == ST_HANDSHAKE) {
      // Accumulate HTTP 101 response header until "\r\n\r\n"
      size_t consumed = 0;
      while (consumed < (size_t)got && gHttpHdrLen < sizeof(gHttpHdrBuf) - 1) {
        gHttpHdrBuf[gHttpHdrLen++] = (char)tmp[consumed++];
        gHttpHdrBuf[gHttpHdrLen]   = '\0';
        if (gHttpHdrLen >= 4 &&
            gHttpHdrBuf[gHttpHdrLen - 4] == '\r' &&
            gHttpHdrBuf[gHttpHdrLen - 3] == '\n' &&
            gHttpHdrBuf[gHttpHdrLen - 2] == '\r' &&
            gHttpHdrBuf[gHttpHdrLen - 1] == '\n') {
          // Verify "101" status code in first line
          if (strncmp(gHttpHdrBuf, "HTTP/1.1 101", 12) != 0 &&
              strncmp(gHttpHdrBuf, "HTTP/1.0 101", 12) != 0) {
            closeKiwiConnection();
            gState = ST_ERROR;
            setStatusNote("WS Upgrade Rejected");
            gLastReconnectMs = gOs->millis();
            return;
          }
          gState = ST_BUFFERING;
          setStatusNote("Buffering ADPCM Stream...");
          sendKiwiLoginSequence();
          if (consumed < (size_t)got) {
            feedWebSocketBytes(tmp + consumed, (size_t)got - consumed);
          }
          break;
        }
      }
      if (gState == ST_HANDSHAKE && gHttpHdrLen >= sizeof(gHttpHdrBuf) - 1) {
        closeKiwiConnection();
        gState = ST_ERROR;
        setStatusNote("Invalid HTTP Header");
        gLastReconnectMs = gOs->millis();
        return;
      }
    } else {
      feedWebSocketBytes(tmp, (size_t)got);
    }
  }

  uint32_t now = gOs->millis();
  if (gState == ST_HANDSHAKE && (uint32_t)(now - gConnectStartMs) > 8000u) {
    closeKiwiConnection();
    gState = ST_ERROR;
    setStatusNote("Handshake Timeout");
    gLastReconnectMs = now;
    return;
  }

  // Send pending frequency retune if throttle interval has elapsed
  if (gTunePending && (gState == ST_BUFFERING || gState == ST_PLAYING)) {
    if ((uint32_t)(now - gLastTuneSendMs) >= 80u) {
      sendKiwiTuning();
    }
  }

  // Send KiwiSDR keepalive every 4 seconds
  if ((gState == ST_BUFFERING || gState == ST_PLAYING) &&
      (uint32_t)(now - gLastKeepaliveMs) >= 4000u) {
    wsSendText("SET keepalive");
    gLastKeepaliveMs = now;
  }
}

// ---------------------------------------------------------------------------
// UI Rendering (320x240 Landscape)
// ---------------------------------------------------------------------------
static constexpr int kFreqSliderY = 131;
static constexpr int kFreqSliderH = 38;
static constexpr int kFreqTrkX    = 14;
static constexpr int kFreqTrkW    = 292;

static constexpr int kRow1Y       = 172;
static constexpr int kRow1H       = 29;
static constexpr int kRow2Y       = 205;
static constexpr int kRow2H       = 30;
static constexpr int kVolBoxX     = 128;
static constexpr int kVolBoxW     = 188;
static constexpr int kVolTrkX     = 160;
static constexpr int kVolTrkW     = 121; // 11 steps * 11 px

static void drawStatusBadge() {
  const char* txt = "OFFLINE";
  uint16_t bg = RGB565(80, 25, 35);
  uint16_t fg = kColText;

  if (gState == ST_PLAYING) {
    txt = "ON AIR";
    bg  = RGB565(20, 130, 65);
  } else if (gState == ST_BUFFERING || gState == ST_HANDSHAKE) {
    txt = "BUFFER";
    bg  = RGB565(150, 105, 15);
  }

  gOs->tft_fill_round_rect(254, 2, 62, 18, 4, bg);
  gOs->tft_draw_round_rect(254, 2, 62, 18, 4, kColGridBorder);
  gOs->tft_set_text_datum(LAKATOS_MC_DATUM);
  gOs->tft_set_text_color(fg, bg);
  gOs->tft_draw_string(txt, 285, 11, 1);
  gOs->tft_set_text_datum(LAKATOS_TL_DATUM);
}

static void drawHeaderBar() {
  int cs = lakatos_corner_size(gOs);
  gOs->tft_fill_rect(0, 0, 320, 22, kColHeader);
  gOs->tft_draw_fast_h_line(0, 22, 320, kColGtGold);

  // Standard NW Exit X mark (sized via Lakatos corner size)
  lakatos_draw_exit_wedge(gOs);

  // App Title + KiwiSDR Host
  gOs->tft_set_text_datum(LAKATOS_ML_DATUM);
  gOs->tft_set_text_color(kColGtGoldBright, kColHeader);
  gOs->tft_draw_string("HAM SDR", cs + 6, 11, 2);

  gOs->tft_set_text_color(kColAccent, kColHeader);
  gOs->tft_draw_string(gKiwiHost, cs + 78, 11, 1);

  drawStatusBadge();
  gOs->tft_set_text_datum(LAKATOS_TL_DATUM);
}

static void drawVfoPanel() {
  // Left Box: Digital VFO Frequency & Mode Readout (x=4..184, y=25..72)
  gOs->tft_fill_rect(4, 25, 180, 47, kColPanel);
  gOs->tft_draw_rect(4, 25, 180, 47, kColGridBorder);

  // Format frequency as e.g. "7,200" or "14,250" kHz
  char fbuf[24];
  int mhz = gFreqKhz / 1000;
  int khz = gFreqKhz % 1000;
  snprintf(fbuf, sizeof(fbuf), "%d.%03d", mhz, khz);

  gOs->tft_set_text_datum(LAKATOS_TL_DATUM);
  gOs->tft_set_text_color(kColGtGoldBright, kColPanel);
  int fw = gOs->tft_draw_string(fbuf, 10, 29, 4);

  gOs->tft_set_text_color(kColSub, kColPanel);
  gOs->tft_draw_string("MHz", 14 + fw, 37, 2);

  // Mode badge on top-right of VFO box
  uint16_t modeBg = (gModeIdx == MODE_LSB) ? RGB565(22, 60, 105)
                  : ((gModeIdx == MODE_USB) ? RGB565(80, 42, 16) : RGB565(28, 78, 45));
  gOs->tft_fill_round_rect(140, 29, 38, 18, 3, modeBg);
  gOs->tft_draw_round_rect(140, 29, 38, 18, 3, kColAccent);
  gOs->tft_set_text_datum(LAKATOS_MC_DATUM);
  gOs->tft_set_text_color(kColText, modeBg);
  gOs->tft_draw_string(kModes[gModeIdx].label, 159, 38, 2);

  // Bottom status line inside VFO box
  gOs->tft_set_text_datum(LAKATOS_TL_DATUM);
  char subLine[40];
  uint16_t subCol = kColAccent;
  if (gState == ST_PLAYING) {
    snprintf(subLine, sizeof(subLine), "%s | %d kHz | %s",
             kBands[gBandIdx].label, gFreqKhz,
             gIsCompressed ? "ADPCM 12k" : "PCM 12k");
  } else {
    snprintf(subLine, sizeof(subLine), "%s", gStatusNote);
    subCol = (gState == ST_ERROR) ? kColRed : kColYellow;
  }
  gOs->tft_set_text_color(subCol, kColPanel);
  gOs->tft_draw_string(subLine, 10, 58, 1);
}

static void drawSmeterPanel() {
  // Right Box: Live HF S-Meter & Buffer Health (x=188..316, y=25..72)
  gOs->tft_fill_rect(188, 25, 128, 47, kColPanel);
  gOs->tft_draw_rect(188, 25, 128, 47, kColGridBorder);

  // Compute S-units from gSmeterDbm (S9 = -73 dBm, 6 dB per S-unit below S9)
  char sStr[28];
  if (gState != ST_PLAYING && gState != ST_BUFFERING) {
    snprintf(sStr, sizeof(sStr), "S0   --- dBm");
  } else if (gSmeterDbm >= -73) {
    int over = gSmeterDbm + 73;
    snprintf(sStr, sizeof(sStr), "S9+%d  %ddBm", over, gSmeterDbm);
  } else {
    int sUnit = 9 - ((-73 - gSmeterDbm + 3) / 6);
    if (sUnit < 1) sUnit = 1;
    snprintf(sStr, sizeof(sStr), "S%d   %d dBm", sUnit, gSmeterDbm);
  }

  gOs->tft_set_text_datum(LAKATOS_TL_DATUM);
  gOs->tft_set_text_color(kColText, kColPanel);
  gOs->tft_draw_string(sStr, 194, 28, 2);

  // S-Meter horizontal bar (12 segments: S1..S9 in green/gold, +10..+30 in red)
  int barX = 194;
  int barY = 46;
  // Map dBm [-127 .. -43] -> 0..12 segments
  int activeSegs = (gSmeterDbm + 127) / 7;
  if (gState != ST_PLAYING && gState != ST_BUFFERING) activeSegs = 0;
  if (activeSegs < 0) activeSegs = 0;
  if (activeSegs > 12) activeSegs = 12;

  for (int i = 0; i < 12; i++) {
    int sx = barX + i * 9;
    uint16_t col = RGB565(22, 32, 48);
    if (i < activeSegs) {
      if (i < 6) col = kColGreen;
      else if (i < 9) col = kColGtGoldBright;
      else col = kColRed;
    }
    gOs->tft_fill_rect(sx, barY, 7, 8, col);
  }

  // Buffer health & status note on bottom row of S-Meter box
  uint32_t pcmBuffered = (gPcmWriteIdx >= gPcmReadIdx) ? (gPcmWriteIdx - gPcmReadIdx) : 0;
  int bufPct = (int)((pcmBuffered * 100u) / kPcmRingSize);
  if (bufPct > 100) bufPct = 100;

  char bStr[24];
  snprintf(bStr, sizeof(bStr), "RX:%luKB BUF:%d%%",
           (unsigned long)(gBytesReceived >> 10), bufPct);
  gOs->tft_set_text_color(kColSub, kColPanel);
  gOs->tft_draw_string(bStr, 194, 59, 1);
}

static void drawVisualizerFrames() {
  // Left Box: 16-Band Audio Spectrum (x=4..158, y=75..128)
  gOs->tft_fill_rect(4, 75, 154, 53, kColPanel);
  gOs->tft_draw_rect(4, 75, 154, 53, kColGridBorder);

  // Right Box: Live Waveform Scope (x=162..316, y=75..128)
  gOs->tft_fill_rect(162, 75, 154, 53, kColPanel);
  gOs->tft_draw_rect(162, 75, 154, 53, kColGridBorder);
  gOs->tft_draw_fast_h_line(166, 101, kScopeCols, RGB565(25, 38, 62));

  for (int i = 0; i < kNumBars; i++) {
    gPrevBarH[i]  = -1;
    gPrevPeakY[i] = -1;
  }
  for (int i = 0; i < kScopeCols; i++) {
    gPrevScopeY[i] = -1;
  }
}

static void updateVisualizersIncremental() {
  const int baseBottom = 125;
  const int maxBarH    = 46;

  for (int b = 0; b < kNumBars; b++) {
    int bx = 9 + b * 9;
    int bw = 7;
    int h  = ((int)gBarLevel[b] * maxBarH) >> 8;
    if (h > maxBarH) h = maxBarH;

    int prevH = gPrevBarH[b];
    if (h != prevH) {
      if (h < prevH || prevH < 0) {
        int clearTop = baseBottom - ((prevH > 0) ? prevH : maxBarH);
        int clearH   = (baseBottom - h) - clearTop;
        if (clearH > 0) {
          gOs->tft_fill_rect(bx, clearTop, bw, clearH, kColPanel);
        }
      }
      if (h > 0) {
        int lowH = (h > 28) ? 28 : h;
        gOs->tft_fill_rect(bx, baseBottom - lowH, bw, lowH, kColGreen);
        if (h > 28) {
          int midH = (h > 38) ? 10 : (h - 28);
          gOs->tft_fill_rect(bx, baseBottom - 28 - midH, bw, midH, kColGtGold);
        }
        if (h > 38) {
          int hiH = h - 38;
          gOs->tft_fill_rect(bx, baseBottom - h, bw, hiH, kColRed);
        }
      }
      gPrevBarH[b] = (int16_t)h;
    }

    int pkH = ((int)gBarPeak[b] * maxBarH) >> 8;
    if (pkH > maxBarH) pkH = maxBarH;
    int pkY = baseBottom - pkH - 2;
    if (pkY < 78) pkY = 78;
    if (gPrevPeakY[b] >= 78 && gPrevPeakY[b] != pkY && gPrevPeakY[b] < baseBottom - h) {
      gOs->tft_draw_fast_h_line(bx, gPrevPeakY[b], bw, kColPanel);
    }
    if (pkH > 2) {
      gOs->tft_draw_fast_h_line(bx, pkY, bw, kColGtGoldBright);
      gPrevPeakY[b] = (int16_t)pkY;
    }

    if (gBarLevel[b] > 6) gBarLevel[b] -= 6;
    else gBarLevel[b] = 0;
    if (gBarPeak[b] > 2) gBarPeak[b] -= 2;
    else gBarPeak[b] = 0;
  }

  // Live Oscilloscope Trace (x=166..309, center Y=101)
  uint32_t rBase   = gPcmReadIdx;
  const int scopeX0 = 166;
  const int centerY = 101;

  for (int x = 0; x < kScopeCols; x++) {
    int8_t s = 0;
    if (gState == ST_PLAYING && gMasterVolStep > 0) {
      s = gPcmRing[(rBase + (uint32_t)(x * 2)) & kPcmRingMask];
    }
    int y = centerY - (((int)s * 44) >> 7);
    if (y < 78) y = 78;
    if (y > 124) y = 124;

    int prevY = gPrevScopeY[x];
    if (prevY >= 78 && prevY != y) {
      gOs->tft_fill_rect(scopeX0 + x, prevY - 1, 1, 3, kColPanel);
      if (prevY - 1 <= centerY && centerY <= prevY + 1) {
        gOs->tft_fill_rect(scopeX0 + x, centerY, 1, 1, RGB565(25, 38, 62));
      }
    }
    gOs->tft_fill_rect(scopeX0 + x, y - 1, 1, 3, kColScopeTrace);
    gPrevScopeY[x] = (int16_t)y;
  }
}

static void drawFreqSlider() {
  // Frequency Tuning Slider Box (x=4..316, y=131..169, h=38)
  gOs->tft_fill_rect(4, kFreqSliderY, 312, kFreqSliderH, kColPanel);
  gOs->tft_draw_rect(4, kFreqSliderY, 312, kFreqSliderH, kColGridBorder);

  const HamBand& b = kBands[gBandIdx];
  int span = b.maxKhz - b.minKhz;
  if (span <= 0) span = 1;

  // Scale endpoint and midpoint labels along top of slider box
  char lMin[16], lMid[24], lMax[16];
  snprintf(lMin, sizeof(lMin), "%d", b.minKhz);
  snprintf(lMid, sizeof(lMid), "<-- %s SLIDER (1 kHz) -->", b.label);
  snprintf(lMax, sizeof(lMax), "%d", b.maxKhz);

  gOs->tft_set_text_datum(LAKATOS_TL_DATUM);
  gOs->tft_set_text_color(kColSub, kColPanel);
  gOs->tft_draw_string(lMin, kFreqTrkX, kFreqSliderY + 3, 1);

  gOs->tft_set_text_datum(LAKATOS_TC_DATUM);
  gOs->tft_set_text_color(kColAccent, kColPanel);
  gOs->tft_draw_string(lMid, 160, kFreqSliderY + 3, 1);

  gOs->tft_set_text_datum(LAKATOS_TR_DATUM);
  gOs->tft_set_text_color(kColSub, kColPanel);
  gOs->tft_draw_string(lMax, kFreqTrkX + kFreqTrkW, kFreqSliderY + 3, 1);

  // Slider Track (y = kFreqSliderY + 14, h = 18)
  int trkY = kFreqSliderY + 14;
  int trkH = 18;
  gOs->tft_fill_round_rect(kFreqTrkX, trkY, kFreqTrkW, trkH, 4, RGB565(8, 12, 20));
  gOs->tft_draw_round_rect(kFreqTrkX, trkY, kFreqTrkW, trkH, 4, kColGridBorder);

  // Highlight SSB phone sub-band portion inside track
  int ssbX0 = kFreqTrkX + ((b.ssbMinKhz - b.minKhz) * kFreqTrkW) / span;
  int ssbX1 = kFreqTrkX + ((b.ssbMaxKhz - b.minKhz) * kFreqTrkW) / span;
  if (ssbX1 > ssbX0 + 2) {
    gOs->tft_fill_rect(ssbX0 + 1, trkY + 3, ssbX1 - ssbX0 - 2, trkH - 6, RGB565(20, 42, 68));
  }

  // Graticule tick marks every 50 kHz
  int firstTick = ((b.minKhz + 49) / 50) * 50;
  for (int tk = firstTick; tk < b.maxKhz; tk += 50) {
    int tx = kFreqTrkX + ((tk - b.minKhz) * kFreqTrkW) / span;
    uint16_t tCol = (tk % 100 == 0) ? kColSub : kColGridBorder;
    int th = (tk % 100 == 0) ? (trkH - 4) : (trkH - 8);
    gOs->tft_draw_fast_v_line(tx, trkY + (trkH - th) / 2, th, tCol);
  }

  // Tuning needle / thumb
  int clampedFreq = gFreqKhz;
  if (clampedFreq < b.minKhz) clampedFreq = b.minKhz;
  if (clampedFreq > b.maxKhz) clampedFreq = b.maxKhz;
  int knobX = kFreqTrkX + ((clampedFreq - b.minKhz) * kFreqTrkW) / span;
  if (knobX < kFreqTrkX + 4) knobX = kFreqTrkX + 4;
  if (knobX > kFreqTrkX + kFreqTrkW - 4) knobX = kFreqTrkX + kFreqTrkW - 4;

  gOs->tft_fill_round_rect(knobX - 4, trkY - 2, 9, trkH + 4, 3, kColGtGoldBright);
  gOs->tft_draw_round_rect(knobX - 4, trkY - 2, 9, trkH + 4, 3, kColText);
  gOs->tft_draw_fast_v_line(knobX, trkY, trkH, kColRed);
  gOs->tft_set_text_datum(LAKATOS_TL_DATUM);
}

static void drawBandAndStepRow() {
  // Row 1 (y = 172..201): [75m] [40m] [20m]   [-10k] [-1k] [+1k] [+10k]
  gOs->tft_set_text_datum(LAKATOS_MC_DATUM);

  // 3 Band buttons on left
  for (int i = 0; i < kNumBands; i++) {
    int bx = 4 + i * 44;
    int bw = 41;
    bool active = (gBandIdx == i);
    uint16_t bg     = active ? RGB565(58, 46, 12) : RGB565(22, 36, 62);
    uint16_t border = active ? kColGtGoldBright   : kColGridBorder;
    uint16_t fg     = active ? kColGtGoldBright   : kColText;

    gOs->tft_fill_round_rect(bx, kRow1Y, bw, kRow1H, 4, bg);
    gOs->tft_draw_round_rect(bx, kRow1Y, bw, kRow1H, 4, border);
    gOs->tft_set_text_color(fg, bg);
    gOs->tft_draw_string(kBands[i].label, bx + bw / 2, kRow1Y + kRow1H / 2, 2);
  }

  // 4 Step buttons on right (-10k, -1k, +1k, +10k)
  static const char* const kStepLabels[4] = { "-10k", "-1k", "+1k", "+10k" };
  for (int i = 0; i < 4; i++) {
    int bx = 140 + i * 45;
    int bw = 41;
    uint16_t bg = (i == 1 || i == 2) ? RGB565(24, 48, 76) : RGB565(18, 30, 52);
    gOs->tft_fill_round_rect(bx, kRow1Y, bw, kRow1H, 4, bg);
    gOs->tft_draw_round_rect(bx, kRow1Y, bw, kRow1H, 4, kColAccent);
    gOs->tft_set_text_color(kColText, bg);
    gOs->tft_draw_string(kStepLabels[i], bx + bw / 2, kRow1Y + kRow1H / 2, 2);
  }
  gOs->tft_set_text_datum(LAKATOS_TL_DATUM);
}

static void drawModeButtons() {
  // 3 Mode buttons on bottom-left (y = 205..235): [LSB] [USB] [AM]
  gOs->tft_set_text_datum(LAKATOS_MC_DATUM);
  for (int i = 0; i < 3; i++) {
    int bx = 4 + i * 40;
    int bw = 37;
    bool active = (gModeIdx == i);
    uint16_t bg     = active ? RGB565(20, 70, 110) : RGB565(20, 28, 46);
    uint16_t border = active ? kColAccent          : kColGridBorder;
    uint16_t fg     = active ? kColGtGoldBright    : kColText;

    gOs->tft_fill_round_rect(bx, kRow2Y, bw, kRow2H, 4, bg);
    gOs->tft_draw_round_rect(bx, kRow2Y, bw, kRow2H, 4, border);
    gOs->tft_set_text_color(fg, bg);
    gOs->tft_draw_string(kModes[i].label, bx + bw / 2, kRow2Y + kRow2H / 2, 2);
  }
  gOs->tft_set_text_datum(LAKATOS_TL_DATUM);
}

static void drawVolumeBar() {
  // Bottom-Right Volume Slider (x=128..316, y=205..235)
  gOs->tft_fill_rect(kVolBoxX, kRow2Y, kVolBoxW, kRow2H, kColPanel);
  gOs->tft_draw_rect(kVolBoxX, kRow2Y, kVolBoxW, kRow2H, kColGridBorder);

  gOs->tft_set_text_datum(LAKATOS_ML_DATUM);
  gOs->tft_set_text_color(kColSub, kColPanel);
  gOs->tft_draw_string("VOL", kVolBoxX + 5, kRow2Y + kRow2H / 2, 1);

  int trkY = kRow2Y + 7;
  int trkH = kRow2H - 14;
  gOs->tft_fill_round_rect(kVolTrkX, trkY, kVolTrkW, trkH, 3, RGB565(8, 12, 22));
  gOs->tft_draw_round_rect(kVolTrkX, trkY, kVolTrkW, trkH, 3, kColGridBorder);

  int segW = kVolTrkW / 11; // 11 px per step
  for (int i = 1; i <= 11; i++) {
    int sx = kVolTrkX + (i - 1) * segW + 1;
    int sw = segW - 2;
    if (i <= (int)gMasterVolStep) {
      uint16_t col = kColGreen;
      if (i == 7 || i == 8) col = kColYellow;
      else if (i == 9 || i == 10) col = kColOrange;
      else if (i == 11) col = kColRed;
      gOs->tft_fill_rect(sx, trkY + 2, sw, trkH - 4, col);
    } else {
      uint16_t dim = (i > 8) ? RGB565(45, 20, 22) : RGB565(18, 28, 42);
      gOs->tft_fill_rect(sx, trkY + 4, sw, trkH - 8, dim);
    }
  }

  // Unity (8) tick mark
  int unityX = kVolTrkX + 8 * segW;
  gOs->tft_draw_fast_v_line(unityX, kRow2Y + 4, kRow2H - 8, kColText);

  // Knob thumb
  int knobX = kVolTrkX + (int)gMasterVolStep * segW;
  if (knobX < kVolTrkX + 2) knobX = kVolTrkX + 2;
  if (knobX > kVolTrkX + kVolTrkW - 2) knobX = kVolTrkX + kVolTrkW - 2;
  uint16_t knobCol = (gMasterVolStep > 8) ? kColRed
                   : ((gMasterVolStep == 0) ? kColSub : kColAccent);
  gOs->tft_fill_rect(knobX - 2, kRow2Y + 4, 5, kRow2H - 8, knobCol);

  char vbuf[12];
  uint16_t txtCol = kColGreen;
  if (gMasterVolStep == 0) {
    snprintf(vbuf, sizeof(vbuf), "MUTE");
    txtCol = kColSub;
  } else if (gMasterVolStep > 8) {
    snprintf(vbuf, sizeof(vbuf), "+%u", (unsigned)(gMasterVolStep - 8));
    txtCol = kColRed;
  } else {
    snprintf(vbuf, sizeof(vbuf), "%u/11", (unsigned)gMasterVolStep);
    txtCol = (gMasterVolStep == 8) ? kColYellow : kColGreen;
  }
  gOs->tft_set_text_datum(LAKATOS_MC_DATUM);
  gOs->tft_set_text_color(txtCol, kColPanel);
  gOs->tft_draw_string(vbuf, 298, kRow2Y + kRow2H / 2, 1);
  gOs->tft_set_text_datum(LAKATOS_TL_DATUM);
}

static void drawFullUi() {
  gOs->tft_fill_screen(kColBg);
  drawHeaderBar();
  drawVfoPanel();
  drawSmeterPanel();
  drawVisualizerFrames();
  drawFreqSlider();
  drawBandAndStepRow();
  drawModeButtons();
  drawVolumeBar();
}

// ---------------------------------------------------------------------------
// Touch Input Handlers
// ---------------------------------------------------------------------------
static void setFrequencyKhz(int newKhz, bool immediateSend) {
  const HamBand& b = kBands[gBandIdx];
  if (newKhz < b.minKhz) newKhz = b.minKhz;
  if (newKhz > b.maxKhz) newKhz = b.maxKhz;
  if (newKhz == gFreqKhz && !immediateSend) return;

  gFreqKhz = newKhz;
  gBandFreqKhz[gBandIdx] = newKhz;
  drawVfoPanel();
  drawFreqSlider();

  uint32_t now = gOs->millis();
  if (immediateSend || (uint32_t)(now - gLastTuneSendMs) >= 80u) {
    sendKiwiTuning();
  } else {
    gTunePending = true;
  }
}

static bool handleFreqSliderTouch(int tx, int ty) {
  if (ty < kFreqSliderY || ty > kFreqSliderY + kFreqSliderH) return false;
  const HamBand& b = kBands[gBandIdx];
  int span = b.maxKhz - b.minKhz;
  int khz = b.minKhz;
  if (tx <= kFreqTrkX) {
    khz = b.minKhz;
  } else if (tx >= kFreqTrkX + kFreqTrkW) {
    khz = b.maxKhz;
  } else {
    // Quantize to exact 1 kHz steps across the band
    khz = b.minKhz + ((tx - kFreqTrkX) * span + (kFreqTrkW / 2)) / kFreqTrkW;
  }
  setFrequencyKhz(khz, false);
  return true;
}

static bool handleVolumeTouch(int tx, int ty) {
  if (ty < kRow2Y || ty > kRow2Y + kRow2H || tx < kVolBoxX) return false;
  int step = (int)gMasterVolStep;
  if (tx < kVolTrkX) {
    step = 0;
  } else if (tx >= kVolTrkX + kVolTrkW) {
    step = 11;
  } else {
    step = ((tx - kVolTrkX) * 11 + (kVolTrkW / 2)) / kVolTrkW;
    if (step < 0) step = 0;
    if (step > 11) step = 11;
  }
  if ((uint8_t)step != gMasterVolStep) {
    gMasterVolStep = (uint8_t)step;
    applyHardwareVoiceVolumes(false);
    drawVolumeBar();
  }
  return true;
}

// ---------------------------------------------------------------------------
// Application Entry Point
// ---------------------------------------------------------------------------
void app_main(const LakatosApi* os) {
  gOs = os;
  os->set_rgb_led(true, false, false, 0);

  os->audio_start();
  drawFullUi();

  connectToKiwi();
  drawStatusBadge();
  drawSmeterPanel();

  bool        prevTouch       = false;
  bool        draggingSlider  = false;
  uint32_t    lastTouchDownMs = 0;
  uint32_t    lastBtnClickMs  = 0;
  uint32_t    lastUiMs        = os->millis();
  uint32_t    lastInfoMs      = os->millis();
  StreamState prevDrawnState  = gState;

  while (true) {
    pumpKiwiNetwork();

    // Auto-reconnect every 4s on error/disconnect
    uint32_t now = os->millis();
    if (gState == ST_ERROR && (uint32_t)(now - gLastReconnectMs) >= 4000u) {
      connectToKiwi();
    }

    if (gState != prevDrawnState) {
      drawStatusBadge();
      drawVfoPanel();
      drawSmeterPanel();
      prevDrawnState = gState;
    }

    if (os->serial_available && os->serial_available() > 0) {
      int ch = os->serial_read();
      if (ch == 0x03 || ch == 'q') break;
    }

    LakatosTouchSample ts = os->read_touch();
    if (ts.touched) {
      lastTouchDownMs = now;
      if ((!prevTouch || draggingSlider) && handleFreqSliderTouch(ts.x, ts.y)) {
        prevTouch      = true;
        draggingSlider = true;
      } else if ((!prevTouch || draggingSlider) && handleVolumeTouch(ts.x, ts.y)) {
        prevTouch      = true;
        draggingSlider = true;
      } else if (!prevTouch &&
                 (lastBtnClickMs == 0 || (uint32_t)(now - lastBtnClickMs) >= 220u)) {
        prevTouch      = true;
        draggingSlider = false;
        lastBtnClickMs = now;
        // 1. Top Header Bar (y <= 24)
        if (ts.y <= 24) {
          if (lakatos_is_exit_wedge(os, ts.x, ts.y)) {
            break; // EXIT
          } else if (ts.x < 250) {
            // Edit KiwiSDR Host:Port
            closeKiwiConnection();
            int r = os->prompt_keyboard("KIWISDR HOST:PORT", gKiwiHost, sizeof(gKiwiHost), false);
            os->set_rgb_led(true, false, false, 0);
            drawFullUi();
            if (r == 1 && gKiwiHost[0]) {
              connectToKiwi();
            } else {
              connectToKiwi();
            }
            drawStatusBadge();
            lastTouchDownMs = os->millis();
            lastBtnClickMs  = lastTouchDownMs;
          }
        }
        // 2. VFO Frequency Box tap -> direct frequency entry in kHz
        else if (ts.y >= 25 && ts.y <= 72 && ts.x <= 184) {
          char fstr[16];
          snprintf(fstr, sizeof(fstr), "%d", gFreqKhz);
          int r = os->prompt_keyboard("FREQUENCY IN KHZ:", fstr, sizeof(fstr), false);
          os->set_rgb_led(true, false, false, 0);
          if (r == 1 && fstr[0]) {
            int entered = parseIntSimple(fstr);
            if (entered >= 100 && entered <= 30000) {
              // Automatically switch band if entered frequency falls in 75m/40m/20m
              for (int b = 0; b < kNumBands; b++) {
                if (entered >= kBands[b].minKhz && entered <= kBands[b].maxKhz) {
                  gBandIdx = b;
                  gModeIdx = kBands[b].defaultMode;
                  break;
                }
              }
              gFreqKhz = entered;
              gBandFreqKhz[gBandIdx] = entered;
              sendKiwiTuning();
            }
          }
          drawFullUi();
          lastTouchDownMs = os->millis();
          lastBtnClickMs  = lastTouchDownMs;
        }
        // 3. Band & Step Row (y = 172..201)
        else if (ts.y >= kRow1Y && ts.y <= kRow1Y + kRow1H) {
          if (ts.x < 136) {
            int bIdx = (ts.x - 4) / 44;
            if (bIdx < 0) bIdx = 0;
            if (bIdx >= kNumBands) bIdx = kNumBands - 1;
            if (bIdx != gBandIdx) {
              gBandIdx = bIdx;
              gFreqKhz = gBandFreqKhz[gBandIdx];
              gModeIdx = gBandMode[gBandIdx];
              drawBandAndStepRow();
              drawModeButtons();
              drawVfoPanel();
              drawFreqSlider();
              sendKiwiTuning();
            }
          } else {
            int sIdx = (ts.x - 140) / 45;
            if (sIdx < 0) sIdx = 0;
            if (sIdx > 3) sIdx = 3;
            static const int kSteps[4] = { -10, -1, +1, +10 };
            setFrequencyKhz(gFreqKhz + kSteps[sIdx], true);
          }
        }
        // 4. Mode Buttons (y = 205..235, x < 126)
        else if (ts.y >= kRow2Y && ts.y <= kRow2Y + kRow2H && ts.x < 126) {
          int mIdx = (ts.x - 4) / 40;
          if (mIdx < 0) mIdx = 0;
          if (mIdx > 2) mIdx = 2;
          if (mIdx != gModeIdx) {
            gModeIdx = mIdx;
            gBandMode[gBandIdx] = mIdx;
            drawModeButtons();
            drawVfoPanel();
            sendKiwiTuning();
          }
        }
      }
    } else if (prevTouch && (uint32_t)(now - lastTouchDownMs) >= 90u) {
      if (gTunePending) {
        // Flush any pending frequency update immediately on debounced touch release
        sendKiwiTuning();
      }
      prevTouch      = false;
      draggingSlider = false;
    }

    // Refresh visualizers at ~28 FPS and S-meter / buffer bar at ~5 FPS
    if ((uint32_t)(now - lastUiMs) >= 35u) {
      lastUiMs = now;
      updateVisualizersIncremental();
    }
    if ((uint32_t)(now - lastInfoMs) >= 200u) {
      lastInfoMs = now;
      drawSmeterPanel();
    }

    os->yield();
  }

  closeKiwiConnection();
  os->audio_stop();
}
