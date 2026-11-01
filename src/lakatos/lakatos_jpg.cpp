#pragma GCC optimize("O3")
#include "lakatos_state.h"

#include <esp32/rom/tjpgd.h>
#include <stdlib.h>
#include <string.h>

namespace {

static constexpr size_t kTjpgdWorkSize = 4096;

struct JpgReaderContext {
  LakatosVfsStream* stream  = nullptr;
  const uint8_t*    memData = nullptr;
  size_t            memLen  = 0;
  uint32_t          offset  = 0;
  int               dstX    = 0;
  int               dstY    = 0;

  ~JpgReaderContext() {
    if (stream) {
      lakatosFsClose(stream);
      stream = nullptr;
    }
  }
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

  if (ctx->stream) {
    if (!buff) {
      if (!lakatosFsStreamSkip(ctx->stream, nbyte)) return 0;
      ctx->offset += (uint32_t)nbyte;
      return nbyte;
    }
    int32_t got = lakatosFsStreamRead(ctx->stream, buff, nbyte);
    if (got <= 0) return 0;
    ctx->offset += (uint32_t)got;
    return (UINT)got;
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

bool decodeAndRenderJpg(JpgReaderContext& ctx, int x, int y, int scale,
                        int boxW = 0, int boxH = 0) {
  uint32_t tStart = millis();
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

  if (boxW > 0 && boxH > 0) {
    scale = 0;
    int maxW = (boxW < LAKATOS_SCREEN_W) ? LAKATOS_SCREEN_W : boxW;
    int maxH = (boxH < LAKATOS_SCREEN_H) ? LAKATOS_SCREEN_H : boxH;
    while (scale < 3 && (((int)jdec.width >> scale) > maxW || ((int)jdec.height >> scale) > maxH)) {
      scale++;
    }
    int sw = (int)jdec.width >> scale;
    int sh = (int)jdec.height >> scale;
    ctx.dstX = x + ((boxW - sw) > 0 ? (boxW - sw) / 2 : 0);
    ctx.dstY = y + ((boxH - sh) > 0 ? (boxH - sh) / 2 : 0);
  }

  bool prevSwap = tft.getSwapBytes();
  tft.setSwapBytes(true);

  res = jd_decomp(&jdec, jpgOutCallback, (BYTE)scale);

  tft.setSwapBytes(prevSwap);
  free(workBuf);

  Serial.printf("[TIMING] JPG %ux%u scale=%d decode+draw=%lu ms\r\n",
                (unsigned)jdec.width, (unsigned)jdec.height, scale,
                (unsigned long)(millis() - tStart));

  // Note: JDR_INTR means jpgOutCallback returned 0 because the rest of the image
  // was below SCREEN_H, and JDR_INP after jd_prepare succeeds means a truncated
  // buffer rendered all available upper MCUs.
  return (res == JDR_OK || res == JDR_INTR || (res == JDR_INP && jdec.width > 0));
}

} // namespace

bool lakatosJpgInfo(const char* vfsPath, int* outWidth, int* outHeight) {
  if (outWidth) *outWidth = 0;
  if (outHeight) *outHeight = 0;
  if (!vfsPath || !vfsPath[0]) return false;

  JpgReaderContext ctx;
  ctx.stream = lakatosFsOpenRead(vfsPath);
  if (!ctx.stream) return false;

  uint8_t* workBuf = (uint8_t*)malloc(kTjpgdWorkSize);
  if (!workBuf) return false;

  JDEC jdec;
  JRESULT res = jd_prepare(&jdec, jpgInCallback, workBuf, kTjpgdWorkSize, &ctx);
  bool ok = (res == JDR_OK && jdec.width > 0 && jdec.height > 0);
  if (ok) {
    if (outWidth) *outWidth = (int)jdec.width;
    if (outHeight) *outHeight = (int)jdec.height;
  }

  free(workBuf);
  return ok;
}

bool lakatosJpgInfoMem(const uint8_t* data, size_t len, int* outWidth, int* outHeight) {
  if (outWidth) *outWidth = 0;
  if (outHeight) *outHeight = 0;
  if (!data || len < 4) return false;

  JpgReaderContext ctx;
  ctx.memData = data;
  ctx.memLen  = len;

  uint8_t* workBuf = (uint8_t*)malloc(kTjpgdWorkSize);
  if (!workBuf) return false;

  JDEC jdec;
  JRESULT res = jd_prepare(&jdec, jpgInCallback, workBuf, kTjpgdWorkSize, &ctx);
  bool ok = (res == JDR_OK && jdec.width > 0 && jdec.height > 0);
  if (ok) {
    if (outWidth) *outWidth = (int)jdec.width;
    if (outHeight) *outHeight = (int)jdec.height;
  }
  free(workBuf);
  return ok;
}

bool lakatosDrawJpgFile(const char* vfsPath, int x, int y, int scale) {
  if (!vfsPath || !vfsPath[0]) return false;
  JpgReaderContext ctx;
  ctx.stream = lakatosFsOpenRead(vfsPath);
  if (!ctx.stream) return false;
  return decodeAndRenderJpg(ctx, x, y, scale, 0, 0);
}

bool lakatosDrawJpgFileCentered(const char* vfsPath, int boxX, int boxY, int boxW, int boxH) {
  if (!vfsPath || !vfsPath[0]) return false;
  JpgReaderContext ctx;
  ctx.stream = lakatosFsOpenRead(vfsPath);
  if (!ctx.stream) return false;
  return decodeAndRenderJpg(ctx, boxX, boxY, 0, boxW, boxH);
}

bool lakatosDrawJpgMem(const uint8_t* data, size_t len, int x, int y, int scale) {
  if (!data || len < 4) return false;
  JpgReaderContext ctx;
  ctx.memData = data;
  ctx.memLen  = len;
  if (x < 0) {
    int boxY = (y >= 0 && y < LAKATOS_SCREEN_H) ? y : 0;
    return decodeAndRenderJpg(ctx, 0, boxY, 0, LAKATOS_SCREEN_W, LAKATOS_SCREEN_H - boxY);
  }
  return decodeAndRenderJpg(ctx, x, y, scale, 0, 0);
}
