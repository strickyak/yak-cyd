# Writing Lakatos OS Sub-Apps (`.sapp` Guide & `ambient` Architecture)

A **Lakatos Sub-App** (`.sapp`) is a self-contained **LGolf / LCode** bytecode program stored in `/{sd,net,flash}/subapps/` that can be invoked as a subroutine by a running **Native C++ App** (`native.bin`) via `os->run_subapp(name_or_path, arg)`.

Sub-apps were created for the native [`ambient`](../apps/ambient/ambient.cpp) application so that `ambient` can interleave static JPEG slides from `/net/pix/` with live, algorithmically generated visual programs, test patterns, clocks, shaders, or procedural animations—and then resume the native app right where it left off without flashing or losing native state.

---

## 1. Quick Start: Building, Testing & Deploying a `.sapp`

Sub-apps use the exact same LGolf compiler (`./build/lakat compile`) and binary container (`LakatosAppHeader` + `LCOD` bytecode) as `.lapp` apps, but use the `.sapp` file extension and reside in `/subapps/` instead of `/apps/` so they do not clutter the Lakatos Home screen launcher.

### Directory Layout
- **Source code**: `subapps/<name>/<name>.lgolf` (e.g., [`subapps/colorbars/colorbars.lgolf`](../subapps/colorbars/colorbars.lgolf))
- **Compiled output**: `build/subapps/<name>.sapp`
- **Device VFS search paths** (checked in priority order by `os->run_subapp`):
  1. `/sd/subapps/<name>.sapp`
  2. `/net/subapps/<name>.sapp`
  3. `/flash/subapps/<name>.sapp`

### Compile, Emulate & Push
```bash
# 1. Compile .lgolf source into a .sapp binary
./build/lakat compile -title "Color Bars" -o build/subapps/colorbars.sapp subapps/colorbars/colorbars.lgolf

# 2. Run & verify in the headless 320x240 emulator
./build/lakat run --steps 500000 --ascii --dump-ppm /tmp/colorbars.ppm build/subapps/colorbars.sapp

# 3. Push to the CYD's SD card (/sd/subapps/) over USB serial
./build/lakat push build/subapps/colorbars.sapp /sd/subapps/
```

---

## 2. Concrete Rules: What Sub-Apps CAN and SHOULD NOT Do

### What a Sub-App CAN Do
1. **Declare global variables, structs, and 1D/2D arrays for all working memory**:
   - Global `var` declarations (such as `var topCols [7]int`, `var pal [16]int`, or `var fb [9600]byte` for a 4-bit `160×120` paletted framebuffer) are allocated inside a single contiguous `vm->mem` block when the sub-app starts and freed as a single block when the sub-app exits.
2. **Use all 2D/3D TFT and Pal4 rendering syscalls**:
   - Direct TFT drawing: `os_tft_fill_screen`, `os_tft_fill_rect`, `os_tft_draw_line`, `os_tft_fill_circle`, `os_tft_fill_triangle`, `os_tft_draw_string`, etc.
   - Flicker-free 4-bit paletted framebuffers & 3D software rendering: `os_pal4_clear`, `os_pal4_fill_triangle`, `os_pal4_fill_circle`, `os_pal4_draw_line`, `os_pal4_render_3d`, and `os_tft_draw_pal4`.
   - Image rendering: `os_tft_draw_jpg_file`, `os_tft_draw_png_file`.
3. **Use hardware floating-point (`float32`) and math intrinsics**:
   - `sin`, `cos`, `atan2`, `sqrt`, `fabs`, `floor`, `ceil`, `round` are all hardware-accelerated by the ESP32 Xtensa LX6 FPU.
4. **Read sensors, clock, and arguments**:
   - `os_get_app_arg()` retrieves the `arg` string passed to `os->run_subapp(name, arg)`.
   - `os_get_unix_time()`, `os_format_local_time()`, `os_read_ldr()`, `os_read_cpu_temp()`, `os_get_airport()`, `os_get_device_name()`.
5. **Play synthesized audio (if appropriate)**:
   - A sub-app can call `os_audio_start()` / `os_audio_note()`. When the sub-app exits, the VM automatically calls `lakatosAudioStop()` so no lingering tones leak into the parent native app.

### What a Sub-App SHOULD NOT Do

1. **DO NOT call `malloc()`, `zalloc()`, `realloc()`, `free()`, `new()`, or `make()`**:
   - **Rule**: Sub-apps should be **100% static-allocation programs** using top-level `var` globals and local stack variables.
   - **Why**: Static globals allow `lakatosRunLCodeApp` to know the sub-app's exact memory footprint from its 24-byte `LCOD` header before executing a single instruction. Avoiding dynamic allocations inside the sub-app guarantees deterministic memory usage and zero internal or external heap fragmentation across thousands of back-to-back slideshow runs.
2. **DO NOT exceed ~64 KB of total global variables**:
   - **Rule**: Keep the sum of all global `var` declarations under `64 KB` (and ideally under `24 KB`).
   - **Why**: Every LCode VM instance allocates a single contiguous `vm->mem` buffer from the ESP32 8-bit DRAM heap (`DRAM1`) to hold the string pool, `512 B` OS scratch buffer, global variables, and `4 KB` call-frame stack. Although `ambient` frees its `64 KB` JPEG buffer before calling `os->run_subapp()` (exposing ~`110 KB` of contiguous DRAM1), staying under `64 KB` leaves comfortable headroom for Wi-Fi/TCP buffers and file streams.
3. **DO NOT run an infinite loop without a bounded exit timer**:
   - **Rule**: Unlike standalone `.lapp` games that run until the user quits, a sub-app called from `ambient` is a **timed visual slide** (typically **10 to 30 seconds**). Track elapsed time (or frame count) and call `os_exit(0)` (or return from `main()`) when finished.
4. **DO NOT busy-wait without polling touch (`os_read_click` / `os_read_touch`) and yielding (`os_delay_ms`)**:
   - **Rule**: Every animation or wait loop **must** poll `os_read_click()` (or `os_read_touch()`) and call `os_delay_ms(15..40)` on each frame/tick:
     ```go
     clicked, _, _ := os_read_click()
     if clicked != 0 {
         os_exit(0) // User tapped the screen to advance to the next slide
     }
     os_delay_ms(40)
     ```
   - **Why**:
     - Polling `os_read_click()` / `os_read_touch()` allows Lakatos OS to intercept taps in the **NW Corner Exit X** (`0..corner_size, 0..corner_size`). If the user taps the NW Exit corner while a sub-app is running, the sub-app terminates immediately and `os->run_subapp()` automatically exits the parent `ambient` app back to the Lakatos Home screen.
     - Tapping anywhere else on the screen lets the user skip ahead to the next slide in `ambient`, matching the tap-to-advance behavior of JPEG slides.
     - Calling `os_delay_ms()` yields Core 1 to the FreeRTOS scheduler and resets the task watchdog.
5. **DO NOT call `os_join_native()`**:
   - **Rule**: Never call `os_join_native()` from a `.sapp`.
   - **Why**: A sub-app is already running *inside* a native app (`native.bin`) mapped into the hardware Flash MMU slot (`app_slot`). `api_run_subapp` explicitly clears and ignores `gLakatosJoinNativeTarget` to prevent recursive native re-entry.
6. **DO NOT leave background VFS prefetch jobs or half-written files open**:
   - If a sub-app ever writes a file, write it atomically with `os_fs_write()` rather than leaving state across multiple frames, because a user tap or NW corner exit can terminate the sub-app on any frame.

---

## 3. Reference Example: `colorbars.lgolf`

[`subapps/colorbars/colorbars.lgolf`](../subapps/colorbars/colorbars.lgolf) (compiled to `679` bytes in `build/subapps/colorbars.sapp`) demonstrates all of the rules above:
- Uses only global arrays (`topCols`, `midCols`) and local variables—zero `malloc()`/`free()`.
- Draws an NTSC SMPTE color bars test pattern algorithmically (`7` top 75% bars, `7` middle reverse castellations, and bottom `-I`, `100% White`, `+Q`, and `-4/0/+4 IRE` PLUGE bars).
- Runs a 2-second (`2000 ms`) loop in `40 ms` steps, checking serial (`q`/`^C`) and touch (`os_read_click()`) so tapping advances immediately and tapping the NW corner exits `ambient` to Home.

```go
package main

var topCols [7]int
var midCols [7]int

func initColors() {
	// Top 2/3: 75% White, Yellow, Cyan, Green, Magenta, Red, Blue
	topCols[0] = 0xBDF7
	topCols[1] = 0xFFE0
	topCols[2] = 0x07FF
	topCols[3] = 0x07E0
	topCols[4] = 0xF81F
	topCols[5] = 0xF800
	topCols[6] = 0x001F

	// Middle castellations: Blue, Black, Magenta, Black, Cyan, Black, 75% White
	midCols[0] = 0x001F
	midCols[1] = 0x0000
	midCols[2] = 0xF81F
	midCols[3] = 0x0000
	midCols[4] = 0x07FF
	midCols[5] = 0x0000
	midCols[6] = 0xBDF7
}

func drawColorBars() {
	for i := 0; i < 7; i = i + 1 {
		x0 := (i * 320) / 7
		x1 := ((i + 1) * 320) / 7
		w := x1 - x0
		os_tft_fill_rect(x0, 0, w, 160, topCols[i])
		os_tft_fill_rect(x0, 160, w, 20, midCols[i])
	}

	// Bottom 1/4 (y = 180..239): -I, 100% White, +Q, Black, PLUGE (-4/0/+4 IRE), Black
	os_tft_fill_rect(0, 180, 57, 60, 0x0195)   // -I (deep indigo)
	os_tft_fill_rect(57, 180, 57, 60, 0xFFFF)  // 100% White
	os_tft_fill_rect(114, 180, 57, 60, 0x3815) // +Q (deep violet)
	os_tft_fill_rect(171, 180, 58, 60, 0x0000) // Black

	// PLUGE pulse bars under column 5 (x = 229..273)
	os_tft_fill_rect(229, 180, 15, 60, 0x0000) // -4 IRE
	os_tft_fill_rect(244, 180, 15, 60, 0x0841) //  0 IRE
	os_tft_fill_rect(259, 180, 15, 60, 0x18C3) // +4 IRE

	// Rightmost black reference under column 6 (x = 274..319)
	os_tft_fill_rect(274, 180, 46, 60, 0x0000)
}

func main() {
	initColors()
	drawColorBars()

	elapsed := 0
	for elapsed < 2000 {
		if os_serial_available() > 0 {
			ch := os_serial_read()
			if ch == 'q' || ch == 'Q' || ch == 3 {
				os_exit(0)
			}
		}
		clicked, _, _ := os_read_click()
		if clicked != 0 {
			os_exit(0)
		}
		os_delay_ms(40)
		elapsed = elapsed + 40
	}
	os_exit(0)
}
```

---

## 4. How `ambient` Calls Sub-Apps: Design Decisions & Rationale

### 4.1 Why `ambient` Is Native C++ and Sub-Apps Are LCode (`.sapp`)

We considered two possible architectures for `ambient` and its pluggable visual modules:
1. **Native parent (`ambient` in `native.bin`) + LCode bytecode sub-apps (`.sapp`)** *(chosen)*
2. **Native parent + native machine-code sub-apps** *(rejected)*

**Why LCode `.sapp` sub-apps won**:
- **Zero Flash Wear & Instant Loading**: The ESP32 has no general-purpose RAM instruction MMU for position-independent C++ binaries, and flashing a native binary into `app_slot` on every slide transition would wear out the SPI flash and take several seconds per slide. By contrast, an LCode `.sapp` is `0.5 KB – 8 KB`, streams from `/sd/subapps/` or `/net/subapps/` straight into RAM in a few milliseconds, and causes **zero flash wear**.
- **Memory Safety & Clean Teardown**: The LCode VM bounds-checks all memory accesses against `vm->memSize` and frees the entire VM arena (`vm->mem`, `vm->codeBuf`, `vm`) in a single step when the sub-app returns. A buggy or prematurely exited sub-app cannot corrupt `ambient`'s C++ state or leak heap blocks.
- **High-Level Expressiveness with Native-Speed Drawing**: While LCode instructions are interpreted, all heavy pixel pushing (`os_tft_fill_rect`, `os_pal4_render_3d`, `os_tft_draw_pal4`) and math (`sin`, `cos`, `sqrt`) execute in native C++ / hardware FPU inside the Lakatos kernel.

### 4.2 Why `ambient` Doesn't "Overwrite" Its Own `.lapp` Stub in RAM

When the user launches `ambient` from the Home screen:
1. [`apps/ambient/ambient.lgolf`](../apps/ambient/ambient.lgolf) (`374 B`) runs briefly in the LCode VM and calls `os_join_native("native.bin", "ambient")`.
2. That initial LCode VM **completely exits and frees all of its RAM** before `lakatosRunAppFromFile()` maps `native.bin` into the Flash MMU (`0x40300000` IROM / `0x3F700000` DROM) and jumps to `native.bin`'s `app_main()`.
3. Once `native.bin` is running, the LCode VM C++ interpreter (`lakatosRunLCodeApp` in [`src/lakatos/lakatos_lcode_vm.cpp`](../src/lakatos/lakatos_lcode_vm.cpp)) sits idle in kernel flash, ready to be invoked as a C++ subroutine at any time via `os->run_subapp()`.

### 4.3 Keeping `gLakatosAppRamArena` (`0x3FFC8000`) Untouched During Sub-Apps

A critical subtlety we solved in [`src/lakatos/lakatos_loader.cpp`](../src/lakatos/lakatos_loader.cpp):
- When a **standalone** `.lapp` is launched from the Home screen, no native app is running, so the loader temporarily uses the 32 KB `gLakatosAppRamArena` (`0x3FFC8000 .. 0x3FFCFFFF`) to hold the raw `.lapp` file payload while starting the VM.
- However, when `ambient` is running as a **native app**, `native.bin`'s `.data` and `.bss` sections—including `ambient`'s [`AmbientState S`](../apps/ambient/ambient.cpp) (`~4.1 KB`, holding the 500-photo shuffled catalog `S.imgNames[500][8]`, current slide index, and timing settings)—live inside `gLakatosAppRamArena`!
- **Design Decision**: [`api_run_subapp()`](../src/lakatos/lakatos_loader.cpp) **never touches `gLakatosAppRamArena`**. Instead, it allocates `payloadBuf` on the heap (`malloc(payloadLen)`), verifies the SHA-256 signature, runs `lakatosRunLCodeApp()`, and frees `payloadBuf`. Similarly, the PNG decoder checks `!lakatosIsNativeAppRunning()` before borrowing `gLakatosAppRamArena`. As a result, `ambient`'s C++ global state in `0x3FFC8000` remains 100% intact across every sub-app invocation.

### 4.4 Cooperative Heap Hand-Off (`runSubAppSlide` in `ambient.cpp`)

While displaying JPEGs, `ambient` keeps a `64 KB` heap buffer (`S.imgBuf`) allocated so Core 0 can prefetch the next JPEG from `/net/pix/` in the background while the user views the current slide.

Before calling `os->run_subapp()`, [`runSubAppSlide()`](../apps/ambient/ambient.cpp) performs a clean heap hand-off:
```cpp
static void runSubAppSlide(const char* subappName) {
  const LakatosApi* os = S.os;
  if (!os->run_subapp) return;

  // 1. Stop Core 0 prefetcher and free the 64 KB JPEG buffer
  os->prefetch_cancel();
  S.prefetchIdx = -1;
  if (S.imgBuf) {
    os->free(S.imgBuf);
    S.imgBuf = nullptr;
  }

  // 2. Run the .sapp in the LCode VM (with ~110 KB contiguous DRAM1 free)
  os->run_subapp(subappName, "");

  // 3. Re-claim the 64 KB JPEG buffer when the sub-app returns
  S.imgBuf = (uint8_t*)os->malloc(kBufMax);
}
```
Because the LCode VM frees 100% of its allocations before `os->run_subapp()` returns, `S.imgBuf = (uint8_t*)os->malloc(65536)` immediately re-claims the exact same contiguous 64 KB block without heap fragmentation.

### 4.5 Unified Session Lifecycle & NW Corner Exit Propagation

When `ambient` starts, `lakatosRunAppFromFile()` calls `lakatosBeginAppSession()`. To make sub-apps behave as seamless subroutines of `ambient`:
1. **Preserving the Parent Session**: In [`lakatosRunLCodeApp()`](../src/lakatos/lakatos_lcode_vm.cpp), `lakatosBeginAppSession()` and `lakatosEndAppSession()` are guarded by `if (!lakatosIsNativeAppRunning())`. Thus, when a `.sapp` finishes normally via `os_exit(0)`, the parent native app's session (including any Demo-mode timeout deadline) remains active.
2. **Propagating User Exit (`NW Corner X`)**:
   - Normal completion of a sub-app (`os_exit(0)`) sets `vm->running = false` without setting the global `gPleaseExitApp` flag, so `os->run_subapp()` simply returns `0` to `ambient`.
   - If the user taps the **NW Corner Exit X** (or sends the serial exit fuse, or the Demo-mode timer expires) *while the sub-app is on screen*, `lakatosShouldExitApp()` becomes `true`. The LCode VM immediately halts and cleans up its RAM, and [`api_run_subapp()`](../src/lakatos/lakatos_loader.cpp) detects `if (gAppRunning && lakatosShouldExitApp())` and calls `api_exit(0)` (`longjmp`), exiting `ambient` cleanly back to the Lakatos Home screen.
3. **State Restoration**: `api_run_subapp()` saves and restores `gLakatosAppArg`, the corner-exit enable flag (`lakatosGetCornerExitEnabled()`), and the TFT text datum (`TL_DATUM`) around every sub-app call so a sub-app cannot accidentally alter the parent native app's environment.
