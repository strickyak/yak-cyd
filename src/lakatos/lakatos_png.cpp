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
