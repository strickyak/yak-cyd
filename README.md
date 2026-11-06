# Yak-CYD Firmware Suite (`yak-cyd`)

Firmware, **Lakatos OS / LCode** 32-bit bytecode runtime, **LGolf** compiler toolchain, and multi-head native app suite for the **ESP32-2432S028R** ("Cheap Yellow Display" / **ELEGOO 2.8" ESP32 Display**).

## Quick Start (`Makefile` Targets)

```bash
make push           # Build all .lapp apps + native.bin, flash Lakatos OS, and push apps to /sd/apps/
make lakatos        # Build .lapp apps + native.bin, compile & upload Lakatos OS firmware
make apps           # Compile LGolf (.lgolf) apps and 6-headed native.bin into build/apps/
make lakat          # Build ./build/lakat (compiler, assembler, disassembler, emulator, pusher, tether)
make web-installer  # Build self-contained WebSerial installer bundle in build/web-installer/
make monitor        # Open 115200-baud serial monitor
```

---

## 1. `lakatos` (`src/lakatos/`, `cmd/lakat/`, `pkg/lakat/`) — Lakatos OS & LCode Runtime

### Core OS Modules (`src/lakatos/`)
- [`src/lakatos/lakatos_main.cpp`](file:///home/strick/github.com/strickyak/yak-cyd/src/lakatos/lakatos_main.cpp) — Lakatos OS graphical shell, file browser across `/flash`, `/sd`, and `/net`, Storage & About screens, Core 0 5 ms XPT2046 touchscreen scanner with top-left corner exit, and serial `@FUSE` protocol handler.
- [`src/lakatos/lakatos_config.cpp`](file:///home/strick/github.com/strickyak/yak-cyd/src/lakatos/lakatos_config.cpp) — Persistent NVS configuration (`cyd_cfg` & `cyd_cal`): 3-tap touch calibration, Wi-Fi scanner, stylus keyboard, airport/timezone/NTP, device name + serial number, and Superhighway URL/password.
- [`src/lakatos/lakatos_vfs.cpp`](file:///home/strick/github.com/strickyak/yak-cyd/src/lakatos/lakatos_vfs.cpp) — Unified VFS mounting `/flash` (`LittleFS`), `/sd` (`MicroSD`), and `/net` (Wi-Fi HTTP Superhighway client + HTTP/WebSocket bidirectional stream API and Core 0 background file prefetcher).
- [`src/lakatos/lakatos_loader.cpp`](file:///home/strick/github.com/strickyak/yak-cyd/src/lakatos/lakatos_loader.cpp) — Fixed-address native `app_slot` loader (verifying SHA-256 against the `app_slot` partition to avoid redundant flash writes), transparent `.zip` deflate stream reader, and Core 0 16-voice `15,625 Hz` audio synthesizer & sample mixer on GPIO 26.
- [`src/lakatos/lakatos_lcode_vm.cpp`](file:///home/strick/github.com/strickyak/yak-cyd/src/lakatos/lakatos_lcode_vm.cpp) — Resident 32-bit **LCode** bytecode virtual machine with IEEE-754 `float32` support, coalescing `malloc`/`free` heap, and `OpSYSCALL` bridge, executing `.lapp` binaries directly in RAM with zero Flash wear.
- [`src/lakatos/lakatos_png.cpp`](file:///home/strick/github.com/strickyak/yak-cyd/src/lakatos/lakatos_png.cpp) & [`src/lakatos/lakatos_jpg.cpp`](file:///home/strick/github.com/strickyak/yak-cyd/src/lakatos/lakatos_jpg.cpp) — Streaming PNG and ROM `tjpgd` JPEG decoders/renderers plus 4-bit indexed palette rendering and PNG saving.

### Go Toolchain (`cmd/lakat/`, `pkg/lakat/`)
- [`cmd/lakat/main.go`](file:///home/strick/github.com/strickyak/yak-cyd/cmd/lakat/main.go) & [`pkg/lakat/`](file:///home/strick/github.com/strickyak/yak-cyd/pkg/lakat/compiler.go) — Go CLI (`./build/lakat`) providing:
  - `lakat compile` — Compiles **LGolf** (`.lgolf`) source files into signed `.lapp` bytecode packages.
  - `lakat asm` / `lakat dis` — LCode (`.lasm`) assembler and disassembler.
  - `lakat run` — Headless `320×240` `RGB565` LCode emulator with touch injection and framebuffer dump.
  - `lakat push` / `lakat ls` — Direct USB serial file transfer to `/flash` or `/sd` via the `@FUSE` protocol.
  - `lakat tether` — Mounts the device's live `/flash`, `/sd`, and `/net` VFS at `/tmp/lakatos/` via Linux FUSE.
- [`doc/writing-lakatos-apps.md`](file:///home/strick/github.com/strickyak/yak-cyd/doc/writing-lakatos-apps.md) — Guide to writing, compiling, emulating, and publishing `.lgolf` apps and games for Lakatos OS.

---

## 2. LGolf Bytecode Apps (`apps/*/*.lgolf` $\rightarrow$ `build/apps/*.lapp`)

RAM-resident `.lapp` apps compiled from **LGolf** (a Go-like language targeting the 32-bit LCode VM):

- [`apps/wind/wind.lgolf`](file:///home/strick/github.com/strickyak/yak-cyd/apps/wind/wind.lgolf) — **Wind Chimes**: Polyphonic sine-wave wind chimes with selectable **E Min Pentatonic (Just)** (`E3..E6`), **G Maj Pentatonic (Just)** (`G3..G6`), and **Diminished** (`G3..G6` even-tempered) scales, 5.2s ADSR envelopes, Poisson arrival timing, interactive chime tubes, and `PACE` / `VOL` (`0..11`) sliders.
- [`apps/life/life.lgolf`](file:///home/strick/github.com/strickyak/yak-cyd/apps/life/life.lgolf) — **Conway's Life**: Toroidal cellular automaton (`2,382` bytes compiled `.lapp`, also embedded in ROM seed).
- [`apps/gallery/gallery.lgolf`](file:///home/strick/github.com/strickyak/yak-cyd/apps/gallery/gallery.lgolf) — **Gallery Viewer**: Full-screen PNG/JPEG slideshow viewer across `/sd`, `/flash`, and `/net` with Core 0 background prefetching.
- [`apps/weather/weather.lgolf`](file:///home/strick/github.com/strickyak/yak-cyd/apps/weather/weather.lgolf) — **NOAA Weather**: Live NOAA Aviation Weather METAR viewer with local caching.
- [`apps/paint/paint.lgolf`](file:///home/strick/github.com/strickyak/yak-cyd/apps/paint/paint.lgolf) — **Paint**: 16-color 4-bit indexed palette drawing canvas with PNG export to `/sd` or `/flash`.
- [`apps/mandelbrot/mandelbrot.lgolf`](file:///home/strick/github.com/strickyak/yak-cyd/apps/mandelbrot/mandelbrot.lgolf) — **Mandelbrot**: Interactive tap-to-zoom Mandelbrot set explorer.
- [`apps/ising/ising.lgolf`](file:///home/strick/github.com/strickyak/yak-cyd/apps/ising/ising.lgolf) — **Ising Spin**: 2D ferromagnetic Ising model Monte Carlo simulation around $T_c \approx 2.27$.
- [`apps/dendrite/dendrite.lgolf`](file:///home/strick/github.com/strickyak/yak-cyd/apps/dendrite/dendrite.lgolf) — **Dendrite DLA**: Diffusion-Limited Aggregation crystal growth simulation.
- [`apps/bounce/bounce.lgolf`](file:///home/strick/github.com/strickyak/yak-cyd/apps/bounce/bounce.lgolf) — **Bouncing Ball**: Checkered 3D bouncing sphere animation.
- [`apps/rubik/rubik.lgolf`](file:///home/strick/github.com/strickyak/yak-cyd/apps/rubik/rubik.lgolf) — **Rubik's Cube**: Interactive 3D projected Rubik's Cube.
- [`apps/puzzle15/puzzle15.lgolf`](file:///home/strick/github.com/strickyak/yak-cyd/apps/puzzle15/puzzle15.lgolf) — **15-Puzzle**: Classic sliding tile puzzle.
- **Arcade Games**:
  - [`apps/breakout/breakout.lgolf`](file:///home/strick/github.com/strickyak/yak-cyd/apps/breakout/breakout.lgolf) — **Breakout**
  - [`apps/invaders/invaders.lgolf`](file:///home/strick/github.com/strickyak/yak-cyd/apps/invaders/invaders.lgolf) — **Space Invaders**
  - [`apps/tetris/tetris.lgolf`](file:///home/strick/github.com/strickyak/yak-cyd/apps/tetris/tetris.lgolf) — **Tetris**
  - [`apps/galaga/galaga.lgolf`](file:///home/strick/github.com/strickyak/yak-cyd/apps/galaga/galaga.lgolf) — **Galaga**
  - [`apps/defender/defender.lgolf`](file:///home/strick/github.com/strickyak/yak-cyd/apps/defender/defender.lgolf) — **Defender**

---

## 3. 6-Headed Native App Bundle (`build/apps/native.bin`)

Built by [`tools/build_lapp.py`](file:///home/strick/github.com/strickyak/yak-cyd/tools/build_lapp.py) and linked with [`ld/lakatos_app.ld`](file:///home/strick/github.com/strickyak/yak-cyd/ld/lakatos_app.ld) into a single `native.bin` (`~140 KB`, `17.8%` of the `768 KB` `app_slot` partition) with overlaid per-app `.bss` sections in the `32 KB` `app_ram` arena. Tiny ~210-byte `.lapp` launcher stubs call `join_native("native.bin", "<app>")`, so switching between any of the 6 native apps verifies the existing SHA-256 in `app_slot` and launches instantaneously with **zero flash wear**:

1. [`apps/piano/`](file:///home/strick/github.com/strickyak/yak-cyd/apps/piano/piano.cpp) (`piano.lapp`) — **Piano**: Polyphonic touch keyboard with selectable instrument waveforms and envelopes.
2. [`apps/modtrack/`](file:///home/strick/github.com/strickyak/yak-cyd/apps/modtrack/modtrack.cpp) (`modtrack.lapp`) — **MOD Tracker**: 4-channel Amiga ProTracker (`.mod` / `.mod.zip`) player and live pattern view.
3. [`apps/midiplay/`](file:///home/strick/github.com/strickyak/yak-cyd/apps/midiplay/midiplay.cpp) (`midiplay.lapp`) — **MIDI Player**: Standard MIDI file (`.mid` / `.mid.zip`) synthesizer and 16-channel note waterfall visualizer.
4. [`apps/radio/`](file:///home/strick/github.com/strickyak/yak-cyd/apps/radio/radio.cpp) (`radio.lapp`) — **Radio**: Internet MP3 streaming radio player (`minimp3` decoder) with real-time spectrum and waveform scope.
5. [`apps/ham/`](file:///home/strick/github.com/strickyak/yak-cyd/apps/ham/ham.cpp) (`ham.lapp`) — **Ham SDR**: KiwiSDR HF receiver (`sybil.yak.net:8073`) over WebSocket with 4:1 IMA-ADPCM audio decompression, **75m / 40m / 20m** band buttons (`LSB` / `USB` / `AM`), `1 kHz` frequency slider + `±1 kHz` step buttons, and live dBm S-Meter.
6. [`apps/smallertalk/`](file:///home/strick/github.com/strickyak/yak-cyd/apps/smallertalk/app_main.cpp) (`smallertalk.lapp`) — **SmallerTalk-80**: Smalltalk-80 bytecode VM, compiler, `ST80` image snapshot loader/saver, and windowed Class Browser + Workspace + Transcript GUI.

---

## 4. WebSerial Browser Installer (`web-installer/`)

Run `make web-installer` to assemble a self-contained static site in `build/web-installer/` (using [`web-installer/index.html`](file:///home/strick/github.com/strickyak/yak-cyd/web-installer/index.html)) that flashes the bootloader, partitions, Lakatos OS firmware, and pre-populated LittleFS image (`/flash/apps/*.lapp` and `/flash/apps/native.bin`) directly from Chrome/Edge over WebSerial.

---

## 5. Standalone Demo Firmwares (`demo1`, `demo2`)

### `demo1` (`src/demo1/`) — Multi-App Launcher Suite
Build & flash with `make demo1`:
- [`src/demo1/cyd_shell.h`](file:///home/strick/github.com/strickyak/yak-cyd/src/demo1/cyd_shell.h) & [`src/demo1/launcher.cpp`](file:///home/strick/github.com/strickyak/yak-cyd/src/demo1/launcher.cpp) — Shared hardware API, main launcher menu, and 3-tap NVS touch calibration.
- [`src/demo1/demo_whack.cpp`](file:///home/strick/github.com/strickyak/yak-cyd/src/demo1/demo_whack.cpp) — **Whack-a-Mole**: 30s arcade target tap & reflex test.
- [`src/demo1/demo_life.cpp`](file:///home/strick/github.com/strickyak/yak-cyd/src/demo1/demo_life.cpp) — **Conway's Life**: White-on-black cellular automaton (`80x54` toroidal grid) with stylus line drawing.
- [`src/demo1/demo_rainbow.cpp`](file:///home/strick/github.com/strickyak/yak-cyd/src/demo1/demo_rainbow.cpp) — **65K Rainbow**: Interactive `RGB565` HSV spectrum picker, plasma waves, and rainbow brush.
- [`src/demo1/demo_taz.cpp`](file:///home/strick/github.com/strickyak/yak-cyd/src/demo1/demo_taz.cpp) — **TAZ.TXT Reader**: Fetches random paragraphs from `http://yak.net/taz.txt` via HTTP `Range` requests (`HTTP 206 Partial Content`).
- [`src/demo1/demo_thumb.cpp`](file:///home/strick/github.com/strickyak/yak-cyd/src/demo1/demo_thumb.cpp) — **YAK THUMBS**: Fetches random JPEG thumbnails from `http://yak.net/strick/thumb/` and decodes them via ESP32 ROM `tjpgd` (`2X ZOOM`, `1X`, `4-UP`).
- [`src/demo1/demo_weather.cpp`](file:///home/strick/github.com/strickyak/yak-cyd/src/demo1/demo_weather.cpp) — **NOAA WEATHER**: Live NOAA METAR & NWS station observations for **NYC, LAX, ATL, ORD, SFO, Denver, and Seattle**.
- [`src/demo1/demo_settings.cpp`](file:///home/strick/github.com/strickyak/yak-cyd/src/demo1/demo_settings.cpp) & [`src/demo1/demo_wifi.cpp`](file:///home/strick/github.com/strickyak/yak-cyd/src/demo1/demo_wifi.cpp) — Brightness, Home Airport & Timezone, Wi-Fi scanner, and touch calibration.

### `demo2` (`src/demo2/`) — Second Standalone Firmware
Build & flash with `make demo2`.
