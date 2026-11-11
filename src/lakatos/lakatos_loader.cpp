#include "lakatos_state.h"

#include <driver/ledc.h>
#include <esp_heap_caps.h>
#include <esp_heap_caps_init.h>
#include <esp_intr_alloc.h>
#include <esp_partition.h>
#include <esp_timer.h>
#include <mbedtls/sha256.h>
#include <setjmp.h>
#include <soc/interrupts.h>
#include <soc/ledc_struct.h>
#include <soc/timer_group_struct.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include <miniz.h>
}

#include "esp32/rom/cache.h"
#include "esp_private/cache_utils.h"

// ---------------------------------------------------------------------------
// Fixed-Address 32 KB Static Application DRAM Arena (0x3FFC8000 .. 0x3FFCFFFF)
// Placed in .noinit right after _data_end (~0x3FFC4057) and aligned to 0x8000.
// ---------------------------------------------------------------------------
uint8_t gLakatosAppRamArena[LAKATOS_APP_DRAM_SIZE]
    __attribute__((section(".noinit"), aligned(0x8000), used));

uint8_t* gLakatosPngScratch     = nullptr;
size_t   gLakatosPngScratchSize = 0;

static const esp_partition_t* gAppSlotPart = nullptr;
static jmp_buf gAppExitJmp;
static volatile bool gAppRunning = false;
static volatile int gAppLastExitCode = 0;

bool lakatosIsNativeAppRunning() {
  return gAppRunning;
}

// ---------------------------------------------------------------------------
// App Dynamic Heap Tracker (automatically freed when app exits)
// ---------------------------------------------------------------------------
static constexpr int kMaxTrackedAllocs = 128;
static void* gTrackedAllocs[kMaxTrackedAllocs];

static void recordAlloc(void* ptr) {
  if (!ptr) return;
  for (int i = 0; i < kMaxTrackedAllocs; i++) {
    if (!gTrackedAllocs[i]) {
      gTrackedAllocs[i] = ptr;
      return;
    }
  }
}

static void forgetAlloc(void* ptr) {
  if (!ptr) return;
  for (int i = 0; i < kMaxTrackedAllocs; i++) {
    if (gTrackedAllocs[i] == ptr) {
      gTrackedAllocs[i] = nullptr;
      return;
    }
  }
}

static void cleanupAppHeap() {
  for (int i = 0; i < kMaxTrackedAllocs; i++) {
    if (gTrackedAllocs[i]) {
      free(gTrackedAllocs[i]);
      gTrackedAllocs[i] = nullptr;
    }
  }
}

// ---------------------------------------------------------------------------
// LakatosApi Syscall Implementations
// ---------------------------------------------------------------------------
static void api_exit(int code) {
  gAppLastExitCode = code;
  if (gAppRunning) {
    longjmp(gAppExitJmp, (code == 0) ? 256 : code);
  }
}

static void api_yield(void) {
  if (gAppRunning && lakatosShouldExitApp()) {
    api_exit(0);
  }
  updateLakatosConfigBackground();
  lakatosUpdateRgbLed();
  yield();
}

static uint32_t api_millis(void) {
  return millis();
}

static uint32_t api_micros(void) {
  return micros();
}

static void api_delay_ms(uint32_t ms) {
  uint32_t start = millis();
  while ((uint32_t)(millis() - start) < ms) {
    if (gAppRunning && lakatosShouldExitApp()) {
      api_exit(0);
    }
    updateLakatosConfigBackground();
    lakatosUpdateRgbLed();
    uint32_t rem = ms - (uint32_t)(millis() - start);
    delay(rem > 10 ? 10 : rem);
  }
}

static void* api_malloc(size_t size) {
  void* p = malloc(size);
  recordAlloc(p);
  return p;
}

static void* api_calloc(size_t nmemb, size_t size) {
  void* p = calloc(nmemb, size);
  recordAlloc(p);
  return p;
}

static void* api_realloc(void* ptr, size_t size) {
  if (!ptr) return api_malloc(size);
  uintptr_t oldAddr = (uintptr_t)ptr;
  void* p = realloc(ptr, size);
  if (p) {
    forgetAlloc((void*)oldAddr);
    recordAlloc(p);
  }
  return p;
}

static void api_free(void* ptr) {
  if (!ptr) return;
  forgetAlloc(ptr);
  free(ptr);
}

static size_t api_free_heap_bytes(void) {
  return ESP.getFreeHeap();
}

static void api_tft_fill_screen(uint16_t color) {
  LakatosTftGuard tftLock;
  tft.fillScreen(color);
}

static void api_tft_fill_rect(int x, int y, int w, int h, uint16_t color) {
  LakatosTftGuard tftLock;
  tft.fillRect(x, y, w, h, color);
}

static void api_tft_draw_rect(int x, int y, int w, int h, uint16_t color) {
  LakatosTftGuard tftLock;
  tft.drawRect(x, y, w, h, color);
}

static void api_tft_fill_round_rect(int x, int y, int w, int h, int r, uint16_t color) {
  LakatosTftGuard tftLock;
  tft.fillRoundRect(x, y, w, h, r, color);
}

static void api_tft_draw_round_rect(int x, int y, int w, int h, int r, uint16_t color) {
  LakatosTftGuard tftLock;
  tft.drawRoundRect(x, y, w, h, r, color);
}

static void api_tft_draw_line(int x0, int y0, int x1, int y1, uint16_t color) {
  LakatosTftGuard tftLock;
  tft.drawLine(x0, y0, x1, y1, color);
}

static void api_tft_draw_fast_h_line(int x, int y, int w, uint16_t color) {
  LakatosTftGuard tftLock;
  tft.drawFastHLine(x, y, w, color);
}

static void api_tft_draw_fast_v_line(int x, int y, int h, uint16_t color) {
  LakatosTftGuard tftLock;
  tft.drawFastVLine(x, y, h, color);
}

static void api_tft_fill_circle(int x, int y, int r, uint16_t color) {
  LakatosTftGuard tftLock;
  tft.fillCircle(x, y, r, color);
}

static void api_tft_draw_circle(int x, int y, int r, uint16_t color) {
  LakatosTftGuard tftLock;
  tft.drawCircle(x, y, r, color);
}

static void api_tft_fill_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t color) {
  LakatosTftGuard tftLock;
  tft.fillTriangle(x0, y0, x1, y1, x2, y2, color);
}

static void api_tft_draw_char(int x, int y, char c, uint16_t fg, uint16_t bg, uint8_t size) {
  LakatosTftGuard tftLock;
  tft.drawChar(x, y, c, fg, bg, size);
}

static void api_tft_set_text_color(uint16_t fg, uint16_t bg) {
  LakatosTftGuard tftLock;
  tft.setTextColor(fg, bg);
}

static void api_tft_set_text_datum(uint8_t datum) {
  LakatosTftGuard tftLock;
  tft.setTextDatum(datum);
}

static int api_tft_draw_string(const char* str, int x, int y, uint8_t font) {
  LakatosTftGuard tftLock;
  return tft.drawString(str ? str : "", x, y, font);
}

static void api_tft_push_rgb565(int x, int y, int w, int h, const uint16_t* pixels) {
  if (!pixels || w <= 0 || h <= 0) return;
  LakatosTftGuard tftLock;
  tft.pushImage(x, y, w, h, (uint16_t*)pixels);
}

static LakatosTouchSample api_read_touch(void) {
  LakatosTouchSample ts = lakatosReadTouch();
  if (gAppRunning && lakatosShouldExitApp()) {
    api_exit(0);
  }
  return ts;
}

static int api_prompt_keyboard(const char* title, char* buf, size_t max_len, bool allow_ctrl_c) {
  return lakatosPromptKeyboard(title, buf, max_len, allow_ctrl_c);
}

static void api_set_rgb_led(bool r, bool g, bool b, uint32_t duration_ms) {
  lakatosSetRgbLed(r, g, b, duration_ms);
}

static int api_serial_available(void) {
  lakatosPollSerialFuse();
  return Serial.available();
}

static int api_serial_read(void) {
  lakatosPollSerialFuse();
  return Serial.read();
}

static size_t api_serial_write(const void* buf, size_t len) {
  if (!buf || len == 0) return 0;
  return Serial.write((const uint8_t*)buf, len);
}

// ---------------------------------------------------------------------------
// Core 0 4-Channel Audio Synthesizer & Sample Mixer (15,625 Hz on GPIO 26)
// ---------------------------------------------------------------------------
struct LakatosVoiceState {
  const int8_t* pcm;
  uint32_t      len;
  uint32_t      loopStart;
  uint32_t      loopLen;
  uint32_t      posFp16;
  uint32_t      stepFp16;
  int32_t       lastSampleOut; // Most recent volume-scaled sample (s * curVol)
  int32_t       tailSampleOut; // Outgoing level captured at note-on / note-off / retrigger
  uint8_t       fadeIdx;       // 0..32 raised-cosine fade-in / fade-out / cross-fade index
  uint8_t       curVol;        // Current interpolated volume (0..64)
  uint8_t       startVol;      // Starting volume of current 2ms volume ramp
  uint8_t       targetVol;     // Target volume (0..64)
  uint8_t       volFadeIdx;    // 0..32 raised-cosine volume slew index
  uint8_t       waveType;
  bool          active;
};

static DRAM_ATTR uint8_t           gAudioRing[1024];
static DRAM_ATTR volatile uint32_t gAudioRingRead       = 0;
static DRAM_ATTR volatile uint32_t gAudioRingWrite      = 0;
static DRAM_ATTR volatile uint8_t  gAudioLastDuty       = 0;
static DRAM_ATTR volatile bool     gAudioHwTimerStarted = false;
static intr_handle_t               gAudioIsrHandle      = nullptr;
static esp_timer_handle_t          gAudioMixerTimer     = nullptr;
static volatile bool               gAudioActive         = false;
static uint8_t                     gAudioDcBias         = 0;

static LakatosVoiceState           gVoices[LAKATOS_AUDIO_VOICES];
static lakatos_audio_tick_cb_t     gAudioTickCb         = nullptr;
static void*                       gAudioTickUser       = nullptr;
static uint32_t                    gTickPeriodSamples   = 0;
static uint32_t                    gSamplesUntilTick    = 0;
static uint32_t                    gNoiseLfsr           = 0xACE1u;

// 32-sample (2.048 ms @ 15,625 Hz) Raised Cosine Envelope (0..256):
// w[k] = round(256 * (1 - cos(pi * (k + 1) / 32)) / 2)
static const uint16_t kRaisedCosine32[32] = {
      1,   2,   6,  10,  15,  22,  29,  38,
     47,  57,  68,  79,  91, 103, 115, 128,
    141, 153, 165, 177, 188, 199, 209, 218,
    227, 234, 241, 246, 250, 254, 255, 256
};

// Quarter-wave 64-entry sine table (0..127) for compact built-in LAKATOS_WAVE_SINE
static const uint8_t kQuarterSine[64] = {
      0,   3,   6,   9,  12,  16,  19,  22,  25,  28,  31,  34,  37,  40,  43,  46,
     49,  51,  54,  57,  60,  63,  65,  68,  71,  73,  76,  78,  81,  83,  85,  88,
     90,  92,  94,  96,  98, 100, 102, 104, 106, 107, 109, 111, 112, 113, 115, 116,
    117, 118, 120, 121, 122, 122, 123, 124, 125, 125, 126, 126, 126, 127, 127, 127
};

static inline int8_t evalBuiltinWave(uint8_t waveType, uint8_t phase8) {
  switch (waveType) {
    case LAKATOS_WAVE_SINE: {
      uint8_t q = phase8 & 0x3Fu;
      uint8_t mag = (phase8 & 0x40u) ? kQuarterSine[63u - q] : kQuarterSine[q];
      return (phase8 & 0x80u) ? (int8_t)(-(int)mag) : (int8_t)mag;
    }
    case LAKATOS_WAVE_RAMP:
      return (int8_t)(phase8 - 128);
    case LAKATOS_WAVE_SQUARE:
      return (phase8 & 0x80u) ? 127 : -127;
    case LAKATOS_WAVE_TRIANGLE: {
      uint8_t tri = (phase8 & 0x80u) ? (uint8_t)((255u - phase8) << 1) : (uint8_t)(phase8 << 1);
      return (int8_t)(tri - 128);
    }
    case LAKATOS_WAVE_NOISE: {
      gNoiseLfsr = (gNoiseLfsr >> 1) ^ (-(int32_t)(gNoiseLfsr & 1u) & 0xB400u);
      return (int8_t)(gNoiseLfsr & 0xFFu);
    }
    default:
      return 0;
  }
}

static void IRAM_ATTR audioSampleIsr(void* arg) {
  (void)arg;
  TIMERG0.int_clr_timers.t1_int_clr = 1;
  TIMERG0.hw_timer[1].config.tx_alarm_en = 1;

  uint32_t r = gAudioRingRead;
  uint8_t duty = gAudioLastDuty;
  if (r != gAudioRingWrite) {
    duty = gAudioRing[r & 1023u];
    gAudioRingRead = r + 1u;
    gAudioLastDuty = duty;
  } else {
    // If ring buffer ever underruns during heavy I/O, hold and gently relax toward 128 midpoint
    if (duty > 128u) duty--;
    else if (duty < 128u) duty++;
    gAudioLastDuty = duty;
  }
  LEDC.channel_group[0].channel[7].duty.val = (uint32_t)duty << 4;
  LEDC.channel_group[0].channel[7].conf1.duty_start = 1;
}

static void audioMixerTimerCb(void* arg) {
  (void)arg;
  if (!gAudioActive) return;

  if (!gAudioHwTimerStarted) {
    TIMERG0.hw_timer[1].config.tx_en = 0;
    TIMERG0.hw_timer[1].config.tx_divider = 2;       // 80 MHz / 2 = 40 MHz
    TIMERG0.hw_timer[1].config.tx_increase = 1;
    TIMERG0.hw_timer[1].config.tx_autoreload = 1;
    TIMERG0.hw_timer[1].config.tx_level_int_en = 1;
    TIMERG0.hw_timer[1].config.tx_edge_int_en = 0;
    TIMERG0.hw_timer[1].loadlo.tx_load_lo = 0;
    TIMERG0.hw_timer[1].loadhi.tx_load_hi = 0;
    TIMERG0.hw_timer[1].load.tx_load = 1;
    TIMERG0.hw_timer[1].alarmlo.tx_alarm_lo = 2560;  // 2560 / 40 MHz = 64.0 us (15,625 Hz)
    TIMERG0.hw_timer[1].alarmhi.tx_alarm_hi = 0;
    TIMERG0.int_clr_timers.t1_int_clr = 1;
    TIMERG0.int_ena_timers.t1_int_ena = 1;
    if (!gAudioIsrHandle) {
      esp_intr_alloc(ETS_TG0_T1_LEVEL_INTR_SOURCE, ESP_INTR_FLAG_IRAM,
                     audioSampleIsr, nullptr, &gAudioIsrHandle);
    }
    TIMERG0.hw_timer[1].config.tx_alarm_en = 1;
    TIMERG0.hw_timer[1].config.tx_en = 1;
    gAudioHwTimerStarted = true;
  }

  uint32_t w = gAudioRingWrite;
  while ((uint32_t)(w - gAudioRingRead) < 512u) {
    if (gAudioTickCb != nullptr && gTickPeriodSamples > 0) {
      if (gSamplesUntilTick == 0) {
        gSamplesUntilTick = gTickPeriodSamples;
        gAudioTickCb(gAudioTickUser);
      }
      gSamplesUntilTick--;
    }

    int32_t mix = 0;
    for (int c = 0; c < (int)LAKATOS_AUDIO_VOICES; c++) {
      LakatosVoiceState& v = gVoices[c];

      // Smooth 2 ms raised-cosine volume slew
      if (v.volFadeIdx < 32u) {
        uint16_t wv = kRaisedCosine32[v.volFadeIdx++];
        v.curVol = (uint8_t)(((uint32_t)v.startVol * (256u - wv) + (uint32_t)v.targetVol * wv) >> 8);
      } else {
        v.curVol = v.targetVol;
      }

      if (!v.active || v.len == 0 || v.stepFp16 == 0) {
        // Voice is inactive: if a 2 ms raised-cosine fade-out tail is still active, play it out
        if (v.fadeIdx < 32u && v.tailSampleOut != 0) {
          uint16_t wf = kRaisedCosine32[v.fadeIdx++];
          int32_t tail = (v.tailSampleOut * (int32_t)(256u - wf)) >> 8;
          v.lastSampleOut = tail;
          mix += tail;
        } else {
          v.fadeIdx = 32u;
          v.tailSampleOut = 0;
          v.lastSampleOut = 0;
        }
        continue;
      }

      uint32_t idx = v.posFp16 >> 16;
      if (idx >= v.len) {
        if (v.loopLen > 2 && v.loopStart < v.len) {
          uint32_t lEnd = v.loopStart + v.loopLen;
          if (lEnd > v.len) lEnd = v.len;
          uint32_t lSpan = lEnd - v.loopStart;
          if (lSpan > 0) {
            uint32_t lStartFp = v.loopStart << 16;
            uint32_t lSpanFp  = lSpan << 16;
            v.posFp16 = lStartFp + ((v.posFp16 - lStartFp) % lSpanFp);
            idx = v.posFp16 >> 16;
          } else {
            v.tailSampleOut = v.lastSampleOut;
            v.fadeIdx = 0;
            v.active = false;
            continue;
          }
        } else {
          v.tailSampleOut = v.lastSampleOut;
          v.fadeIdx = 0;
          v.active = false;
          continue;
        }
      }

      int32_t rawOut = 0;
      if (v.curVol > 0) {
        int8_t s = (v.pcm != nullptr) ? v.pcm[idx] : evalBuiltinWave(v.waveType, (uint8_t)(idx & 0xFFu));
        rawOut = (int32_t)s * (int32_t)v.curVol;

        // End-of-sample 2 ms (32-sample) raised-cosine fade-out for non-looped one-shot samples
        if (v.loopLen <= 2 && v.len <= 0xFFFFu) {
          uint32_t endFp16  = v.len << 16;
          uint32_t remFp16  = (endFp16 > v.posFp16) ? (endFp16 - v.posFp16) : 0u;
          uint32_t spanFp16 = v.stepFp16 << 5; // 32 output samples (~2.048 ms)
          if (remFp16 < spanFp16 && v.stepFp16 > 0) {
            uint32_t remSamples = remFp16 / v.stepFp16;
            if (remSamples < 32u) {
              rawOut = (rawOut * (int32_t)kRaisedCosine32[remSamples]) >> 8;
            }
          }
        }
      }

      // Apply 2 ms (32-sample) raised-cosine fade-in / cross-fade from previous tailSampleOut
      if (v.fadeIdx < 32u) {
        uint16_t wf = kRaisedCosine32[v.fadeIdx++];
        rawOut = (v.tailSampleOut * (int32_t)(256u - wf) + rawOut * (int32_t)wf) >> 8;
      }

      v.lastSampleOut = rawOut;
      mix += rawOut;

      v.posFp16 += v.stepFp16;
      if ((v.posFp16 >> 16) >= v.len) {
        if (v.loopLen > 2 && v.loopStart < v.len) {
          uint32_t lEnd = v.loopStart + v.loopLen;
          if (lEnd > v.len) lEnd = v.len;
          uint32_t lSpan = lEnd - v.loopStart;
          if (lSpan > 0) {
            v.posFp16 -= (lSpan << 16);
          } else {
            v.tailSampleOut = v.lastSampleOut;
            v.fadeIdx = 0;
            v.active = false;
          }
        } else {
          v.tailSampleOut = v.lastSampleOut;
          v.fadeIdx = 0;
          v.active = false;
        }
      }
    }

    // Soft-start DC bias ramp (0 -> 128 over ~8 ms on audio_start) so enabling PWM never pops
    if (gAudioDcBias < 128u) {
      gAudioDcBias++;
    }

    // Scale 4-channel mix (vol 64 = normal 1.0x; vol > 64 drives into overmodulation up to [4, 252])
    int32_t scaled = (mix * 3) >> 9;
    int32_t out = scaled + (int32_t)gAudioDcBias;
    if (out < 4) out = 4;
    else if (out > 252) out = 252;

    gAudioRing[w & 1023u] = (uint8_t)out;
    w++;
    gAudioRingWrite = w;
  }
}

static void api_audio_stop(void) {
  gAudioActive = false;
  gAudioTickCb = nullptr;
  gAudioTickUser = nullptr;
  gTickPeriodSamples = 0;
  gSamplesUntilTick = 0;

  if (gAudioMixerTimer) {
    esp_timer_stop(gAudioMixerTimer);
  }
  if (gAudioHwTimerStarted) {
    TIMERG0.hw_timer[1].config.tx_alarm_en = 0;
    TIMERG0.hw_timer[1].config.tx_en = 0;
    TIMERG0.int_ena_timers.t1_int_ena = 0;
    TIMERG0.int_clr_timers.t1_int_clr = 1;
    gAudioHwTimerStarted = false;
  }
  memset(gVoices, 0, sizeof(gVoices));
  memset(gAudioRing, 0, sizeof(gAudioRing));
  gAudioRingRead = 0;
  gAudioRingWrite = 0;
  gAudioLastDuty = 0;
  gAudioDcBias = 0;

  LEDC.channel_group[0].channel[7].duty.val = 0;
  LEDC.channel_group[0].channel[7].conf0.sig_out_en = 0;
  LEDC.channel_group[0].channel[7].conf1.duty_start = 1;
  lakatosSetRgbLed(false, false, false, 0);
}

static void api_audio_start(void) {
  api_audio_stop();

  for (int c = 0; c < (int)LAKATOS_AUDIO_VOICES; c++) {
    gVoices[c].fadeIdx    = 32u;
    gVoices[c].volFadeIdx = 32u;
  }

  // Route GPIO 26 pad mux to digital IO_MUX (clear RTC_IO_PDAC2_MUX_SEL)
  *(volatile uint32_t*)0x3FF48488u = (2u << 30);

  ledc_timer_config_t timer_cfg = {};
  timer_cfg.speed_mode      = LEDC_HIGH_SPEED_MODE;
  timer_cfg.duty_resolution = LEDC_TIMER_8_BIT;
  timer_cfg.timer_num       = LEDC_TIMER_3;
  timer_cfg.freq_hz         = 31250; // 31.25 kHz carrier (2x 15.625 kHz sample rate)
  timer_cfg.clk_cfg         = LEDC_AUTO_CLK;
  ledc_timer_config(&timer_cfg);

  ledc_channel_config_t ch_cfg = {};
  ch_cfg.gpio_num   = 26;
  ch_cfg.speed_mode = LEDC_HIGH_SPEED_MODE;
  ch_cfg.channel    = LEDC_CHANNEL_7;
  ch_cfg.intr_type  = LEDC_INTR_DISABLE;
  ch_cfg.timer_sel  = LEDC_TIMER_3;
  ch_cfg.duty       = 4;
  ch_cfg.hpoint     = 0;
  ledc_channel_config(&ch_cfg);
  LEDC.channel_group[0].channel[7].conf0.sig_out_en = 1;

  // Set maximum 40 mA CMOS push-pull drive strength on GPIO 26
  *(volatile uint32_t*)0x3FF49028u |= (3u << 10);
  // Enable SC8002B amplifier (active-low on GPIO 4) and cancel any pending LED auto-off timer
  lakatosSetRgbLed(true, false, false, 0);

  if (!gAudioMixerTimer) {
    esp_timer_create_args_t targs = {};
    targs.callback        = audioMixerTimerCb;
    targs.arg             = nullptr;
    targs.dispatch_method = ESP_TIMER_TASK;
    targs.name            = "lak_audio";
    esp_timer_create(&targs, &gAudioMixerTimer);
  }

  gAudioActive = true;
  if (gAudioMixerTimer) {
    esp_timer_start_periodic(gAudioMixerTimer, 4000); // Wake every 4 ms on Core 0
  }
}

static void api_audio_set_voice(int ch, const int8_t* pcm, uint32_t len,
                                uint32_t loop_start, uint32_t loop_len,
                                uint32_t step_fp16, uint8_t vol, bool reset_pos) {
  if (ch < 0 || ch >= (int)LAKATOS_AUDIO_VOICES) return;
  LakatosVoiceState& v = gVoices[ch];
  if (pcm == nullptr) {
    if (len == LAKATOS_WAVE_NONE || step_fp16 == 0 || vol == 0) {
      if (v.active || v.lastSampleOut != 0) {
        v.tailSampleOut = v.lastSampleOut;
        v.fadeIdx       = 0;
      }
      v.active    = false;
      v.pcm       = nullptr;
      v.targetVol = 0;
      return;
    }
    v.pcm       = nullptr;
    v.waveType  = (uint8_t)len;
    v.len       = 256u;
    v.loopStart = 0u;
    v.loopLen   = 256u;
    v.stepFp16  = step_fp16;
    if (reset_pos || !v.active) {
      v.tailSampleOut = v.lastSampleOut;
      v.fadeIdx       = 0;
      v.posFp16       = 0u;
      v.curVol        = vol;
      v.startVol      = vol;
      v.targetVol     = vol;
      v.volFadeIdx    = 32u;
      v.active        = true;
    } else if (vol != v.targetVol) {
      v.startVol   = v.curVol;
      v.targetVol  = vol;
      v.volFadeIdx = 0;
    }
  } else {
    if (len == 0 || step_fp16 == 0) {
      if (v.active || v.lastSampleOut != 0) {
        v.tailSampleOut = v.lastSampleOut;
        v.fadeIdx       = 0;
      }
      v.active    = false;
      v.pcm       = nullptr;
      v.targetVol = 0;
      return;
    }
    v.pcm       = pcm;
    v.waveType  = LAKATOS_WAVE_NONE;
    v.len       = len;
    v.loopStart = loop_start;
    v.loopLen   = loop_len;
    v.stepFp16  = step_fp16;
    if (reset_pos) {
      v.tailSampleOut = v.lastSampleOut;
      v.fadeIdx       = 0;
      v.posFp16       = 0u;
      v.curVol        = vol;
      v.startVol      = vol;
      v.targetVol     = vol;
      v.volFadeIdx    = 32u;
      v.active        = true;
    } else if (vol != v.targetVol) {
      v.startVol   = v.curVol;
      v.targetVol  = vol;
      v.volFadeIdx = 0;
    }
  }
}

static void api_audio_set_tick_cb(lakatos_audio_tick_cb_t cb, void* user, uint32_t tick_samples) {
  gAudioTickUser     = user;
  gTickPeriodSamples = tick_samples;
  if (gSamplesUntilTick > tick_samples) {
    gSamplesUntilTick = tick_samples;
  }
  gAudioTickCb       = cb;
}

void lakatosAudioStart() {
  api_audio_start();
}

void lakatosAudioStop() {
  api_audio_stop();
}

void lakatosAudioSetVoice(int ch, const int8_t* pcm, uint32_t len,
                          uint32_t loopStart, uint32_t loopLen,
                          uint32_t stepFp16, uint8_t vol, bool resetPos) {
  api_audio_set_voice(ch, pcm, len, loopStart, loopLen, stepFp16, vol, resetPos);
}

// ---------------------------------------------------------------------------
// Streaming VFS Reader with Transparent .zip (Store / Deflate) Decompression
// ---------------------------------------------------------------------------
static int32_t api_fs_read_stream(const char* path, lakatos_stream_cb_t on_chunk, void* user) {
  if (!path || !on_chunk) return -1;
  LakatosVfsStream* s = lakatosFsOpenRead(path);
  if (!s) return -1;

  uint8_t hdr[30];
  int32_t gotHdr = lakatosFsStreamRead(s, hdr, 4);
  if (gotHdr <= 0) {
    lakatosFsClose(s);
    return (gotHdr == 0) ? 0 : -1;
  }

  // Case 1: Plain uncompressed file (does not begin with ZIP "PK\x03\x04")
  if (gotHdr < 4 || hdr[0] != 0x50 || hdr[1] != 0x4B || hdr[2] != 0x03 || hdr[3] != 0x04) {
    int32_t totalOut = gotHdr;
    if (!on_chunk(hdr, (size_t)gotHdr, user)) {
      lakatosFsClose(s);
      return totalOut;
    }
    uint8_t* buf = (uint8_t*)malloc(2048);
    if (!buf) {
      lakatosFsClose(s);
      return -1;
    }
    for (;;) {
      int32_t n = lakatosFsStreamRead(s, buf, 2048);
      if (n <= 0) break;
      totalOut += n;
      if (!on_chunk(buf, (size_t)n, user)) break;
    }
    free(buf);
    lakatosFsClose(s);
    return totalOut;
  }

  // Case 2: ZIP Archive Local File Header ("PK\x03\x04")
  int32_t remHdr = lakatosFsStreamRead(s, hdr + 4, 26);
  if (remHdr != 26) {
    lakatosFsClose(s);
    return -1;
  }
  uint16_t method   = (uint16_t)(hdr[8]  | (hdr[9] << 8));
  uint32_t uncompSz = (uint32_t)hdr[22] | ((uint32_t)hdr[23] << 8) |
                      ((uint32_t)hdr[24] << 16) | ((uint32_t)hdr[25] << 24);
  uint16_t fnLen    = (uint16_t)(hdr[26] | (hdr[27] << 8));
  uint16_t extLen   = (uint16_t)(hdr[28] | (hdr[29] << 8));

  if (!lakatosFsStreamSkip(s, (size_t)fnLen + (size_t)extLen)) {
    lakatosFsClose(s);
    return -1;
  }

  if (method == 0) {
    // Stored uncompressed inside .zip
    uint8_t* buf = (uint8_t*)malloc(2048);
    if (!buf) {
      lakatosFsClose(s);
      return -1;
    }
    int32_t totalOut = 0;
    while (uncompSz == 0 || (uint32_t)totalOut < uncompSz) {
      size_t ask = 2048;
      if (uncompSz > 0 && uncompSz - (uint32_t)totalOut < ask) {
        ask = uncompSz - (uint32_t)totalOut;
      }
      int32_t n = lakatosFsStreamRead(s, buf, ask);
      if (n <= 0) break;
      totalOut += n;
      if (!on_chunk(buf, (size_t)n, user)) break;
    }
    free(buf);
    lakatosFsClose(s);
    return totalOut;
  }

  if (method != 8) {
    lakatosFsClose(s);
    return -1;
  }

  // Method 8: Raw Deflate stream via ESP32 ROM tinfl_decompress
  bool useScratch = (gLakatosPngScratch != nullptr) &&
                    (gLakatosPngScratchSize >= sizeof(tinfl_decompressor));
  tinfl_decompressor* decomp = useScratch
                                   ? (tinfl_decompressor*)gLakatosPngScratch
                                   : (tinfl_decompressor*)malloc(sizeof(tinfl_decompressor));
  uint8_t* dict  = (uint8_t*)malloc(TINFL_LZ_DICT_SIZE);
  uint8_t* inBuf = (uint8_t*)malloc(2048);
  if (!decomp || !dict || !inBuf) {
    if (!useScratch && decomp) free(decomp);
    if (dict) free(dict);
    if (inBuf) free(inBuf);
    lakatosFsClose(s);
    return -1;
  }

  tinfl_init(decomp);
  size_t dictOfs = 0;
  int32_t totalOut = 0;
  bool done = false;

  while (!done) {
    int32_t gotIn = lakatosFsStreamRead(s, inBuf, 2048);
    bool hasMoreIn = (gotIn > 0);
    size_t inPos = 0;
    size_t availIn = (gotIn > 0) ? (size_t)gotIn : 0;

    for (;;) {
      size_t inBytes  = availIn - inPos;
      size_t outBytes = TINFL_LZ_DICT_SIZE - dictOfs;
      uint32_t flags  = hasMoreIn ? TINFL_FLAG_HAS_MORE_INPUT : 0;
      tinfl_status status = tinfl_decompress(
          decomp, inBuf + inPos, &inBytes,
          dict, dict + dictOfs, &outBytes, flags);
      inPos += inBytes;

      if (outBytes > 0) {
        totalOut += (int32_t)outBytes;
        if (!on_chunk(dict + dictOfs, outBytes, user)) {
          done = true;
          break;
        }
        dictOfs = (dictOfs + outBytes) & (TINFL_LZ_DICT_SIZE - 1);
      }
      if (status == TINFL_STATUS_DONE || status < 0) {
        done = true;
        break;
      }
      if (inPos >= availIn && status != TINFL_STATUS_HAS_MORE_OUTPUT) {
        break;
      }
    }
    if (!hasMoreIn) break;
  }

  free(inBuf);
  free(dict);
  if (!useScratch) free(decomp);
  lakatosFsClose(s);
  return totalOut;
}

static const LakatosApi gLakatosApi = {
    .api_version          = LAKATOS_API_VERSION,
    .struct_size          = sizeof(LakatosApi),
    .exit                 = api_exit,
    .yield                = api_yield,
    .millis               = api_millis,
    .micros               = api_micros,
    .delay_ms             = api_delay_ms,
    .malloc               = api_malloc,
    .calloc               = api_calloc,
    .realloc              = api_realloc,
    .free                 = api_free,
    .free_heap_bytes      = api_free_heap_bytes,
    .get_airport          = lakatosGetAirportCode,
    .get_airport_info     = lakatosGetAirportInfo,
    .get_device_name      = lakatosGetDeviceName,
    .get_serial_number    = lakatosGetSerialNumber,
    .get_superhighway_url = lakatosGetSuperhighwayUrl,
    .get_unix_time        = lakatosGetUnixTime,
    .format_local_time    = lakatosFormatLocalTime,
    .tft_fill_screen      = api_tft_fill_screen,
    .tft_fill_rect        = api_tft_fill_rect,
    .tft_draw_rect        = api_tft_draw_rect,
    .tft_fill_round_rect  = api_tft_fill_round_rect,
    .tft_draw_round_rect  = api_tft_draw_round_rect,
    .tft_draw_line        = api_tft_draw_line,
    .tft_draw_fast_h_line = api_tft_draw_fast_h_line,
    .tft_draw_fast_v_line = api_tft_draw_fast_v_line,
    .tft_fill_circle      = api_tft_fill_circle,
    .tft_draw_circle      = api_tft_draw_circle,
    .tft_fill_triangle    = api_tft_fill_triangle,
    .tft_draw_char        = api_tft_draw_char,
    .tft_set_text_color   = api_tft_set_text_color,
    .tft_set_text_datum   = api_tft_set_text_datum,
    .tft_draw_string      = api_tft_draw_string,
    .tft_push_rgb565      = api_tft_push_rgb565,
    .read_touch           = api_read_touch,
    .prompt_keyboard      = api_prompt_keyboard,
    .set_rgb_led          = api_set_rgb_led,
    .read_ldr             = lakatosReadLdrRaw,
    .read_cpu_temp_c      = lakatosReadCpuTempC,
    .fs_ls                = lakatosFsLs,
    .fs_stat              = lakatosFsStat,
    .fs_read              = lakatosFsRead,
    .fs_write             = lakatosFsWrite,
    .fs_write_at          = lakatosFsWriteAt,
    .fs_mkdir             = lakatosFsMkdir,
    .fs_rmdir             = lakatosFsRmdir,
    .fs_rm                = lakatosFsRm,
    .fs_du                = lakatosFsDu,
    .wifi_is_connected    = lakatosWifiIsConnected,
    .http_get             = lakatosHttpGet,
    .serial_available     = api_serial_available,
    .serial_read          = api_serial_read,
    .serial_write         = api_serial_write,
    .vsnprintf            = vsnprintf,
    .city_hash32          = lakatosCityHash32,
    .fetch_with_cache     = lakatosHttpGetCached,
    .audio_start          = api_audio_start,
    .audio_stop           = api_audio_stop,
    .audio_set_voice      = api_audio_set_voice,
    .audio_set_tick_cb    = api_audio_set_tick_cb,
    .fs_read_stream       = api_fs_read_stream,
    .http_stream_open     = lakatosHttpStreamOpen,
    .http_stream_read     = lakatosHttpStreamRead,
    .http_stream_close    = lakatosHttpStreamClose,
    .http_stream_write    = lakatosHttpStreamWrite,
    .get_corner_size      = lakatosGetCornerSize,
};

// ---------------------------------------------------------------------------
// Hardware Flash MMU Mapper (IRAM-resident)
// Maps physical flash pages of app_slot (0x1A0000+) simultaneously into:
//   - DROM: 0x3F700000 (read-only data & header)
//   - IROM: 0x40300000 (executable instructions)
// ---------------------------------------------------------------------------
static void IRAM_ATTR mapAppFlashMmu(uint32_t flashOffset, uint32_t mapBytes) {
  uint32_t maxPages = LAKATOS_APP_FLASH_MAX_SIZE >> 16;
  uint32_t numPages = (mapBytes + 0xFFFFu) >> 16;
  if (numPages == 0) numPages = 1;
  if (numPages > maxPages) numPages = maxPages;

  spi_flash_disable_interrupts_caches_and_other_cpu();

  cache_flash_mmu_set(0, 0, LAKATOS_APP_DROM_BASE, flashOffset, 64, numPages);
  cache_flash_mmu_set(1, 0, LAKATOS_APP_DROM_BASE, flashOffset, 64, numPages);
  cache_flash_mmu_set(0, 0, LAKATOS_APP_IROM_BASE, flashOffset, 64, numPages);
  cache_flash_mmu_set(1, 0, LAKATOS_APP_IROM_BASE, flashOffset, 64, numPages);

  Cache_Flush(0);
  Cache_Flush(1);

  spi_flash_enable_interrupts_caches_and_other_cpu();
}

extern "C" {
extern uint8_t _data_end[];
extern uint8_t _noinit_start[];
}

bool initLakatosLoader() {
  memset(gTrackedAllocs, 0, sizeof(gTrackedAllocs));

  // Reclaim the ~15.2 KB linker alignment gap between _data_end (~0x3FFC4360)
  // and _noinit_start (0x3FFC8000, aligned to 32 KB for gLakatosAppRamArena).
  // Keep 12,864 bytes as a dedicated static scratch buffer for tinfl_decompressor
  // (10,992 B) + PngDecoderState (1,856 B) so Wi-Fi/TCP never fragments it, and
  // add the remaining ~2.6 KB to the system 8-bit heap.
  uintptr_t gapStart = ((uintptr_t)_data_end + 15u) & ~15u;
  uintptr_t gapEnd   = (uintptr_t)_noinit_start & ~15u;
  if (gapEnd > gapStart) {
    gLakatosPngScratch     = (uint8_t*)gapStart;
    gLakatosPngScratchSize = (size_t)(gapEnd - gapStart);
    Serial.printf("[Lakatos] PNG scratch: %u B @ 0x%08lX\r\n",
                  (unsigned)gLakatosPngScratchSize,
                  (unsigned long)(uintptr_t)gLakatosPngScratch);
  }

  gAppSlotPart = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "app_slot");
  uintptr_t arenaAddr = (uintptr_t)gLakatosAppRamArena;
  Serial.printf("[Lakatos] App DRAM Arena: 0x%08lX (expected 0x%08lX, %u bytes)\r\n",
                (unsigned long)arenaAddr,
                (unsigned long)LAKATOS_APP_DRAM_BASE,
                (unsigned)LAKATOS_APP_DRAM_SIZE);
  if (gAppSlotPart) {
    Serial.printf("[Lakatos] app_slot partition: 0x%06lX (%lu bytes)\r\n",
                  (unsigned long)gAppSlotPart->address,
                  (unsigned long)gAppSlotPart->size);
  } else {
    Serial.println("[Lakatos] WARNING: app_slot partition not found!");
  }
  return gAppSlotPart != nullptr && arenaAddr == LAKATOS_APP_DRAM_BASE;
}

static void drawLoaderStatusBox(const char* title, const char* detail, int pct) {
  LakatosTftGuard tftLock;
  tft.fillRoundRect(24, 72, 272, 96, 8, 0x10A2);
  tft.drawRoundRect(24, 72, 272, 96, 8, TFT_CYAN);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_CYAN, 0x10A2);
  tft.drawString(title, 160, 84, 2);
  tft.setTextColor(TFT_WHITE, 0x10A2);
  tft.drawString(detail, 160, 108, 2);
  if (pct >= 0) {
    tft.fillRect(44, 136, 232, 14, TFT_BLACK);
    tft.drawRect(44, 136, 232, 14, TFT_DARKGREY);
    int fillW = constrain((pct * 228) / 100, 0, 228);
    if (fillW > 0) {
      tft.fillRect(46, 138, fillW, 10, TFT_GREEN);
    }
  }
  tft.setTextDatum(TL_DATUM);
}

static bool verifyPayloadSha256(const uint8_t* payloadBuf,
                                uint32_t payloadLen,
                                const uint8_t expectedSha[32]) {
  uint8_t calcSha[32];
  mbedtls_sha256_context shaCtx;
  mbedtls_sha256_init(&shaCtx);
  mbedtls_sha256_starts(&shaCtx, 0);
  mbedtls_sha256_update(&shaCtx, payloadBuf, payloadLen);
  mbedtls_sha256_finish(&shaCtx, calcSha);
  mbedtls_sha256_free(&shaCtx);
  return memcmp(calcSha, expectedSha, 32) == 0;
}

static char gLakatosAppArg[64]           = {0};
static char gLakatosJoinNativeTarget[64] = {0};

void lakatosSetAppArg(const char* arg) {
  if (!arg) {
    gLakatosAppArg[0] = '\0';
    return;
  }
  strncpy(gLakatosAppArg, arg, sizeof(gLakatosAppArg) - 1);
  gLakatosAppArg[sizeof(gLakatosAppArg) - 1] = '\0';
}

const char* lakatosGetAppArg() {
  return gLakatosAppArg;
}

void lakatosRequestJoinNative(const char* binName) {
  if (!binName) {
    gLakatosJoinNativeTarget[0] = '\0';
    return;
  }
  strncpy(gLakatosJoinNativeTarget, binName, sizeof(gLakatosJoinNativeTarget) - 1);
  gLakatosJoinNativeTarget[sizeof(gLakatosJoinNativeTarget) - 1] = '\0';
}

const char* lakatosGetJoinNativeTarget() {
  return gLakatosJoinNativeTarget;
}

// Resolves a native binary filename (e.g. "native.bin") across /{sd,net,flash}/apps
// using the same priority order as Home Screen .lapp discovery.
static bool resolveNativeBinHeader(const char* binName, char* outPath, size_t outPathCap,
                                   LakatosAppHeader* outHdr) {
  if (!binName || !binName[0]) return false;

  if (binName[0] == '/') {
    if (lakatosFsRead(binName, 0, outHdr, sizeof(*outHdr)) == (int32_t)sizeof(*outHdr) &&
        outHdr->magic == LAKATOS_APP_MAGIC) {
      strncpy(outPath, binName, outPathCap - 1);
      outPath[outPathCap - 1] = '\0';
      return true;
    }
    return false;
  }

  // Priority 1: /sd/apps/<binName>
  if (lakatosSdIsMounted()) {
    snprintf(outPath, outPathCap, "/sd/apps/%s", binName);
    if (lakatosFsRead(outPath, 0, outHdr, sizeof(*outHdr)) == (int32_t)sizeof(*outHdr) &&
        outHdr->magic == LAKATOS_APP_MAGIC) {
      return true;
    }
  }

  // Priority 2: /net/apps/<binName>
  if (lakatosWifiIsConnected() && strlen(lakatosGetSuperhighwayPassword()) > 0) {
    snprintf(outPath, outPathCap, "/net/apps/%s", binName);
    if (lakatosFsRead(outPath, 0, outHdr, sizeof(*outHdr)) == (int32_t)sizeof(*outHdr) &&
        outHdr->magic == LAKATOS_APP_MAGIC) {
      return true;
    }
  }

  // Priority 3: /flash/apps/<binName>
  snprintf(outPath, outPathCap, "/flash/apps/%s", binName);
  if (lakatosFsRead(outPath, 0, outHdr, sizeof(*outHdr)) == (int32_t)sizeof(*outHdr) &&
      outHdr->magic == LAKATOS_APP_MAGIC) {
    return true;
  }

  outPath[0] = '\0';
  return false;
}

bool lakatosRunAppFromFile(const char* vfsPath, int* outExitCode, String* outErrMsg) {
  if (!gAppSlotPart) {
    if (outErrMsg) *outErrMsg = "app_slot partition missing";
    return false;
  }

  gLakatosJoinNativeTarget[0] = '\0';
  static char resolvedBinPath[128];

  drawLoaderStatusBox("LAKATOS APP LOADER", vfsPath, 5);

  // 1. Read and validate 128-byte .lapp header from VFS
  static LakatosAppHeader hdr;
  memset(&hdr, 0, sizeof(hdr));
  int32_t hdrGot = lakatosFsRead(vfsPath, 0, &hdr, sizeof(hdr));
  if (hdrGot != (int32_t)sizeof(hdr)) {
    if (outErrMsg) *outErrMsg = "Failed to read .lapp header";
    return false;
  }
  if (hdr.magic != LAKATOS_APP_MAGIC) {
    if (outErrMsg) *outErrMsg = "Invalid magic (not a .lapp file)";
    return false;
  }
  if (hdr.abi_version != LAKATOS_API_VERSION) {
    if (outErrMsg) *outErrMsg = "Incompatible ABI version";
    return false;
  }
  if (hdr.total_file_size < sizeof(LakatosAppHeader) ||
      hdr.total_file_size > LAKATOS_APP_FLASH_MAX_SIZE) {
    if (outErrMsg) *outErrMsg = "Invalid .lapp size";
    return false;
  }

  // -------------------------------------------------------------------------
  // Fast RAM-Only Path for LCode Bytecode Apps (Zero Flash Wear!)
  // -------------------------------------------------------------------------
  if ((hdr.flags & LAKATOS_APP_FLAG_LCODE) != 0 || hdr.entry_addr == 0) {
    uint32_t payloadLen = hdr.total_file_size - sizeof(LakatosAppHeader);
    if (payloadLen < 24 || payloadLen > 65536) {
      if (outErrMsg) *outErrMsg = "Invalid LCode payload size";
      return false;
    }

    drawLoaderStatusBox("LOADING LCODE TO RAM", hdr.name, 40);

    bool heapAllocated = false;
    uint8_t* payloadBuf = nullptr;
    if (payloadLen <= LAKATOS_APP_DRAM_SIZE) {
      // Reuse the 32 KB static .noinit arena at 0x3FFC8000 for the LCOD image!
      payloadBuf = gLakatosAppRamArena;
    } else {
      payloadBuf = (uint8_t*)malloc(payloadLen);
      heapAllocated = true;
    }
    if (!payloadBuf) {
      if (outErrMsg) *outErrMsg = "Out of RAM for LCode payload";
      return false;
    }

    uint32_t readOff = 0;
    while (readOff < payloadLen) {
      uint32_t chunk = payloadLen - readOff;
      if (chunk > 4096) chunk = 4096;
      int32_t got = lakatosFsRead(vfsPath, sizeof(LakatosAppHeader) + readOff,
                                  payloadBuf + readOff, chunk);
      if (got <= 0) {
        if (heapAllocated) free(payloadBuf);
        if (outErrMsg) *outErrMsg = "Short read loading LCode .lapp";
        return false;
      }
      readOff += (uint32_t)got;
    }

    if (!verifyPayloadSha256(payloadBuf, payloadLen, hdr.sha256)) {
      if (heapAllocated) free(payloadBuf);
      if (outErrMsg) *outErrMsg = "LCode SHA-256 verification failed";
      return false;
    }

    drawLoaderStatusBox("STARTING LCODE VM", hdr.name, 95);
    tft.setTextDatum(TL_DATUM);
    bool ok = lakatosRunLCodeApp(hdr, payloadBuf, payloadLen, outExitCode, outErrMsg);
    tft.setTextDatum(TL_DATUM);
    if (heapAllocated) free(payloadBuf);
    if (!ok || gLakatosJoinNativeTarget[0] == '\0') {
      return ok;
    }

    // LCode app requested join_native(gLakatosJoinNativeTarget) with gLakatosAppArg:
    // resolve across /{sd,net,flash}/apps/<target>, or fall back to app_slot if
    // the matching native image is already burned into flash.
    Serial.printf("[Lakatos] Joining native '%s' (arg='%s')...\r\n",
                  gLakatosJoinNativeTarget, gLakatosAppArg);
    memset(&hdr, 0, sizeof(hdr));
    if (resolveNativeBinHeader(gLakatosJoinNativeTarget, resolvedBinPath,
                               sizeof(resolvedBinPath), &hdr)) {
      vfsPath = resolvedBinPath;
    } else {
      LakatosAppHeader slotCheck;
      memset(&slotCheck, 0, sizeof(slotCheck));
      if (esp_partition_read(gAppSlotPart, 0, &slotCheck, sizeof(slotCheck)) == ESP_OK &&
          slotCheck.magic == LAKATOS_APP_MAGIC &&
          strcmp(slotCheck.name, gLakatosJoinNativeTarget) == 0) {
        hdr = slotCheck;
        resolvedBinPath[0] = '\0';
        vfsPath = gLakatosJoinNativeTarget;
      } else {
        lakatosEndAppSession();
        if (outErrMsg) {
          *outErrMsg = String("Missing ") + gLakatosJoinNativeTarget;
        }
        return false;
      }
    }
  }

  if (hdr.total_file_size > gAppSlotPart->size) {
    if (outErrMsg) *outErrMsg = "Invalid .lapp size";
    return false;
  }
  if (hdr.entry_addr < LAKATOS_APP_IROM_BASE ||
      hdr.entry_addr >= (LAKATOS_APP_IROM_BASE + LAKATOS_APP_FLASH_MAX_SIZE)) {
    if (outErrMsg) *outErrMsg = "Entry address outside IROM slot";
    return false;
  }

  // 2. Compare against header currently staged in app_slot
  LakatosAppHeader slotHdr;
  memset(&slotHdr, 0, sizeof(slotHdr));
  bool alreadyStaged = false;
  if (esp_partition_read(gAppSlotPart, 0, &slotHdr, sizeof(slotHdr)) == ESP_OK) {
    if (memcmp(&hdr, &slotHdr, sizeof(LakatosAppHeader)) == 0) {
      alreadyStaged = true;
    }
  }

  // 3. Stage into app_slot flash if needed, verifying SHA-256 on the fly
  if (!alreadyStaged) {
    uint32_t eraseBytes = (hdr.total_file_size + 4095u) & ~4095u;
    drawLoaderStatusBox("STAGING TO FLASH SLOT", hdr.name, 15);
    if (esp_partition_erase_range(gAppSlotPart, 0, eraseBytes) != ESP_OK) {
      if (outErrMsg) *outErrMsg = "Flash erase of app_slot failed";
      return false;
    }

    constexpr size_t kChunk = 4096;
    uint8_t* chunkBuf = (uint8_t*)malloc(kChunk);
    if (!chunkBuf) {
      if (outErrMsg) *outErrMsg = "Out of heap for staging buffer";
      return false;
    }

    mbedtls_sha256_context shaCtx;
    mbedtls_sha256_init(&shaCtx);
    mbedtls_sha256_starts(&shaCtx, 0);

    uint32_t offset = 0;
    bool stageOk = true;
    while (offset < hdr.total_file_size) {
      uint32_t toRead = hdr.total_file_size - offset;
      if (toRead > kChunk) toRead = kChunk;
      int32_t got = lakatosFsRead(vfsPath, offset, chunkBuf, toRead);
      if (got <= 0) {
        stageOk = false;
        if (outErrMsg) *outErrMsg = "Short read while staging .lapp";
        break;
      }
      if (esp_partition_write(gAppSlotPart, offset, chunkBuf, got) != ESP_OK) {
        stageOk = false;
        if (outErrMsg) *outErrMsg = "Flash write to app_slot failed";
        break;
      }
      if (offset + (uint32_t)got > sizeof(LakatosAppHeader)) {
        uint32_t shaStart = (offset < sizeof(LakatosAppHeader))
                                ? (sizeof(LakatosAppHeader) - offset)
                                : 0;
        mbedtls_sha256_update(&shaCtx, chunkBuf + shaStart, (uint32_t)got - shaStart);
      }
      offset += (uint32_t)got;
      int pct = 20 + (int)((offset * 70ULL) / hdr.total_file_size);
      drawLoaderStatusBox("STAGING TO FLASH SLOT", hdr.name, pct);
    }

    uint8_t calcSha[32];
    mbedtls_sha256_finish(&shaCtx, calcSha);
    mbedtls_sha256_free(&shaCtx);
    free(chunkBuf);

    if (!stageOk) return false;

    if (memcmp(calcSha, hdr.sha256, 32) != 0) {
      // Wipe header in app_slot so corrupt image isn't cached
      esp_partition_erase_range(gAppSlotPart, 0, 4096);
      if (outErrMsg) *outErrMsg = "SHA-256 verification failed";
      return false;
    }
  }

  drawLoaderStatusBox("MAPPING MMU & STARTING", hdr.name, 95);

  // 4. Map physical app_slot (0x180000) into DROM (0x3F700000) and IROM (0x40300000)
  mapAppFlashMmu(gAppSlotPart->address, hdr.total_file_size);

  // 5. Initialize App Static DRAM Arena (0x3FFC8000 .. 0x3FFCFFFF)
  memset(gLakatosAppRamArena, 0, LAKATOS_APP_DRAM_SIZE);
  *(const LakatosApi**)(uintptr_t)LAKATOS_APP_DRAM_BASE        = &gLakatosApi;
  *(const char**)(uintptr_t)(LAKATOS_APP_DRAM_BASE + 4u)       = gLakatosAppArg;

  if (hdr.data_size > 0) {
    if (hdr.data_dst_addr < LAKATOS_APP_DRAM_BASE + 8 ||
        (hdr.data_dst_addr + hdr.data_size) > (LAKATOS_APP_DRAM_BASE + LAKATOS_APP_DRAM_SIZE)) {
      if (outErrMsg) *outErrMsg = "App .data outside static arena";
      return false;
    }
    const void* srcData = (const void*)(uintptr_t)(LAKATOS_APP_DROM_BASE + hdr.data_offset);
    memcpy((void*)(uintptr_t)hdr.data_dst_addr, srcData, hdr.data_size);
  }

  if (hdr.bss_size > 0) {
    if (hdr.bss_dst_addr < LAKATOS_APP_DRAM_BASE + 8 ||
        (hdr.bss_dst_addr + hdr.bss_size) > (LAKATOS_APP_DRAM_BASE + LAKATOS_APP_DRAM_SIZE)) {
      if (outErrMsg) *outErrMsg = "App .bss outside static arena";
      return false;
    }
    memset((void*)(uintptr_t)hdr.bss_dst_addr, 0, hdr.bss_size);
  }

  // 6. Jump to app entry point and catch os->exit(code) via setjmp/longjmp
  cleanupAppHeap();
  lakatosWaitTouchRelease();
  {
    LakatosTftGuard tftGuard;
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
  }
  // Pre-configure GPIO 26 (CYD speaker pin) as a digital push-pull output (LOW)
  pinMode(26, OUTPUT);
  digitalWrite(26, LOW);

  lakatosBeginAppSession();
  gAppLastExitCode = 0;
  gAppRunning = true;

  Serial.printf("[Lakatos] Launching '%s' (arg='%s') at entry 0x%08lX (size %lu bytes)\r\n",
                hdr.name, gLakatosAppArg, (unsigned long)hdr.entry_addr,
                (unsigned long)hdr.total_file_size);

  int jmpVal = setjmp(gAppExitJmp);
  if (jmpVal == 0) {
    lakatos_app_entry_t entryFn = (lakatos_app_entry_t)(uintptr_t)hdr.entry_addr;
    entryFn(&gLakatosApi);
  }

  gAppRunning = false;
  api_audio_stop();
  lakatosHttpStreamClose();
  pinMode(26, OUTPUT);
  digitalWrite(26, LOW);
  lakatosPrefetchFree();
  cleanupAppHeap();
  lakatosEndAppSession();

  if (outExitCode) *outExitCode = gAppLastExitCode;
  Serial.printf("[Lakatos] App '%s' exited with code %d, returning to Lakatos OS.\r\n",
                hdr.name, gAppLastExitCode);
  return true;
}
