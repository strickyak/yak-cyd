# LCode & LGolf Game Authoring Guide for AI Agents

This guide teaches AI Coding Agents how to write, compile, emulate, debug, and publish games and applications for **Lakatos OS** on the **ESP32-2432S028R (Cheap Yellow Display / CYD)** using the **LCode** 32-bit bytecode engine and the **LGolf** (`.lgolf`) language.

---

## 1. Architecture Overview

- **Target Hardware**: ESP32-2432S028R ("Cheap Yellow Display")
  - **Display**: `320×240` 16-bit `RGB565` ILI9341 TFT (landscape orientation: `x = 0..319`, `y = 0..239`).
  - **Touchscreen**: XPT2046 resistive touch controller (`os_read_touch()` returns calibrated screen coordinates).
  - **Peripherals**: Active-low RGB LED (`os_set_rgb_led`), Light Dependent Resistor (`os_read_ldr`), internal temperature sensor (`os_read_cpu_temp`), and 3 unified VFS trees (`/lo` LittleFS, `/sd` MicroSD card, `/sh` Wi-Fi HTTP Superhighway).
- **Zero Flash Wear Execution**:
  - `.lapp` binaries compiled with `bin/lcode` have `LAKATOS_APP_FLAG_LCODE = 1` in their 128-byte SHA-256 `LakatosAppHeader` and contain a compact `LCOD` bytecode module (typically `1 KB – 16 KB`).
  - When selected in Lakatos OS (from `/lo`, `/sd`, or `/sh`), the `.lapp` streams directly into ESP32 RAM and runs inside the resident C++ LCode VM (`lakatosRunLCodeApp`) with **zero Flash erases**.
- **Flat 32-Bit Memory Arena (Up to 128 KB)**:
  - `0x00000 .. 0x000FF`: Null guard page (`256 B`, traps null pointer dereferences).
  - `0x00100 .. StringPoolEnd`: Read-only string pool.
  - `OsScratchBase`: `512 B` scratch ring buffer for strings returned by OS syscalls.
  - `GlobalsBase .. FrameBase`: Global variables, structs, and 1D/2D arrays.
  - `FrameBase .. HeapBase`: Local function call frames (`16 KB – 32 KB`).
  - `HeapBase .. End`: Coalescing free-list dynamic heap (`malloc`, `zalloc`, `realloc`, `free`, `new`, `make`, `delete`, `heap_free`).

---

## 2. Toolchain CLI (`./bin/lcode`)

Build the toolchain binary (if not already built):

```bash
make lcode-tool
# or directly:
cd tools/lcode && GOWORK=off go build -o ../../bin/lcode ./cmd/lcode
```

### Subcommands

1. **Compile `.lgolf` to `.lapp`**:
   ```bash
   ./bin/lcode compile -title "My Game" -o build/apps/mygame.lapp apps/mygame/mygame.lgolf
   ```
   - Use `-S` to inspect generated `.lasm` assembly.
2. **Run & Test in Headless 320×240 Emulator**:
   ```bash
   ./bin/lcode run --steps 300000 --ascii --dump-ppm /tmp/screen.ppm build/apps/mygame.lapp
   ```
   - Inject touch events with `--tap x,y` (can be repeated, e.g., `--tap 160,120 --tap 30,12`).
   - Always test that your game renders properly with `--ascii` (or `--dump-ppm`) and exits cleanly (`exit=0`) when its top-left `HOME` button (`x <= 64, y < 25`) is tapped!
3. **Disassemble `.lapp`**:
   ```bash
   ./bin/lcode dis build/apps/mygame.lapp
   ```
4. **Upload `.lapp` to the Superhighway (`/sh`)**:
   ```bash
   ./bin/lcode upload -url http://pizga.net:1908/ build/apps/mygame.lapp mygame.lapp
   ```
   - Optional Basic Auth can be passed via `-u user:pass` or environment variables `CYD_SH_USER` / `CYD_SH_PASS`.

---

## 3. LGolf Language Reference (`.lgolf`)

`LGolf` is a clean, statically-typed Go subset tailored for 32-bit embedded games:

### Scalar, Struct, Array, Pointer & Slice Types
- **Scalar types**: `int` (signed 32-bit), `uint` / `word` (unsigned 32-bit), `float32` / `float` (IEEE-754 single-precision floating point accelerated by the ESP32 Xtensa LX6 hardware FPU), `byte` / `uint8` (unsigned 8-bit), `int8` (signed 8-bit), `uint16` / `half` (unsigned 16-bit), `int16` (signed 16-bit), `bool`, `string`.
  - In arrays and structs, `byte`/`int8` pack into 1 byte and `uint16`/`int16` pack into 2 bytes! For example, `var board [42][64]byte` uses only `2,688` bytes of RAM.
- **Floating-Point Numbers, Casts & Hardware Math Intrinsics**:
  - Floating-point literals (`3.14`, `0.5`, `1e-3`), arithmetic (`+`, `-`, `*`, `/`, unary `-`, `+=`, `-=`, `*=`, `/=`), and comparisons (`==`, `!=`, `<`, `<=`, `>`, `>=`) emit hardware-FPU `float32` opcodes (`FADD`, `FSUB`, `FMUL`, `FDIV`, `FNEG`, `FCMP_*`).
  - Built-in float constants: `PI` (`3.1415927`), `TWO_PI`, `HALF_PI`, `DEG_TO_RAD`, `RAD_TO_DEG`, `E`.
  - Type conversions: `float32(x)` / `float(x)` (`I32_TO_F32`), `int(f)` (`F32_TO_I32`), `float32_bits(f)`, `float32_from_bits(u)`.
  - Math & trig intrinsics: `sqrt(x)`, `sin(rad)`, `cos(rad)`, `atan2(y, x)`, `fabs(x)` / `abs(x)`, `floor(x)`, `ceil(x)`, `round(x)`, `min(a, b)`, `max(a, b)`.
  - Formatting & display: `ftoa(f, decimals)` / `float_to_str(f, decimals)` returns a formatted `string` slice; `tft_draw_float(f, decimals, x, y, font)` draws a float directly on the TFT; `println(...)` prints both integers and `float32` values automatically.
- **Structs & Pointers**:
  ```go
  type Sprite struct {
      X     int
      Y     int
      VX    int
      VY    int
      Color int
      Next  *Sprite
  }

  var player Sprite
  var enemies [16]Sprite

  func moveSprite(s *Sprite) {
      s.X += s.VX
      s.Y += s.VY
  }
  ```
- **Dynamic Heap Allocation (`new`, `make`, `delete`, `malloc`, `free`)**:
  ```go
  node := new(Sprite)          // Zero-allocated struct on heap (*Sprite)
  buf  := make([]uint16, 256)  // Dynamic slice on heap
  rem  := heap_free()          // Free bytes remaining in LCode heap
  delete(buf)                  // Frees backing array of slice
  delete(node)                 // Frees struct pointer
  ```
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

### Built-in Syscall Functions

| Category | Function Signature | Description |
| :--- | :--- | :--- |
| **Lifecycle** | `os_exit(code int)` | Exits the app and returns to Lakatos OS shell |
| | `os_yield()` | Feeds watchdog & updates background Wi-Fi/LED tasks |
| | `os_millis() int` | Milliseconds since boot |
| | `os_micros() int` | Microseconds since boot |
| | `os_delay_ms(ms int)` | Sleeps `ms` milliseconds while servicing background tasks |
| | `os_rand(max int) int` | Pseudo-random integer in `[0, max)` |
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
| | `tft_draw_string(s string, x, y, font int) int` | Draws string using TFT font (`1`, `2`, `4`) |
| | `tft_draw_int(val, x, y, font int) int` | Draws integer at `(x,y)` using TFT font (`1`, `2`, `4`) |
| | `tft_draw_float(val float32, decimals, x, y, font int) int` | Draws `float32` with `decimals` places at `(x,y)` |
| | `ftoa(val float32, decimals int) string` | Formats `float32` as a `string` slice |
| | `tft_push_rgb565(x, y, w, h, pixAddr int)` | Pushes raw `uint16` RGB565 sprite buffer at `pixAddr` |
| | `tft_draw_png_file(path string, x, y int) bool` | Streams and draws a `.png` file from `/lo`, `/sd`, or `/sh` at `(x,y)` (transparent pixels skipped) |
| | `tft_draw_png(bufAddr, len, x, y int) bool` | Decodes and draws a PNG image from RAM buffer (or 3-arg `(sliceOrPath, x, y)`) |
| | `ok, w, h := os_png_info(path string)` | Reads the `IHDR` header of a `.png` file and returns `(ok, width, height)` |
| | `tft_draw_jpg_file(path string, x, y[, scale] int) bool` | Streams and draws a `.jpg` file via ROM TJpgDec (`scale`: `0`=1/1, `1`=1/2, `2`=1/4, `3`=1/8) |
| | `tft_draw_jpg(bufAddr, len, x, y[, scale] int) bool` | Decodes and draws a JPEG image from RAM buffer (or slice `(sliceOrPath, x, y[, scale])`) |
| | `ok, w, h := os_jpg_info(path string)` | Reads the JPEG header via ROM `jd_prepare` and returns `(ok, width, height)` |
| **Input & IO** | `touched, tx, ty := os_read_touch()` | Polls calibrated touch coordinates (`tx` in `0..319`, `ty` in `0..239`) |
| | `os_poll_touch(&tx, &ty) bool` | Pointer-based touch polling helper |
| | `os_prompt_keyboard(title string, bufAddr, maxLen, allowCtrlC int) int` | Opens Lakatos modal stylus keyboard (`1`=Enter, `0`=Cancel, `-1`=`^C`) |
| | `os_set_rgb_led(r, g, b, durationMs int)` | Controls on-board RGB LED |
| | `os_read_ldr() int` | Reads ambient light sensor (`0..4095`) |
| | `os_read_cpu_temp() int` | Reads ESP32 temperature in tenths of °C (e.g. `425` = `42.5°C`) |
| **Identity** | `os_get_airport() string` | Configured 3-letter airport code (e.g. `"PDX"`) |
| | `os_get_device_name() string` | Configured device name |
| | `os_get_serial_number() string` | Configured device serial number |
| | `os_get_unix_time() int` | Current UTC epoch timestamp |
| | `os_wifi_connected() int` | Returns `1` if Wi-Fi is connected, `0` otherwise |
| | `os_serial_available() int` | Number of bytes waiting on hardware Serial |
| | `os_serial_read() int` | Reads one byte from hardware Serial (`-1` if none) |
| | `os_prompt_keyboard(title string, bufAddr, maxLen int, allowSlash bool) int` | Opens the on-screen touch keyboard modal into `bufAddr` |
| **Core 0 Prefetch** | `os_prefetch_start(path string, bufAddr, maxLen int) int` | Starts asynchronous file prefetch on Core 0 (`PRO_CPU`) into `bufAddr` (`maxLen` cap) |
| | `os_prefetch_status() (state int, bytesRead int)` | Polls Core 0 prefetcher (`0`=IDLE, `1`=BUSY, `2`=READY, `<0`=ERROR) and bytes read |
| | `os_prefetch_cancel()` | Cancels any in-flight Core 0 prefetch and waits for idle |
| **VFS** | `fs_ls(path string, entriesBufAddr, maxEntries int) int` | Lists up to `maxEntries` `LakatosFsEntry` (`88 B` each) records (`<0` on error) |
| | `fs_stat(path string, entryBufAddr int) int` | Stats a single path into an `88 B` `LakatosFsEntry` struct (`1` if ok, `0` on error) |
| | `fs_read(path string, off, bufAddr, len int) int` | Reads bytes from `/lo/...`, `/sd/...`, or `/sh/...` |
| | `fs_write(path string, bufAddr, len int) int` | Writes file to `/lo/...`, `/sd/...`, or `/sh/...` |
| | `println(args...)` | Prints values to Serial / emulator stdout |

---

## 5. Minimal Game Skeleton

Every YAK-CYD / Lakatos game must provide a standardized `HOME` button in the **top-left corner** (`x <= 64, y < 25`, button box `(2, 2, 58, 20)`) labeled `"HOME"` that calls `os_exit(0)` so the user can return to the YAK-CYD index page at any time.

```go
package main

func drawHud(score int) {
    tft_fill_rect(0, 0, SCREEN_W, 24, 0x10A2)

    // Standardized top-left HOME button
    tft_fill_round_rect(2, 2, 58, 20, 4, 0x2945)
    tft_draw_round_rect(2, 2, 58, 20, 4, CYAN)
    tft_set_text_color(CYAN, 0x2945)
    tft_draw_string("HOME", 15, 4, 2)

    tft_set_text_color(CYAN, 0x10A2)
    tft_draw_string("MY GAME", 68, 4, 2)
    tft_set_text_color(YELLOW, 0x10A2)
    tft_draw_int(score, 160, 4, 2)
}

func main() {
    tft_fill_screen(BLACK)
    score := 0
    drawHud(score)

    px := 160
    py := 130
    tft_fill_circle(px, py, 10, GREEN)

    for {
        touched, tx, ty := os_read_touch()
        if touched {
            if ty < 25 && tx <= 64 {
                os_exit(0)
                return
            }
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

---

## 6. Controlling, Scripting, and Debugging the Device

Developers and AI agents have two complementary tools for inspecting the display, injecting inputs, and remotely controlling the CYD:

### 6.1 Reading the Display & Testing Touch (`./bin/lcode run`)

Rather than reading pixels back over the physical SPI bus, the host emulator in `tools/lcode/pkg/lcode/vm.go` executes the exact same `.lapp` bytecode against a virtual `320×240` `uint16` `RGB565` framebuffer (`VM.Screen[240][320]`):

- **ASCII Screen Snapshot (`80×30`)**:
  ```bash
  ./bin/lcode run --steps 400000 --ascii build/apps/life.lapp
  ```
- **Exact `320×240` Pixel Image Dump (`.ppm`)**:
  ```bash
  ./bin/lcode run --steps 400000 --dump-ppm /tmp/screen.ppm build/apps/life.lapp
  ```
- **Deterministic Touch Injection (`--tap x,y`)**:
  Queue one or more synthetic stylus taps into `os_read_touch()` to verify button hitboxes (such as the top-left `HOME` button at `30,12`):
  ```bash
  ./bin/lcode run --steps 400000 --tap 180,12 --tap 30,12 build/apps/life.lapp
  ```
- **Virtual VFS Root (`-vfs <dir>`)**:
  Mount a local host directory as `/` so `/lo/...`, `/sd/...`, and `/sh/...` file reads/writes and `tft_draw_png_file` / `tft_draw_jpg_file` calls can be tested offline.

### 6.2 Remote Control over the USB Serial Cable (`/dev/ttyUSB0`)

When the physical CYD is connected via USB (`/dev/ttyUSB0` at `115200` baud), `handleSerialCommand()` in `src/lakatos/lakatos_main.cpp` listens for newline-terminated commands whenever Lakatos OS is on the index screen:

| Serial Command | Description |
|---|---|
| `info` | Prints Lakatos OS version, device ID, airport code, `/sh` URL, Wi-Fi state, and free RAM (`ESP.getFreeHeap()`). |
| `ls [path]` | Lists any VFS directory directly on the device (`ls /lo`, `ls /sd`, `ls /sh`). |
| `run <path>` | Remotely launches a `.lapp` on the physical screen (e.g. `run /lo/life.lapp` or `run /sh/life.lapp`). The loader and VM print exact arena/heap/bytecode telemetry (`[LCODE] Launching ...`) and any `println()` output back over USB. |
| `cp <src> <dst>` | Copies a file between VFS trees on the device (e.g. `cp /sh/life.lapp /lo/life.lapp`). |
| `shpass <password>` | Saves the Superhighway `/sh` password directly into NVS (`cyd_cfg`) so credentials never appear in source code. |
| `config` | Opens the interactive `CONFIG` screen on the device. |

### 6.3 Python One-Liner to Inject USB Serial Commands

Use the project's `.venv/bin/python` (`pyserial`) to send commands to `/dev/ttyUSB0` and capture the device's response without toggling DTR/RTS (which would otherwise reset the ESP32):

```bash
.venv/bin/python -c '
import serial, time
s = serial.Serial()
s.port = "/dev/ttyUSB0"
s.baudrate = 115200
s.dtr = False
s.rts = False
s.timeout = 0.5
s.open()
s.reset_input_buffer()
for cmd in ["info", "ls /lo", "ls /sh"]:
    s.write((cmd + "\n").encode())
    time.sleep(1.2)
    out = s.read(8192).decode("utf-8", errors="replace")
    print(f"=== {cmd} ===\n{out.strip()}")
s.close()
'
```

To remotely copy an updated `.lapp` from the Superhighway (`/sh`) to `/lo` and launch it on the physical CYD over USB:

```bash
.venv/bin/python -c '
import serial, time
s = serial.Serial()
s.port = "/dev/ttyUSB0"
s.baudrate = 115200
s.dtr = False
s.rts = False
s.timeout = 0.5
s.open()
s.reset_input_buffer()
for cmd in ["cp /sh/life.lapp /lo/life.lapp", "run /lo/life.lapp"]:
    s.write((cmd + "\n").encode())
    time.sleep(2.5)
    print(s.read(8192).decode("utf-8", errors="replace"), end="")
s.close()
'
```

