#include "lakatos_state.h"

#include <esp_heap_caps.h>
#include <esp_heap_caps_init.h>
#include <esp_partition.h>
#include <mbedtls/sha256.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
  void* p = realloc(ptr, size);
  if (p) {
    forgetAlloc(ptr);
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
  tft.fillScreen(color);
}

static void api_tft_fill_rect(int x, int y, int w, int h, uint16_t color) {
  tft.fillRect(x, y, w, h, color);
}

static void api_tft_draw_rect(int x, int y, int w, int h, uint16_t color) {
  tft.drawRect(x, y, w, h, color);
}

static void api_tft_fill_round_rect(int x, int y, int w, int h, int r, uint16_t color) {
  tft.fillRoundRect(x, y, w, h, r, color);
}

static void api_tft_draw_round_rect(int x, int y, int w, int h, int r, uint16_t color) {
  tft.drawRoundRect(x, y, w, h, r, color);
}

static void api_tft_draw_line(int x0, int y0, int x1, int y1, uint16_t color) {
  tft.drawLine(x0, y0, x1, y1, color);
}

static void api_tft_draw_fast_h_line(int x, int y, int w, uint16_t color) {
  tft.drawFastHLine(x, y, w, color);
}

static void api_tft_draw_fast_v_line(int x, int y, int h, uint16_t color) {
  tft.drawFastVLine(x, y, h, color);
}

static void api_tft_fill_circle(int x, int y, int r, uint16_t color) {
  tft.fillCircle(x, y, r, color);
}

static void api_tft_draw_circle(int x, int y, int r, uint16_t color) {
  tft.drawCircle(x, y, r, color);
}

static void api_tft_fill_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t color) {
  tft.fillTriangle(x0, y0, x1, y1, x2, y2, color);
}

static void api_tft_draw_char(int x, int y, char c, uint16_t fg, uint16_t bg, uint8_t size) {
  tft.drawChar(x, y, c, fg, bg, size);
}

static void api_tft_set_text_color(uint16_t fg, uint16_t bg) {
  tft.setTextColor(fg, bg);
}

static void api_tft_set_text_datum(uint8_t datum) {
  tft.setTextDatum(datum);
}

static int api_tft_draw_string(const char* str, int x, int y, uint8_t font) {
  return tft.drawString(str ? str : "", x, y, font);
}

static void api_tft_push_rgb565(int x, int y, int w, int h, const uint16_t* pixels) {
  if (!pixels || w <= 0 || h <= 0) return;
  tft.pushImage(x, y, w, h, (uint16_t*)pixels);
}

static int api_prompt_keyboard(const char* title, char* buf, size_t max_len, bool allow_ctrl_c) {
  return lakatosPromptKeyboard(title, buf, max_len, allow_ctrl_c, false);
}

static void api_set_rgb_led(bool r, bool g, bool b, uint32_t duration_ms) {
  lakatosSetRgbLed(r, g, b, duration_ms);
}

static int api_serial_available(void) {
  return Serial.available();
}

static int api_serial_read(void) {
  return Serial.read();
}

static size_t api_serial_write(const void* buf, size_t len) {
  if (!buf || len == 0) return 0;
  return Serial.write((const uint8_t*)buf, len);
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
    .read_touch           = lakatosReadTouch,
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
};

// ---------------------------------------------------------------------------
// Hardware Flash MMU Mapper (IRAM-resident)
// Maps physical flash pages of app_slot (0x180000+) simultaneously into:
//   - DROM: 0x3F700000 (read-only data & header)
//   - IROM: 0x40300000 (executable instructions)
// ---------------------------------------------------------------------------
static void IRAM_ATTR mapAppFlashMmu(uint32_t flashOffset, uint32_t mapBytes) {
  uint32_t numPages = (mapBytes + 0xFFFFu) >> 16;
  if (numPages == 0) numPages = 1;
  if (numPages > 12) numPages = 12;

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

bool lakatosRunAppFromFile(const char* vfsPath, int* outExitCode, String* outErrMsg) {
  if (!gAppSlotPart) {
    if (outErrMsg) *outErrMsg = "app_slot partition missing";
    return false;
  }

  drawLoaderStatusBox("LAKATOS APP LOADER", vfsPath, 5);

  // 1. Read and validate 128-byte .lapp header from VFS
  LakatosAppHeader hdr;
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

    uint8_t calcSha[32];
    mbedtls_sha256_context shaCtx;
    mbedtls_sha256_init(&shaCtx);
    mbedtls_sha256_starts(&shaCtx, 0);
    mbedtls_sha256_update(&shaCtx, payloadBuf, payloadLen);
    mbedtls_sha256_finish(&shaCtx, calcSha);
    mbedtls_sha256_free(&shaCtx);

    if (memcmp(calcSha, hdr.sha256, 32) != 0) {
      if (heapAllocated) free(payloadBuf);
      if (outErrMsg) *outErrMsg = "LCode SHA-256 verification failed";
      return false;
    }

    drawLoaderStatusBox("STARTING LCODE VM", hdr.name, 95);
    tft.setTextDatum(TL_DATUM);
    bool ok = lakatosRunLCodeApp(hdr, payloadBuf, payloadLen, outExitCode, outErrMsg);
    tft.setTextDatum(TL_DATUM);
    if (heapAllocated) free(payloadBuf);
    return ok;
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
  *(const LakatosApi**)(uintptr_t)LAKATOS_APP_DRAM_BASE = &gLakatosApi;

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
  gAppLastExitCode = 0;
  gAppRunning = true;

  Serial.printf("[Lakatos] Launching '%s' at entry 0x%08lX (size %lu bytes)\r\n",
                hdr.name, (unsigned long)hdr.entry_addr,
                (unsigned long)hdr.total_file_size);

  int jmpVal = setjmp(gAppExitJmp);
  if (jmpVal == 0) {
    lakatos_app_entry_t entryFn = (lakatos_app_entry_t)(uintptr_t)hdr.entry_addr;
    entryFn(&gLakatosApi);
  }

  gAppRunning = false;
  lakatosPrefetchFree();
  cleanupAppHeap();
  tft.setTextDatum(TL_DATUM);

  if (outExitCode) *outExitCode = gAppLastExitCode;
  Serial.printf("[Lakatos] App '%s' exited with code %d, returning to Lakatos OS.\r\n",
                hdr.name, gAppLastExitCode);
  return true;
}
