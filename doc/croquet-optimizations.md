# Croquet 3D Rendering Profiling & Optimizations (`ESP32-2432S028R`)

This document records the performance investigation, instruction-level LCode VM profiling, on-device ESP32 `micros()` hardware measurements, and the rendering optimizations implemented for the multi-user **Croquet 3D World** app (`apps/croquet/croquet.lgolf`) and the Lakatos OS `pal4` 4-bit software rasterizer (`src/lakatos/lakatos_png.cpp`, `pkg/lakat/vm.go`).

---

## 1. The Problem: Why Frame Rate Dropped from 17 FPS (Detail 1) to 3 FPS (Detail 4)

The `croquet` app includes a 4-position detail slider at the top of the screen:

- **Detail 1 (`11` quads + `2` avatars = `13` items)**: Courtyard walkway path, house interior stone floor, and sky/grass horizon.
- **Detail 2 (`40` quads + `2` avatars = `42` items)**: Adds the 3D stucco house with open front/back doorways, timber frames, pitched roof slopes, and croquet wickets.
- **Detail 3 (`49` quads + `2` avatars = `51` items)**: Adds 1 North brown mountain peak, 1 West 3D pine tree, and 1 South railroad gravel bedrock.
- **Detail 4 (`117` quads + `2` avatars = `119` items)**: Adds the remaining 3 North mountains, 4 West forest pine trees, the East azure lake + shore + water shimmer, and 30 South railroad cross-ties and steel rails.

Before optimization, increasing the slider from **Detail 1** to **Detail 4** caused the frame rate on the physical CYD (`ESP32-2432S028R @ 240 MHz`) to drop from **17 FPS (`~58 ms/frame`)** down to **3–4 FPS (`~220–268 ms/frame`)**.

---

## 2. Baseline Profiling Breakdown (VM Bytecode + ESP32 Hardware `micros()`)

To separate LCode bytecode interpreter overhead from C++ pixel rasterization and SPI display transfer, we instrumented:

1. **The Go LCode VM (`pkg/lakat/vm.go`)** via `VM.StepHook` to count exact bytecode instructions per function, per phase of `renderScene3D()`, and per-call rasterizer scanline/pixel work.
2. **The Physical CYD (`/dev/ttyUSB0`)** using `micros()` around each stage of `renderScene3D()`.

### Baseline Per-Frame Measurements (Before Optimizations)

| Metric / Stage | Detail 1 (Floor + Horizon) | Detail 2 (+ 3D House) | Detail 3 (+1 Tree/Mtn/Track) | Detail 4 (Full World) | D1 $\rightarrow$ D4 Delta |
| :--- | :---: | :---: | :---: | :---: | :---: |
| **Active 3D Quads + Avatars** | `11 + 2 = 13` | `40 + 2 = 42` | `49 + 2 = 51` | `117 + 2 = 119` | **`9.15×` items** |
| **Total VM Bytecode Steps / Frame** | `13,106` | `43,749` | `55,678` | `157,240 – 173,658` | **`12.0× – 13.2×` steps** |
| **1. `clearUs`** (`pal4_clear` + horizon) | `0.38 ms` | `0.37 ms` | `0.38 ms` | `0.38 ms` | `+0.00 ms` (`0%`) |
| **2. `collectUs`** (LCode centroids + depth) | `3.65 ms` *(3,744 steps)* | `7.77 ms` *(7,688 steps)* | `9.08 ms` *(8,912 steps)* | **`19.31 ms`** *(19,207 steps)* | **`+15.66 ms` (`8.5%`)** |
| **3. `sortUs`** (LCode $O(N^2)$ insertion sort) | `1.83 ms` *(2,131 steps)* | `13.78 ms` *(16,508 steps)* | `19.67 ms` *(23,551 steps)* | **`74.85 – 118.87 ms`** *(84k–132k steps)* | **`+73–117 ms` (`49%–60%`)** |
| **4. `projVmUs`** (LCode `renderQuad` + `projectPoint`) | `6.62 ms` *(6,366 steps)* | `19.74 ms` *(18,688 steps)* | `23.84 ms` *(22,350 steps)* | **`54.76 ms`** *(52,638 steps)* | **`+48.14 ms` (`26.1%`)** |
| **5. `cppRasterUs`** (C++ `pal4` triangle/line/circle) | `1.80 ms` *(26 tris)* | `5.90 ms` *(84 tris)* | `7.07 ms` *(102 tris)* | **`13.82 ms`** *(238 tris)* | **`+12.02 ms` (`6.5%`)** |
| **6. `blitUs`** (`tft_draw_pal4` 320×216 SPI push) | **`39.81 ms`** | `39.80 ms` | `39.90 ms` | `39.89 ms` | `+0.08 ms` (`0%`) |
| **Total `renderScene3D()` Time** | **`54.09 ms`** | **`87.36 ms`** | **`99.94 ms`** | **`203.01 – 238.26 ms`** | **`+149 – 184 ms`** |
| **Measured App FPS (with loop + WS)** | **`17 FPS`** | **`10–11 FPS`** | **`9–10 FPS`** | **`3–4 FPS`** | — |

---

## 3. Five Key Insights Taught by Profiling

### Insight 1: C++ Pixel Filling Was *Not* the Bottleneck (`< 7%` of the frame)
A common assumption in 3D software rendering is that triangle scanline filling dominates frame time. On the CYD, because `pal4HLine` writes 2 pixels per byte in a 34.5 KB 4-bit buffer (`320 × 216 / 2`), C++ rasterization (`cppRasterUs`) took only **`1.80 ms` at Detail 1** and **`13.82 ms` at Detail 4** — accounting for only **`+12.0 ms` (`6.5%`)** of the `+184 ms` slowdown.

### Insight 2: 92% of the Slowdown (`+137 ms` to `+182 ms`) Was LCode Bytecode Interpretation
Comparing hardware `micros()` against VM step counts reveals that the ESP32 executes LCode bytecode at **`~0.90 µs/step` (`~1.11 MIPS`)**:
- **Detail 1**: `13,106` VM steps $\times$ `0.92 µs/step` $\approx$ **`12.1 ms`** of bytecode execution.
- **Detail 4**: `157,240 – 173,658` VM steps $\times$ `0.95 µs/step` $\approx$ **`148.9 – 192.9 ms`** of bytecode execution.

### Insight 3: The $O(N^2)$ Insertion Sort in Bytecode Cost `75–119 ms/frame` at Detail 4
At Detail 4 (`N = 119`), the painter's algorithm insertion sort in `renderScene3D()` was responsible for **49% to 60% of the entire slowdown**:
- Each inner-loop iteration (`for b >= 0 && itemDepth[b] < kDepth`) and array swap (`itemKind[b+1] = itemKind[b]; itemDepth[b+1] = itemDepth[b]`) compiled to **36 LCode bytecode instructions**.
- Because `initWorldGeometry()` originally added Detail 1..4 quads from near-to-far (`Y = -204` walkway up to `Y = +320` mountains), the input array was fed into the descending-depth sort in **nearly reverse-sorted order**, triggering `~2,500–3,200` swaps (`84,026` to `132,000` bytecode steps) on every single frame.

### Insight 4: Zero Quads Were Frustum-Culled Before Sorting or Vertex Projection
Because the third-person camera (`camY = myY - 82.0`) sits 82 units behind the avatar, at the starting position (`myY = -175`, `camY = -257`) all 117 quads in the world had camera depth `d > 5.0` relative to `camY`:
- Even the **31 South railroad quads** (`Y = -226..-204`) that project below the bottom edge of the 320×216 screen (`sy > 216`) and trees/lake quads outside the horizontal FOV were collected, sorted, and had all 4 vertices projected in bytecode (`468` calls to `projectPoint` = `52,638` steps).
- Furthermore, **30 of the 117 quads** (8 mountain slopes + 20 tree canopy triangles + 2 house gable triangles) are 3-vertex triangles stored with `v2 == v3`, wasting a 4th `projectPoint` call and a zero-area 2nd `pal4_fill_triangle` call every frame.
- In C++, `lakatosPal4FillTriangle` looped over all `y` from `y0` to `y2` even when `y < 0` or `y >= 216` (`52.9%` to `81.7%` of all scanline iterations at Detail 4 were off-screen), and `lakatosPal4DrawLine` stepped pixel-by-pixel along unclipped off-screen lines (up to `22,949` off-screen Bresenham iterations when facing East).

### Insight 5: At Detail 1, the 17 FPS Ceiling Was Set by `tft_draw_pal4` (`39.81 ms` = 73.6% of Frame)
At Detail 1, `renderScene3D()` spent **`39.81 ms` out of `54.09 ms` (`73.6%`)** inside `lakatosTftDrawPal4`:
- It called `tft.pushImage(x, sy, w, 1, line565)` **216 times per frame** (one SPI transaction and address window setup per scanline).
- It unpacked one 4-bit pixel at a time and relied on `tft.setSwapBytes(true)` to byte-swap every 16-bit pixel during the SPI push.

---

## 4. Optimizations Implemented

### A. LGolf Scene & Bytecode Optimizations (`apps/croquet/croquet.lgolf`)

1. **Precomputed Static Quad Metadata (`addQuad`)**:
   - Precomputes static quad centroids (`qCX, qCY, qCZ`), XY bounding half-extents (`qHalfX, qHalfY`), ground painter sort bias (`qBias`), minimum `ry` visibility distance (`qMinRY`), and degenerate-triangle flag (`qIsTri`) once in `addQuad()` at startup instead of recomputing 4-vertex averages every frame.
2. **Camera-Space OBB Frustum & Ground Bottom-of-Screen Culling (`renderScene3D`)**:
   - Computes `camAbsSin = abs(camSin)` and `camAbsCos = abs(camCos)` once per frame in `updateCamera()`.
   - Before adding a quad to `itemKind`, computes its maximum camera-space forward reach `ryMax = ry + halfX*camAbsSin + halfY*camAbsCos`:
     - Rejects ground quads (`Z <= 1.0`) when `ryMax < 62.0` (which are guaranteed to project to `sy > 216` below the bottom of the screen) and non-ground quads when `ryMax < 8.0`.
     - Rejects quads outside the horizontal field of view when `|rx| > ryMax + halfX*camAbsCos + halfY*camAbsSin`.
   - At Detail 4, this culls `119` items down to `45–86` visible items before sorting and projection.
3. **3-Pass Shell Sort (`gaps = 13, 4, 1`) + Far-to-Near Geometry Ordering**:
   - Ordered `initWorldGeometry()` from North/far to South/near so the default view is nearly pre-sorted.
   - Replaced the $O(N^2)$ insertion sort with a 3-pass Shell sort (`gaps = 13, 4, 1`), reducing Detail 4 sorting from **`74.9–118.9 ms` (`84k–132k` VM steps)** down to **`9.6–18.6 ms` (`10.8k–21.3k` VM steps)** — up to **9× faster**.
4. **3-Vertex Triangle Fast Path & 2D Screen BBox Rejection (`renderQuad`)**:
   - When `qIsTri[qi] != 0`, skips the 4th `projectPoint` call and skips the degenerate 2nd `pal4_fill_triangle` call.
   - Rejects any triangle or quad whose projected 2D vertices lie completely left, right, above, or below the `320 × 216` viewport before invoking native rasterizer syscalls.

### B. Native C++ Rasterizer & SPI Blitter Optimizations (`src/lakatos/lakatos_png.cpp`, `pkg/lakat/vm.go`)

5. **2D BBox Rejection, Scanline Clamping, and Cohen-Sutherland Line Clipping**:
   - `lakatosPal4FillTriangle` & `lakatosPal4FillCircle`: Added 2D bounding-box rejection and clamped scanline `y` loops directly to `[0, h - 1]` so off-screen scanlines are skipped in $O(1)$ without looping or dividing.
   - `lakatosPal4DrawLine`: Added fast horizontal line dispatch (`y0 == y1` $\rightarrow$ `pal4HLine`) and Cohen-Sutherland 2D viewport clipping to `[0, w - 1] × [0, h - 1]`, eliminating thousands of off-screen Bresenham iterations when large walls or lake/mountain edges extend past the screen.
6. **Single-Transaction 2-Pixel LUT Blitter (`lakatosTftDrawPal4`)**:
   - Builds a 256-entry `uint32_t pairLut[256]` at the start of `lakatosTftDrawPal4` that maps every packed byte of `pal4` data (two 4-bit pixels) directly into a pre-byte-swapped 32-bit RGB565 pixel pair.
   - Streams all 216 scanlines inside a single `tft.startWrite()` + `tft.setAddrWindow(x, y + r0, w, r1 - r0)` SPI transaction using `tft.pushPixels(line32, w)` with `swapBytes = false`, cutting `blitUs` from **`39.81 ms` to `31.73–32.21 ms`**.

---

## 5. Before vs. After Results on Physical CYD (`/dev/ttyUSB0`)

| View / Detail Level | Baseline `renderScene3D` | Optimized `renderScene3D` | Speedup | Baseline Sort (`sortUs`) | Optimized Sort (`sortUs`) | Resulting FPS |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: |
| **Detail 1 (Floor + Horizon)** | `54.09 ms` *(13,106 steps)* | **`46.82 ms`** *(12,305 steps)* | **1.16×** | `1.83 ms` | **`1.24 ms`** | **17 $\rightarrow$ 20–21 FPS** |
| **Detail 2 (+ 3D House)** | `87.36 ms` *(43,749 steps)* | **`73.56 ms`** *(36,780 steps)* | **1.19×** | `13.78 ms` | **`7.82 ms`** | **10 $\rightarrow$ 13 FPS** |
| **Detail 3 (+1 Tree/Mtn/Track)** | `99.94 ms` *(55,678 steps)* | **`80.30 ms`** *(42,883 steps)* | **1.24×** | `19.67 ms` | **`9.50 ms`** | **9 $\rightarrow$ 12 FPS** |
| **Detail 4 — Facing North (86 vis)** | `203.01 ms` *(157,240 steps)* | **`113.71 ms`** *(73,411 steps)* | **1.79×** | `74.85 ms` | **`18.62 ms`** | **4 $\rightarrow$ 8–9 FPS** |
| **Detail 4 — Walking Courtyard** | `238.26 ms` *(173,658 steps)* | **`89.76 – 99.01 ms`** | **2.41× – 2.65×** | `118.87 ms` | **`12.74 – 15.91 ms`** | **3 $\rightarrow$ 10–11 FPS** |
| **Detail 4 — Facing West (Forest)** | `225.73 ms` *(173,658 steps)* | **`97.03 ms`** | **2.33×** | `103.30 ms` | **`14.75 ms`** | **3–4 $\rightarrow$ 10 FPS** |
| **Detail 4 — Facing East (Lake)** | `204.50 ms` *(158,100 steps)* | **`79.73 ms`** | **2.56×** | `75.10 ms` | **`9.58 ms`** | **4 $\rightarrow$ 12 FPS** |

### Detailed Phase Comparison at Detail 4 (Facing North vs. Walking)

| `renderScene3D()` Phase | Baseline (North) | Optimized (North) | Baseline (Walking) | Optimized (Walking) |
| :--- | :---: | :---: | :---: | :---: |
| **`clearUs`** (`pal4_clear` + horizon) | `0.38 ms` | `0.47 ms` | `0.38 ms` | `0.47 ms` |
| **`collectUs`** (Centroids + OBB frustum cull) | `19.31 ms` | **`14.93 ms`** | `19.35 ms` | **`13.16 – 14.42 ms`** |
| **`sortUs`** (Insertion sort $\rightarrow$ Shell sort) | `74.85 ms` | **`18.62 ms`** | `118.87 ms` | **`12.74 – 15.91 ms`** |
| **`rasterUs`** (`projVmUs` + `cppRasterUs`) | `68.58 ms` | **`47.48 ms`** | `59.77 ms` | **`30.83 – 35.99 ms`** |
| **`blitUs`** (`tft_draw_pal4` SPI transfer) | `39.89 ms` | **`32.21 ms`** | `39.89 ms` | **`32.13 ms`** |
| **Total `renderScene3D()`** | **`203.01 ms`** | **`113.71 ms`** | **`238.26 ms`** | **`89.76 – 99.01 ms`** |
