# CYD-One Multi-Demo Launcher (`cyd-one`)

Multi-demo & game suite for the **ESP32-2432S028R** ("Cheap Yellow Display" / **ELEGOO 2.8" ESP32 Display**).

## Project Structure

- [`src/cyd_shell.h`](file:///home/strick/github.com/strickyak/cyd-one/src/cyd_shell.h) — Shared hardware API (`tft`, `TouchSample`, RGB LED helper, `DemoApp` struct, `returnToMenu()`, `startCalibration()`).
- [`src/launcher.cpp`](file:///home/strick/github.com/strickyak/cyd-one/src/launcher.cpp) — Main launcher menu, 3-tap smart touchscreen calibration with NVS (`Preferences`) persistence, and event dispatcher.
- [`src/demo_whack.cpp`](file:///home/strick/github.com/strickyak/cyd-one/src/demo_whack.cpp) — **Whack-a-Mole**: 30s arcade target tap & reflex test with reaction-time and pixel-precision telemetry.
- [`src/demo_life.cpp`](file:///home/strick/github.com/strickyak/cyd-one/src/demo_life.cpp) — **Conway's Life**: White-on-black cellular automaton (`80x54` toroidal grid) where tapping or dragging lines with the stylus births live white cells that evolve immediately on release.
- [`src/demo_rainbow.cpp`](file:///home/strick/github.com/strickyak/cyd-one/src/demo_rainbow.cpp) — **65K Rainbow**: Interactive `RGB565` showcase with 2D HSV spectrum color picker, stylus-tracking rainbow plasma waves, and a rainbow ribbon finger-paint mode.

## Build & Flash

```bash
.venv/bin/pio run -e cyd -t upload
```
