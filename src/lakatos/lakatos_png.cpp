#pragma GCC optimize("O3")
#include "lakatos_state.h"

#include <esp_heap_caps.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include <miniz.h>
}

namespace {

static const uint8_t kPngSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

static inline uint32_t readBE32(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) |
         ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) |
         (uint32_t)p[3];
}

static inline uint16_t readBE16(const uint8_t* p) {
  return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static inline uint16_t rgb888To565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static inline uint8_t paethPredictor(uint8_t a, uint8_t b, uint8_t c) {
  int p = (int)a + (int)b - (int)c;
  int pa = abs(p - (int)a);
  int pb = abs(p - (int)b);
  int pc = abs(p - (int)c);
  if (pa <= pb && pa <= pc) return a;
  return (pb <= pc) ? b : c;
}

struct PngReader {
  LakatosVfsStream* stream  = nullptr;
  const uint8_t*    memData = nullptr;
  size_t            memLen  = 0;
  uint32_t          offset  = 0;
  uint32_t          readUs  = 0;

  ~PngReader() {
    if (stream) {
      lakatosFsClose(stream);
      stream = nullptr;
    }
  }

  bool readExact(void* dst, size_t n) {
    if (n == 0) return true;
    if (memData) {
      if (offset + n > memLen) return false;
      memcpy(dst, memData + offset, n);
      offset += (uint32_t)n;
      return true;
    }
    if (stream) {
      uint32_t t0 = micros();
      int32_t got = lakatosFsStreamRead(stream, dst, n);
      readUs += (micros() - t0);
      if (got != (int32_t)n) return false;
      offset += (uint32_t)got;
      return true;
    }
    return false;
  }

  size_t readUpTo(void* dst, size_t maxN) {
    if (maxN == 0) return 0;
    if (memData) {
      if (offset >= memLen) return 0;
      size_t avail = memLen - offset;
      size_t take = (maxN < avail) ? maxN : avail;
      memcpy(dst, memData + offset, take);
      offset += (uint32_t)take;
      return take;
    }
    if (stream) {
      uint32_t t0 = micros();
      int32_t got = lakatosFsStreamRead(stream, dst, maxN);
      readUs += (micros() - t0);
      if (got <= 0) return 0;
      offset += (uint32_t)got;
      return (size_t)got;
    }
    return 0;
  }

  bool skip(size_t n) {
    if (n == 0) return true;
    if (memData) {
      if (offset + n > memLen) return false;
      offset += (uint32_t)n;
      return true;
    }
    if (stream) {
      if (!lakatosFsStreamSkip(stream, n)) return false;
      offset += (uint32_t)n;
      return true;
    }
    return false;
  }
};

struct PngDecoderState {
  uint32_t width     = 0;
  uint32_t height    = 0;
  uint8_t  bitDepth  = 0;
  uint8_t  colorType = 0;
  uint32_t bpp       = 1;
  uint32_t stride    = 0;
  uint32_t rowBytes  = 0;
  uint32_t curY      = 0;
  uint32_t curRowPos = 0;

  int dstX = 0;
  int dstY = 0;
  int step = 1;
  uint32_t drawUs = 0;
  uint32_t tinflUs = 0;

  uint8_t paletteR[256];
  uint8_t paletteG[256];
  uint8_t paletteB[256];
  uint8_t paletteA[256];

  bool     hasColorKey = false;
  uint16_t keyR = 0, keyG = 0, keyB = 0;

  uint8_t* curRow  = nullptr;
  uint8_t* prevRow = nullptr;
  uint16_t line565[LAKATOS_SCREEN_W];

  void unfilterCurrentRow() {
    uint8_t filterType = curRow[0];
    uint8_t* raw = curRow + 1;
    const uint8_t* prev = prevRow + 1;

    switch (filterType) {
      case 0: // None
        break;
      case 1: // Sub
        for (uint32_t i = bpp; i < stride; i++) {
          raw[i] = (uint8_t)(raw[i] + raw[i - bpp]);
        }
        break;
      case 2: // Up
        for (uint32_t i = 0; i < stride; i++) {
          raw[i] = (uint8_t)(raw[i] + prev[i]);
        }
        break;
      case 3: // Average
        for (uint32_t i = 0; i < stride; i++) {
          uint8_t a = (i >= bpp) ? raw[i - bpp] : 0;
          uint8_t b = prev[i];
          raw[i] = (uint8_t)(raw[i] + (((int)a + (int)b) >> 1));
        }
        break;
      case 4: // Paeth
        for (uint32_t i = 0; i < stride; i++) {
          uint8_t a = (i >= bpp) ? raw[i - bpp] : 0;
          uint8_t b = prev[i];
          uint8_t c = (i >= bpp) ? prev[i - bpp] : 0;
          raw[i] = (uint8_t)(raw[i] + paethPredictor(a, b, c));
        }
        break;
      default:
        break;
    }
  }

  inline void extractPixel(const uint8_t* raw, uint32_t x,
                           uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) const {
    a = 255;
    switch (colorType) {
      case 0: { // Grayscale
        uint8_t v = 0;
        if (bitDepth == 8) {
          v = raw[x];
          if (hasColorKey && v == (uint8_t)keyR) a = 0;
        } else if (bitDepth == 16) {
          uint16_t v16 = readBE16(raw + x * 2);
          if (hasColorKey && v16 == keyR) a = 0;
          v = (uint8_t)(v16 >> 8);
        } else if (bitDepth == 4) {
          uint8_t nib = (raw[x >> 1] >> ((1 - (x & 1)) * 4)) & 0x0F;
          if (hasColorKey && nib == keyR) a = 0;
          v = (uint8_t)(nib * 17);
        } else if (bitDepth == 2) {
          uint8_t cr = (raw[x >> 2] >> ((3 - (x & 3)) * 2)) & 0x03;
          if (hasColorKey && cr == keyR) a = 0;
          v = (uint8_t)(cr * 85);
        } else if (bitDepth == 1) {
          uint8_t bit = (raw[x >> 3] >> (7 - (x & 7))) & 0x01;
          if (hasColorKey && bit == keyR) a = 0;
          v = bit ? 255 : 0;
        }
        r = g = b = v;
        break;
      }
      case 2: { // Truecolor RGB
        if (bitDepth == 16) {
          const uint8_t* p = raw + x * 6;
          uint16_t r16 = readBE16(p);
          uint16_t g16 = readBE16(p + 2);
          uint16_t b16 = readBE16(p + 4);
          if (hasColorKey && r16 == keyR && g16 == keyG && b16 == keyB) a = 0;
          r = p[0];
          g = p[2];
          b = p[4];
        } else {
          const uint8_t* p = raw + x * 3;
          r = p[0];
          g = p[1];
          b = p[2];
          if (hasColorKey && r == (uint8_t)keyR && g == (uint8_t)keyG && b == (uint8_t)keyB) {
            a = 0;
          }
        }
        break;
      }
      case 3: { // Indexed Palette
        uint8_t idx = 0;
        if (bitDepth == 8) {
          idx = raw[x];
        } else if (bitDepth == 4) {
          idx = (raw[x >> 1] >> ((1 - (x & 1)) * 4)) & 0x0F;
        } else if (bitDepth == 2) {
          idx = (raw[x >> 2] >> ((3 - (x & 3)) * 2)) & 0x03;
        } else if (bitDepth == 1) {
          idx = (raw[x >> 3] >> (7 - (x & 7))) & 0x01;
        }
        r = paletteR[idx];
        g = paletteG[idx];
        b = paletteB[idx];
        a = paletteA[idx];
        break;
      }
      case 4: { // Grayscale + Alpha
        if (bitDepth == 16) {
          const uint8_t* p = raw + x * 4;
          r = g = b = p[0];
          a = p[2];
        } else {
          const uint8_t* p = raw + x * 2;
          r = g = b = p[0];
          a = p[1];
        }
        break;
      }
      case 6: { // Truecolor RGBA
        if (bitDepth == 16) {
          const uint8_t* p = raw + x * 8;
          r = p[0];
          g = p[2];
          b = p[4];
          a = p[6];
        } else {
          const uint8_t* p = raw + x * 4;
          r = p[0];
          g = p[1];
          b = p[2];
          a = p[3];
        }
        break;
      }
      default:
        r = g = b = 0;
        break;
    }
  }

  void renderCurrentRow() {
    if (step > 1 && (curY % (uint32_t)step) != 0) return;
    int sy = dstY + (int)(curY / (uint32_t)step);
    if (sy < 0 || sy >= LAKATOS_SCREEN_H) return;

    const uint8_t* raw = curRow + 1;
    int spanStartScreenX = -1;
    int spanLen = 0;

    uint32_t outX = 0;
    for (uint32_t x = 0; x < width; x += (uint32_t)step, outX++) {
      int sx = dstX + (int)outX;
      if (sx < 0) continue;
      if (sx >= LAKATOS_SCREEN_W) break;

      uint8_t r, g, b, a;
      extractPixel(raw, x, r, g, b, a);

      if (a >= 128) {
        if (spanStartScreenX < 0) {
          spanStartScreenX = sx;
          spanLen = 0;
        }
        line565[spanStartScreenX + spanLen] = rgb888To565(r, g, b);
        spanLen++;
      } else if (spanStartScreenX >= 0) {
        LakatosTftGuard tftLock;
        tft.pushImage(spanStartScreenX, sy, spanLen, 1, line565 + spanStartScreenX);
        spanStartScreenX = -1;
        spanLen = 0;
      }
    }

    if (spanStartScreenX >= 0 && spanLen > 0) {
      LakatosTftGuard tftLock;
      tft.pushImage(spanStartScreenX, sy, spanLen, 1, line565 + spanStartScreenX);
    }
  }

  void feedDecompressedBytes(const uint8_t* data, size_t len) {
    size_t pos = 0;
    while (pos < len && curY < height) {
      size_t need = rowBytes - curRowPos;
      size_t avail = len - pos;
      size_t take = (avail < need) ? avail : need;
      memcpy(curRow + curRowPos, data + pos, take);
      curRowPos += (uint32_t)take;
      pos += take;

      if (curRowPos == rowBytes) {
        uint32_t t0 = micros();
        unfilterCurrentRow();
        renderCurrentRow();
        drawUs += (micros() - t0);
        uint8_t* tmp = prevRow;
        prevRow = curRow;
        curRow = tmp;
        curRowPos = 0;
        curY++;
      }
    }
  }
};

static bool decodeAndRenderPng(PngReader& reader, int dstX, int dstY,
                               int boxW = 0, int boxH = 0) {
  uint32_t tStart = millis();
  uint8_t sig[8];
  if (!reader.readExact(sig, 8) || memcmp(sig, kPngSig, 8) != 0) {
    Serial.println("[Lakatos PNG] Invalid PNG signature");
    return false;
  }

  bool dictFromArena = !lakatosIsNativeAppRunning() && (LAKATOS_APP_DRAM_SIZE >= TINFL_LZ_DICT_SIZE);
  bool stFromArena   = dictFromArena &&
                       (LAKATOS_APP_DRAM_SIZE >= TINFL_LZ_DICT_SIZE + sizeof(PngDecoderState));
  PngDecoderState* st = nullptr;
  if (stFromArena) {
    st = (PngDecoderState*)(gLakatosAppRamArena + TINFL_LZ_DICT_SIZE);
    memset(st, 0, sizeof(PngDecoderState));
  } else {
    st = (PngDecoderState*)calloc(1, sizeof(PngDecoderState));
  }
  if (!st) {
    Serial.println("[Lakatos PNG] Out of memory for PngDecoderState");
    return false;
  }
  st->dstX = dstX;
  st->dstY = dstY;
  st->step = 1;
  memset(st->paletteA, 255, sizeof(st->paletteA));

  bool useScratch = (gLakatosPngScratch != nullptr) &&
                    (gLakatosPngScratchSize >= sizeof(tinfl_decompressor));
  tinfl_decompressor* decomp = useScratch
                                   ? (tinfl_decompressor*)gLakatosPngScratch
                                   : (tinfl_decompressor*)malloc(sizeof(tinfl_decompressor));

  uint8_t* dict = dictFromArena ? gLakatosAppRamArena : (uint8_t*)malloc(TINFL_LZ_DICT_SIZE);
  constexpr size_t kInChunkSize = 4096;
  uint8_t* inBuf = (reader.memData != nullptr) ? nullptr : (uint8_t*)malloc(kInChunkSize);

  if (!decomp || !dict || (reader.memData == nullptr && !inBuf)) {
    Serial.printf("[Lakatos PNG] Out of memory: decomp=%p (need %u), dict=%p, maxContig=%u, free8Bit=%u\r\n",
                  decomp, (unsigned)sizeof(tinfl_decompressor), dict,
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    if (inBuf) free(inBuf);
    if (!dictFromArena && dict) free(dict);
    if (!useScratch && decomp) free(decomp);
    if (!stFromArena) free(st);
    return false;
  }

  tinfl_init(decomp);
  size_t dictOfs = 0;
  bool gotIhdr = false;
  bool ok = true;

  // Save and restore TFT byte swap setting so tft.pushImage(uint16_t*) renders native RGB565
  bool prevSwap = tft.getSwapBytes();
  tft.setSwapBytes(true);

  while (ok) {
    uint8_t chHdr[8];
    if (!reader.readExact(chHdr, 8)) break;
    uint32_t chLen = readBE32(chHdr);
    const uint8_t* chType = chHdr + 4;

    if (memcmp(chType, "IHDR", 4) == 0) {
      if (chLen < 13) {
        ok = false;
        break;
      }
      uint8_t ihdr[13];
      if (!reader.readExact(ihdr, 13)) {
        ok = false;
        break;
      }
      if (chLen > 13 && !reader.skip(chLen - 13)) {
        ok = false;
        break;
      }
      st->width     = readBE32(ihdr);
      st->height    = readBE32(ihdr + 4);
      st->bitDepth  = ihdr[8];
      st->colorType = ihdr[9];
      uint8_t comp  = ihdr[10];
      uint8_t filt  = ihdr[11];
      uint8_t inter = ihdr[12];

      if (st->width == 0 || st->height == 0 || st->width > 2048 || st->height > 2048 ||
          comp != 0 || filt != 0 || inter != 0) {
        ok = false;
        break;
      }

      uint32_t channels = 0;
      switch (st->colorType) {
        case 0: channels = 1; break;
        case 2: channels = 3; break;
        case 3: channels = 1; break;
        case 4: channels = 2; break;
        case 6: channels = 4; break;
        default: ok = false; break;
      }
      if (!ok) break;

      uint32_t bitsPerPixel = channels * st->bitDepth;
      st->bpp = (bitsPerPixel + 7) / 8;
      if (st->bpp == 0) st->bpp = 1;
      st->stride = (st->width * bitsPerPixel + 7) / 8;
      st->rowBytes = 1 + st->stride;

      st->curRow  = (uint8_t*)calloc(1, st->rowBytes);
      st->prevRow = (uint8_t*)calloc(1, st->rowBytes);
      if (!st->curRow || !st->prevRow) {
        ok = false;
        break;
      }
      if (boxW > 0 && boxH > 0) {
        int step = 1;
        int maxW = (boxW < LAKATOS_SCREEN_W) ? LAKATOS_SCREEN_W : boxW;
        int maxH = (boxH < LAKATOS_SCREEN_H) ? LAKATOS_SCREEN_H : boxH;
        while (step < 8 && ((int)st->width / step > maxW || (int)st->height / step > maxH)) {
          step++;
        }
        st->step = step;
        int sw = (int)st->width / step;
        int sh = (int)st->height / step;
        st->dstX = dstX + ((boxW - sw) > 0 ? (boxW - sw) / 2 : 0);
        st->dstY = dstY + ((boxH - sh) > 0 ? (boxH - sh) / 2 : 0);
      }
      gotIhdr = true;
    } else if (memcmp(chType, "PLTE", 4) == 0) {
      uint32_t entries = chLen / 3;
      if (entries > 256) entries = 256;
      uint32_t readBytes = entries * 3;
      uint8_t plteBuf[768];
      if (readBytes > 0) {
        if (!reader.readExact(plteBuf, readBytes)) {
          ok = false;
          break;
        }
        for (uint32_t i = 0; i < entries; i++) {
          st->paletteR[i] = plteBuf[i * 3 + 0];
          st->paletteG[i] = plteBuf[i * 3 + 1];
          st->paletteB[i] = plteBuf[i * 3 + 2];
        }
      }
      if (ok && chLen > readBytes) {
        ok = reader.skip(chLen - readBytes);
      }
    } else if (memcmp(chType, "tRNS", 4) == 0) {
      if (st->colorType == 3) {
        uint32_t n = (chLen < 256) ? chLen : 256;
        if (!reader.readExact(st->paletteA, n)) {
          ok = false;
          break;
        }
        if (chLen > n && !reader.skip(chLen - n)) {
          ok = false;
          break;
        }
      } else if (st->colorType == 0 && chLen >= 2) {
        uint8_t buf[2];
        if (!reader.readExact(buf, 2)) {
          ok = false;
          break;
        }
        st->hasColorKey = true;
        st->keyR = readBE16(buf);
        if (chLen > 2) ok = reader.skip(chLen - 2);
      } else if (st->colorType == 2 && chLen >= 6) {
        uint8_t buf[6];
        if (!reader.readExact(buf, 6)) {
          ok = false;
          break;
        }
        st->hasColorKey = true;
        st->keyR = readBE16(buf);
        st->keyG = readBE16(buf + 2);
        st->keyB = readBE16(buf + 4);
        if (chLen > 6) ok = reader.skip(chLen - 6);
      } else {
        ok = reader.skip(chLen);
      }
    } else if (memcmp(chType, "IDAT", 4) == 0) {
      if (!gotIhdr) {
        ok = false;
        break;
      }
      uint32_t rem = chLen;
      while (rem > 0 && ok) {
        size_t req = (rem > kInChunkSize) ? kInChunkSize : rem;
        const uint8_t* chunkPtr = nullptr;
        size_t toRead = 0;
        if (reader.memData != nullptr) {
          if (reader.offset >= reader.memLen) {
            break; // Truncated buffer reached EOF cleanly
          }
          size_t avail = reader.memLen - reader.offset;
          toRead = (req < avail) ? req : avail;
          chunkPtr = reader.memData + reader.offset;
          reader.offset += (uint32_t)toRead;
        } else {
          toRead = reader.readUpTo(inBuf, req);
          if (toRead == 0) {
            break;
          }
          chunkPtr = inBuf;
        }
        rem -= (uint32_t)toRead;

        size_t inPos = 0;
        tinfl_status status = TINFL_STATUS_HAS_MORE_OUTPUT;
        while (inPos < toRead || status == TINFL_STATUS_HAS_MORE_OUTPUT) {
          size_t inBytes = toRead - inPos;
          size_t outBytes = TINFL_LZ_DICT_SIZE - dictOfs;
          uint32_t flags = TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_HAS_MORE_INPUT;
          uint32_t t0 = micros();
          status = tinfl_decompress(
              decomp, chunkPtr + inPos, &inBytes,
              dict, dict + dictOfs, &outBytes, flags);
          st->tinflUs += (micros() - t0);
          inPos += inBytes;

          if (outBytes > 0) {
            st->feedDecompressedBytes(dict + dictOfs, outBytes);
            dictOfs = (dictOfs + outBytes) & (TINFL_LZ_DICT_SIZE - 1);
          }
          if (status < 0) {
            ok = false;
            break;
          }
          if (status == TINFL_STATUS_DONE ||
              st->curY >= st->height ||
              (status == TINFL_STATUS_NEEDS_MORE_INPUT && inPos >= toRead)) {
            break;
          }
        }
        if (st->curY >= st->height) break;
        if (reader.memData != nullptr && reader.offset >= reader.memLen) break;
      }
      if (st->curY >= st->height) break;
      if (reader.memData != nullptr && reader.offset >= reader.memLen) break;
    } else if (memcmp(chType, "IEND", 4) == 0) {
      break;
    } else {
      if (!reader.skip(chLen)) {
        ok = false;
        break;
      }
    }

    // Skip 4-byte chunk CRC
    if (!reader.skip(4)) break;
  }

  tft.setSwapBytes(prevSwap);

  lakatosDevLogf("PNG %lux%lu color=%u step=%d rows=%lu read=%lu ms tinfl=%lu ms draw=%lu ms total=%lu ms",
                 (unsigned long)st->width, (unsigned long)st->height,
                 (unsigned)st->colorType, st->step,
                 (unsigned long)st->curY,
                 (unsigned long)(reader.readUs / 1000),
                 (unsigned long)(st->tinflUs / 1000),
                 (unsigned long)(st->drawUs / 1000),
                 (unsigned long)(millis() - tStart));

  if (st->curRow) free(st->curRow);
  if (st->prevRow) free(st->prevRow);
  if (inBuf) free(inBuf);
  if (!dictFromArena && dict) free(dict);
  if (!useScratch) free(decomp);
  bool success = gotIhdr && (st->curY > 0);
  if (!stFromArena) free(st);
  return success;
}

} // namespace

bool lakatosPngInfo(const char* vfsPath, int* outWidth, int* outHeight) {
  if (outWidth) *outWidth = 0;
  if (outHeight) *outHeight = 0;
  if (!vfsPath) return false;

  uint8_t hdr[24];
  if (lakatosFsRead(vfsPath, 0, hdr, 24) != 24) return false;
  if (memcmp(hdr, kPngSig, 8) != 0) return false;
  if (memcmp(hdr + 12, "IHDR", 4) != 0) return false;

  uint32_t w = readBE32(hdr + 16);
  uint32_t h = readBE32(hdr + 20);
  if (w == 0 || h == 0) return false;
  if (outWidth) *outWidth = (int)w;
  if (outHeight) *outHeight = (int)h;
  return true;
}

bool lakatosDrawPngFile(const char* vfsPath, int x, int y) {
  if (!vfsPath || !vfsPath[0]) return false;
  PngReader reader;
  reader.stream = lakatosFsOpenRead(vfsPath);
  if (!reader.stream) return false;
  if (x < 0) {
    int boxY = (y >= 0 && y < LAKATOS_SCREEN_H) ? y : 0;
    return decodeAndRenderPng(reader, 0, boxY, LAKATOS_SCREEN_W, LAKATOS_SCREEN_H - boxY);
  }
  return decodeAndRenderPng(reader, x, y, 0, 0);
}

bool lakatosDrawPngFileCentered(const char* vfsPath, int boxX, int boxY, int boxW, int boxH) {
  if (!vfsPath || !vfsPath[0]) return false;
  PngReader reader;
  reader.stream = lakatosFsOpenRead(vfsPath);
  if (!reader.stream) return false;
  return decodeAndRenderPng(reader, boxX, boxY, boxW, boxH);
}

bool lakatosDrawPngMem(const uint8_t* data, size_t len, int x, int y) {
  if (!data || len < 33) return false;
  PngReader reader;
  reader.memData = data;
  reader.memLen  = len;
  if (x < 0) {
    int boxY = (y >= 0 && y < LAKATOS_SCREEN_H) ? y : 0;
    return decodeAndRenderPng(reader, 0, boxY, LAKATOS_SCREEN_W, LAKATOS_SCREEN_H - boxY);
  }
  return decodeAndRenderPng(reader, x, y);
}

void lakatosTftDrawPal4(int x, int y, int w, int h,
                        const uint32_t* pal565, const uint8_t* pix4bpp) {
  if (!pal565 || !pix4bpp || w <= 0 || h <= 0 || (w & 1) != 0) return;
  if (w > LAKATOS_SCREEN_W) w = LAKATOS_SCREEN_W;

  int r0 = 0;
  int r1 = h;
  if (y < 0) r0 = -y;
  if (y + r1 > LAKATOS_SCREEN_H) r1 = LAKATOS_SCREEN_H - y;
  if (r0 >= r1) return;

  LakatosTftGuard tftLock;
  bool prevSwap = tft.getSwapBytes();

  uint16_t pal[16];
  for (int i = 0; i < 16; i++) {
    uint16_t c = (uint16_t)(pal565[i] & 0xFFFFu);
    if (prevSwap) {
      c = (uint16_t)((c << 8) | (c >> 8));
    }
    pal[i] = c;
  }

  uint32_t pairLut[256];
  for (int b = 0; b < 256; b++) {
    pairLut[b] = (uint32_t)pal[(b >> 4) & 0x0F] | ((uint32_t)pal[b & 0x0F] << 16);
  }

  uint32_t line32[LAKATOS_SCREEN_W / 2];
  int rowBytes = w >> 1;

  tft.setSwapBytes(false);
  tft.startWrite();
  tft.setAddrWindow(x, y + r0, w, r1 - r0);
  for (int row = r0; row < r1; row++) {
    const uint8_t* src = pix4bpp + row * rowBytes;
    for (int i = 0; i < rowBytes; i++) {
      line32[i] = pairLut[src[i]];
    }
    tft.pushPixels(line32, (uint32_t)w);
  }
  tft.endWrite();
  tft.setSwapBytes(prevSwap);
}

static inline void pal4Plot(uint8_t* pix4bpp, int w, int h, int x, int y, uint8_t col) {
  if ((unsigned)x >= (unsigned)w || (unsigned)y >= (unsigned)h) return;
  int idx = y * (w >> 1) + (x >> 1);
  if ((x & 1) == 0) {
    pix4bpp[idx] = (uint8_t)((pix4bpp[idx] & 0x0Fu) | ((col & 0x0Fu) << 4));
  } else {
    pix4bpp[idx] = (uint8_t)((pix4bpp[idx] & 0xF0u) | (col & 0x0Fu));
  }
}

static inline void pal4HLine(uint8_t* pix4bpp, int w, int h, int x0, int x1, int y, uint8_t col) {
  if ((unsigned)y >= (unsigned)h) return;
  if (x0 > x1) {
    int t = x0; x0 = x1; x1 = t;
  }
  if (x1 < 0 || x0 >= w) return;
  if (x0 < 0) x0 = 0;
  if (x1 >= w) x1 = w - 1;

  uint8_t c    = col & 0x0Fu;
  uint8_t pair = (uint8_t)((c << 4) | c);
  uint8_t* row = pix4bpp + y * (w >> 1);

  int x = x0;
  if ((x & 1) != 0) {
    int bi = x >> 1;
    row[bi] = (uint8_t)((row[bi] & 0xF0u) | c);
    x++;
  }
  int spanPairs = (x1 - x + 1) >> 1;
  if (spanPairs > 0) {
    memset(row + (x >> 1), pair, (size_t)spanPairs);
    x += spanPairs << 1;
  }
  if (x <= x1) {
    int bi = x >> 1;
    row[bi] = (uint8_t)((row[bi] & 0x0Fu) | (uint8_t)(c << 4));
  }
}

void lakatosPal4Clear(uint8_t* pix4bpp, int w, int h, int topRows, uint8_t topCol, uint8_t botCol) {
  if (!pix4bpp || w <= 0 || h <= 0 || (w & 1) != 0) return;
  int rowBytes = w >> 1;
  if (topRows < 0) topRows = 0;
  if (topRows > h) topRows = h;
  uint8_t topByte = (uint8_t)(((topCol & 0x0Fu) << 4) | (topCol & 0x0Fu));
  uint8_t botByte = (uint8_t)(((botCol & 0x0Fu) << 4) | (botCol & 0x0Fu));
  size_t splitByte = (size_t)topRows * (size_t)rowBytes;
  size_t totalBytes = (size_t)h * (size_t)rowBytes;
  if (splitByte > 0) {
    memset(pix4bpp, topByte, splitByte);
  }
  if (totalBytes > splitByte) {
    memset(pix4bpp + splitByte, botByte, totalBytes - splitByte);
  }
}

void lakatosPal4FillTriangle(uint8_t* pix4bpp, int w, int h,
                             int x0, int y0, int x1, int y1, int x2, int y2, uint8_t col) {
  if (!pix4bpp || w <= 0 || h <= 0 || (w & 1) != 0) return;
  if (y0 > y1) { int tx = x0, ty = y0; x0 = x1; y0 = y1; x1 = tx; y1 = ty; }
  if (y1 > y2) { int tx = x1, ty = y1; x1 = x2; y1 = y2; x2 = tx; y2 = ty; }
  if (y0 > y1) { int tx = x0, ty = y0; x0 = x1; y0 = y1; x1 = tx; y1 = ty; }

  int minX = x0, maxX = x0;
  if (x1 < minX) minX = x1;
  if (x2 < minX) minX = x2;
  if (x1 > maxX) maxX = x1;
  if (x2 > maxX) maxX = x2;
  if (y2 < 0 || y0 >= h || maxX < 0 || minX >= w) return;

  if (y0 == y2) {
    pal4HLine(pix4bpp, w, h, minX, maxX, y0, col);
    return;
  }

  int dx01 = x1 - x0, dy01 = y1 - y0;
  int dx02 = x2 - x0, dy02 = y2 - y0;
  int dx12 = x2 - x1, dy12 = y2 - y1;
  int last = (y1 == y2) ? y1 : (y1 - 1);

  int yA = (y0 < 0) ? 0 : y0;
  int yB = (last >= h) ? (h - 1) : last;
  if (yA <= yB) {
    int32_t sa = (int32_t)dx01 * (int32_t)(yA - y0);
    int32_t sb = (int32_t)dx02 * (int32_t)(yA - y0);
    for (int y = yA; y <= yB; y++) {
      int a = x0 + (int)(sa / dy01);
      int b = x0 + (int)(sb / dy02);
      sa += dx01;
      sb += dx02;
      pal4HLine(pix4bpp, w, h, a, b, y, col);
    }
  }

  if (y1 < y2) {
    int yC = (y1 < 0) ? 0 : y1;
    int yD = (y2 >= h) ? (h - 1) : y2;
    if (yC <= yD) {
      int32_t sa = (int32_t)dx12 * (int32_t)(yC - y1);
      int32_t sb = (int32_t)dx02 * (int32_t)(yC - y0);
      for (int y = yC; y <= yD; y++) {
        int a = x1 + (int)(sa / dy12);
        int b = x0 + (int)(sb / dy02);
        sa += dx12;
        sb += dx02;
        pal4HLine(pix4bpp, w, h, a, b, y, col);
      }
    }
  }
}

void lakatosPal4FillCircle(uint8_t* pix4bpp, int w, int h, int cx, int cy, int r, uint8_t col) {
  if (!pix4bpp || w <= 0 || h <= 0 || (w & 1) != 0 || r < 0) return;
  if (cx + r < 0 || cx - r >= w || cy + r < 0 || cy - r >= h) return;
  int dy0 = -r;
  int dy1 = r;
  if (cy + dy0 < 0) dy0 = -cy;
  if (cy + dy1 >= h) dy1 = h - 1 - cy;
  int r2 = r * r;
  for (int dy = dy0; dy <= dy1; dy++) {
    int y = cy + dy;
    int dx = 0;
    int rem = r2 - dy * dy;
    while ((dx + 1) * (dx + 1) <= rem) {
      dx++;
    }
    pal4HLine(pix4bpp, w, h, cx - dx, cx + dx, y, col);
  }
}

void lakatosPal4DrawLine(uint8_t* pix4bpp, int w, int h, int x0, int y0, int x1, int y1, uint8_t col) {
  if (!pix4bpp || w <= 0 || h <= 0 || (w & 1) != 0) return;
  if (y0 == y1) {
    pal4HLine(pix4bpp, w, h, x0, x1, y0, col);
    return;
  }
  if ((x0 < 0 && x1 < 0) || (x0 >= w && x1 >= w) ||
      (y0 < 0 && y1 < 0) || (y0 >= h && y1 >= h)) {
    return;
  }

  // Cohen-Sutherland 2D clipping to [0, w-1] x [0, h-1] so far-offscreen endpoints
  // never cause thousands of wasted off-screen Bresenham iterations.
  auto outCode = [w, h](int x, int y) -> int {
    int c = 0;
    if (x < 0) c |= 1;
    else if (x >= w) c |= 2;
    if (y < 0) c |= 4;
    else if (y >= h) c |= 8;
    return c;
  };
  int c0 = outCode(x0, y0);
  int c1 = outCode(x1, y1);
  for (int iter = 0; iter < 8 && (c0 | c1) != 0; iter++) {
    if ((c0 & c1) != 0) return;
    int co = c0 ? c0 : c1;
    int x = 0, y = 0;
    int32_t ddx = (int32_t)(x1 - x0);
    int32_t ddy = (int32_t)(y1 - y0);
    if (co & 8) {
      y = h - 1;
      x = x0 + (int)(ddx * (int32_t)(y - y0) / (ddy ? ddy : 1));
    } else if (co & 4) {
      y = 0;
      x = x0 + (int)(ddx * (int32_t)(y - y0) / (ddy ? ddy : 1));
    } else if (co & 2) {
      x = w - 1;
      y = y0 + (int)(ddy * (int32_t)(x - x0) / (ddx ? ddx : 1));
    } else {
      x = 0;
      y = y0 + (int)(ddy * (int32_t)(x - x0) / (ddx ? ddx : 1));
    }
    if (co == c0) {
      x0 = x; y0 = y; c0 = outCode(x0, y0);
    } else {
      x1 = x; y1 = y; c1 = outCode(x1, y1);
    }
  }
  if ((c0 | c1) != 0) return;

  int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
  int sx = (x0 < x1) ? 1 : -1;
  int dy = (y1 > y0) ? -(y1 - y0) : -(y0 - y1);
  int sy = (y0 < y1) ? 1 : -1;
  int err = dx + dy;
  for (;;) {
    pal4Plot(pix4bpp, w, h, x0, y0, col);
    if (x0 == x1 && y0 == y1) break;
    int e2 = err << 1;
    if (e2 >= dy) {
      err += dy;
      x0 += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y0 += sy;
    }
  }
}

namespace {

static uint32_t calcPngCrc32(const uint8_t* data, size_t len) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; i++) {
    c ^= data[i];
    for (int k = 0; k < 8; k++) {
      c = (c & 1u) ? ((c >> 1) ^ 0xEDB88320u) : (c >> 1);
    }
  }
  return ~c;
}

static inline void writeBE32(uint8_t* dst, uint32_t v) {
  dst[0] = (uint8_t)(v >> 24);
  dst[1] = (uint8_t)(v >> 16);
  dst[2] = (uint8_t)(v >> 8);
  dst[3] = (uint8_t)v;
}

struct PngPal4Encoder {
  uint8_t* buf      = nullptr;
  size_t   cap      = 0;
  size_t   len      = 0;
  bool     overflow = false;

  uint32_t bitBuf   = 0;
  uint32_t bitCnt   = 0;
  uint32_t s1       = 1;
  uint32_t s2       = 0;
  int      prevByte = -1;
  int      runLen   = 0;

  void putByte(uint8_t b) {
    if (len < cap) {
      buf[len++] = b;
    } else {
      overflow = true;
    }
  }

  void putBytes(const void* src, size_t n) {
    if (len + n <= cap) {
      memcpy(buf + len, src, n);
      len += n;
    } else {
      overflow = true;
    }
  }

  void emitBits(uint32_t val, uint32_t nbits) {
    bitBuf |= (val & ((1u << nbits) - 1u)) << bitCnt;
    bitCnt += nbits;
    while (bitCnt >= 8) {
      putByte((uint8_t)(bitBuf & 0xFFu));
      bitBuf >>= 8;
      bitCnt -= 8;
    }
  }

  void emitHuff(uint32_t code, uint32_t nbits) {
    uint32_t rev = 0;
    for (uint32_t i = 0; i < nbits; i++) {
      rev = (rev << 1) | ((code >> i) & 1u);
    }
    emitBits(rev, nbits);
  }

  void emitLit(int b) {
    if (b < 144) {
      emitHuff((uint32_t)(0x30 + b), 8);
    } else {
      emitHuff((uint32_t)(0x190 + (b - 144)), 9);
    }
  }

  void emitLenDist1(int length) {
    static const uint16_t kBaseLen[29] = {
      3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
      35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
    };
    static const uint8_t kExtraBits[29] = {
      0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
      3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
    };
    int idx = 28;
    for (int i = 0; i < 28; i++) {
      if (length >= kBaseLen[i] && length < kBaseLen[i + 1]) {
        idx = i;
        break;
      }
    }
    int sym = 257 + idx;
    if (sym <= 279) {
      emitHuff((uint32_t)(sym - 256), 7);
    } else {
      emitHuff((uint32_t)(0xC0 + (sym - 280)), 8);
    }
    if (kExtraBits[idx] > 0) {
      emitBits((uint32_t)(length - kBaseLen[idx]), kExtraBits[idx]);
    }
    emitBits(0, 5);  // distance = 1 -> distance code 0 (5 zero bits)
  }

  void flushRun() {
    while (runLen >= 3) {
      int take = (runLen > 258) ? 258 : runLen;
      emitLenDist1(take);
      runLen -= take;
    }
    while (runLen > 0) {
      emitLit(prevByte);
      runLen--;
    }
  }

  void feedByte(uint8_t b) {
    s1 = (s1 + b) % 65521u;
    s2 = (s2 + s1) % 65521u;
    if ((int)b == prevByte) {
      runLen++;
      if (runLen == 258) {
        emitLenDist1(258);
        runLen = 0;
      }
    } else {
      flushRun();
      emitLit((int)b);
      prevByte = (int)b;
      runLen = 0;
    }
  }

  void writeChunk(const char typ[4], const uint8_t* payload, size_t plen) {
    uint8_t lenBytes[4];
    writeBE32(lenBytes, (uint32_t)plen);
    putBytes(lenBytes, 4);
    size_t typePos = len;
    putBytes(typ, 4);
    if (plen > 0 && payload) {
      putBytes(payload, plen);
    }
    if (!overflow) {
      uint32_t crc = calcPngCrc32(buf + typePos, 4 + plen);
      uint8_t crcBytes[4];
      writeBE32(crcBytes, crc);
      putBytes(crcBytes, 4);
    }
  }
};

}  // namespace

bool lakatosSavePngPal4(const char* vfsPath, int w, int h,
                        const uint32_t* pal565, const uint8_t* pix4bpp) {
  if (!vfsPath || !vfsPath[0] || !pal565 || !pix4bpp || w <= 0 || h <= 0 || (w & 1) != 0) {
    return false;
  }

  PngPal4Encoder enc;
  enc.buf = gLakatosAppRamArena;
  enc.cap = LAKATOS_APP_DRAM_SIZE;

  // 1. PNG Signature
  enc.putBytes(kPngSig, 8);

  // 2. IHDR Chunk (bitDepth = 4, colorType = 3 Indexed-color)
  uint8_t ihdr[13];
  memset(ihdr, 0, sizeof(ihdr));
  writeBE32(ihdr + 0, (uint32_t)w);
  writeBE32(ihdr + 4, (uint32_t)h);
  ihdr[8] = 4;
  ihdr[9] = 3;
  enc.writeChunk("IHDR", ihdr, sizeof(ihdr));

  // 3. PLTE Chunk (16 RGB triples converted from RGB565)
  uint8_t plte[16 * 3];
  for (int i = 0; i < 16; i++) {
    uint16_t c = (uint16_t)(pal565[i] & 0xFFFFu);
    uint8_t r5 = (c >> 11) & 0x1F;
    uint8_t g6 = (c >> 5) & 0x3F;
    uint8_t b5 = c & 0x1F;
    plte[i * 3 + 0] = (uint8_t)((r5 << 3) | (r5 >> 2));
    plte[i * 3 + 1] = (uint8_t)((g6 << 2) | (g6 >> 4));
    plte[i * 3 + 2] = (uint8_t)((b5 << 3) | (b5 >> 2));
  }
  enc.writeChunk("PLTE", plte, sizeof(plte));

  // 4. IDAT Chunk streamed directly into enc.buf
  size_t idatLenPos  = enc.len;
  uint8_t zero4[4]   = {0, 0, 0, 0};
  enc.putBytes(zero4, 4);
  size_t idatTypePos = enc.len;
  enc.putBytes("IDAT", 4);
  size_t idatDataPos = enc.len;

  // Zlib header (0x78, 0x01) + Fixed-Huffman block header (BFINAL=1, BTYPE=01)
  enc.putByte(0x78);
  enc.putByte(0x01);
  enc.emitBits(1, 1);
  enc.emitBits(1, 2);

  int rowBytes = w / 2;
  for (int y = 0; y < h; y++) {
    const uint8_t* cur = pix4bpp + y * rowBytes;
    if (y == 0) {
      enc.feedByte(0);  // Filter 0 (None)
      for (int i = 0; i < rowBytes; i++) {
        enc.feedByte(cur[i]);
      }
    } else {
      const uint8_t* prev = pix4bpp + (y - 1) * rowBytes;
      enc.feedByte(2);  // Filter 2 (Up)
      for (int i = 0; i < rowBytes; i++) {
        enc.feedByte((uint8_t)(cur[i] - prev[i]));
      }
    }
  }
  enc.flushRun();
  enc.emitHuff(0, 7);  // EOB (256)
  if (enc.bitCnt > 0) {
    enc.putByte((uint8_t)(enc.bitBuf & 0xFFu));
  }
  uint32_t adler = (enc.s2 << 16) | enc.s1;
  uint8_t adlerBytes[4];
  writeBE32(adlerBytes, adler);
  enc.putBytes(adlerBytes, 4);

  if (enc.overflow) {
    return false;
  }

  size_t idatPayloadLen = enc.len - idatDataPos;
  writeBE32(enc.buf + idatLenPos, (uint32_t)idatPayloadLen);
  uint32_t idatCrc = calcPngCrc32(enc.buf + idatTypePos, 4 + idatPayloadLen);
  uint8_t idatCrcBytes[4];
  writeBE32(idatCrcBytes, idatCrc);
  enc.putBytes(idatCrcBytes, 4);

  // 5. IEND Chunk
  enc.writeChunk("IEND", nullptr, 0);
  if (enc.overflow) {
    return false;
  }

  return lakatosFsWrite(vfsPath, enc.buf, enc.len);
}
