# Lakatos OS Application Binary Interface (`ABI v1`)

**Lakatos** (named for Imre Lakatos, with the `-os` ending) is the resident operating system and fixed-address native application loader for the **ESP32-2432S028R (CYD)** board.

Lakatos owns hardware configuration (touch calibration, Wi-Fi auto-connect, LDR auto-brightness, home airport + timezone + NTP, device name & serial number, Superhighway URL & password) in NVS (`cyd_cfg`, `cyd_cal`, `cyd_wifi`), provides three unified filesystem trees (`/flash/...`, `/sd/...`, `/net/...`), and loads standalone `.lapp` binaries into a fixed virtual memory map.

---

## 1. Flash Partition Layout (`partitions_lakatos.csv` — 4 MB Flash)

| Name | Type / Subtype | Flash Offset | Size | Purpose |
| :--- | :--- | :--- | :--- | :--- |
| `nvs` | `data / nvs` | `0x009000` | `20 KB` (`0x005000`) | Persistent system config (`cyd_cal`, `cyd_wifi`, `cyd_cfg`) |
| `otadata` | `data / ota` | `0x00E000` | `8 KB` (`0x002000`) | Boot metadata |
| `app0` | `app / ota_0` | `0x010000` | `1,472 KB` (`0x170000`) | **Lakatos Resident OS** firmware |
| `app_slot` | `data / 0x40` | `0x180000` | `768 KB` (`0x0C0000`) | **App Staging & MMU Execution Slot** (`12 × 64 KB` MMU pages) |
| `spiffs` | `data / spiffs` | `0x240000` | `1,728 KB` (`0x1B0000`) | **Local Flash Filesystem (`/flash/...`)** via `LittleFS` |

---

## 2. Fixed Virtual Memory Map for `.lapp` Applications

When Lakatos launches a `.lapp` binary from `/flash/...`, `/sd/...`, or `/net/...`, it stages the image into `app_slot` (`0x00180000`) and programs the ESP32 Hardware Flash MMU on both CPU cores (`PRO_CPU` and `APP_CPU`) to map those physical 64 KB flash pages simultaneously into `IROM` and `DROM`:

| Region | Fixed Virtual Address Range | Max Size | Backing Storage | Description |
| :--- | :--- | :--- | :--- | :--- |
| **App Header** | `0x3F700000 .. 0x3F70007F` | `128 B` | `app_slot` (`0x180000`) | `LakatosAppHeader` (`"LAKA"` magic, entry point, section bounds, SHA-256) |
| **App `IROM` (`.text`)** | `0x40300080 .. 0x403BFFFF` | `767.8 KB` | `app_slot` via ICache MMU | Executable Xtensa LX6 instructions |
| **App `DROM` (`.rodata`)** | `0x3F700080 .. 0x3F7BFFFF` | `767.8 KB` | `app_slot` via DCache MMU | Read-only constants, strings, and initial `.data` load image |
| **OS API Pointer Slot** | `0x3FFC8000 .. 0x3FFC8003` | `4 B` | Internal SRAM | Holds `const LakatosApi*` (`lakatos_os()`) |
| **App Static `DRAM`** | `0x3FFC8008 .. 0x3FFCFFFF` | `32,760 B` (`~32 KB`) | Internal SRAM (`gLakatosAppRamArena`) | App `.data` and `.bss` at fixed addresses |
| **App Dynamic Heap** | `os->malloc(size)` | `~140 KB` | Shared ESP32 Heap | Tracked by Lakatos and automatically reclaimed on `os->exit(code)` |

---

## 3. `.lapp` Binary Header (`128` Bytes at File Offset `0`)

Defined in [`include/lakatos_api.h`](../include/lakatos_api.h):

```c
typedef struct LakatosAppHeader {
  uint32_t magic;            // 0x414B414C ("LAKA")
  uint32_t header_version;   // 1
  uint32_t abi_version;      // LAKATOS_API_VERSION (1)
  uint32_t entry_addr;       // Virtual address of app_main in IROM (0x40300080+)
  uint32_t text_offset;      // File offset of .text (0x80)
  uint32_t text_size;        // Size of .text in bytes
  uint32_t rodata_offset;    // File offset of .rodata (mapped at 0x3F700000 + rodata_offset)
  uint32_t rodata_size;      // Size of .rodata in bytes
  uint32_t data_offset;      // File offset of initial .data image
  uint32_t data_dst_addr;    // Target RAM address for .data (0x3FFC8008+)
  uint32_t data_size;        // Size of .data in bytes
  uint32_t bss_dst_addr;     // Target RAM address for .bss
  uint32_t bss_size;         // Size of .bss in bytes (zeroed before entry)
  uint32_t total_file_size;  // Total size of the .lapp file in bytes
  uint8_t  sha256[32];       // SHA-256 of bytes [128 .. total_file_size)
  char     name[32];         // Null-terminated application title
  uint32_t flags;            // Reserved flags (0)
  uint32_t reserved;         // Reserved padding to 128 bytes (0)
} LakatosAppHeader;
```

---

## 4. Three Unified Filesystem Trees (`fs_*` APIs)

All Lakatos filesystem calls (`fs_ls`, `fs_stat`, `fs_read`, `fs_write`, `fs_write_at`, `fs_mkdir`, `fs_rmdir`, `fs_rm`, `fs_du`) accept paths in three namespaces:

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

## 5. Building Standalone `.lapp` Applications

Add your application under `apps/<name>/<name>.cpp` implementing:

```cpp
#include "lakatos_api.h"

extern "C" void app_main(const LakatosApi* os) {
  // Query configured airport:
  const char* airport = os->get_airport();
  // Draw to TFT, read touch, access /flash, /sd, /net...
  // Return to Lakatos OS:
  os->exit(0);
}
```

Compile all `.lapp` binaries with:

```bash
make apps
```

Output `.lapp` binaries are written to `build/apps/<name>.lapp` and can be placed on `/flash/...`, `/sd/...`, or uploaded to `/net/...` via `PUT`.
