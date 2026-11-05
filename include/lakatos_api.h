#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Lakatos Fixed Memory Map & Binary Format Constants
// ---------------------------------------------------------------------------
#define LAKATOS_API_VERSION         1u
#define LAKATOS_APP_MAGIC           0x414B414Cu  // "LAKA" in little-endian
#define LAKATOS_LCODE_MAGIC         0x444F434Cu  // "LCOD" in little-endian
#define LAKATOS_APP_HEADER_SIZE     128u         // 0x80 bytes
#define LAKATOS_APP_FLAG_LCODE      1u           // Bit 0: LCode bytecode payload (runs in RAM, 0 flash wear)

#define LAKATOS_APP_FLASH_OFFSET    0x00180000u  // Physical flash offset of app_slot
#define LAKATOS_APP_FLASH_MAX_SIZE  0x000C0000u  // 768 KB (12 x 64 KB MMU pages)

#define LAKATOS_APP_IROM_BASE       0x40300000u  // Virtual instruction address base (.text)
#define LAKATOS_APP_DROM_BASE       0x3F700000u  // Virtual read-only data address base (.rodata)
#define LAKATOS_APP_DRAM_BASE       0x3FFC8000u  // Fixed static RAM arena base
#define LAKATOS_APP_DRAM_SIZE       0x00008000u  // 32 KB static arena (+ ~140 KB dynamic heap)

#define LAKATOS_SCREEN_W            320
#define LAKATOS_SCREEN_H            240

// RGB565 Color Constants (matching TFT_eSPI)
#define LAKATOS_BLACK       0x0000
#define LAKATOS_NAVY        0x000F
#define LAKATOS_DARKGREEN   0x03E0
#define LAKATOS_DARKCYAN    0x03EF
#define LAKATOS_MAROON      0x7800
#define LAKATOS_PURPLE      0x780F
#define LAKATOS_OLIVE       0x7BE0
#define LAKATOS_LIGHTGREY   0xD69A
#define LAKATOS_DARKGREY    0x7BEF
#define LAKATOS_BLUE        0x001F
#define LAKATOS_GREEN       0x07E0
#define LAKATOS_CYAN        0x07FF
#define LAKATOS_RED         0xF800
#define LAKATOS_MAGENTA     0xF81F
#define LAKATOS_YELLOW      0xFFE0
#define LAKATOS_WHITE       0xFFFF
#define LAKATOS_ORANGE      0xFDA0
#define LAKATOS_GREENYELLOW 0xB7E0
#define LAKATOS_PINK        0xFE19

// Text alignment datum constants (matching TFT_eSPI)
#define LAKATOS_TL_DATUM    0  // Top left
#define LAKATOS_TC_DATUM    1  // Top centre
#define LAKATOS_TR_DATUM    2  // Top right
#define LAKATOS_ML_DATUM    3  // Middle left
#define LAKATOS_MC_DATUM    4  // Middle centre
#define LAKATOS_MR_DATUM    5  // Middle right
#define LAKATOS_BL_DATUM    6  // Bottom left
#define LAKATOS_BC_DATUM    7  // Bottom centre
#define LAKATOS_BR_DATUM    8  // Bottom right

// Built-in waveform IDs for audio_set_voice(..., pcm = NULL, len = LAKATOS_WAVE_*, ...)
#define LAKATOS_AUDIO_RATE      15625u
#define LAKATOS_AUDIO_VOICES    16u
#define LAKATOS_WAVE_NONE       0u
#define LAKATOS_WAVE_SINE       1u
#define LAKATOS_WAVE_RAMP       2u
#define LAKATOS_WAVE_SQUARE     3u
#define LAKATOS_WAVE_TRIANGLE   4u
#define LAKATOS_WAVE_NOISE      5u

typedef void (*lakatos_audio_tick_cb_t)(void* user);
typedef bool (*lakatos_stream_cb_t)(const uint8_t* data, size_t len, void* user);

// ---------------------------------------------------------------------------
// 128-Byte .lapp Application Header (located at offset 0 of .lapp file,
// mapped at LAKATOS_APP_DROM_BASE = 0x3F700000)
// ---------------------------------------------------------------------------
typedef struct LakatosAppHeader {
  uint32_t magic;            // LAKATOS_APP_MAGIC ("LAKA")
  uint32_t header_version;   // 1
  uint32_t abi_version;      // LAKATOS_API_VERSION (1)
  uint32_t entry_addr;       // Virtual address of app_main in IROM (0x40300080+)
  uint32_t text_offset;      // File offset of .text (0x80)
  uint32_t text_size;        // Size of .text in bytes
  uint32_t rodata_offset;    // File offset of .rodata (mapped at 0x3F700000 + rodata_offset)
  uint32_t rodata_size;      // Size of .rodata in bytes
  uint32_t data_offset;      // File offset of initial .data image
  uint32_t data_dst_addr;    // Target RAM address for .data (0x3FFC8004+)
  uint32_t data_size;        // Size of .data in bytes
  uint32_t bss_dst_addr;     // Target RAM address for .bss
  uint32_t bss_size;         // Size of .bss in bytes (zeroed before entry)
  uint32_t total_file_size;  // Total size of the .lapp file in bytes
  uint8_t  sha256[32];       // SHA-256 of bytes [128 .. total_file_size)
  char     name[32];         // Null-terminated application title
  uint32_t flags;            // Reserved flags (0)
  uint32_t reserved;         // Reserved padding to 128 bytes (0)
} LakatosAppHeader;

#ifdef __cplusplus
static_assert(sizeof(LakatosAppHeader) == LAKATOS_APP_HEADER_SIZE,
              "LakatosAppHeader must be exactly 128 bytes");
#endif

// ---------------------------------------------------------------------------
// Lakatos OS Data Structures
// ---------------------------------------------------------------------------
typedef struct LakatosTouchSample {
  bool touched;
  int  x;
  int  y;
  int  rawX;
  int  rawY;
  int  rawZ;
} LakatosTouchSample;

typedef struct LakatosAirportInfo {
  char  code[4];   // 3-letter IATA code, e.g. "SFO"
  char  city[24];  // City name, e.g. "San Francisco"
  char  tz[32];    // POSIX timezone string
  float lat;
  float lon;
} LakatosAirportInfo;

typedef struct LakatosFsEntry {
  char    name[64];  // Entry base name (without trailing '/')
  int64_t size;      // File size in bytes
  int64_t mtime;     // UTC modification timestamp (seconds)
  bool    isDir;     // True if directory
} LakatosFsEntry;

// ---------------------------------------------------------------------------
// Lakatos System Call Table (passed to app_main and stored at 0x3FFC8000)
// ---------------------------------------------------------------------------
typedef struct LakatosApi {
  uint32_t api_version;
  uint32_t struct_size;

  // --- Lifecycle & Timing ---
  void     (*exit)(int code);
  void     (*yield)(void);
  uint32_t (*millis)(void);
  uint32_t (*micros)(void);
  void     (*delay_ms)(uint32_t ms);

  // --- Memory (Auto-reclaimed by Lakatos on app exit) ---
  void*    (*malloc)(size_t size);
  void*    (*calloc)(size_t nmemb, size_t size);
  void*    (*realloc)(void* ptr, size_t size);
  void     (*free)(void* ptr);
  size_t   (*free_heap_bytes)(void);

  // --- Device Identity, Config & Time ---
  const char* (*get_airport)(void);
  bool        (*get_airport_info)(LakatosAirportInfo* out);
  const char* (*get_device_name)(void);
  const char* (*get_serial_number)(void);
  const char* (*get_superhighway_url)(void);
  int64_t     (*get_unix_time)(void);
  bool        (*format_local_time)(char* buf, size_t len, const char* strftime_fmt);

  // --- Display (320x240 ILI9341) ---
  void     (*tft_fill_screen)(uint16_t color);
  void     (*tft_fill_rect)(int x, int y, int w, int h, uint16_t color);
  void     (*tft_draw_rect)(int x, int y, int w, int h, uint16_t color);
  void     (*tft_fill_round_rect)(int x, int y, int w, int h, int r, uint16_t color);
  void     (*tft_draw_round_rect)(int x, int y, int w, int h, int r, uint16_t color);
  void     (*tft_draw_line)(int x0, int y0, int x1, int y1, uint16_t color);
  void     (*tft_draw_fast_h_line)(int x, int y, int w, uint16_t color);
  void     (*tft_draw_fast_v_line)(int x, int y, int h, uint16_t color);
  void     (*tft_fill_circle)(int x, int y, int r, uint16_t color);
  void     (*tft_draw_circle)(int x, int y, int r, uint16_t color);
  void     (*tft_fill_triangle)(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t color);
  void     (*tft_draw_char)(int x, int y, char c, uint16_t fg, uint16_t bg, uint8_t size);
  void     (*tft_set_text_color)(uint16_t fg, uint16_t bg);
  void     (*tft_set_text_datum)(uint8_t datum);
  int      (*tft_draw_string)(const char* str, int x, int y, uint8_t font);
  void     (*tft_push_rgb565)(int x, int y, int w, int h, const uint16_t* pixels);

  // --- Touch, Stylus Keyboard, Sensors & LED ---
  LakatosTouchSample (*read_touch)(void);
  // Opens modal stylus keyboard; returns 1 if Enter pressed, 0 if Cancelled, -1 if ^C pressed
  int      (*prompt_keyboard)(const char* title, char* buf, size_t max_len, bool allow_ctrl_c);
  void     (*set_rgb_led)(bool r, bool g, bool b, uint32_t duration_ms);
  int      (*read_ldr)(void);
  float    (*read_cpu_temp_c)(void);

  // --- Unified VFS: /flash/..., /sd/..., /net/... ---
  int      (*fs_ls)(const char* path, LakatosFsEntry* out_entries, int max_entries);
  bool     (*fs_stat)(const char* path, LakatosFsEntry* out_entry);
  int32_t  (*fs_read)(const char* path, uint32_t offset, void* buf, size_t len);
  bool     (*fs_write)(const char* path, const void* buf, size_t len);
  bool     (*fs_write_at)(const char* path, uint32_t offset, const void* buf, size_t len);
  bool     (*fs_mkdir)(const char* path, bool parents);
  bool     (*fs_rmdir)(const char* path);
  bool     (*fs_rm)(const char* path);
  int64_t  (*fs_du)(const char* path);

  // --- Network, Formatting & Serial ---
  bool     (*wifi_is_connected)(void);
  int      (*http_get)(const char* url, void* buf, size_t max_len);
  int      (*serial_available)(void);
  int      (*serial_read)(void);
  size_t   (*serial_write)(const void* buf, size_t len);
  int      (*vsnprintf)(char* buf, size_t size, const char* fmt, va_list ap);
  uint32_t (*city_hash32)(const void* buf, size_t len);
  int      (*fetch_with_cache)(const char* url, void* buf, size_t max_len, uint32_t max_age_sec);

  // --- 4-Channel Core 0 Audio Synthesizer & Sample Mixer (15,625 Hz) ---
  void     (*audio_start)(void);
  void     (*audio_stop)(void);
  void     (*audio_set_voice)(int ch, const int8_t* pcm, uint32_t len,
                              uint32_t loop_start, uint32_t loop_len,
                              uint32_t step_fp16, uint8_t vol, bool reset_pos);
  void     (*audio_set_tick_cb)(lakatos_audio_tick_cb_t cb, void* user, uint32_t tick_samples);

  // --- Streaming VFS Reader with Transparent .zip Extraction ---
  int32_t  (*fs_read_stream)(const char* path, lakatos_stream_cb_t on_chunk, void* user);
} LakatosApi;

typedef void (*lakatos_app_entry_t)(const LakatosApi* os);

// Helper for apps to retrieve the LakatosApi pointer from the fixed RAM slot
static inline const LakatosApi* lakatos_os(void) {
  return *(const LakatosApi* const*)(uintptr_t)LAKATOS_APP_DRAM_BASE;
}

// Entry point declaration that every .lapp must implement
void app_main(const LakatosApi* os);

#ifdef __cplusplus
}
#endif
