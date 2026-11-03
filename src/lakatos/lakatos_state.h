#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <TFT_eSPI.h>
#include "lakatos_api.h"

// ---------------------------------------------------------------------------
// Hardware Pins for ESP32-2432S028R (CYD)
// ---------------------------------------------------------------------------
// Touch XPT2046 (uses dedicated fast GPIO SPI so hardware VSPI stays free for SD)
#define XPT2046_IRQ  36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK  25
#define XPT2046_CS   33

// MicroSD Card Slot on Hardware VSPI
#define SD_CS_PIN    5
#define SD_MOSI_PIN  23
#define SD_MISO_PIN  19
#define SD_SCK_PIN   18

// On-board RGB LED (active-low), LDR, and Backlight
#define LED_R_PIN    4
#define LED_G_PIN    16
#define LED_B_PIN    17
#define LDR_PIN      34
#define TFT_BL_PIN   21

extern TFT_eSPI tft;
extern Preferences prefs;

// ---------------------------------------------------------------------------
// Touch, TFT Mutex & LED Helpers (implemented in lakatos_main.cpp)
// ---------------------------------------------------------------------------
void lakatosTftLock();
bool lakatosTftTryLock(uint32_t waitMs = 1);
void lakatosTftUnlock();

struct LakatosTftGuard {
  LakatosTftGuard()  { lakatosTftLock(); }
  ~LakatosTftGuard() { lakatosTftUnlock(); }
};

void initTouchPins();
LakatosTouchSample lakatosReadTouch();
void lakatosPollTouchLatch();
void lakatosClearTouchLatch();
void lakatosWaitTouchRelease();
LakatosTouchSample lakatosReadTouchLatched();
LakatosTouchSample lakatosReadClick();
void lakatosSetRgbLed(bool r, bool g, bool b, uint32_t durationMs = 0);
void lakatosUpdateRgbLed();
int  lakatosReadLdrRaw();
float lakatosReadCpuTempC();

// App lifecycle & Core 0 top-left 10x10 corner exit / timeout mechanism
void lakatosBeginAppSession();
void lakatosEndAppSession();
void lakatosSetInConfigScreen(bool inCfg);
void lakatosSetCornerExitEnabled(bool en);
bool lakatosGetCornerExitEnabled();
bool lakatosIsCornerExitActive();
void lakatosSetAppTimeoutMs(uint32_t timeoutMs);
void lakatosRequestAppExit();
bool lakatosShouldExitApp();

// ---------------------------------------------------------------------------
// Configuration, Calibration & Stylus Keyboard (implemented in lakatos_config.cpp)
// ---------------------------------------------------------------------------
void initLakatosConfig();
void updateLakatosConfigBackground();  // Brightness, Wi-Fi auto-reconnect, NTP

bool loadTouchCalibrationFromNvs();
void mapRawTouchToScreen(int rawX, int rawY, int& sx, int& sy);

const char* lakatosGetAirportCode();
bool        lakatosGetAirportInfo(LakatosAirportInfo* out);
const char* lakatosGetDeviceName();
const char* lakatosGetSerialNumber();
const char* lakatosGetSuperhighwayUrl();
const char* lakatosGetSuperhighwayPassword();
void        lakatosSetDeviceName(const char* name);
void        lakatosSetSerialNumber(const char* sn);
void        lakatosSetSuperhighwayUrl(const char* url);
void        lakatosSetSuperhighwayPassword(const char* pass);
void        lakatosSetAirportCode(const char* code);
void        lakatosSetWifiCredentials(const char* ssid, const char* pass);
bool        lakatosConsumeConfigRedraw();
bool        lakatosGetDeveloperMode();
void        lakatosSetDeveloperMode(bool en);
bool        lakatosGetTouchSpotEnabled();
void        lakatosSetTouchSpotEnabled(bool en);
void        lakatosDevLogf(const char* fmt, ...);
void        lakatosPollSerialFuse();
bool        lakatosFormatLocalTime(char* buf, size_t len, const char* fmt);
int64_t     lakatosGetUnixTime();
bool        lakatosWifiIsConnected();

// Opens a modal stylus keyboard; returns 1 on Enter, 0 on Cancel/Hide, -1 on ^C
int lakatosPromptKeyboard(const char* title, char* buf, size_t maxLen,
                          bool allowCtrlC, bool maskPassword = false);

// Enters the interactive Settings / Calibration UI (returns when user taps BACK)
void runLakatosConfigScreen(bool startInCalibration = false);

// ---------------------------------------------------------------------------
// Unified VFS (/flash/..., /sd/..., /net/...) (implemented in lakatos_vfs.cpp)
// ---------------------------------------------------------------------------
void initLakatosVfs();
bool lakatosSdIsMounted();

int     lakatosFsLs(const char* path, LakatosFsEntry* outEntries, int maxEntries);
bool    lakatosFsStat(const char* path, LakatosFsEntry* outEntry);
int32_t lakatosFsRead(const char* path, uint32_t offset, void* buf, size_t len);
bool    lakatosFsWrite(const char* path, const void* buf, size_t len);
bool    lakatosFsWriteAt(const char* path, uint32_t offset, const void* buf, size_t len);
bool    lakatosFsMkdir(const char* path, bool parents);
bool    lakatosFsRmdir(const char* path);
bool    lakatosFsRm(const char* path);
int64_t lakatosFsDu(const char* path);
int     lakatosHttpGet(const char* url, void* buf, size_t maxLen);
int     lakatosHttpGetCached(const char* url, void* buf, size_t maxLen, uint32_t maxAgeSec);

struct LakatosVfsStream;
LakatosVfsStream* lakatosFsOpenRead(const char* path);
int32_t           lakatosFsStreamRead(LakatosVfsStream* s, void* buf, size_t len);
bool              lakatosFsStreamSkip(LakatosVfsStream* s, size_t len);
void              lakatosFsClose(LakatosVfsStream* s);

// Core 0 Background File Prefetcher (implemented in lakatos_vfs.cpp)
// Status codes: 0 = IDLE, 1 = BUSY, 2 = READY, -1 = ERROR
// If dstBuf == nullptr, allocates/reuses a dedicated heap buffer of maxLen bytes.
bool           lakatosPrefetchReserve(size_t cap);
bool           lakatosPrefetchStart(const char* vfsPath, uint8_t* dstBuf, size_t maxLen);
int            lakatosPrefetchStatus(uint32_t* outBytesGot = nullptr);
const uint8_t* lakatosPrefetchBuffer(uint32_t* outBytesGot = nullptr);
void           lakatosPrefetchCancel();
void           lakatosPrefetchFree();

// ---------------------------------------------------------------------------
// Fixed-Address & LCode App Loader (lakatos_loader.cpp & lakatos_lcode_vm.cpp)
// ---------------------------------------------------------------------------
extern uint8_t  gLakatosAppRamArena[LAKATOS_APP_DRAM_SIZE];
extern uint8_t* gLakatosPngScratch;
extern size_t   gLakatosPngScratchSize;
bool lakatosIsNativeAppRunning();
bool initLakatosLoader();
// Stages and executes a .lapp binary from /flash/..., /sd/..., or /net/...
// Returns to caller when the app calls os->exit(code) or returns from app_main.
bool lakatosRunAppFromFile(const char* vfsPath, int* outExitCode = nullptr,
                           String* outErrMsg = nullptr);

// Executes a RAM-loaded LCode ("LCOD") bytecode application with zero Flash wear.
bool lakatosRunLCodeApp(const LakatosAppHeader& hdr, const uint8_t* payload,
                        size_t payloadLen, int* outExitCode = nullptr,
                        String* outErrMsg = nullptr);
uint32_t lakatosCityHash32(const void* buf, size_t len);

// ---------------------------------------------------------------------------
// PNG Image Decoder & Renderer (implemented in lakatos_png.cpp)
// ---------------------------------------------------------------------------
bool lakatosPngInfo(const char* vfsPath, int* outWidth, int* outHeight);
bool lakatosDrawPngFile(const char* vfsPath, int x, int y);
bool lakatosDrawPngFileCentered(const char* vfsPath, int boxX, int boxY, int boxW, int boxH);
bool lakatosDrawPngMem(const uint8_t* data, size_t len, int x, int y);
void lakatosTftDrawPal4(int x, int y, int w, int h, const uint32_t* pal565, const uint8_t* pix4bpp);
bool lakatosSavePngPal4(const char* vfsPath, int w, int h, const uint32_t* pal565, const uint8_t* pix4bpp);

// ---------------------------------------------------------------------------
// JPEG Image Decoder & Renderer via ROM tjpgd (implemented in lakatos_jpg.cpp)
// ---------------------------------------------------------------------------
bool lakatosJpgInfo(const char* vfsPath, int* outWidth, int* outHeight);
bool lakatosJpgInfoMem(const uint8_t* data, size_t len, int* outWidth, int* outHeight);
bool lakatosDrawJpgFile(const char* vfsPath, int x, int y, int scale = 0);
bool lakatosDrawJpgFileCentered(const char* vfsPath, int boxX, int boxY, int boxW, int boxH);
bool lakatosDrawJpgMem(const uint8_t* data, size_t len, int x, int y, int scale = 0);


