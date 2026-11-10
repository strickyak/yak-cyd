# Writing Lakatos OS Apps & Games (LGolf & Native C++ Guide)

This guide explains how to write, compile, emulate, debug, and deploy games and applications for **Lakatos OS** on the **ESP32-2432S028R (Cheap Yellow Display / CYD)** using either the **LGolf** (`.lgolf`) language on the **LCode** 32-bit bytecode VM or **Native C/C++** via the multi-head `native.bin` architecture.

---

## 1. Architecture Overview

- **Target Hardware**: ESP32-2432S028R ("Cheap Yellow Display")
  - **Display**: `320×240` 16-bit `RGB565` ILI9341 TFT (landscape orientation: `x = 0..319`, `y = 0..239`).
  - **Touchscreen**: XPT2046 resistive touch controller (`os_read_touch()` / `os_read_click()` return calibrated screen coordinates; Core 0 continuously scans touch and intercepts top-left `12×12` corner exit taps).
  - **Audio**: Core 0 16-voice `15,625 Hz` DMA/timer wavetable & PCM synthesizer on GPIO 26 feeding the on-board SC8002B speaker amplifier.
  - **Peripherals**: Active-low RGB LED (`os_set_rgb_led`), Light Dependent Resistor (`os_read_ldr`), internal temperature sensor (`os_read_cpu_temp`), and 3 unified VFS trees (`/flash` LittleFS, `/sd` MicroSD card, `/net` Wi-Fi HTTP Superhighway).
- **Two App Execution Models**:
  1. **LCode / LGolf Bytecode Apps (`.lapp`, Zero Flash Wear)**:
     - `.lapp` binaries compiled with `./build/lakat compile` set `LAKATOS_APP_FLAG_LCODE = 1` in their 128-byte SHA-256 `LakatosAppHeader` and contain a compact `LCOD` bytecode module (typically `1 KB – 16 KB`).
     - When selected on the Lakatos Home screen (which scans `/sd/apps/` and `/flash/apps/`) or File Browser, the `.lapp` streams directly into ESP32 RAM and runs inside the resident C++ LCode VM (`lakatosRunLCodeApp`) with **zero Flash erases**.
  2. **Multi-Head Native C++ Apps (`native.bin` + `.lgolf` Launcher Stubs)**:
     - Compute- or DSP-intensive C/C++ apps (`piano`, `modtrack`, `midiplay`, `radio`, `ham`, `smallertalk`) are compiled against [`include/lakatos_api.h`](../include/lakatos_api.h) and linked together by [`tools/build_lapp.py`](../tools/build_lapp.py) into a single multi-head `native.bin` with overlaid `.bss` segments in the 32 KB `gLakatosAppRamArena` (`0x3FFC8000 .. 0x3FFCFFFF`).
     - `native.bin` is cached once in the `app_slot` flash partition (`0x180000`, `768 KB`); lightweight `.lgolf` stubs (`join_native("native.bin", "<head>")`) launch into any native head instantly without reflashing when switching between them.
- **LCode Flat 32-Bit Memory Arena (Up to 128 KB)**:
  - `0x00000 .. 0x000FF`: Null guard page (`256 B`, traps null pointer dereferences).
  - `0x00100 .. StringPoolEnd`: Read-only string pool.
  - `OsScratchBase`: `512 B` scratch ring buffer for strings returned by OS syscalls.
  - `GlobalsBase .. FrameBase`: Global variables, structs, and 1D/2D arrays.
  - `FrameBase .. HeapBase`: Local function call frames (`16 KB – 32 KB`).
  - `HeapBase .. End`: Coalescing free-list dynamic heap (`malloc`, `zalloc`, `realloc`, `free`, `new`, `make`, `delete`, `heap_free`).

---

## 2. Toolchain CLI (`./build/lakat`)

Build the Go toolchain binary (always with `GOWORK=off` if you have a parent Go workspace):

```bash
make lakat
# or directly:
GOWORK=off go build -o build/lakat ./cmd/lakat
```

### Subcommands

1. **Compile `.lgolf` (or `.lasm`) to `.lapp`**:
   ```bash
   ./build/lakat compile -title "Wind" -o build/apps/wind.lapp apps/wind/wind.lgolf
   ```
   - Optional flags: `-S` (emit `.lasm` assembly instead of `.lapp`), `-ram <bytes>` (arena size request), `-stack <bytes>` (frame stack size).
2. **Push Files to the CYD over USB Serial (`push`)**:
   ```bash
   # Push one or more compiled apps to /sd/apps/ (or /flash/apps/):
   ./build/lakat push build/apps/wind.lapp /sd/apps/

   # Or build and push all apps + native.bin via Makefile:
   make push
   ```
   - `lakat push` communicates over the `@FUSE` USB serial protocol (even if an LCode app is currently running, it exits cleanly back to the shell before transferring) and skips uploading files whose size and CRC32 already match on the device.
3. **Run & Test in Headless 320×240 Emulator (`run`)**:
   ```bash
   ./build/lakat run --steps 300000 --ascii --dump-ppm /tmp/screen.ppm build/apps/wind.lapp
   ```
   - Inject touch events with `--tap x,y` (can be repeated, e.g., `--tap 160,120 --tap 20,10`).
   - Mount a local directory as the virtual `/` root for `/flash`, `/sd`, and `/net` testing with `-vfs <dir>`.
4. **Disassemble `.lapp` (`dis`)**:
   ```bash
   ./build/lakat dis build/apps/wind.lapp
   ```
5. **Tether Serial Monitor, Remote Commands & Linux FUSE Mount (`tether`)**:
   ```bash
   ./build/lakat tether --wire=/dev/ttyUSB0
   ```
6. **Batch-Convert Images for CYD (`imagine`)**:
   ```bash
   ./build/lakat imagine ./raw_photos /tmp/cyd_photos
   ```
   - Scales `PNG`, `JPEG`, `GIF`, and `WebP` images down to fit within `320×240` and encodes baseline JPEGs ready for the CYD hardware ROM JPEG decoder.
7. **Upload `.lapp` to an HTTP Superhighway Server (`upload`)**:
   ```bash
   ./build/lakat upload -url http://pizga.net:1908/ build/apps/wind.lapp wind.lapp
   ```
   - Optional Basic Auth can be passed via `-u user:pass` or environment variables `CYD_SH_USER` / `CYD_SH_PASS`.

---

## 3. LGolf Language Reference (`.lgolf`)

`LGolf` is a clean, statically-typed Go subset tailored for 32-bit embedded apps and games:

### Scalar, Struct, Array, Pointer & Slice Types
- **Scalar types**: `int` (signed 32-bit), `uint` / `word` (unsigned 32-bit), `float32` / `float` (IEEE-754 single-precision floating point accelerated by the ESP32 Xtensa LX6 hardware FPU), `byte` / `uint8` (unsigned 8-bit), `int8` (signed 8-bit), `uint16` / `half` (unsigned 16-bit), `int16` (signed 16-bit), `bool`, `string`.
  - In arrays and structs, `byte`/`int8` pack into 1 byte and `uint16`/`int16` pack into 2 bytes! For example, `var board [42][64]byte` uses only `2,688` bytes of RAM.
- **Top-Level `const` and `var` Declarations**:
  - Top-level `const` declarations support integer, boolean, float, and string expressions.
  - Top-level scalar `var` declarations support compile-time constant initializers (e.g. `var volLevel int = 6` or `var pace float32 = 2.0`), which are automatically initialized at the start of `main()`.
- **Floating-Point Numbers, Casts & Hardware Math Intrinsics**:
  - Floating-point literals (`3.14`, `0.5`, `1e-3`), arithmetic (`+`, `-`, `*`, `/`, unary `-`, `+=`, `-=`, `*=`, `/=`), and comparisons (`==`, `!=`, `<`, `<=`, `>`, `>=`) emit hardware-FPU `float32` opcodes (`FADD`, `FSUB`, `FMUL`, `FDIV`, `FNEG`, `FCMP_*`).
  - Built-in float constants: `PI` (`3.1415927`), `TWO_PI`, `HALF_PI`, `DEG_TO_RAD`, `RAD_TO_DEG`, `E`.
  - Type conversions: `float32(x)` / `float(x)` (`I32_TO_F32`), `int(f)` (`F32_TO_I32`), `float32_bits(f)`, `float32_from_bits(u)`.
  - Math & trig intrinsics: `sqrt(x)`, `sin(rad)`, `cos(rad)`, `atan2(y, x)`, `fabs(x)` / `abs(x)`, `floor(x)`, `ceil(x)`, `round(x)`, `min(a, b)`, `max(a, b)`.
- **Strings & Text Formatting Rules**:
  - A `string` in LGolf is a 2-word read-only byte slice descriptor (`ptr`, `len`).
  - **Important**: The `+` operator performs numeric addition and **does not** concatenate strings! Using `+` on `string` values will add their slice descriptors as integers and trap on a bad address.
  - To draw composite text on screen, chain the pixel-width return values of `tft_draw_string`, `tft_draw_int`, or `tft_draw_float` (when using `TL_DATUM`), or use `itoa(n)` / `utoa(u)` / `ftoa(f, decimals)`:
    ```go
    tft_set_text_datum(TL_DATUM)
    tft_set_text_color(CYAN, BLACK)
    x := 50
    x += tft_draw_string("Note: ", x, 4, 2)
    x += tft_draw_string(noteName, x, 4, 2)
    x += tft_draw_string(" (", x, 4, 2)
    x += tft_draw_int(freqHz, x, 4, 2)
    x += tft_draw_string(" Hz)", x, 4, 2)
    ```
- **Built-in `"wedge"` Package, NW Exit X & NE Menu Wedge Conventions**:
  - Both the NW Exit `X` and the NE Menu Wedge use the OS corner control target size `s = os_get_corner_size()` (or `CornerSize()`), which is configurable in NVS (`"cornerSz"`) and on the Lakatos OS **CONFIG** screen from `12` up to `24` pixels (default `12`).
  - Core 0's background touch scanner automatically intercepts taps in the top-left `s×s` corner (`0 <= x < s && 0 <= y < s`) to exit the running app and return to Lakatos OS (unless disabled via `os_set_corner_exit(0)`).
  - Import `"wedge"` to draw and detect the standard corner controls and query `CornerSize()` when laying out adjacent top-bar buttons:
    - `DrawExitWedge()` — renders the white `s×s` `X` mark on a black background in the NW corner `(0..s-1, 0..s-1)`.
    - `DrawMenuWedge()` — renders the white downward-pointing right-triangle Menu Wedge (hypotenuse across `y=0`, 90° right angle pointing down) on a black background in the NE corner `(320-s..319, 0..s-1)`.
    - `IsMenuWedge(tx, ty) bool` — returns `true` when `(tx, ty)` is inside the NE Menu Wedge target `(320-s <= tx < 320 && 0 <= ty < s)`.
    - `IsExitWedge(tx, ty) bool` — returns `true` when `(tx, ty)` is inside the NW Exit `X` target `(0 <= tx < s && 0 <= ty < s)`.
    - `CornerSize() int` — returns the configured corner target size (`12..24`).
    ```go
    import "wedge"
    // ...
    DrawExitWedge()
    DrawMenuWedge()
    s := CornerSize() // 12..24: use to position adjacent top-bar buttons
    ```
- **Structs, Pointers & Dynamic Heap Allocation (`new`, `make`, `delete`, `malloc`, `free`)**:
  ```go
  type Sprite struct {
      X, Y, VX, VY int
      Color        uint16
      Next         *Sprite
  }

  node := new(Sprite)          // Zero-allocated struct on heap (*Sprite)
  buf  := make([]uint16, 256)  // Dynamic slice on heap (len=256, cap=256)
  raw  := malloc(1024)         // Raw byte buffer address (or zalloc(1024))
  rem  := heap_free()          // Free bytes remaining in LCode heap
  delete(buf)                  // Frees backing array of slice
  delete(node)                 // Frees struct pointer
  free(raw)                    // Frees raw buffer
  ```
- **Low-Level Memory & Slice Intrinsics**:
  - Byte access: `peek1(addr)` (unsigned `0..255`), `peeks1(addr)` (sign-extended `-128..127`), `poke1(addr, val)`.
  - 16-bit access: `peek2(addr)` (unsigned `uint16`), `peeks2(addr)` (signed `int16`), `poke2(addr, val)`.
  - 32-bit access: `peek4(addr)` (`int`/`uint`), `poke4(addr, val)`.
  - Bulk memory: `memcpy(dstAddr, srcAddr, len)`, `memset(dstAddr, byteVal, len)`, `memcmp(aAddr, bAddr, len) int`.
  - Slice helpers: `len(s)`, `cap(s)`, `cstr(s)` (returns pointer to NUL-terminated copy in scratch memory).
- **Control Flow**:
  - `if cond { ... } else if cond2 { ... } else { ... }`
  - `for i := 0; i < n; i++ { ... }`, `for cond { ... }`, `for { ... }`, `break`, `continue`
  - `switch x { case 1, 2: ... default: ... }`

---

## 4. Built-in Constants & Lakatos OS Syscalls

### Predefined Constants
- Display dimensions: `SCREEN_W = 320`, `SCREEN_H = 240`
- `RGB565` Colors (available both with and without `COLOR_` prefix):
  - `BLACK` (`0x0000`), `WHITE` (`0xFFFF`), `RED` (`0xF800`), `GREEN` (`0x07E0`), `BLUE` (`0x001F`), `CYAN` (`0x07FF`), `MAGENTA` (`0xF81F`), `YELLOW` (`0xFFE0`), `ORANGE` (`0xFDA0`), `PINK` (`0xFE19`), `GREENYELLOW` (`0xB7E0`), `NAVY` (`0x000F`), `DARKGREEN` (`0x03E0`), `DARKCYAN` (`0x03EF`), `MAROON` (`0x7800`), `PURPLE` (`0x780F`), `OLIVE` (`0x7BE0`), `LIGHTGREY` (`0xD69A`), `DARKGREY` (`0x7BEF`).
- Text alignment datums:
  - `TL_DATUM` (0), `TC_DATUM` (1), `TR_DATUM` (2), `ML_DATUM` (3), `MC_DATUM` (4), `MR_DATUM` (5), `BL_DATUM` (6), `BC_DATUM` (7), `BR_DATUM` (8).
- Audio synthesizer constants (`15,625 Hz`, 16 voices):
  - `AUDIO_RATE` (`15625`), `AUDIO_VOICES` (`16`)
  - Built-in waveforms: `WAVE_NONE` (`0`), `WAVE_SINE` (`1`), `WAVE_RAMP` (`2`), `WAVE_SQUARE` (`3`), `WAVE_TRIANGLE` (`4`), `WAVE_NOISE` (`5`).

### Built-in Syscall Functions

| Category | Function Signature | Description |
| :--- | :--- | :--- |
| **Lifecycle** | `os_exit(code int)` | Exits the app and returns to Lakatos OS shell |
| | `os_set_corner_exit(enable int)` | Enables (`1`, default) or disables (`0`) Core 0's automatic top-left `s×s` corner tap app exit |
| | `os_get_corner_size() int` | Returns the configured corner target size `s` (`12..24` pixels, default `12`) for the NW Exit `X` and NE Menu Wedge |
| | `os_set_corner_size(size int)` | Sets and persists the corner target size `s` (`12..24` pixels) |
| | `os_yield()` | Feeds watchdog, polls `@FUSE` serial commands, & updates background tasks |
| | `os_millis() int` | Milliseconds since boot |
| | `os_micros() int` | Microseconds since boot |
| | `os_delay_ms(ms int)` | Sleeps `ms` milliseconds while servicing background & `@FUSE` tasks |
| | `os_rand(max uint) uint` | True random `uint` in `[0, max)` via ESP32 hardware TRNG (`esp_random()`; full 32-bit word if `max == 0`) |
| **Display** | `rgb565(r int, g int, b int) int` | Packs 8-bit `(r, g, b)` into 16-bit `RGB565` color |
| | `tft_fill_screen(color int)` | Fills entire `320×240` screen |
| | `tft_fill_rect(x, y, w, h, color int)` | Fills rectangle |
| | `tft_draw_rect(x, y, w, h, color int)` | Draws rectangle outline |
| | `tft_fill_round_rect(x, y, w, h, r, color int)` | Fills rounded rectangle |
| | `tft_draw_round_rect(x, y, w, h, r, color int)` | Draws rounded rectangle outline |
| | `tft_draw_line(x0, y0, x1, y1, color int)` | Draws line between `(x0,y0)` and `(x1,y1)` |
| | `tft_draw_fast_hline(x, y, w, color int)` | Fast horizontal line |
| | `tft_draw_fast_vline(x, y, h, color int)` | Fast vertical line |
| | `tft_fill_circle(x, y, r, color int)` | Fills circle |
| | `tft_draw_circle(x, y, r, color int)` | Draws circle outline |
| | `tft_fill_triangle(x0, y0, x1, y1, x2, y2, color int)` | Fills triangle |
| | `tft_set_text_color(fg int, bg int)` | Sets foreground and background text colors |
| | `tft_set_text_datum(datum int)` | Sets text alignment datum (`TL_DATUM`, `MC_DATUM`, etc.) |
| | `tft_draw_string(s string, x, y, font int) int` | Draws string using TFT font (`1`, `2`, `4`) and returns pixel width; also supports 6-arg `(x, y, s, fg, bg, font)` |
| | `tft_draw_int(val, x, y, font int) int` | Draws integer at `(x,y)` using TFT font (`1`, `2`, `4`) and returns pixel width; also supports 6-arg `(x, y, val, fg, bg, font)` |
| | `tft_draw_float(val float32, decimals, x, y, font int) int` | Draws `float32` with `decimals` places at `(x,y)` and returns pixel width |
| | `tft_draw_char(x, y, ch, fg, bg, size int)` | Draws a single ASCII character `ch` at `(x,y)` scaled by `size` |
| | `itoa(val int) string` / `utoa(val uint) string` | Formats signed `int` or unsigned `uint` as a `string` slice |
| | `ftoa(val float32, decimals int) string` | Formats `float32` as a `string` slice |
| | `tft_push_rgb565(x, y, w, h, pixAddr int)` | Pushes raw `uint16` RGB565 sprite buffer at `pixAddr` |
| | `tft_draw_pal4(x, y, w, h, palAddr, pix4bppAddr int)` | Renders a 4-bit indexed bitmap (`2 pixels/byte`, high nibble first) using 16-entry `uint16` RGB565 palette at `palAddr` |
| | `pal4_clear(bufAddr, w, h, colorIdx int)` | Fast-clears a 4-bit `pal4` buffer (`w * h / 2` bytes) to `colorIdx` (`0..15`) |
| | `pal4_fill_triangle(bufAddr, w, h, x0, y0, x1, y1, x2, y2, colorIdx int)` | Rasterizes a clipped filled triangle into a 4-bit `pal4` buffer |
| | `pal4_fill_circle(bufAddr, w, h, cx, cy, r, colorIdx int)` | Rasterizes a clipped filled circle into a 4-bit `pal4` buffer |
| | `pal4_draw_line(bufAddr, w, h, x0, y0, x1, y1, colorIdx int)` | Draws a Cohen-Sutherland viewport-clipped line into a 4-bit `pal4` buffer |
| | `pal4_render_3d(bufAddr, w, h, camAddr, primsAddr, primCount int) int` | Renders a 3D scene graph (`primCount` 64-byte `float32[16]` primitives at `primsAddr` with 48-byte `float32[12]` camera at `camAddr`) into a 4-bit `pal4` buffer and returns visible count |
| | `save_png_pal4(path string, w, h, palAddr, pix4bppAddr int) int` | Encodes and writes a 4-bit indexed-color `.png` file to VFS (`1` on success, `0` on error) |
| | `tft_draw_png_file(path string, x, y int) bool` | Streams and draws a `.png` file from `/flash`, `/sd`, or `/net` at `(x,y)` (transparent pixels skipped) |
| | `tft_draw_png(bufAddr, len, x, y int) bool` | Decodes and draws a PNG image from RAM buffer (or 3-arg `(sliceOrPath, x, y)`) |
| | `ok, w, h := os_png_info(path string)` | Reads the `IHDR` header of a `.png` file and returns `(ok, width, height)` |
| | `tft_draw_jpg_file(path string, x, y[, scale] int) bool` | Streams and draws a `.jpg` file via ROM TJpgDec (`scale`: `0`=1/1, `1`=1/2, `2`=1/4, `3`=1/8) |
| | `tft_draw_jpg(bufAddr, len, x, y[, scale] int) bool` | Decodes and draws a JPEG image from RAM buffer (or slice `(sliceOrPath, x, y[, scale])`) |
| | `ok, w, h := os_jpg_info(path string)` | Reads the JPEG header via ROM `jd_prepare` and returns `(ok, width, height)` |
| **Input & IO** | `touched, tx, ty := os_read_touch()` | Continuous canvas/game touch stream (`tx` in `0..319`, `ty` in `0..239`); stays `true` while held/dragged |
| | `clicked, tx, ty := os_read_click()` | Discrete button/link tap; fires at most once per physical press (`110 ms` continuous release debounce + `250 ms` inter-click cooldown) |
| | `os_poll_touch(&tx, &ty) bool` | Pointer-based continuous touch polling helper |
| | `os_poll_click(&tx, &ty) bool` | Pointer-based discrete click polling helper |
| | `os_prompt_keyboard(title string, bufAddr, maxLen, allowCtrlC int) int` | Opens Lakatos modal stylus keyboard (`1`=Enter, `0`=Cancel, `-1`=`^C`) |
| | `os_set_rgb_led(r, g, b, durationMs int)` | Controls on-board RGB LED (see hardware audio note below) |
| | `os_read_ldr() int` | Reads ambient light sensor (`0..4095`) |
| | `os_read_cpu_temp() int` | Reads ESP32 temperature in tenths of °C (e.g. `425` = `42.5°C`) |
| | `os_serial_available() int` | Number of bytes waiting on hardware Serial |
| | `os_serial_read() int` | Reads one byte from hardware Serial (`-1` if none) |
| | `os_serial_write(bufAddr, len int) int` | Writes `len` bytes from `bufAddr` to hardware Serial |
| | `println(args...)` | Prints values to Serial / emulator stdout |
| **Identity, Time & Net** | `os_get_airport() string` | Configured 3-letter IATA airport code (e.g. `"PDX"`) |
| | `os_get_airport_info(outStructAddr int) int` | Fills a 52-byte `LakatosAirportInfo` struct (`code[4]`, `name[36]`, `tzOffsetMin`, `latE4`, `lonE4`) |
| | `os_get_device_name() string` | Configured device name |
| | `os_get_serial_number() string` | Configured device serial number |
| | `os_get_sh_url() string` | Configured Superhighway `/net` base URL |
| | `os_get_unix_time() int` | Current UTC epoch timestamp |
| | `os_format_local_time(fmtStr string, bufAddr, maxLen int) int` | Formats local airport-timezone time via `strftime` into `bufAddr` |
| | `os.CityHash32(ptr *byte, size uint) uint` | Computes Google's 32-bit `CityHash32` (also accepts a 1-arg `string` or `[]byte` slice, or `os_city_hash32`) |
| | `os_wifi_connected() int` | Returns `1` if Wi-Fi is connected, `0` otherwise |
| | `os_http_get(url string, bufAddr *byte, maxLen int) int` | Performs an HTTP/HTTPS `GET` into `bufAddr` (up to `maxLen` bytes); returns bytes read or `<0` on error (also `os.HttpGet`) |
| | `os.FetchWithCache(url string, bufAddr *byte, maxLen int, maxAgeSec int) int` | Reads `/net/cache/<CityHash32(url)>`; if missing or older than `maxAgeSec`, fetches `url` and caches it (also `os_fetch_with_cache`) |
| | `ws_open(urlOrPath string) int` | Connects an RFC 6455 WebSocket to a Superhighway room path (e.g. `"/ws/chat"`, `"/croquet"`) or `ws://` URL (`1` on success, `0` on failure) |
| | `ws_send(bufAddr *byte, len int) int` | Sends a WebSocket text frame (`1` on success, `-1` on disconnect; also accepts a 1-arg `string`/`[]byte` slice) |
| | `ws_recv(bufAddr *byte, maxLen int) int` | Non-blocking poll for the next WebSocket frame into `bufAddr` (`>0` bytes read, `0` if none waiting, `<0` on disconnect) |
| | `ws_close()` | Closes the active WebSocket connection |
| **Core 0 Prefetch** | `os_prefetch_start(path string, bufAddr, maxLen int) int` | Starts asynchronous file prefetch on Core 0 (`PRO_CPU`) into `bufAddr` (`maxLen` cap) |
| | `os_prefetch_status() (state int, bytesRead int)` | Polls Core 0 prefetcher (`0`=IDLE, `1`=BUSY, `2`=READY, `<0`=ERROR) and bytes read |
| | `os_prefetch_cancel()` | Cancels any in-flight Core 0 prefetch and waits for idle |
| **VFS** | `fs_ls(path string, entriesBufAddr, maxEntries int) int` | Lists up to `maxEntries` `LakatosFsEntry` (`88 B` each) records (`<0` on error) |
| | `fs_stat(path string, entryBufAddr int) int` | Stats a single path into an `88 B` `LakatosFsEntry` struct (`1` if ok, `0` on error) |
| | `fs_read(path string, off, bufAddr, len int) int` | Reads bytes from `/flash/...`, `/sd/...`, or `/net/...` |
| | `fs_write(path string, bufAddr, len int) int` | Writes/overwrites file on `/flash/...`, `/sd/...`, or `/net/...` |
| | `fs_write_at(path string, offset, bufAddr, len int) int` | Writes `len` bytes at `offset` (`offset == 0` truncates; `offset > 0` patches in-place) |
| | `fs_mkdir(path string, parents int) int` | Creates directory (recursive `mkdir -p` if `parents != 0`) |
| | `fs_rmdir(path string) int` | Removes an empty directory |
| | `fs_rm(path string) int` | Deletes a file |
| | `fs_du(path string) int` | Recursively computes disk usage in bytes |
| **Audio Synth** | `audio_start()` | Starts the Core 0 16-voice `15,625 Hz` audio synthesizer on GPIO 26 and enables the speaker amplifier |
| | `audio_stop()` | Stops the audio synthesizer and disables the speaker amplifier (also called automatically when the app exits) |
| | `audio_set_voice(ch, waveType, stepFp16, vol, resetPos int)` | Sets channel `ch` (`0..15`) to play built-in waveform `waveType` (`WAVE_SINE`, `WAVE_SQUARE`, etc.) at `stepFp16 = round(freqHz * 256 * 65536 / 15625)` and volume `vol` (`0..64` normal, `>64` overmodulated); also supports 8-arg custom PCM `(ch, pcmAddr, len, loopStart, loopLen, stepFp16, vol, resetPos)` |
| **Native Chain** | `join_native(binName string, appArg string)` | Chains directly into a multi-head native binary (e.g. `"native.bin"`, `"piano"`) in `app_slot` |

> [!WARNING]
> **Hardware Audio Amplifier & GPIO 4 (`LED_R_PIN`)**: On the CYD board, GPIO 4 (`LED_R_PIN`, active-low) also controls the SC8002B speaker amplifier enable (`r = 1` enables the amplifier, `r = 0` mutes it). When `audio_start()` is active, if your app calls `os_set_rgb_led(r, g, b, durationMs)` for visual effects, always pass `r = 1` so you do not inadvertently mute the speaker amplifier!

---

## 5. Minimal LGolf App Skeleton & Touch Debouncing Best Practices

Every Lakatos app should draw the standard top-left `12×12` exit wedge (`DrawExitWedge()` from `import "wedge"`). Core 0 automatically intercepts taps in the `12×12` NW corner (`x < 12 && y < 12`) to exit the app cleanly:

```go
package main

import "wedge"

func drawHud(score int) {
    tft_fill_rect(0, 0, SCREEN_W, 22, 0x10A2)
    DrawExitWedge()

    tft_set_text_datum(TL_DATUM)
    tft_set_text_color(CYAN, 0x10A2)
    x := 18
    x += tft_draw_string("MY GAME  Score: ", x, 3, 2)
    tft_set_text_color(YELLOW, 0x10A2)
    tft_draw_int(score, x, 3, 2)
}

func main() {
    tft_fill_screen(BLACK)
    score := 0
    drawHud(score)

    px := 160
    py := 130
    tft_fill_circle(px, py, 10, GREEN)

    for {
        clicked, tx, ty := os_read_click()
        if clicked {
            if ty >= 26 {
                tft_fill_circle(px, py, 10, BLACK)
                px = tx
                py = ty
                tft_fill_circle(px, py, 10, GREEN)
                score++
                drawHud(score)
            }
        }
        os_delay_ms(20)
    }
}
```

### 5.1 Touch Input & Debouncing Best Practices

The CYD uses an **XPT2046 4-wire resistive touchscreen** where stylus or finger contact frequently exhibits brief Z-pressure dropouts during a hold and contact bounce during lift-off. Follow these rules to prevent accidental double-clicks and modal bleed-through:

1. **Choose `os_read_click()` vs. `os_read_touch()` Deliberately**:
   - **Use `os_read_click()`** for all discrete UI actions: toolbar buttons, list rows, toggles, and dialog buttons. In [`src/lakatos/lakatos_main.cpp`](../src/lakatos/lakatos_main.cpp), `os_read_click()` (`lakatosReadClick`) enforces:
     - **`kTouchHoldBridgeMs = 35 ms`**: Bridges momentary Core 0 ADC conversion gaps.
     - **`kTouchReleaseDebounceMs = 110 ms`**: Requires `XPT2046_IRQ` to remain continuously `HIGH` (no physical contact) for `110 ms` before re-arming for a new click.
     - **`kClickCooldownMs = 250 ms`**: Enforces a minimum `250 ms` interval between consecutive delivered clicks.
   - **Use `os_read_touch()`** *only* for continuous dragging or holding (e.g., drawing strokes in `paint`, moving the paddle in `breakout`, or continuous camera rotation in `croquet`). Never wire `os_read_touch()` directly to a state-toggling button without edge detection and release debouncing.

2. **Prevent Modal & Screen-Transition Bleed-Through (`settleModalTransition` + `pollFilteredClick`)**:
   - `os_read_click()` fires on the **leading edge** (initial touch-down) so buttons feel responsive. However, when a tap opens or closes a modal screen (such as clicking a line in `basic` or `logo` to open the Line Editor Modal, or tapping `[SAVE]` / `[CANCEL]` to return), the new screen draws while the user's finger is **still pressed down**. As the finger lifts off ~150–220 ms later, resistive lift-off bounce can trigger an unintended click on whatever button occupies the same `(x, y)` on the new screen.
   - Whenever an LGolf app transitions into or out of a modal dialog (including after `os_prompt_keyboard`), insert a `220 ms` settle pause and guard click polling with a `220 ms` timestamp filter (as in [`apps/basic/basic.lgolf`](../apps/basic/basic.lgolf) and [`apps/logo/logo.lgolf`](../apps/logo/logo.lgolf)):
     ```go
     var lastAcceptedClickMs int = -1000

     func settleModalTransition() {
         os_delay_ms(220)
     }

     func pollFilteredClick() (bool, int, int) {
         clicked, tx, ty := os_read_click()
         if clicked {
             now := os_millis()
             if now-lastAcceptedClickMs < 220 {
                 return false, 0, 0
             }
             lastAcceptedClickMs = now
             return true, tx, ty
         }
         return false, 0, 0
     }
     ```

3. **Native C/C++ Apps (`native.bin`): Use `TouchClickFilter`**:
   - Because [`LakatosApi`](../include/lakatos_api.h) exposes raw `os->read_touch()`, native C++ apps with discrete UI controls should wrap `os->read_touch()` in [`TouchClickFilter`](../apps/smallertalk/touch_filter.cpp) (used by `smallertalk`), which provides:
     - A **2-sample spatial cluster gate** (`clusterRadiusPx = 14 px` within `80 ms`) so single-sample resistive spike outliers during finger contact/lift-off are rejected.
     - **Release dropout hysteresis** (`releaseDebounceMs = 95 ms`) so brief pressure dips while holding do not split one press into two clicks.
     - **Inter-click cooldown** (`clickCooldownMs = 210 ms`) and **`touchFilterResetAfterModal(filter, nowMs)`** when entering or leaving modal dialogs.

---

## 6. Writing Native C/C++ Apps & Multi-Head `native.bin`

When an application requires heavy real-time C/C++ processing (such as the 16-voice audio apps `piano`, `modtrack`, and `midiplay`, streaming web `radio`, `ham` solar/DX propagation globe, or the `smallertalk` Smalltalk-80 VM), it is built as a head inside the unified `build/apps/native.bin` image and launched via a tiny `.lgolf` stub.

### 6.1 Native C/C++ App Structure & `LakatosApi`

Each native app lives in `apps/<name>/<name>.cpp` and exports a single entry point placed in `.entry` (or dispatched by `apps/native_multi/dispatch.cpp`):

```cpp
#include "../../include/lakatos_api.h"

extern "C" void app_main(const LakatosApi* os) {
  os->tft_fill_screen(LAKATOS_BLACK);
  // Main loop...
  for (;;) {
    LakatosTouchSample ts = os->read_touch();
    if (ts.touched && ts.y < 22 && ts.x < 48) {
      os->exit(0);
    }
    os->delay_ms(16);
  }
}
```

Key capabilities exposed in [`LakatosApi`](../include/lakatos_api.h) (`LAKATOS_API_VERSION = 1`, documented in [`doc/lakatos-abi.md`](lakatos-abi.md)) beyond the standard LCode syscalls:
- **Core 0 Audio Sequencer Tick Callback**:
  - `os->audio_set_tick_cb(cb, user, tick_samples)` registers a callback invoked directly by the Core 0 `15,625 Hz` synthesizer every `tick_samples` samples (e.g., `312` samples = `50 Hz` PAL tick for ProTracker `.mod` or MIDI sequencing) with zero jitter from Core 1 TFT drawing.
- **Streaming VFS & Transparent `.zip` Extraction**:
  - `os->fs_read_stream(path, on_chunk, user)` streams arbitrary-sized files from `/flash`, `/sd`, or `/net` in 2 KB chunks without buffering the entire file in RAM, and transparently inflates single-file `.zip` archives on the fly (e.g., `.mod.zip`, `.mid.zip`, `.ima.zip`).
- **Streaming HTTP & WebSocket Client**:
  - `os->http_stream_open(url)`, `os->http_stream_read(buf, max_len)`, `os->http_stream_write(buf, len)`, and `os->http_stream_close()` provide non-blocking HTTP and WebSocket (`ws://`) streams for internet radio (`radio`) and KiwiSDR (`ham`).
- **32 KB Static `gLakatosAppRamArena` (`0x3FFC8000 .. 0x3FFCFFFF`)**:
  - `0x3FFC8000` holds `const LakatosApi*` (`lakatos_os()`) and `0x3FFC8004` holds `const char*` (`lakatos_app_arg()`, e.g. `"piano"`, `"modtrack"`, `"midiplay"`, `"radio"`, `"ham"`, `"smallertalk"`).
  - All native heads in `native.bin` overlay their static `.bss` / `COMMON` globals in `0x3FFC8008 .. 0x3FFCFFFF` ([`ld/lakatos_app.ld`](../ld/lakatos_app.ld)), so adding new heads to `native.bin` does not reduce available RAM.
  - Dynamic allocations during execution use `os->malloc(size)` / `os->calloc(nmemb, size)` / `os->realloc(ptr, size)` / `os->free(ptr)` (automatically reclaimed when the app exits).

### 6.2 Adding or Rebuilding a Native Head

1. Add your C/C++ implementation under `apps/<name>/` and register its dispatch entry in `apps/native_multi/dispatch.cpp` and the `native-multi` target of [`Makefile`](../Makefile) (which invokes [`tools/build_lapp.py`](../tools/build_lapp.py)).
2. Create a 5-line `.lgolf` launcher stub in `apps/<name>/<name>.lgolf`:
   ```go
   package main

   func main() {
       join_native("native.bin", "<name>")
   }
   ```
3. Build and push both the `.lapp` stub and `native.bin`:
   ```bash
   make apps && make push
   ```

---

## 7. Controlling, Scripting, and Debugging the Device

Developers and AI agents have two complementary tools for inspecting the display, injecting inputs, and remotely controlling the CYD:

### 7.1 Reading the Display & Testing Touch (`./build/lakat run`)

Rather than reading pixels back over the physical SPI bus, the host emulator in [`pkg/lakat/vm.go`](../pkg/lakat/vm.go) executes the exact same `.lapp` bytecode against a virtual `320×240` `uint16` `RGB565` framebuffer (`VM.Screen[240][320]`):

- **ASCII Screen Snapshot (`80×30`)**:
  ```bash
  ./build/lakat run --steps 400000 --ascii build/apps/wind.lapp
  ```
- **Exact `320×240` Pixel Image Dump (`.ppm`)**:
  ```bash
  ./build/lakat run --steps 400000 --dump-ppm /tmp/screen.ppm build/apps/wind.lapp
  ```
- **Deterministic Touch Injection (`--tap x,y`)**:
  Queue one or more synthetic stylus taps into `os_read_touch()` / `os_read_click()` to verify button hitboxes (such as the top-left `EXIT` button at `20,10`):
  ```bash
  ./build/lakat run --steps 400000 --tap 160,10 --tap 20,10 build/apps/wind.lapp
  ```
- **Virtual VFS Root (`-vfs <dir>`)**:
  Mount a local host directory as `/` so `/flash/...`, `/sd/...`, and `/net/...` file reads/writes and `tft_draw_png_file` / `tft_draw_jpg_file` calls can be tested offline.

### 7.2 Remote Control over the USB Serial Cable (`/dev/ttyUSB0`)

When the physical CYD is connected via USB (`/dev/ttyUSB0` at `115200` baud), `lakatosPollSerialFuse()` in [`src/lakatos/lakatos_main.cpp`](../src/lakatos/lakatos_main.cpp) services `@FUSE` transfers at all times (even while an LCode app is inside `os_delay_ms()` or `os_yield()`) and executes interactive shell commands whenever Lakatos OS is on the Home or File Browser screen:

| Serial Command | Description |
|---|---|
| `info` | Prints Lakatos OS version, device ID (`name/serial`), airport code, `/net` URL, Wi-Fi state, and free RAM (`ESP.getFreeHeap()`). |
| `df` / `storage` | Prints mounted/free/used storage for `/flash` and `/sd` (`storage` also opens the interactive `STORAGE` screen on the device). |
| `ls [path]` | Lists any VFS directory directly on the device (`ls /sd/apps`, `ls /flash/apps`, `ls /net`). |
| `run <path>` | Remotely launches an app on the physical screen (e.g. `run /sd/apps/wind.lapp`). The loader and VM print arena/heap/bytecode telemetry (`[LCODE] Launching ...`) and any `println()` output back over USB. |
| `preview <path>` | Opens the file preview modal on the physical screen for `<path>`. |
| `cp <src> <dst>` | Copies a file between VFS trees on the device (e.g. `cp /net/wind.lapp /sd/apps/wind.lapp`). |
| `wifi <ssid> [pw]` | Saves Wi-Fi credentials to NVS (`cyd_wifi`) and connects immediately. |
| `airport <iata>` | Sets the 3-letter home airport code (e.g. `airport PDX`) for local timezone & weather. |
| `devname <name>` / `serial <sn>` | Sets the device name or serial number in NVS (`cyd_cfg`). |
| `neturl <url>` / `netpass <pw>` | Sets the Superhighway `/net` base URL or Basic Auth password in NVS (`cyd_cfg`). |
| `dev [on\|off]` | Queries or toggles `DEVELOPER` mode (shows the Core 0 white/yellow touch indicator dot and logs `[DEV]` events for button clicks, VFS file opens/reads/writes, image decoding, and app launches). |
| `config` | Opens the interactive `CONFIG` screen on the device. |

### 7.3 Using `./build/lakat tether` for Live Logs & Serial Commands

`./build/lakat tether` connects directly to the CYD over USB serial (default `--wire=/dev/ttyUSB0`, or `--wire=/dev/ttyACM0`), streams log output to `stdout`, and sends lines read from `stdin` (or positional arguments) to the CYD:

```bash
# Interactive cooked-stdin session (type commands + Enter, Ctrl-D or Ctrl-C to exit):
./build/lakat tether --wire=/dev/ttyUSB0

# One-shot scripted commands with a 1.5s response drain:
./build/lakat tether --wire=/dev/ttyUSB0 -drain 1500ms info df "ls /sd/apps"

# Pipe commands from stdin:
printf "info\nrun /sd/apps/wind.lapp\n" | ./build/lakat tether -drain 2s
```

### 7.4 Linux FUSE Filesystem at `/tmp/lakatos/{flash,sd,net}`

When `./build/lakat tether` is running and connected to a CYD on Linux, it automatically mounts a live FUSE filesystem at `/tmp/lakatos` (customizable via `-mount`, or disabled with `-mount=""`).

Beneath `/tmp/lakatos` are the three VFS directories:
- `/tmp/lakatos/flash/` — On-board ESP32 LittleFS (`/flash`)
- `/tmp/lakatos/sd/` — MicroSD card (`/sd`)
- `/tmp/lakatos/net/` — Superhighway HTTP VFS (`/net`, proxied through the CYD)

While `./build/lakat tether` is running in one terminal, you can use standard Linux shell commands in another terminal to inspect, copy, edit, or delete files on the CYD:

```bash
# List installed apps on the CYD's MicroSD card and Flash:
ls -la /tmp/lakatos/sd/apps /tmp/lakatos/flash/apps

# Copy a compiled .lapp directly into the CYD's /sd/apps/:
cp build/apps/wind.lapp /tmp/lakatos/sd/apps/wind.lapp

# Read or copy files out of the CYD's VFS:
cp /tmp/lakatos/sd/paint/painting1.png /tmp/
```
