# Yak-CYD Firmware Suite (`yak-cyd`)

Firmware and **Lakatos OS / LCode** toolchain for the **ESP32-2432S028R** ("Cheap Yellow Display" / **ELEGOO 2.8" ESP32 Display**).

## Environments & Directories

### 1. `lakatos` (`src/lakatos/` & `tools/lcode/`) — Lakatos OS & LCode 32-Bit Bytecode Runtime
Build LCode `.lapp` apps, compile Lakatos OS, and flash to CYD with:
```bash
make lakatos
```
- [`src/lakatos/lakatos_main.cpp`](file:///home/strick/github.com/strickyak/yak-cyd/src/lakatos/lakatos_main.cpp) — Lakatos OS shell, file browser across `/lo`, `/sd`, and `/sh`, and bit-banged XPT2046 touchscreen driver.
- [`src/lakatos/lakatos_config.cpp`](file:///home/strick/github.com/strickyak/yak-cyd/src/lakatos/lakatos_config.cpp) — Persistent NVS configuration (`cyd_cfg` & `cyd_cal`): 3-tap touch calibration, Wi-Fi scanner, stylus keyboard, airport/timezone/NTP, device name + serial number, and Superhighway URL/password.
- [`src/lakatos/lakatos_vfs.cpp`](file:///home/strick/github.com/strickyak/yak-cyd/src/lakatos/lakatos_vfs.cpp) — Unified VFS mounting `/lo` (`LittleFS`), `/sd` (`MicroSD`), and `/sh` (Wi-Fi HTTP Superhighway client).
- [`src/lakatos/lakatos_lcode_vm.cpp`](file:///home/strick/github.com/strickyak/yak-cyd/src/lakatos/lakatos_lcode_vm.cpp) — Resident 32-bit **LCode** bytecode virtual machine with coalescing `malloc`/`free` heap and `OpSYSCALL` bridge, executing `.lapp` binaries directly in RAM with zero Flash wear.
- [`tools/lcode/`](file:///home/strick/github.com/strickyak/yak-cyd/tools/lcode/cmd/lcode/main.go) — Go toolchain (`./bin/lcode`) providing the **LGolf** (`.lgolf`) compiler, `.lasm` assembler, disassembler, headless `320×240` `RGB565` emulator, and Superhighway uploader.
- [`apps/life/life.lgolf`](file:///home/strick/github.com/strickyak/yak-cyd/apps/life/life.lgolf) — Conway's Game of Life in LGolf (`2,196` bytes compiled `.lapp`).
- [`doc/lcode-agent-guide.md`](file:///home/strick/github.com/strickyak/yak-cyd/doc/lcode-agent-guide.md) — Guide for AI Agents authoring `.lgolf` games for Lakatos OS.

### 2. `demo1` (`src/demo1/`) — Multi-App Launcher Suite
Build & flash with:
```bash
make demo1
```
- [`src/demo1/cyd_shell.h`](file:///home/strick/github.com/strickyak/yak-cyd/src/demo1/cyd_shell.h) — Shared hardware API (`tft`, `TouchSample`, RGB LED helper, `DemoApp` struct, `returnToMenu()`, `startCalibration()`, `ensureWifiOrPrompt()`).
- [`src/demo1/launcher.cpp`](file:///home/strick/github.com/strickyak/yak-cyd/src/demo1/launcher.cpp) — Main launcher menu, 3-tap touchscreen calibration with NVS (`"cyd_cal"`) persistence, and event dispatcher.
- [`src/demo1/demo_whack.cpp`](file:///home/strick/github.com/strickyak/yak-cyd/src/demo1/demo_whack.cpp) — **Whack-a-Mole**: 30s arcade target tap & reflex test.
- [`src/demo1/demo_life.cpp`](file:///home/strick/github.com/strickyak/yak-cyd/src/demo1/demo_life.cpp) — **Conway's Life**: White-on-black cellular automaton (`80x54` toroidal grid) with stylus line drawing.
- [`src/demo1/demo_rainbow.cpp`](file:///home/strick/github.com/strickyak/yak-cyd/src/demo1/demo_rainbow.cpp) — **65K Rainbow**: Interactive `RGB565` HSV spectrum picker, plasma waves, and rainbow brush.
- [`src/demo1/demo_taz.cpp`](file:///home/strick/github.com/strickyak/yak-cyd/src/demo1/demo_taz.cpp) — **TAZ.TXT Reader**: Fetches random paragraphs from `http://yak.net/taz.txt` via HTTP `Range` requests (`HTTP 206 Partial Content`).
- [`src/demo1/demo_thumb.cpp`](file:///home/strick/github.com/strickyak/yak-cyd/src/demo1/demo_thumb.cpp) — **YAK THUMBS**: Fetches random JPEG thumbnails from `http://yak.net/strick/thumb/` and decodes them via ESP32 ROM `tjpgd` (`2X ZOOM`, `1X`, `4-UP`).
- [`src/demo1/demo_weather.cpp`](file:///home/strick/github.com/strickyak/yak-cyd/src/demo1/demo_weather.cpp) — **NOAA WEATHER**: Live NOAA METAR & NWS station observations for **NYC, LAX, ATL, ORD, SFO, Denver, and Seattle**, with the user's selected Home Airport & Timezone highlighted at the top.
- [`src/demo1/demo_settings.cpp`](file:///home/strick/github.com/strickyak/yak-cyd/src/demo1/demo_settings.cpp) — **SETTINGS**: Sub-menu for screen brightness (interactive slider + `LDR` auto-brightness ON by default), Home Airport & Timezone selection (16 quick-pick hubs + custom 3-letter IATA keypad), Wi-Fi configuration, and 3-tap touchscreen calibration.
- [`src/demo1/demo_wifi.cpp`](file:///home/strick/github.com/strickyak/yak-cyd/src/demo1/demo_wifi.cpp) — **WIFI CONFIG**: 2.4 GHz Wi-Fi scanner + on-screen stylus keyboard stored in ESP32 NVS (`"cyd_wifi"`).

### 3. `demo2` (`src/demo2/`) — Second Standalone Firmware
Build & flash with:
```bash
make demo2
```
