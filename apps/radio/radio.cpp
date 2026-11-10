#include "lakatos_api.h"
#include "mp3dec.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// RGB565 Palette (Georgia Tech Gold & Navy + Retro Broadcast UI)
// ---------------------------------------------------------------------------
#define RGB565(r, g, b) \
  ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | (((b) & 0xF8) >> 3)))

static constexpr uint16_t kColBg         = RGB565(8, 12, 22);
static constexpr uint16_t kColHeader     = RGB565(16, 26, 48);
static constexpr uint16_t kColPanel      = RGB565(14, 20, 34);
static constexpr uint16_t kColPanelHi    = RGB565(24, 34, 56);
static constexpr uint16_t kColGridBorder = RGB565(42, 58, 90);
static constexpr uint16_t kColGtGold     = RGB565(238, 178, 17);
static constexpr uint16_t kColGtGoldBright = RGB565(255, 220, 80);
static constexpr uint16_t kColText       = RGB565(235, 242, 255);
static constexpr uint16_t kColSub        = RGB565(135, 155, 185);
static constexpr uint16_t kColAccent     = RGB565(0, 215, 255);
static constexpr uint16_t kColGreen      = RGB565(40, 230, 115);
static constexpr uint16_t kColYellow     = RGB565(255, 205, 50);
static constexpr uint16_t kColOrange     = RGB565(255, 130, 35);
static constexpr uint16_t kColRed        = RGB565(255, 65, 75);
static constexpr uint16_t kColScopeTrace = RGB565(60, 255, 160);

// ---------------------------------------------------------------------------
// Preset Radio Streams (6 Callsign Buttons)
// ---------------------------------------------------------------------------
struct RadioPreset {
  const char* label;
  const char* stationName;
  const char* location;
  const char* url;
  const char* fallbackUrl;
};

static const RadioPreset kPresets[] = {
  {
    "WREK",
    "WREK 91.1 FM",
    "Georgia Tech - Atlanta, GA",
    "http://streaming.wrek.org:8000/main/24kb.mp3",
    nullptr
  },
  {
    "WREK-2",
    "WREK 91.1 HD2",
    "Georgia Tech - Atlanta, GA",
    "http://streaming.wrek.org:8000/hd2/24kb.mp3",
    nullptr
  },
  {
    "WDAV",
    "WDAV 89.9 FM",
    "Davidson College - Davidson, NC",
    "http://audio-mp3.ibiblio.org:8000/wdav-24k",
    "http://audio-mp3.ibiblio.org:8000/wdav-56k"
  },
  {
    "WNCW",
    "WNCW 88.7 FM",
    "Isothermal CC - Spindale, NC",
    "http://audio-mp3.ibiblio.org:8000/wncw-24k",
    "https://audio-mp3.ibiblio.org/wncw-24k"
  },
  {
    "WFMU",
    "WFMU 91.1 FM",
    "Freeform - East Orange / Jersey City, NJ",
    "http://stream0.wfmu.org/freeform-32k",
    nullptr
  },
  {
    "WRVO",
    "WRVO Public Media",
    "SUNY Oswego - Oswego, NY",
    "http://wrvo-ice.streamguys.org/wrvo1-low",
    "http://playerservices.streamtheworld.com/api/livestream-redirect/WRVOHD2.mp3"
  },
  {
    "BBC",
    "BBC World Service",
    "BBC Broadcasting House - London, UK",
    "http://stream.live.vc.bbcmedia.co.uk/bbc_world_service",
    nullptr
  },
};
static constexpr int kNumPresets = 7;

// ---------------------------------------------------------------------------
// Audio & Network Ring Buffers
// ---------------------------------------------------------------------------
static constexpr uint32_t kPcmRingSize     = 16384u; // 1.048s @ 15,625 Hz (must be power of 2)
static constexpr uint32_t kPcmRingMask     = kPcmRingSize - 1u;
static constexpr uint32_t kPrebufferSamps  = 6144u;  // ~0.39s decoded audio before starting voice
static constexpr uint32_t kNetFifoSize     = 16384u; // 16 KB compressed MP3 jitter FIFO (power of 2)
static constexpr uint32_t kNetFifoMask     = kNetFifoSize - 1u;
static constexpr int      kMp3InBufSize    = 4096;   // Linear decode frame buffer

static const LakatosApi* gOs = nullptr;
static HMP3Decoder       gMp3Dec = nullptr;

// Static .bss buffers (fits within 32 KB app arena: 16 KB + 4 KB + 4.6 KB = ~25 KB)
static int8_t   gPcmRing[kPcmRingSize];
static uint8_t  gMp3InBuf[kMp3InBufSize];
static int16_t  gDecodedPcm[2304]; // Up to 1152 stereo samples per frame

// Heap-allocated 16 KB network jitter FIFO
static uint8_t* gNetFifo      = nullptr;
static uint32_t gNetFifoRead  = 0;
static uint32_t gNetFifoWrite = 0;
static int      gMp3InBytes   = 0;

// Core 0 <-> Core 1 lockless PCM ring indices
static volatile uint32_t gPcmReadIdx   = 0;
static volatile uint32_t gPcmWriteIdx  = 0;
static volatile bool     gAudioPlaying = false;
static volatile bool     gFirstTick    = true;

// Resampler state (16.16 fixed-point input sample accumulator + error feedback dither)
static uint32_t gResampPhaseFp16 = 0;
static int32_t  gDitherErr       = 0;

// Stream & Player State
enum StreamState {
  ST_DISCONNECTED = 0,
  ST_CONNECTING   = 1,
  ST_BUFFERING    = 2,
  ST_PLAYING      = 3,
  ST_PAUSED       = 4,
  ST_ERROR        = 5,
};

static StreamState gState             = ST_DISCONNECTED;
static int         gPresetIdx         = 0; // 0..6 = preset, 7 = custom URL
static char        gCustomUrl[128]    = "http://streaming.wrek.org:8000/main/24kb.mp3";
static char        gStationTitle[48]  = "Georgia Tech - Atlanta, GA";
static uint8_t     gMasterVolStep     = 8;  // 0..11 (0=silence, 8=100%, 9..11=overmodulated)
static uint32_t    gBytesReceived     = 0;
static uint32_t    gFramesDecoded     = 0;
static uint32_t    gLastReconnectMs   = 0;
static int         gStreamBitrate     = 24;
static int         gStreamSamprate    = 44100;
static int         gStreamChans       = 2;

// Real-time Visualizer State (16 frequency/energy bars + 140-point waveform scope)
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

// ---------------------------------------------------------------------------
// Volume Mapping (0..11 Slider -> 4 Lockstep Voices + Overdrive Pre-Gain)
// ---------------------------------------------------------------------------
static uint8_t getVoiceVolumeForStep(uint8_t step) {
  if (step == 0) return 0;
  if (step <= 8) {
    // 1..8 maps linearly up to 44 per voice (4 voices * 44 * 3 / 512 = 1.03x full-scale)
    return (uint8_t)((step * 44u) / 8u);
  }
  // 9..11 drives into overmodulation
  if (step == 9)  return 58u;
  if (step == 10) return 76u;
  return 96u; // step 11
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

// ---------------------------------------------------------------------------
// Core 0 Hardware Timer Callback (fires every 64 output samples @ 15,625 Hz)
// ---------------------------------------------------------------------------
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
// Stream Connection Management
// ---------------------------------------------------------------------------
static void closeCurrentStream() {
  muteHardwareVoices();
  if (gOs && gOs->http_stream_close) {
    gOs->http_stream_close();
  }
  gNetFifoRead     = 0;
  gNetFifoWrite    = 0;
  gMp3InBytes      = 0;
  gPcmReadIdx      = 0;
  gPcmWriteIdx     = 0;
  gResampPhaseFp16 = 0;
  gDitherErr       = 0;
  for (uint32_t i = 0; i < kPcmRingSize; i++) gPcmRing[i] = 0;
}

static bool connectToStream(const char* url, const char* fallbackUrl, const char* title) {
  closeCurrentStream();
  if (title && title[0]) {
    int i = 0;
    while (title[i] && i < (int)sizeof(gStationTitle) - 1) {
      gStationTitle[i] = title[i];
      i++;
    }
    gStationTitle[i] = '\0';
  }
  if (!gOs->wifi_is_connected()) {
    gState = ST_ERROR;
    return false;
  }
  gState = ST_CONNECTING;
  serialLogf("[RADIO] Connecting to %s ...\r\n", url);
  if (!gOs->http_stream_open(url)) {
    if (fallbackUrl && fallbackUrl[0]) {
      serialLogf("[RADIO] Primary failed, trying fallback %s ...\r\n", fallbackUrl);
      if (gOs->http_stream_open(fallbackUrl)) {
        gOs->set_rgb_led(true, false, false, 0);
        gState = ST_BUFFERING;
        return true;
      }
    }
    serialLogf("[RADIO] Connection failed: %s\r\n", url);
    gState = ST_ERROR;
    gLastReconnectMs = gOs->millis();
    return false;
  }
  // Keep SC8002B speaker amplifier enabled on GPIO 4
  gOs->set_rgb_led(true, false, false, 0);
  gState = ST_BUFFERING;
  return true;
}

static bool connectToPreset(int idx) {
  if (idx < 0 || idx >= kNumPresets) return false;
  gPresetIdx = idx;
  return connectToStream(kPresets[idx].url, kPresets[idx].fallbackUrl, kPresets[idx].location);
}

// ---------------------------------------------------------------------------
// Network FIFO & MP3 Frame Decoding + Anti-Aliased Downsampler
// ---------------------------------------------------------------------------
static void pumpNetworkToFifo() {
  if (!gNetFifo || (gState != ST_BUFFERING && gState != ST_PLAYING)) return;

  uint8_t tmp[1024];
  for (int iter = 0; iter < 4; iter++) {
    uint32_t used = gNetFifoWrite - gNetFifoRead;
    if (used >= kNetFifoSize - 256u) break;
    uint32_t space = (kNetFifoSize - 1u) - used;
    size_t toRead = (space < sizeof(tmp)) ? (size_t)space : sizeof(tmp);
    int32_t got = gOs->http_stream_read(tmp, toRead);
    if (got > 0) {
      uint32_t w = gNetFifoWrite;
      for (int32_t i = 0; i < got; i++) {
        gNetFifo[(w + (uint32_t)i) & kNetFifoMask] = tmp[i];
      }
      gNetFifoWrite = w + (uint32_t)got;
      gBytesReceived += (uint32_t)got;
    } else if (got < 0) {
      // Disconnected
      muteHardwareVoices();
      gState = ST_ERROR;
      gLastReconnectMs = gOs->millis();
      break;
    } else {
      break; // No more bytes currently waiting in socket
    }
  }
}

static void refillMp3LinearBuffer() {
  if (!gNetFifo) return;
  while (gMp3InBytes < kMp3InBufSize && gNetFifoRead != gNetFifoWrite) {
    gMp3InBuf[gMp3InBytes++] = gNetFifo[gNetFifoRead & kNetFifoMask];
    gNetFifoRead++;
  }
}

static void updateVisualizerFromFrame(const int16_t* pcm, int numFrames, int nChans) {
  if (numFrames <= 0) return;

  // Compute 16 sub-segment energy + zero-crossing pseudo-spectrum bars across the frame
  int segLen = numFrames / kNumBars;
  if (segLen < 8) segLen = 8;

  int32_t prevSample = 0;
  for (int b = 0; b < kNumBars; b++) {
    int start = (b * numFrames) / kNumBars;
    int32_t sumAbs = 0;
    int32_t sumDiff = 0;
    for (int i = 0; i < segLen; i++) {
      int idx = (start + i) * nChans;
      int32_t s = pcm[idx];
      if (nChans == 2) s = (s + pcm[idx + 1]) >> 1;
      int32_t a = (s < 0) ? -s : s;
      int32_t d = s - prevSample;
      if (d < 0) d = -d;
      prevSample = s;
      sumAbs += a;
      sumDiff += d;
    }
    int32_t avgLow  = sumAbs / segLen;
    int32_t avgHigh = sumDiff / segLen;
    // Blend low-frequency envelope (left bars) to high-frequency derivative energy (right bars)
    int32_t energy = ((avgLow * (kNumBars - b)) + (avgHigh * b * 2)) / kNumBars;
    int32_t lvl = energy >> 5; // 0..255
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

static void pushDecodedFrameToRing(const int16_t* pcm, int totalSamps, int nChans, int samprate) {
  if (totalSamps <= 0 || nChans <= 0 || samprate <= 0) return;
  int numFrames = totalSamps / nChans;
  if (numFrames <= 0) return;

  updateVisualizerFromFrame(pcm, numFrames, nChans);

  // Digital pre-gain for overmodulation steps 9..11
  int32_t preGainNum = 4; // 4/4 = 1.0x
  if (gMasterVolStep == 9)       preGainNum = 5; // 1.25x
  else if (gMasterVolStep == 10) preGainNum = 6; // 1.50x
  else if (gMasterVolStep >= 11) preGainNum = 8; // 2.00x

  // Step size in 16.16 fixed-point input frames per 15,625 Hz output sample
  uint32_t stepFp16 = (uint32_t)(((uint64_t)samprate << 16) / (uint64_t)LAKATOS_AUDIO_RATE);
  if (stepFp16 == 0) stepFp16 = 65536u;

  uint32_t phase = gResampPhaseFp16;
  uint32_t endFp16 = (uint32_t)numFrames << 16;
  uint32_t w = gPcmWriteIdx;
  int32_t  err = gDitherErr;

  if (nChans == 2) {
    while (phase < endFp16) {
      int idx0 = (int)(phase >> 16);
      int idx1 = (idx0 + 1 < numFrames) ? (idx0 + 1) : idx0;
      int idx2 = (idx0 + 2 < numFrames) ? (idx0 + 2) : idx1;

      // 3-tap box anti-aliasing filter (1:2:1 weighted average across ~2.82 input samples)
      int32_t m0 = (int32_t)pcm[idx0 * 2] + (int32_t)pcm[idx0 * 2 + 1];
      int32_t m1 = (int32_t)pcm[idx1 * 2] + (int32_t)pcm[idx1 * 2 + 1];
      int32_t m2 = (int32_t)pcm[idx2 * 2] + (int32_t)pcm[idx2 * 2 + 1];
      int32_t mono16 = (m0 + (m1 << 1) + m2) >> 3; // [-32768 .. 32767]

      if (preGainNum > 4) {
        mono16 = (mono16 * preGainNum) >> 2;
      }

      // First-order error-feedback noise shaping for 8-bit DAC clarity
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
  } else {
    while (phase < endFp16) {
      int idx0 = (int)(phase >> 16);
      int idx1 = (idx0 + 1 < numFrames) ? (idx0 + 1) : idx0;
      int idx2 = (idx0 + 2 < numFrames) ? (idx0 + 2) : idx1;

      int32_t mono16 = ((int32_t)pcm[idx0] + ((int32_t)pcm[idx1] << 1) + (int32_t)pcm[idx2]) >> 2;
      if (preGainNum > 4) {
        mono16 = (mono16 * preGainNum) >> 2;
      }

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
  }

  gPcmWriteIdx     = w;
  gResampPhaseFp16 = phase - endFp16;
  gDitherErr       = err;
}

static void decodeAvailableFrames() {
  if (!gMp3Dec || (gState != ST_BUFFERING && gState != ST_PLAYING)) return;

  for (int decodedCount = 0; decodedCount < 4; decodedCount++) {
    uint32_t pcmBuffered = gPcmWriteIdx - gPcmReadIdx;
    if (pcmBuffered >= kPcmRingSize - 1024u) {
      break; // PCM ring buffer is nearly full
    }

    refillMp3LinearBuffer();
    if (gMp3InBytes < 256) break;

    int syncOff = MP3FindSyncWord(gMp3InBuf, gMp3InBytes);
    if (syncOff < 0) {
      // Keep last byte in case it is 0xFF
      gMp3InBuf[0] = gMp3InBuf[gMp3InBytes - 1];
      gMp3InBytes = 1;
      refillMp3LinearBuffer();
      break;
    }
    if (syncOff > 0) {
      int rem = gMp3InBytes - syncOff;
      for (int i = 0; i < rem; i++) gMp3InBuf[i] = gMp3InBuf[syncOff + i];
      gMp3InBytes = rem;
      refillMp3LinearBuffer();
    }

    if (gMp3InBytes < 640) {
      // Wait for at least 640 bytes so a complete frame + next sync header is present
      break;
    }

    // Validate MPEG-1/2 Layer III frame header + next frame sync word before calling MP3Decode
    MP3FrameInfo nextFi;
    int hdrErr = MP3GetNextFrameInfo(gMp3Dec, &nextFi, gMp3InBuf);
    if (hdrErr != ERR_MP3_NONE || nextFi.layer != 3 || nextFi.samprate <= 0 || nextFi.bitrate <= 0) {
      for (int i = 0; i < gMp3InBytes - 1; i++) gMp3InBuf[i] = gMp3InBuf[i + 1];
      gMp3InBytes--;
      continue;
    }
    int coeff = (nextFi.version == MPEG1) ? 144 : 72;
    int pad   = (gMp3InBuf[2] >> 1) & 1;
    int expectedFrameBytes = (coeff * nextFi.bitrate) / nextFi.samprate + pad;
    if (expectedFrameBytes < 24 || expectedFrameBytes > 1440) {
      for (int i = 0; i < gMp3InBytes - 1; i++) gMp3InBuf[i] = gMp3InBuf[i + 1];
      gMp3InBytes--;
      continue;
    }
    if (expectedFrameBytes + 2 <= gMp3InBytes) {
      if (gMp3InBuf[expectedFrameBytes] != 0xFF ||
          (gMp3InBuf[expectedFrameBytes + 1] & 0xF0) != 0xF0) {
        // False sync word in mid-frame data: skip 1 byte and rescan
        for (int i = 0; i < gMp3InBytes - 1; i++) gMp3InBuf[i] = gMp3InBuf[i + 1];
        gMp3InBytes--;
        continue;
      }
    } else {
      break; // Wait until next sync word is in buffer
    }

    unsigned char* readPtr = gMp3InBuf;
    int bytesLeft = gMp3InBytes;
    int err = MP3Decode(gMp3Dec, &readPtr, &bytesLeft, gDecodedPcm, 0);

    if (err == ERR_MP3_NONE) {
      int consumed = (int)(readPtr - gMp3InBuf);
      if (consumed > 0 && bytesLeft >= 0) {
        for (int i = 0; i < bytesLeft; i++) gMp3InBuf[i] = readPtr[i];
        gMp3InBytes = bytesLeft;
      }
      MP3FrameInfo fi;
      MP3GetLastFrameInfo(gMp3Dec, &fi);
      if (fi.samprate > 0 && fi.outputSamps > 0) {
        gStreamBitrate  = fi.bitrate / 1000;
        gStreamSamprate = fi.samprate;
        gStreamChans    = fi.nChans;
        pushDecodedFrameToRing(gDecodedPcm, fi.outputSamps, fi.nChans, fi.samprate);
        gFramesDecoded++;
      }
    } else if (err == ERR_MP3_INDATA_UNDERFLOW) {
      break;
    } else if (err == ERR_MP3_MAINDATA_UNDERFLOW) {
      // Bit reservoir warming up on stream join: advance past frame header/slots
      int consumed = (int)(readPtr - gMp3InBuf);
      if (consumed > 0 && bytesLeft >= 0) {
        for (int i = 0; i < bytesLeft; i++) gMp3InBuf[i] = readPtr[i];
        gMp3InBytes = bytesLeft;
      } else {
        for (int i = 0; i < gMp3InBytes - 1; i++) gMp3InBuf[i] = gMp3InBuf[i + 1];
        gMp3InBytes--;
      }
    } else {
      // False sync word or corrupted frame: skip 1 byte and rescan
      for (int i = 0; i < gMp3InBytes - 1; i++) gMp3InBuf[i] = gMp3InBuf[i + 1];
      gMp3InBytes--;
    }
  }

  // Manage buffering <-> playing transitions
  uint32_t pcmBuffered = gPcmWriteIdx - gPcmReadIdx;
  if (gState == ST_BUFFERING) {
    if (pcmBuffered >= kPrebufferSamps) {
      gState = ST_PLAYING;
      startLockstepPlayback();
      serialLogf("[RADIO] Playing! (%d kbps, %d Hz, ch=%d, buf=%lu samps)\r\n",
                 gStreamBitrate, gStreamSamprate, gStreamChans, (unsigned long)pcmBuffered);
    }
  } else if (gState == ST_PLAYING) {
    // Check for underrun
    if ((int32_t)(gPcmWriteIdx - gPcmReadIdx) < 64) {
      muteHardwareVoices();
      gPcmReadIdx  = 0;
      gPcmWriteIdx = 0;
      gState       = ST_BUFFERING;
    }
  }
}

// ---------------------------------------------------------------------------
// UI Rendering (320x240 Landscape)
// ---------------------------------------------------------------------------
static constexpr int kVolBarY = 156;
static constexpr int kVolBarH = 22;
static constexpr int kVolTrkX = 48;
static constexpr int kVolTrkW = 198;

static void drawStatusBadge() {
  const char* txt = "OFFLINE";
  uint16_t bg = RGB565(80, 25, 35);
  uint16_t fg = kColText;

  if (gState == ST_PLAYING) {
    txt = "ON AIR";
    bg  = RGB565(20, 130, 65);
  } else if (gState == ST_BUFFERING || gState == ST_CONNECTING) {
    txt = "BUFFER";
    bg  = RGB565(150, 105, 15);
  } else if (gState == ST_PAUSED) {
    txt = "PAUSED";
    bg  = RGB565(50, 65, 95);
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

  // Standard NW Exit X mark on Top Left (sized via Lakatos corner size)
  lakatos_draw_exit_wedge(gOs);

  // Station Name in Top Bar
  const char* stName = (gPresetIdx >= 0 && gPresetIdx < kNumPresets)
                           ? kPresets[gPresetIdx].stationName
                           : "Custom Stream";
  gOs->tft_set_text_datum(LAKATOS_ML_DATUM);
  gOs->tft_set_text_color(kColGtGoldBright, kColHeader);
  gOs->tft_draw_string(stName, cs + 6, 11, 2);

  drawStatusBadge();
  gOs->tft_set_text_datum(LAKATOS_TL_DATUM);
}

static void drawInfoPanel() {
  gOs->tft_fill_rect(4, 25, 312, 31, kColPanel);
  gOs->tft_draw_rect(4, 25, 312, 31, kColGridBorder);

  gOs->tft_set_text_datum(LAKATOS_TL_DATUM);
  gOs->tft_set_text_color(kColText, kColPanel);
  gOs->tft_draw_string(gStationTitle, 10, 27, 2);

  char spec[64];
  uint32_t pcmBuffered = (gPcmWriteIdx >= gPcmReadIdx) ? (gPcmWriteIdx - gPcmReadIdx) : 0;
  int bufPct = (int)((pcmBuffered * 100u) / kPcmRingSize);
  if (bufPct > 100) bufPct = 100;

  snprintf(spec, sizeof(spec), "MP3 %dk  %dHz %s | RX:%luKB",
           gStreamBitrate, gStreamSamprate,
           (gStreamChans == 2) ? "J-ST" : "MONO",
           (unsigned long)(gBytesReceived >> 10));
  gOs->tft_set_text_color(kColAccent, kColPanel);
  gOs->tft_draw_string(spec, 10, 44, 1);

  // Buffer health bar on right of info panel
  int bx = 228;
  int by = 44;
  int bw = 80;
  int bh = 8;
  gOs->tft_fill_rect(bx, by, bw, bh, RGB565(8, 12, 20));
  gOs->tft_draw_rect(bx, by, bw, bh, kColGridBorder);
  int fillW = ((bw - 2) * bufPct) / 100;
  uint16_t bCol = (bufPct > 35) ? kColGreen : ((bufPct > 15) ? kColYellow : kColRed);
  if (fillW > 0) {
    gOs->tft_fill_rect(bx + 1, by + 1, fillW, bh - 2, bCol);
  }
}

static void drawVisualizerFrames() {
  // Left Box: 16-Band Spectrum / Energy Analyzer (x=4..158, y=59..153)
  gOs->tft_fill_rect(4, 59, 154, 94, kColPanel);
  gOs->tft_draw_rect(4, 59, 154, 94, kColGridBorder);
  gOs->tft_set_text_datum(LAKATOS_TL_DATUM);
  gOs->tft_set_text_color(kColSub, kColPanel);
  gOs->tft_draw_string("SPECTRUM / LEVEL", 10, 62, 1);

  // Right Box: Live Waveform Oscilloscope (x=162..316, y=59..153)
  gOs->tft_fill_rect(162, 59, 154, 94, kColPanel);
  gOs->tft_draw_rect(162, 59, 154, 94, kColGridBorder);
  gOs->tft_set_text_color(kColSub, kColPanel);
  gOs->tft_draw_string("WAVEFORM SCOPE", 168, 62, 1);
  gOs->tft_draw_fast_h_line(166, 110, kScopeCols, RGB565(25, 38, 62));

  for (int i = 0; i < kNumBars; i++) {
    gPrevBarH[i]  = -1;
    gPrevPeakY[i] = -1;
  }
  for (int i = 0; i < kScopeCols; i++) {
    gPrevScopeY[i] = -1;
  }
}

static void updateVisualizersIncremental() {
  // 1. Update 16 Spectrum Bars (x=8..152, bottom=150, maxH=74)
  const int baseBottom = 150;
  const int maxBarH    = 74;

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
        // Segmented green -> gold -> red bar
        int lowH = (h > 44) ? 44 : h;
        gOs->tft_fill_rect(bx, baseBottom - lowH, bw, lowH, kColGreen);
        if (h > 44) {
          int midH = (h > 60) ? 16 : (h - 44);
          gOs->tft_fill_rect(bx, baseBottom - 44 - midH, bw, midH, kColGtGold);
        }
        if (h > 60) {
          int hiH = h - 60;
          gOs->tft_fill_rect(bx, baseBottom - h, bw, hiH, kColRed);
        }
      }
      gPrevBarH[b] = (int16_t)h;
    }

    // Peak hold dot
    int pkH = ((int)gBarPeak[b] * maxBarH) >> 8;
    if (pkH > maxBarH) pkH = maxBarH;
    int pkY = baseBottom - pkH - 2;
    if (pkY < 73) pkY = 73;
    if (gPrevPeakY[b] >= 73 && gPrevPeakY[b] != pkY && gPrevPeakY[b] < baseBottom - h) {
      gOs->tft_draw_fast_h_line(bx, gPrevPeakY[b], bw, kColPanel);
    }
    if (pkH > 2) {
      gOs->tft_draw_fast_h_line(bx, pkY, bw, kColGtGoldBright);
      gPrevPeakY[b] = (int16_t)pkY;
    }

    // Decay bar & peak
    if (gBarLevel[b] > 6) gBarLevel[b] -= 6;
    else gBarLevel[b] = 0;
    if (gBarPeak[b] > 2) gBarPeak[b] -= 2;
    else gBarPeak[b] = 0;
  }

  // 2. Update Live Oscilloscope Trace (x=166..309, center Y=110, 2x Y-scale)
  uint32_t rBase = gPcmReadIdx;
  const int scopeX0  = 166;
  const int centerY  = 110;

  for (int x = 0; x < kScopeCols; x++) {
    int8_t s = 0;
    if (gState == ST_PLAYING && gMasterVolStep > 0) {
      s = gPcmRing[(rBase + (uint32_t)(x * 2)) & kPcmRingMask];
    }
    int y = centerY - (((int)s * 72) >> 7);
    if (y < 73) y = 73;
    if (y > 149) y = 149;

    int prevY = gPrevScopeY[x];
    if (prevY >= 73 && prevY != y) {
      gOs->tft_fill_rect(scopeX0 + x, prevY - 1, 1, 3, kColPanel);
      if (prevY - 1 <= centerY && centerY <= prevY + 1) {
        gOs->tft_fill_rect(scopeX0 + x, centerY, 1, 1, RGB565(25, 38, 62));
      }
    }
    gOs->tft_fill_rect(scopeX0 + x, y - 1, 1, 3, kColScopeTrace);
    gPrevScopeY[x] = (int16_t)y;
  }
}

static void drawVolumeSliderBar() {
  gOs->tft_fill_rect(4, kVolBarY, 312, kVolBarH, kColPanel);
  gOs->tft_draw_rect(4, kVolBarY, 312, kVolBarH, kColGridBorder);

  gOs->tft_set_text_datum(LAKATOS_ML_DATUM);
  gOs->tft_set_text_color(kColSub, kColPanel);
  gOs->tft_draw_string("VOL", 10, kVolBarY + kVolBarH / 2, 2);

  int trkY = kVolBarY + 5;
  int trkH = kVolBarH - 10;
  gOs->tft_fill_round_rect(kVolTrkX, trkY, kVolTrkW, trkH, 4, RGB565(8, 12, 22));
  gOs->tft_draw_round_rect(kVolTrkX, trkY, kVolTrkW, trkH, 4, kColGridBorder);

  int segW = kVolTrkW / 11; // 18 px per step
  for (int i = 1; i <= 11; i++) {
    int sx = kVolTrkX + (i - 1) * segW + 2;
    int sw = segW - 3;
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
  gOs->tft_draw_fast_v_line(unityX, kVolBarY + 2, kVolBarH - 4, kColText);

  // Knob thumb
  int knobX = kVolTrkX + (int)gMasterVolStep * segW;
  if (knobX < kVolTrkX + 3) knobX = kVolTrkX + 3;
  if (knobX > kVolTrkX + kVolTrkW - 3) knobX = kVolTrkX + kVolTrkW - 3;
  uint16_t knobCol = (gMasterVolStep > 8) ? kColRed
                   : ((gMasterVolStep == 0) ? kColSub : kColAccent);
  gOs->tft_fill_round_rect(knobX - 3, kVolBarY + 2, 7, kVolBarH - 4, 2, knobCol);
  gOs->tft_draw_round_rect(knobX - 3, kVolBarY + 2, 7, kVolBarH - 4, 2, kColText);

  // Numeric readout (0..11)
  char vbuf[16];
  uint16_t txtCol = kColGreen;
  if (gMasterVolStep == 0) {
    snprintf(vbuf, sizeof(vbuf), "0 MUTE");
    txtCol = kColSub;
  } else if (gMasterVolStep > 8) {
    snprintf(vbuf, sizeof(vbuf), "%u OVER", (unsigned)gMasterVolStep);
    txtCol = (gMasterVolStep == 11) ? kColRed : kColOrange;
  } else {
    snprintf(vbuf, sizeof(vbuf), "%u / 11", (unsigned)gMasterVolStep);
    txtCol = (gMasterVolStep == 8) ? kColYellow : kColGreen;
  }
  gOs->tft_set_text_datum(LAKATOS_MC_DATUM);
  gOs->tft_set_text_color(txtCol, kColPanel);
  gOs->tft_draw_string(vbuf, 280, kVolBarY + kVolBarH / 2, 2);
  gOs->tft_set_text_datum(LAKATOS_TL_DATUM);
}

static bool handleVolumeTouch(int tx, int ty) {
  if (ty < kVolBarY - 2 || ty > kVolBarY + kVolBarH + 2) return false;
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
    drawVolumeSliderBar();
  }
  return true;
}

static void drawBottomButtons() {
  // 8 buttons arranged in 2 rows of 4 buttons (y = 181..236)
  const int btnW = 74;
  const int btnH = 26;
  const int gapX = 4;

  gOs->tft_set_text_datum(LAKATOS_MC_DATUM);
  for (int i = 0; i < 8; i++) {
    int row = i / 4;
    int col = i % 4;
    int bx  = 6 + col * (btnW + gapX);
    int by  = (row == 0) ? 181 : 210;

    bool active = (gPresetIdx == i);
    const char* label = (i < kNumPresets) ? kPresets[i].label : "URL...";
    uint16_t baseBg = (i < kNumPresets) ? RGB565(22, 36, 62) : RGB565(45, 32, 75);
    uint16_t bg     = active ? RGB565(55, 45, 12) : baseBg;
    uint16_t border = active ? kColGtGoldBright   : kColGridBorder;
    uint16_t fg     = active ? kColGtGoldBright   : kColText;

    gOs->tft_fill_round_rect(bx, by, btnW, btnH, 4, bg);
    gOs->tft_draw_round_rect(bx, by, btnW, btnH, 4, border);
    gOs->tft_set_text_color(fg, bg);
    gOs->tft_draw_string(label, bx + btnW / 2, by + btnH / 2, 2);
  }
  gOs->tft_set_text_datum(LAKATOS_TL_DATUM);
}

static void drawFullUi() {
  gOs->tft_fill_screen(kColBg);
  drawHeaderBar();
  drawInfoPanel();
  drawVisualizerFrames();
  drawVolumeSliderBar();
  drawBottomButtons();
}

// ---------------------------------------------------------------------------
// Application Entry Point
// ---------------------------------------------------------------------------
void app_main(const LakatosApi* os) {
  gOs = os;
  os->set_rgb_led(true, false, false, 0);

  gNetFifo = (uint8_t*)os->malloc(kNetFifoSize);
  gMp3Dec  = MP3InitDecoder();

  os->audio_start();
  drawFullUi();

  // Connect to default WREK 91.1 FM 24kbps stream immediately
  connectToPreset(0);
  drawStatusBadge();
  drawInfoPanel();

  bool        prevTouch       = false;
  bool        draggingSlider  = false;
  uint32_t    lastTouchDownMs = 0;
  uint32_t    lastBtnClickMs  = 0;
  uint32_t    lastUiMs        = os->millis();
  uint32_t    lastInfoMs      = os->millis();
  StreamState prevDrawnState  = gState;

  while (true) {
    // 1. Pump incoming HTTP stream bytes & decode MP3 frames
    pumpNetworkToFifo();
    decodeAvailableFrames();

    // Auto-reconnect on transient network drop (every 4s)
    uint32_t now = os->millis();
    if (gState == ST_ERROR && (uint32_t)(now - gLastReconnectMs) >= 4000u) {
      if (gPresetIdx >= 0 && gPresetIdx < kNumPresets) {
        connectToPreset(gPresetIdx);
      } else {
        connectToStream(gCustomUrl, nullptr, gCustomUrl);
      }
    }

    if (gState != prevDrawnState) {
      drawStatusBadge();
      drawBottomButtons();
      prevDrawnState = gState;
    }

    if (os->serial_available && os->serial_available() > 0) {
      int ch = os->serial_read();
      if (ch == 0x03 || ch == 'q') break;
    }

    // 2. Poll touchscreen
    LakatosTouchSample ts = os->read_touch();
    if (ts.touched) {
      lastTouchDownMs = now;
      if ((!prevTouch || draggingSlider) && handleVolumeTouch(ts.x, ts.y)) {
        prevTouch      = true;
        draggingSlider = true;
      } else if (!prevTouch &&
                 (lastBtnClickMs == 0 || (uint32_t)(now - lastBtnClickMs) >= 220u)) {
        prevTouch      = true;
        draggingSlider = false;
        lastBtnClickMs = now;
        // NW Exit X (top left)
        if (lakatos_is_exit_wedge(os, ts.x, ts.y)) {
          break;
        }
        // Bottom 2x4 button grid (y = 181..238)
        if (ts.y >= 180) {
          int row = (ts.y >= 209) ? 1 : 0;
          int col = (ts.x - 6) / 78;
          if (col < 0) col = 0;
          if (col > 3) col = 3;
          int idx = row * 4 + col;
          if (idx < kNumPresets) {
            gPresetIdx = idx;
            drawHeaderBar();
            drawBottomButtons();
            drawInfoPanel();
            connectToPreset(idx);
            drawStatusBadge();
            drawInfoPanel();
          } else {
            // 8th button: Custom URL keyboard prompt
            closeCurrentStream();
            int r = os->prompt_keyboard("STREAM URL (HTTP MP3):", gCustomUrl, sizeof(gCustomUrl), false);
            os->set_rgb_led(true, false, false, 0);
            if (r == 1 && gCustomUrl[0]) {
              gPresetIdx = 7;
              drawFullUi();
              connectToStream(gCustomUrl, nullptr, gCustomUrl);
            } else {
              drawFullUi();
              if (gPresetIdx >= 0 && gPresetIdx < kNumPresets) {
                connectToPreset(gPresetIdx);
              } else {
                connectToStream(gCustomUrl, nullptr, gCustomUrl);
              }
            }
            drawStatusBadge();
            drawInfoPanel();
          }
          lastTouchDownMs = os->millis();
          lastBtnClickMs  = lastTouchDownMs;
        }
      }
    } else if (prevTouch && (uint32_t)(now - lastTouchDownMs) >= 90u) {
      prevTouch      = false;
      draggingSlider = false;
    }

    // 3. Refresh visualizers at ~28 FPS and info panel at ~4 FPS
    if ((uint32_t)(now - lastUiMs) >= 35u) {
      lastUiMs = now;
      updateVisualizersIncremental();
    }
    if ((uint32_t)(now - lastInfoMs) >= 250u) {
      lastInfoMs = now;
      drawInfoPanel();
    }

    os->yield();
  }

  closeCurrentStream();
  if (gMp3Dec) {
    MP3FreeDecoder(gMp3Dec);
    gMp3Dec = nullptr;
  }
  os->audio_stop();
}
