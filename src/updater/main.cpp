#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>
#include <SD.h>
#include <SPI.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <mbedtls/sha256.h>

#include "lakatos_api.h"

static constexpr int kSdCsPin   = 5;
static constexpr int kSdSckPin  = 18;
static constexpr int kSdMisoPin = 19;
static constexpr int kSdMosiPin = 23;

static constexpr int kTftMisoPin = 12;
static constexpr int kTftMosiPin = 13;
static constexpr int kTftSclkPin = 14;
static constexpr int kTftCsPin   = 15;
static constexpr int kTftDcPin   = 2;
static constexpr int kTftBlPin   = 21;

static SPIClass gHspi(HSPI);
static uint8_t  gSectorBuf[4096];

// ---------------------------------------------------------------------------
// Minimal Direct-HSPI ILI9341 Driver (320x240 Landscape) for OTA Progress Bar
// ---------------------------------------------------------------------------
static void tftCmd(uint8_t cmd) {
  digitalWrite(kTftDcPin, LOW);
  digitalWrite(kTftCsPin, LOW);
  gHspi.transfer(cmd);
  digitalWrite(kTftCsPin, HIGH);
}

static void tftData8(uint8_t d) {
  digitalWrite(kTftDcPin, HIGH);
  digitalWrite(kTftCsPin, LOW);
  gHspi.transfer(d);
  digitalWrite(kTftCsPin, HIGH);
}

static void tftFillRect(int x, int y, int w, int h, uint16_t color) {
  if (w <= 0 || h <= 0) return;
  uint16_t x1 = (uint16_t)(x + w - 1);
  uint16_t y1 = (uint16_t)(y + h - 1);

  gHspi.beginTransaction(SPISettings(40000000, MSBFIRST, SPI_MODE0));
  digitalWrite(kTftCsPin, LOW);

  digitalWrite(kTftDcPin, LOW);
  gHspi.transfer(0x2A);
  digitalWrite(kTftDcPin, HIGH);
  gHspi.transfer16((uint16_t)x);
  gHspi.transfer16(x1);

  digitalWrite(kTftDcPin, LOW);
  gHspi.transfer(0x2B);
  digitalWrite(kTftDcPin, HIGH);
  gHspi.transfer16((uint16_t)y);
  gHspi.transfer16(y1);

  digitalWrite(kTftDcPin, LOW);
  gHspi.transfer(0x2C);
  digitalWrite(kTftDcPin, HIGH);

  uint32_t total = (uint32_t)w * (uint32_t)h;
  while (total--) {
    gHspi.transfer16(color);
  }
  digitalWrite(kTftCsPin, HIGH);
  gHspi.endTransaction();
}

static void tftDrawRect(int x, int y, int w, int h, uint16_t color) {
  tftFillRect(x, y, w, 1, color);
  tftFillRect(x, y + h - 1, w, 1, color);
  tftFillRect(x, y, 1, h, color);
  tftFillRect(x + w - 1, y, 1, h, color);
}

static void tftInitSimple() {
  pinMode(kTftCsPin, OUTPUT);
  pinMode(kTftDcPin, OUTPUT);
  digitalWrite(kTftCsPin, HIGH);
  digitalWrite(kTftDcPin, HIGH);
  gHspi.begin(kTftSclkPin, kTftMisoPin, kTftMosiPin, kTftCsPin);

  gHspi.beginTransaction(SPISettings(40000000, MSBFIRST, SPI_MODE0));
  tftCmd(0x01); // SWRESET
  gHspi.endTransaction();
  delay(120);

  gHspi.beginTransaction(SPISettings(40000000, MSBFIRST, SPI_MODE0));
  tftCmd(0x11); // SLPOUT
  gHspi.endTransaction();
  delay(120);

  gHspi.beginTransaction(SPISettings(40000000, MSBFIRST, SPI_MODE0));
  tftCmd(0x3A); tftData8(0x55); // 16-bit RGB565
  tftCmd(0x36); tftData8(0x28); // Landscape (rotation 1, BGR)
  tftCmd(0x29);                 // DISPON
  gHspi.endTransaction();
  delay(20);
}

static void drawUpdaterProgress(int pct, uint16_t barColor) {
  int barX = 30, barY = 110, barW = 260, barH = 24;
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  int fillW = (barW * pct) / 100;
  if (fillW > 0) {
    tftFillRect(barX + 2, barY + 2, fillW > 4 ? fillW - 4 : 1, barH - 4, barColor);
  }
  if (fillW < barW - 4) {
    tftFillRect(barX + 2 + fillW, barY + 2, (barW - 4) - fillW, barH - 4, 0x0000);
  }
  tftDrawRect(barX, barY, barW, barH, 0xFFFF);
}

static void bootBackToApp0(const esp_partition_t* app0) {
  if (app0) {
    esp_ota_set_boot_partition(app0);
  }
  delay(300);
  ESP.restart();
}

static bool verifyApp0Sha256(const esp_partition_t* app0, uint32_t size, const uint8_t expectedSha[32]) {
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);

  uint32_t offset = 0;
  while (offset < size) {
    uint32_t chunk = size - offset;
    if (chunk > sizeof(gSectorBuf)) chunk = sizeof(gSectorBuf);
    if (esp_partition_read(app0, offset, gSectorBuf, chunk) != ESP_OK) {
      mbedtls_sha256_free(&ctx);
      return false;
    }
    mbedtls_sha256_update(&ctx, gSectorBuf, chunk);
    offset += chunk;
  }

  uint8_t digest[32];
  mbedtls_sha256_finish(&ctx, digest);
  mbedtls_sha256_free(&ctx);
  return memcmp(digest, expectedSha, 32) == 0;
}

static bool flashFromSdFile(const esp_partition_t* app0, const char* sdPath) {
  File f = SD.open(sdPath, FILE_READ);
  if (!f) return false;

  LakatosKernelHeader hdr;
  if (f.read((uint8_t*)&hdr, sizeof(hdr)) != sizeof(hdr) ||
      hdr.magic != LAKATOS_KERNEL_MAGIC ||
      hdr.kernel_size < 65536 ||
      hdr.kernel_size > app0->size) {
    f.close();
    return false;
  }

  // 1. Verify staged file SHA-256 before erasing app0
  drawUpdaterProgress(0, 0x07FF);
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);

  uint32_t rem = hdr.kernel_size;
  uint32_t done = 0;
  int lastPct = -1;
  while (rem > 0) {
    uint32_t n = (rem > sizeof(gSectorBuf)) ? sizeof(gSectorBuf) : rem;
    if ((uint32_t)f.read(gSectorBuf, n) != n) {
      mbedtls_sha256_free(&ctx);
      f.close();
      return false;
    }
    mbedtls_sha256_update(&ctx, gSectorBuf, n);
    rem -= n;
    done += n;
    int pct = (int)((uint64_t)done * 100ULL / hdr.kernel_size);
    if (pct - lastPct >= 10 || pct == 100) {
      lastPct = pct;
      drawUpdaterProgress(pct, 0x07FF);
    }
  }
  uint8_t digest[32];
  mbedtls_sha256_finish(&ctx, digest);
  mbedtls_sha256_free(&ctx);
  if (memcmp(digest, hdr.kernel_sha256, 32) != 0) {
    f.close();
    return false;
  }

  // 2. Erase and write app0 sector by sector
  if (!f.seek(sizeof(LakatosKernelHeader), SeekSet)) {
    f.close();
    return false;
  }

  uint32_t offset = 0;
  lastPct = -1;
  while (offset < hdr.kernel_size) {
    uint32_t n = hdr.kernel_size - offset;
    if (n > sizeof(gSectorBuf)) n = sizeof(gSectorBuf);
    memset(gSectorBuf, 0xFF, sizeof(gSectorBuf));
    if ((uint32_t)f.read(gSectorBuf, n) != n) {
      f.close();
      return false;
    }
    if (esp_partition_erase_range(app0, offset, sizeof(gSectorBuf)) != ESP_OK ||
        esp_partition_write(app0, offset, gSectorBuf, sizeof(gSectorBuf)) != ESP_OK) {
      f.close();
      return false;
    }
    offset += n;
    int pct = (int)((uint64_t)offset * 100ULL / hdr.kernel_size);
    if (pct - lastPct >= 2 || pct == 100) {
      lastPct = pct;
      drawUpdaterProgress(pct, 0x07E0);
    }
  }
  f.close();

  // 3. Verify app0 flash contents
  drawUpdaterProgress(100, 0xFFE0);
  if (!verifyApp0Sha256(app0, hdr.kernel_size, hdr.kernel_sha256)) {
    return false;
  }

  if (strcmp(sdPath, "/apps/lakatos.krn.stage") == 0) {
    SD.remove(sdPath);
  }
  drawUpdaterProgress(100, 0x07E0);
  return true;
}

static bool flashFromAppSlotAndLittleFs(const esp_partition_t* app0, const esp_partition_t* appSlot) {
  if (!appSlot) return false;

  LakatosKernelHeader hdr;
  if (esp_partition_read(appSlot, 0, &hdr, sizeof(hdr)) != ESP_OK ||
      hdr.magic != LAKATOS_KERNEL_MAGIC ||
      hdr.kernel_size < 65536 ||
      hdr.kernel_size > app0->size) {
    return false;
  }

  if (!LittleFS.begin(false, "/flash", 5, "spiffs")) {
    return false;
  }

  const uint32_t slotCap = appSlot->size - sizeof(LakatosKernelHeader);
  const uint32_t slotBytes = (hdr.kernel_size <= slotCap) ? hdr.kernel_size : slotCap;
  const uint32_t lfsBytes  = (hdr.kernel_size > slotCap) ? (hdr.kernel_size - slotCap) : 0;

  File lf;
  if (lfsBytes > 0) {
    lf = LittleFS.open("/lakatos.krn.stage", "r");
    if (!lf || lf.size() < lfsBytes) {
      if (lf) lf.close();
      return false;
    }
  }

  // 1. Verify combined SHA-256 before erasing app0
  drawUpdaterProgress(0, 0x07FF);
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);

  uint32_t pos = 0;
  int lastPct = -1;
  while (pos < hdr.kernel_size) {
    uint32_t n = hdr.kernel_size - pos;
    if (n > sizeof(gSectorBuf)) n = sizeof(gSectorBuf);
    if (pos < slotBytes) {
      if (pos + n > slotBytes) n = slotBytes - pos;
      if (esp_partition_read(appSlot, sizeof(LakatosKernelHeader) + pos, gSectorBuf, n) != ESP_OK) {
        mbedtls_sha256_free(&ctx);
        if (lf) lf.close();
        return false;
      }
    } else {
      if ((uint32_t)lf.read(gSectorBuf, n) != n) {
        mbedtls_sha256_free(&ctx);
        lf.close();
        return false;
      }
    }
    mbedtls_sha256_update(&ctx, gSectorBuf, n);
    pos += n;
    int pct = (int)((uint64_t)pos * 100ULL / hdr.kernel_size);
    if (pct - lastPct >= 10 || pct == 100) {
      lastPct = pct;
      drawUpdaterProgress(pct, 0x07FF);
    }
  }
  uint8_t digest[32];
  mbedtls_sha256_finish(&ctx, digest);
  mbedtls_sha256_free(&ctx);
  if (memcmp(digest, hdr.kernel_sha256, 32) != 0) {
    if (lf) lf.close();
    return false;
  }

  // 2. Erase and write app0 sector by sector
  if (lf) lf.seek(0, SeekSet);
  uint32_t offset = 0;
  lastPct = -1;
  while (offset < hdr.kernel_size) {
    uint32_t want = hdr.kernel_size - offset;
    if (want > sizeof(gSectorBuf)) want = sizeof(gSectorBuf);
    memset(gSectorBuf, 0xFF, sizeof(gSectorBuf));

    uint32_t filled = 0;
    while (filled < want) {
      uint32_t curPos = offset + filled;
      uint32_t rem = want - filled;
      if (curPos < slotBytes) {
        uint32_t take = slotBytes - curPos;
        if (take > rem) take = rem;
        if (esp_partition_read(appSlot, sizeof(LakatosKernelHeader) + curPos, gSectorBuf + filled, take) != ESP_OK) {
          if (lf) lf.close();
          return false;
        }
        filled += take;
      } else {
        if ((uint32_t)lf.read(gSectorBuf + filled, rem) != rem) {
          if (lf) lf.close();
          return false;
        }
        filled += rem;
      }
    }

    if (esp_partition_erase_range(app0, offset, sizeof(gSectorBuf)) != ESP_OK ||
        esp_partition_write(app0, offset, gSectorBuf, sizeof(gSectorBuf)) != ESP_OK) {
      if (lf) lf.close();
      return false;
    }
    offset += want;
    int pct = (int)((uint64_t)offset * 100ULL / hdr.kernel_size);
    if (pct - lastPct >= 2 || pct == 100) {
      lastPct = pct;
      drawUpdaterProgress(pct, 0x07E0);
    }
  }
  if (lf) lf.close();

  // 3. Verify app0 flash contents
  drawUpdaterProgress(100, 0xFFE0);
  if (!verifyApp0Sha256(app0, hdr.kernel_size, hdr.kernel_sha256)) {
    return false;
  }

  esp_partition_erase_range(appSlot, 0, 4096);
  LittleFS.remove("/lakatos.krn.stage");
  drawUpdaterProgress(100, 0x07E0);
  return true;
}

void setup() {
  Serial.begin(115200);
  pinMode(kTftBlPin, OUTPUT);
  digitalWrite(kTftBlPin, HIGH);

  tftInitSimple();
  tftFillRect(0, 0, 320, 240, 0x0841);
  tftFillRect(0, 0, 320, 26, 0x000F);
  tftFillRect(0, 26, 320, 1, 0x07FF);
  tftFillRect(16, 86, 288, 72, 0x1082);
  tftDrawRect(16, 86, 288, 72, 0x07FF);

  const esp_partition_t* app0 = esp_partition_find_first(
      ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
  const esp_partition_t* appSlot = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "app_slot");

  if (!app0) {
    drawUpdaterProgress(100, 0xF800);
    delay(3000);
    ESP.restart();
    return;
  }

  SPI.begin(kSdSckPin, kSdMisoPin, kSdMosiPin, kSdCsPin);
  bool sdOk = SD.begin(kSdCsPin, SPI, 16000000);
  if (sdOk) {
    if (SD.exists("/apps/lakatos.krn.stage") && flashFromSdFile(app0, "/apps/lakatos.krn.stage")) {
      bootBackToApp0(app0);
      return;
    }
  }

  if (flashFromAppSlotAndLittleFs(app0, appSlot)) {
    bootBackToApp0(app0);
    return;
  }

  bootBackToApp0(app0);
}

void loop() {
  delay(100);
}
