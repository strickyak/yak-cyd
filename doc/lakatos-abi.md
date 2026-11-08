# Lakatos OS Application Binary Interface (`ABI v1`)

**Lakatos** (named for Imre Lakatos, with the `-os` ending) is the resident operating system, LCode 32-bit bytecode runtime, and fixed-address native application loader for the **ESP32-2432S028R (CYD)** board.

Lakatos owns hardware configuration (touch calibration, Wi-Fi auto-connect, LDR auto-brightness, home airport + timezone + NTP, device name & serial number, Superhighway URL & password) in NVS (`cyd_cfg`, `cyd_cal`, `cyd_wifi`), provides three unified filesystem trees (`/flash/...`, `/sd/...`, `/net/...`), and executes both RAM-resident LCode bytecode apps (`.lapp`) and MMU-mapped native binaries (`native.bin`).

---

## 1. Flash Partition Layout (`partitions_lakatos.csv` — 4 MB Flash)

| Name | Type / Subtype | Flash Offset | Size | Purpose |
| :--- | :--- | :--- | :--- | :--- |
| `nvs` | `data / nvs` | `0x009000` | `20 KB` (`0x005000`) | Persistent system config (`cyd_cal`, `cyd_wifi`, `cyd_cfg`) |
| `otadata` | `data / ota` | `0x00E000` | `8 KB` (`0x002000`) | Boot metadata |
| `app0` | `app / ota_0` | `0x010000` | `1,472 KB` (`0x170000`) | **Lakatos Resident OS** firmware |
| `app_slot` | `data / 0x40` | `0x180000` | `768 KB` (`0x0C0000`) | **App Staging & MMU Execution Slot** (`12 × 64 KB` MMU pages) |
| `spiffs` | `data / spiffs` | `0x240000` | `1,728 KB` (`0x1B0000`) | **Local Flash Filesystem (`/flash/...`)** via `LittleFS` |
| `coredump` | `data / coredump` | `0x3F0000` | `64 KB` (`0x010000`) | ESP32 crash core dump storage |

---

## 2. Fixed Virtual Memory Map & Two Execution Paths

### 2.1 LCode Bytecode Fast-Path (`LAKATOS_APP_FLAG_LCODE = 1`, Zero Flash Wear)

When [`lakatosRunAppFromFile()`](../src/lakatos/lakatos_loader.cpp) opens a `.lapp` whose header has `flags & LAKATOS_APP_FLAG_LCODE` (`1`) set:
1. It bypasses `app_slot` flash staging completely (**zero Flash erases**).
2. The `LCOD` bytecode payload (`<= 64 KB`) is read into RAM (reusing the 32 KB `gLakatosAppRamArena` at `0x3FFC8000` if `payloadLen <= 32 KB`, or heap otherwise), verified against `hdr.sha256`, and executed by the resident C++ LCode VM (`lakatosRunLCodeApp`).
3. **Native Chaining (`join_native`)**: If an LCode launcher stub calls `join_native("native.bin", "<head>")` (syscall `0x46`), the VM records `gLakatosJoinNativeTarget = "native.bin"` and `gLakatosAppArg = "<head>"` and exits cleanly back into `lakatosRunAppFromFile()`, which immediately chains into the native MMU loader below.

### 2.2 Native Xtensa LX6 MMU Execution (`native.bin` & Standalone Native `.lapp`)

When Lakatos launches a native binary (`flags == 0`), it compares the 128-byte `LakatosAppHeader` (including `sha256`) against the header already present at offset `0` of `app_slot` (`0x00180000`):
- **Cache Hit**: If the header in `app_slot` matches, Lakatos skips erasing and writing flash (`0 ms` staging overhead, `0` flash wear).
- **Cache Miss**: Lakatos erases the required range of `app_slot`, streams the binary in 4 KB chunks while computing SHA-256 on the fly, and verifies `hdr.sha256`.

Next, `mapAppFlashMmu()` programs the ESP32 Hardware Flash MMU on both CPU cores (`PRO_CPU` and `APP_CPU`) to map those physical 64 KB flash pages simultaneously into `IROM` and `DROM`:

| Region | Fixed Virtual Address Range | Max Size | Backing Storage | Description |
| :--- | :--- | :--- | :--- | :--- |
| **App Header** | `0x3F700000 .. 0x3F70007F` | `128 B` | `app_slot` (`0x180000`) | `LakatosAppHeader` (`"LAKA"` magic, entry point, section bounds, SHA-256) |
| **App `IROM` (`.text`)** | `0x40300080 .. 0x403BFFFF` | `767.8 KB` | `app_slot` via ICache MMU | Executable Xtensa LX6 instructions |
| **App `DROM` (`.rodata`)** | `0x3F700080 .. 0x3F7BFFFF` | `767.8 KB` | `app_slot` via DCache MMU | Read-only constants, strings, C++ `.init_array`, and initial `.data` image |
| **Reclaimed Linker Gap** | `_data_end .. 0x3FFC7FFF` | `~15.2 KB` | Internal SRAM (`gLakatosPngScratch`) | Non-fragmenting static scratch buffer for ROM `tinfl_decompressor` (`.zip` / `.png`) |
| **OS API Pointer Slot** | `0x3FFC8000 .. 0x3FFC8003` | `4 B` | Internal SRAM | Holds `const LakatosApi*` (`lakatos_os()`) |
| **App Argument Slot** | `0x3FFC8004 .. 0x3FFC8007` | `4 B` | Internal SRAM | Holds `const char*` head selector (`lakatos_app_arg()`, e.g. `"piano"`, `"smallertalk"`) |
| **App Static `DRAM`** | `0x3FFC8008 .. 0x3FFCFFFF` | `32,760 B` (`~32 KB`) | Internal SRAM (`gLakatosAppRamArena`) | App `.data` and overlaid per-head `.bss` (`OVERLAY _bss_start` in [`ld/lakatos_app.ld`](../ld/lakatos_app.ld)) |
| **App Dynamic Heap** | `os->malloc(size)` | `~140 KB` | Shared ESP32 Heap | Tracked by Lakatos (`up to 128 blocks`) and automatically freed on `os->exit(code)` |

---

## 3. `.lapp` / `native.bin` Binary Header (`128` Bytes at File Offset `0`)

Defined in [`include/lakatos_api.h`](../include/lakatos_api.h):

```c
#define LAKATOS_API_VERSION     1u
#define LAKATOS_APP_MAGIC       0x414B414Cu  // "LAKA" in little-endian
#define LAKATOS_LCODE_MAGIC     0x444F434Cu  // "LCOD" in little-endian
#define LAKATOS_APP_HEADER_SIZE 128u         // 0x80 bytes
#define LAKATOS_APP_FLAG_LCODE  1u           // Bit 0: LCode bytecode payload (runs in RAM, 0 flash wear)

typedef struct LakatosAppHeader {
  uint32_t magic;            // 0x414B414C ("LAKA")
  uint32_t header_version;   // 1
  uint32_t abi_version;      // LAKATOS_API_VERSION (1)
  uint32_t entry_addr;       // Virtual address of _lakatos_app_entry in IROM (0x40300080+, or 0 for LCode)
  uint32_t text_offset;      // File offset of .text (0x80)
  uint32_t text_size;        // Size of .text in bytes
  uint32_t rodata_offset;    // File offset of .rodata (mapped at 0x3F700000 + rodata_offset)
  uint32_t rodata_size;      // Size of .rodata in bytes
  uint32_t data_offset;      // File offset of initial .data image
  uint32_t data_dst_addr;    // Target RAM address for .data (0x3FFC8008+)
  uint32_t data_size;        // Size of .data in bytes
  uint32_t bss_dst_addr;     // Target RAM address for .bss
  uint32_t bss_size;         // Size of .bss in bytes (zeroed before entry)
  uint32_t total_file_size;  // Total size of the .lapp / native.bin file in bytes
  uint8_t  sha256[32];       // SHA-256 of bytes [128 .. total_file_size)
  char     name[32];         // Null-terminated application title (or "native.bin")
  uint32_t flags;            // Bit 0: LAKATOS_APP_FLAG_LCODE (1 = LCode bytecode, 0 = native Xtensa)
  uint32_t reserved;         // Reserved padding to 128 bytes (0)
} LakatosAppHeader;
```

---

## 4. `LakatosApi` System Call Table (`0x3FFC8000`)

Every native binary receives `const LakatosApi* os` as the first argument to `app_main(const LakatosApi* os)` and can also retrieve it globally via `lakatos_os()` (`*(const LakatosApi* const*)0x3FFC8000u`). Multi-head binaries retrieve their requested head name via `lakatos_app_arg()` (`*(const char* const*)0x3FFC8004u`).

| Group | `LakatosApi` Function Pointer | Behavior |
| :--- | :--- | :--- |
| **Header** | `uint32_t api_version`, `uint32_t struct_size` | `LAKATOS_API_VERSION` (`1`), `sizeof(LakatosApi)` |
| **Lifecycle & Timing** | `void (*exit)(int code)` | Unwinds via `longjmp` to Lakatos loader, stops audio & HTTP streams, frees tracked heap allocations, and restores the Lakatos OS shell |
| | `void (*yield)(void)` | Checks `lakatosShouldExitApp()` (top-left `12×12` corner tap), updates background tasks, and yields |
| | `uint32_t (*millis)(void)` / `uint32_t (*micros)(void)` | Milliseconds / microseconds since boot |
| | `void (*delay_ms)(uint32_t ms)` | Sleeps `ms` milliseconds in 10 ms slices while polling exit & background tasks |
| **Tracked Heap** | `void* (*malloc)(size_t)` / `calloc` / `realloc` / `free` | Allocates/frees from ESP32 heap; all live pointers are automatically freed on app exit |
| | `size_t (*free_heap_bytes)(void)` | Returns `ESP.getFreeHeap()` |
| **Identity & Time** | `const char* (*get_airport)(void)` | 3-letter IATA airport code (e.g. `"PDX"`) |
| | `bool (*get_airport_info)(LakatosAirportInfo* out)` | Populates `code`, `city`, `tz`, `lat`, `lon` |
| | `const char* (*get_device_name)(void)` / `get_serial_number` | Device identity from NVS (`cyd_cfg`) |
| | `const char* (*get_superhighway_url)(void)` | Configured `/net` base URL |
| | `int64_t (*get_unix_time)(void)` | Current UTC epoch timestamp |
| | `bool (*format_local_time)(char* buf, size_t len, const char* fmt)` | Formats local airport-timezone time via `strftime` |
| **Display (`320×240`)** | `tft_fill_screen`, `tft_fill_rect`, `tft_draw_rect`, `tft_fill_round_rect`, `tft_draw_round_rect`, `tft_draw_line`, `tft_draw_fast_h_line`, `tft_draw_fast_v_line`, `tft_fill_circle`, `tft_draw_circle`, `tft_fill_triangle`, `tft_draw_char`, `tft_set_text_color`, `tft_set_text_datum`, `tft_draw_string`, `tft_push_rgb565` | Mutex-guarded (`LakatosTftGuard`) `TFT_eSPI` primitives on the `320×240` ILI9341 display |
| **Input & Sensors** | `LakatosTouchSample (*read_touch)(void)` | Returns calibrated `(touched, x, y, rawX, rawY, rawZ)`; auto-exits if Core 0 latched a corner exit tap |
| | `int (*prompt_keyboard)(const char* title, char* buf, size_t max_len, bool allow_ctrl_c)` | Modal stylus keyboard (`1`=Enter, `0`=Cancel, `-1`=`^C`) |
| | `void (*set_rgb_led)(bool r, bool g, bool b, uint32_t duration_ms)` | Controls RGB LED (note: `r = true` on GPIO 4 also enables the SC8002B speaker amplifier) |
| | `int (*read_ldr)(void)` / `float (*read_cpu_temp_c)(void)` | Ambient light sensor (`0..4095`) and CPU temperature (°C) |
| **Unified VFS** | `fs_ls`, `fs_stat`, `fs_read`, `fs_write`, `fs_write_at`, `fs_mkdir`, `fs_rmdir`, `fs_rm`, `fs_du` | File and directory operations across `/flash`, `/sd`, and `/net` |
| | `int32_t (*fs_read_stream)(const char* path, lakatos_stream_cb_t on_chunk, void* user)` | Streams a file in 2 KB chunks; transparently detects `"PK\x03\x04"` `.zip` headers and inflates Store (`method 0`) or Deflate (`method 8`) payloads via ROM `tinfl_decompress` |
| **Network & Serial** | `wifi_is_connected`, `http_get`, `fetch_with_cache`, `city_hash32` | HTTP `GET`, `/net/cache/<CityHash32(url)>` caching, and Google `CityHash32` |
| | `http_stream_open`, `http_stream_read`, `http_stream_close`, `http_stream_write` | Live non-blocking HTTP and WebSocket (`ws://`) bidirectional streaming |
| | `serial_available`, `serial_read`, `serial_write`, `vsnprintf` | USB Serial I/O (automatically polls `@FUSE` packets on read/available) and `vsnprintf` |
| **Core 0 Audio (`15,625 Hz`)** | `audio_start`, `audio_stop` | Starts/stops the 16-voice `15,625 Hz` Timer Group 0 + 31.25 kHz LEDC PWM synthesizer on GPIO 26 and enables/disables the GPIO 4 speaker amplifier |
| | `audio_set_voice(ch, pcm, len, loop_start, loop_len, step_fp16, vol, reset_pos)` | Configures voice `ch` (`0..15`) for custom signed 8-bit `pcm` or built-in waveform (`pcm == NULL`, `len = LAKATOS_WAVE_*`) with 2 ms raised-cosine anti-pop envelopes |
| | `audio_set_tick_cb(cb, user, tick_samples)` | Registers a sample-accurate sequencer callback invoked on Core 0 every `tick_samples` samples |

---

## 5. Three Unified Filesystem Trees (`fs_*` APIs)

All Lakatos filesystem calls (`fs_ls`, `fs_stat`, `fs_read`, `fs_read_stream`, `fs_write`, `fs_write_at`, `fs_mkdir`, `fs_rmdir`, `fs_rm`, `fs_du`) accept paths in three namespaces:

1. **`/flash/...` — Local Internal Flash (`LittleFS`)**:
   - Mounted on the `1,728 KB` `spiffs` flash partition (`0x240000`).
   - Fast, non-volatile local storage for installed `.lapp` binaries and application data.
2. **`/sd/...` — MicroSD Card (`VSPI`)**:
   - Mounted on hardware `VSPI` (`SCK=18, MISO=19, MOSI=23, CS=5`).
   - Touch controller uses dedicated GPIO SPI (`CLK=25, MISO=39, MOSI=32, CS=33, IRQ=36`) so `/sd/...` and touch never conflict.
3. **`/net/...` — Superhighway Server over Wi-Fi HTTP**:
   - Implements [`doc/superhighway-client.md`](superhighway-client.md) against the configured Superhighway URL (default `http://pizga.net:1908/`).
   - Authenticates every request with HTTP Basic Auth where:
     - `username = <device_name><serial_number>` (from Lakatos `CONFIG`)
     - `password = <net_password>` (entered via stylus keyboard in Lakatos `CONFIG`, stored in NVS `cyd_cfg`, never stored in source code).

---

## 6. Building the 6-Headed `native.bin` & `.lapp` Launcher Stubs

All native C++ applications (`piano`, `modtrack`, `midiplay`, `radio`, `ham`, `smallertalk`) are linked into a single multi-head binary `build/apps/native.bin` (`~140 KB`, `17.8%` of `app_slot`) by [`tools/build_lapp.py`](../tools/build_lapp.py) and [`ld/lakatos_app.ld`](../ld/lakatos_app.ld), with [`apps/native_multi/dispatch.cpp`](../apps/native_multi/dispatch.cpp) dispatching `app_main(os)` based on `lakatos_app_arg()`:

```cpp
#include "lakatos_api.h"

extern "C" void app_main(const LakatosApi* os) {
  const char* head = lakatos_app_arg(); // e.g. "piano", "modtrack", "smallertalk"
  // Dispatch to requested head; return to Lakatos OS via:
  os->exit(0);
}
```

Each head has a corresponding ~210-byte `.lgolf` launcher stub (e.g. `apps/piano/piano.lgolf` $\rightarrow$ `build/apps/piano.lapp`) that calls `join_native("native.bin", "piano")`.

Compile all `.lgolf` apps and `native.bin` with:

```bash
make apps
```

Push updated binaries over USB serial to `/sd/apps/` with:

```bash
make push
```
