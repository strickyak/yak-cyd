#pragma GCC optimize("O3")
#include "lakatos_state.h"

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
  const char*    vfsPath = nullptr;
  const uint8_t* memData = nullptr;
  size_t         memLen  = 0;
  uint32_t       offset  = 0;

  bool readExact(void* dst, size_t n) {
    if (n == 0) return true;
    if (memData) {
      if (offset + n > memLen) return false;
      memcpy(dst, memData + offset, n);
      offset += (uint32_t)n;
      return true;
    }
    if (vfsPath) {
      uint8_t* out = (uint8_t*)dst;
      size_t rem = n;
      while (rem > 0) {
        int32_t got = lakatosFsRead(vfsPath, offset, out, rem);
        if (got <= 0) return false;
        offset += (uint32_t)got;
        out += got;
        rem -= (size_t)got;
      }
      return true;
    }
    return false;
  }

  bool skip(size_t n) {
    if (n == 0) return true;
    if (memData) {
      if (offset + n > memLen) return false;
      offset += (uint32_t)n;
      return true;
    }
    offset += (uint32_t)n;
    return true;
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
    int sy = dstY + (int)curY;
    if (sy < 0 || sy >= LAKATOS_SCREEN_H) return;

    const uint8_t* raw = curRow + 1;
    int spanStartScreenX = -1;
    int spanLen = 0;

    for (uint32_t x = 0; x < width; x++) {
      int sx = dstX + (int)x;
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
        tft.pushImage(spanStartScreenX, sy, spanLen, 1, line565 + spanStartScreenX);
        spanStartScreenX = -1;
        spanLen = 0;
      }
    }

    if (spanStartScreenX >= 0 && spanLen > 0) {
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
        unfilterCurrentRow();
        renderCurrentRow();
        uint8_t* tmp = prevRow;
        prevRow = curRow;
        curRow = tmp;
        curRowPos = 0;
        curY++;
      }
    }
  }
};

static bool decodeAndRenderPng(PngReader& reader, int dstX, int dstY) {
  uint8_t sig[8];
  if (!reader.readExact(sig, 8) || memcmp(sig, kPngSig, 8) != 0) {
    return false;
  }

  PngDecoderState* st = (PngDecoderState*)calloc(1, sizeof(PngDecoderState));
  if (!st) return false;
  st->dstX = dstX;
  st->dstY = dstY;
  memset(st->paletteA, 255, sizeof(st->paletteA));

  tinfl_decompressor* decomp = (tinfl_decompressor*)malloc(sizeof(tinfl_decompressor));
  uint8_t* dict = (uint8_t*)malloc(TINFL_LZ_DICT_SIZE);
  constexpr size_t kInChunkSize = 1024;
  uint8_t* inBuf = (uint8_t*)malloc(kInChunkSize);

  if (!decomp || !dict || !inBuf) {
    if (inBuf) free(inBuf);
    if (dict) free(dict);
    if (decomp) free(decomp);
    free(st);
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
      gotIhdr = true;
    } else if (memcmp(chType, "PLTE", 4) == 0) {
      uint32_t entries = chLen / 3;
      if (entries > 256) entries = 256;
      for (uint32_t i = 0; i < entries; i++) {
        uint8_t rgb[3];
        if (!reader.readExact(rgb, 3)) {
          ok = false;
          break;
        }
        st->paletteR[i] = rgb[0];
        st->paletteG[i] = rgb[1];
        st->paletteB[i] = rgb[2];
      }
      uint32_t readBytes = entries * 3;
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
        size_t toRead = (rem > kInChunkSize) ? kInChunkSize : rem;
        if (!reader.readExact(inBuf, toRead)) {
          ok = false;
          break;
        }
        rem -= (uint32_t)toRead;

        size_t inPos = 0;
        tinfl_status status = TINFL_STATUS_HAS_MORE_OUTPUT;
        while (inPos < toRead || status == TINFL_STATUS_HAS_MORE_OUTPUT) {
          size_t inBytes = toRead - inPos;
          size_t outBytes = TINFL_LZ_DICT_SIZE - dictOfs;
          uint32_t flags = TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_HAS_MORE_INPUT;
          status = tinfl_decompress(
              decomp, inBuf + inPos, &inBytes,
              dict, dict + dictOfs, &outBytes, flags);
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
              (status == TINFL_STATUS_NEEDS_MORE_INPUT && inPos >= toRead)) {
            break;
          }
        }
      }
    } else if (memcmp(chType, "IEND", 4) == 0) {
      reader.skip(chLen + 4);
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

  if (st->curRow) free(st->curRow);
  if (st->prevRow) free(st->prevRow);
  free(inBuf);
  free(dict);
  free(decomp);
  bool success = ok && gotIhdr && (st->curY > 0);
  free(st);
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
  reader.vfsPath = vfsPath;
  return decodeAndRenderPng(reader, x, y);
}

bool lakatosDrawPngMem(const uint8_t* data, size_t len, int x, int y) {
  if (!data || len < 33) return false;
  PngReader reader;
  reader.memData = data;
  reader.memLen  = len;
  return decodeAndRenderPng(reader, x, y);
}
