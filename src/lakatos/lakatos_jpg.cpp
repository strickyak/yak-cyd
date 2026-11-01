#pragma GCC optimize("O3")
#include "lakatos_state.h"

#include <esp32/rom/tjpgd.h>
#include <stdlib.h>
#include <string.h>

namespace {

static constexpr size_t kTjpgdWorkSize = 4096;
static constexpr size_t kVfsCacheSize  = 1024;

struct JpgReaderContext {
  const char*    vfsPath   = nullptr;
  const uint8_t* memData   = nullptr;
  size_t         memLen    = 0;
  uint32_t       offset    = 0;
  uint32_t       cacheBase = 0;
  uint32_t       cacheLen  = 0;
  int            dstX      = 0;
  int            dstY      = 0;
  uint8_t        cache[kVfsCacheSize];
};

UINT jpgInCallback(JDEC* jd, BYTE* buff, UINT nbyte) {
  JpgReaderContext* ctx = (JpgReaderContext*)jd->device;
  if (!ctx || nbyte == 0) return 0;

  if (ctx->memData) {
    size_t avail = (ctx->offset < ctx->memLen) ? (ctx->memLen - ctx->offset) : 0;
    size_t toRead = (nbyte < avail) ? nbyte : avail;
    if (buff && toRead > 0) {
      memcpy(buff, ctx->memData + ctx->offset, toRead);
    }
    ctx->offset += (uint32_t)toRead;
    return (UINT)toRead;
  }

  if (ctx->vfsPath) {
    // When buff == nullptr, TJpgDec is skipping a segment (e.g., EXIF/APPn).
    if (!buff) {
      ctx->offset += (uint32_t)nbyte;
      return nbyte;
    }

    size_t total = 0;
    while (total < nbyte) {
      if (ctx->cacheLen > 0 &&
          ctx->offset >= ctx->cacheBase &&
          ctx->offset < ctx->cacheBase + ctx->cacheLen) {
        uint32_t inCacheOff = ctx->offset - ctx->cacheBase;
        uint32_t avail = ctx->cacheLen - inCacheOff;
        uint32_t take = (uint32_t)(nbyte - total);
        if (take > avail) take = avail;
        memcpy(buff + total, ctx->cache + inCacheOff, take);
        ctx->offset += take;
        total += take;
      } else {
        int32_t got = lakatosFsRead(ctx->vfsPath, ctx->offset, ctx->cache, kVfsCacheSize);
        if (got <= 0) break;
        ctx->cacheBase = ctx->offset;
        ctx->cacheLen  = (uint32_t)got;
      }
    }
    return (UINT)total;
  }

  return 0;
}

UINT jpgOutCallback(JDEC* jd, void* bitmap, JRECT* rect) {
  JpgReaderContext* ctx = (JpgReaderContext*)jd->device;
  lakatosPollTouchLatch();

  int bw = (int)rect->right - (int)rect->left + 1;
  int bh = (int)rect->bottom - (int)rect->top + 1;
  if (bw <= 0 || bh <= 0 || bw > 16 || bh > 16) return 1;

  int dx = ctx->dstX + (int)rect->left;
  int dy = ctx->dstY + (int)rect->top;

  // Since JPEG MCUs are decoded top-to-bottom, we can abort early if below screen.
  if (dy >= LAKATOS_SCREEN_H) return 0;

  // Skip MCUs completely off-screen horizontally or above the top edge.
  if (dx >= LAKATOS_SCREEN_W || dx + bw <= 0 || dy + bh <= 0) return 1;

  const uint8_t* src = (const uint8_t*)bitmap;
  uint16_t mcu565[256];
  int count = bw * bh;
  for (int i = 0; i < count; i++) {
    uint8_t r = src[0];
    uint8_t g = src[1];
    uint8_t b = src[2];
    src += 3;
    mcu565[i] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
  }

  if (dx >= 0 && dy >= 0 && dx + bw <= LAKATOS_SCREEN_W && dy + bh <= LAKATOS_SCREEN_H) {
    tft.pushImage(dx, dy, bw, bh, mcu565);
  } else {
    int cx0 = (dx < 0) ? -dx : 0;
    int cx1 = (dx + bw > LAKATOS_SCREEN_W) ? (LAKATOS_SCREEN_W - dx) : bw;
    int cw  = cx1 - cx0;
    int cy0 = (dy < 0) ? -dy : 0;
    int cy1 = (dy + bh > LAKATOS_SCREEN_H) ? (LAKATOS_SCREEN_H - dy) : bh;
    if (cw > 0) {
      for (int ry = cy0; ry < cy1; ry++) {
        tft.pushImage(dx + cx0, dy + ry, cw, 1, &mcu565[ry * bw + cx0]);
      }
    }
  }

  return 1;
}

bool decodeAndRenderJpg(JpgReaderContext& ctx, int x, int y, int scale) {
  if (scale < 0) scale = 0;
  if (scale > 3) scale = 3;
  ctx.dstX = x;
  ctx.dstY = y;

  uint8_t* workBuf = (uint8_t*)malloc(kTjpgdWorkSize);
  if (!workBuf) return false;

  JDEC jdec;
  JRESULT res = jd_prepare(&jdec, jpgInCallback, workBuf, kTjpgdWorkSize, &ctx);
  if (res != JDR_OK) {
    free(workBuf);
    return false;
  }

  bool prevSwap = tft.getSwapBytes();
  tft.setSwapBytes(true);

  res = jd_decomp(&jdec, jpgOutCallback, (BYTE)scale);

  tft.setSwapBytes(prevSwap);
  free(workBuf);

  // Note: JDR_INTR means jpgOutCallback returned 0 because the rest of the image
  // was below SCREEN_H, which is a valid clipped render.
  return (res == JDR_OK || res == JDR_INTR);
}

} // namespace

bool lakatosJpgInfo(const char* vfsPath, int* outWidth, int* outHeight) {
  if (outWidth) *outWidth = 0;
  if (outHeight) *outHeight = 0;
  if (!vfsPath || !vfsPath[0]) return false;

  JpgReaderContext* ctx = (JpgReaderContext*)malloc(sizeof(JpgReaderContext));
  uint8_t* workBuf = (uint8_t*)malloc(kTjpgdWorkSize);
  if (!ctx || !workBuf) {
    if (ctx) free(ctx);
    if (workBuf) free(workBuf);
    return false;
  }
  memset(ctx, 0, sizeof(JpgReaderContext));
  ctx->vfsPath = vfsPath;

  JDEC jdec;
  JRESULT res = jd_prepare(&jdec, jpgInCallback, workBuf, kTjpgdWorkSize, ctx);
  bool ok = (res == JDR_OK && jdec.width > 0 && jdec.height > 0);
  if (ok) {
    if (outWidth) *outWidth = (int)jdec.width;
    if (outHeight) *outHeight = (int)jdec.height;
  }

  free(workBuf);
  free(ctx);
  return ok;
}

bool lakatosDrawJpgFile(const char* vfsPath, int x, int y, int scale) {
  if (!vfsPath || !vfsPath[0]) return false;
  JpgReaderContext* ctx = (JpgReaderContext*)malloc(sizeof(JpgReaderContext));
  if (!ctx) return false;
  memset(ctx, 0, sizeof(JpgReaderContext));
  ctx->vfsPath = vfsPath;
  bool ok = decodeAndRenderJpg(*ctx, x, y, scale);
  free(ctx);
  return ok;
}

bool lakatosDrawJpgMem(const uint8_t* data, size_t len, int x, int y, int scale) {
  if (!data || len < 4) return false;
  JpgReaderContext* ctx = (JpgReaderContext*)malloc(sizeof(JpgReaderContext));
  if (!ctx) return false;
  memset(ctx, 0, sizeof(JpgReaderContext));
  ctx->memData = data;
  ctx->memLen  = len;
  bool ok = decodeAndRenderJpg(*ctx, x, y, scale);
  free(ctx);
  return ok;
}
