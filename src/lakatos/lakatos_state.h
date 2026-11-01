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
// Touch & LED Helpers (implemented in lakatos_main.cpp)
// ---------------------------------------------------------------------------
void initTouchPins();
LakatosTouchSample lakatosReadTouch();
void lakatosPollTouchLatch();
void lakatosClearTouchLatch();
LakatosTouchSample lakatosReadTouchLatched();
void lakatosSetRgbLed(bool r, bool g, bool b, uint32_t durationMs = 0);
void lakatosUpdateRgbLed();
int  lakatosReadLdrRaw();
float lakatosReadCpuTempC();

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
void        lakatosSetSuperhighwayPassword(const char* pass);
bool        lakatosFormatLocalTime(char* buf, size_t len, const char* fmt);
int64_t     lakatosGetUnixTime();
bool        lakatosWifiIsConnected();

// Opens a modal stylus keyboard; returns 1 on Enter, 0 on Cancel/Hide, -1 on ^C
int lakatosPromptKeyboard(const char* title, char* buf, size_t maxLen,
                          bool allowCtrlC, bool maskPassword = false);

// Enters the interactive Settings / Calibration UI (returns when user taps BACK)
void runLakatosConfigScreen(bool startInCalibration = false);

// ---------------------------------------------------------------------------
// Unified VFS (/lo/..., /sd/..., /sh/...) (implemented in lakatos_vfs.cpp)
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

struct LakatosVfsStream;
LakatosVfsStream* lakatosFsOpenRead(const char* path);
int32_t           lakatosFsStreamRead(LakatosVfsStream* s, void* buf, size_t len);
bool              lakatosFsStreamSkip(LakatosVfsStream* s, size_t len);
void              lakatosFsClose(LakatosVfsStream* s);

// ---------------------------------------------------------------------------
// Fixed-Address & LCode App Loader (lakatos_loader.cpp & lakatos_lcode_vm.cpp)
// ---------------------------------------------------------------------------
bool initLakatosLoader();
// Stages and executes a .lapp binary from /lo/..., /sd/..., or /sh/...
// Returns to caller when the app calls os->exit(code) or returns from app_main.
bool lakatosRunAppFromFile(const char* vfsPath, int* outExitCode = nullptr,
                           String* outErrMsg = nullptr);

// Executes a RAM-loaded LCode ("LCOD") bytecode application with zero Flash wear.
bool lakatosRunLCodeApp(const LakatosAppHeader& hdr, const uint8_t* payload,
                        size_t payloadLen, int* outExitCode = nullptr,
                        String* outErrMsg = nullptr);

// ---------------------------------------------------------------------------
// PNG Image Decoder & Renderer (implemented in lakatos_png.cpp)
// ---------------------------------------------------------------------------
bool lakatosPngInfo(const char* vfsPath, int* outWidth, int* outHeight);
bool lakatosDrawPngFile(const char* vfsPath, int x, int y);
bool lakatosDrawPngFileCentered(const char* vfsPath, int boxX, int boxY, int boxW, int boxH);
bool lakatosDrawPngMem(const uint8_t* data, size_t len, int x, int y);

// ---------------------------------------------------------------------------
// JPEG Image Decoder & Renderer via ROM tjpgd (implemented in lakatos_jpg.cpp)
// ---------------------------------------------------------------------------
bool lakatosJpgInfo(const char* vfsPath, int* outWidth, int* outHeight);
bool lakatosJpgInfoMem(const uint8_t* data, size_t len, int* outWidth, int* outHeight);
bool lakatosDrawJpgFile(const char* vfsPath, int x, int y, int scale = 0);
bool lakatosDrawJpgFileCentered(const char* vfsPath, int boxX, int boxY, int boxW, int boxH);
bool lakatosDrawJpgMem(const uint8_t* data, size_t len, int x, int y, int scale = 0);


