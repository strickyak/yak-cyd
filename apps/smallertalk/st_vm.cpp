#include "st_vm.h"
#include "st_bootstrap.h"
#include "st_compiler.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static constexpr uint16_t kContextStackCap = 16;

// ---------------------------------------------------------------------------
// Context Creation & Stack Manipulation Helpers
// ---------------------------------------------------------------------------
static Oop createMethodContext(VMState* vm, Oop senderCtx, Oop rcvr,
                               Oop meth, const Oop* args, uint8_t numArgs) {
  int32_t hdr = asSmallInt(stSlotAt(vm, meth, SLOT_METH_HEADER));
  uint8_t numTemps = unpackMethodNumTemps(hdr);
  if (numTemps < numArgs) numTemps = numArgs;

  uint16_t saved = vm->tempRootCount;
  stPushRoot(vm, senderCtx);
  stPushRoot(vm, rcvr);
  stPushRoot(vm, meth);
  for (uint8_t i = 0; i < numArgs; i++) stPushRoot(vm, args[i]);

  uint16_t totalSlots = SLOT_CTX_TEMPS_AND_STACK + numTemps + kContextStackCap;
  Oop ctx = stNewOopSlots(vm, CLS_CONTEXT, totalSlots, 0);
  if (ctx != kOopNil) {
    stSlotAtPut(vm, ctx, SLOT_CTX_SENDER,   senderCtx);
    stSlotAtPut(vm, ctx, SLOT_CTX_RECEIVER, rcvr);
    stSlotAtPut(vm, ctx, SLOT_CTX_METHOD,   meth);
    stSlotAtPut(vm, ctx, SLOT_CTX_CLOSURE,  kOopNil);
    stSlotAtPut(vm, ctx, SLOT_CTX_HOME,     ctx);  // Points to self while active on stack
    stSlotAtPut(vm, ctx, SLOT_CTX_PC,       makeSmallInt(0));
    stSlotAtPut(vm, ctx, SLOT_CTX_SP,       makeSmallInt(0));
    for (uint8_t i = 0; i < numArgs; i++) {
      stSlotAtPut(vm, ctx, SLOT_CTX_TEMPS_AND_STACK + i, args[i]);
    }
  }
  stPopRoots(vm, saved);
  return ctx;
}

static Oop createBlockContext(VMState* vm, Oop senderCtx, Oop closure,
                              const Oop* args, uint8_t numArgs) {
  Oop homeCtx  = stSlotAt(vm, closure, SLOT_BLK_HOME_CONTEXT);
  Oop meth     = stSlotAt(vm, closure, SLOT_BLK_METHOD);
  Oop startPc  = stSlotAt(vm, closure, SLOT_BLK_START_PC);
  Oop rcvr     = stSlotAt(vm, homeCtx, SLOT_CTX_RECEIVER);
  uint8_t tempBase = (uint8_t)asSmallInt(stSlotAt(vm, closure, SLOT_BLK_TEMP_BASE));

  uint16_t saved = vm->tempRootCount;
  stPushRoot(vm, senderCtx);
  stPushRoot(vm, closure);
  stPushRoot(vm, homeCtx);
  stPushRoot(vm, meth);
  stPushRoot(vm, rcvr);
  for (uint8_t i = 0; i < numArgs; i++) stPushRoot(vm, args[i]);

  // Reserve `numArgs` slots at `SLOT_CTX_TEMPS_AND_STACK` to save/restore outer block args on recursive calls
  uint16_t totalSlots = SLOT_CTX_TEMPS_AND_STACK + numArgs + kContextStackCap;
  Oop bctx = stNewOopSlots(vm, CLS_CONTEXT, totalSlots, 0);
  if (bctx != kOopNil) {
    stSlotAtPut(vm, bctx, SLOT_CTX_SENDER,   senderCtx);
    stSlotAtPut(vm, bctx, SLOT_CTX_RECEIVER, rcvr);
    stSlotAtPut(vm, bctx, SLOT_CTX_METHOD,   meth);
    stSlotAtPut(vm, bctx, SLOT_CTX_CLOSURE,  closure);
    stSlotAtPut(vm, bctx, SLOT_CTX_HOME,     homeCtx);
    stSlotAtPut(vm, bctx, SLOT_CTX_PC,       startPc);
    stSlotAtPut(vm, bctx, SLOT_CTX_SP,       makeSmallInt(0));

    for (uint8_t i = 0; i < numArgs; i++) {
      Oop prevVal = stSlotAt(vm, homeCtx, SLOT_CTX_TEMPS_AND_STACK + tempBase + i);
      stSlotAtPut(vm, bctx, SLOT_CTX_TEMPS_AND_STACK + i, prevVal);
      stSlotAtPut(vm, homeCtx, SLOT_CTX_TEMPS_AND_STACK + tempBase + i, args[i]);
    }
  }
  stPopRoots(vm, saved);
  return bctx;
}

static inline uint16_t ctxStackBase(VMState* vm, Oop ctx) {
  Oop closure = stSlotAt(vm, ctx, SLOT_CTX_CLOSURE);
  if (closure != kOopNil) {
    uint8_t numBlkArgs = (uint8_t)asSmallInt(stSlotAt(vm, closure, SLOT_BLK_NUM_ARGS));
    return SLOT_CTX_TEMPS_AND_STACK + numBlkArgs;
  }
  Oop meth = stSlotAt(vm, ctx, SLOT_CTX_METHOD);
  int32_t hdr = asSmallInt(stSlotAt(vm, meth, SLOT_METH_HEADER));
  return SLOT_CTX_TEMPS_AND_STACK + unpackMethodNumTemps(hdr);
}

static inline void ctxPush(VMState* vm, Oop ctx, Oop val) {
  int32_t sp = asSmallInt(stSlotAt(vm, ctx, SLOT_CTX_SP));
  uint16_t base = ctxStackBase(vm, ctx);
  if (sp >= 0 && sp < (int32_t)kContextStackCap) {
    stSlotAtPut(vm, ctx, base + (uint16_t)sp, val);
    stSlotAtPut(vm, ctx, SLOT_CTX_SP, makeSmallInt(sp + 1));
  }
}

static inline Oop ctxPop(VMState* vm, Oop ctx) {
  int32_t sp = asSmallInt(stSlotAt(vm, ctx, SLOT_CTX_SP));
  uint16_t base = ctxStackBase(vm, ctx);
  if (sp > 0) {
    sp--;
    Oop v = stSlotAt(vm, ctx, base + (uint16_t)sp);
    stSlotAtPut(vm, ctx, base + (uint16_t)sp, kOopNil);
    stSlotAtPut(vm, ctx, SLOT_CTX_SP, makeSmallInt(sp));
    return v;
  }
  return kOopNil;
}

static inline Oop ctxTop(VMState* vm, Oop ctx) {
  int32_t sp = asSmallInt(stSlotAt(vm, ctx, SLOT_CTX_SP));
  uint16_t base = ctxStackBase(vm, ctx);
  if (sp > 0) {
    return stSlotAt(vm, ctx, base + (uint16_t)(sp - 1));
  }
  return kOopNil;
}

// ---------------------------------------------------------------------------
// Object Formatting (`printString` / `asString`)
// ---------------------------------------------------------------------------
void stFormatOop(VMState* vm, Oop obj, char* outBuf, size_t maxLen) {
  if (!outBuf || maxLen == 0) return;
  if (obj == kOopNil || obj == kOopNull) {
    snprintf(outBuf, maxLen, "nil");
    return;
  }
  if (obj == kOopTrue) {
    snprintf(outBuf, maxLen, "true");
    return;
  }
  if (obj == kOopFalse) {
    snprintf(outBuf, maxLen, "false");
    return;
  }
  if (isSmallInt(obj)) {
    snprintf(outBuf, maxLen, "%ld", (long)asSmallInt(obj));
    return;
  }
  if (isChar(obj)) {
    snprintf(outBuf, maxLen, "$%c", (char)asChar(obj));
    return;
  }
  ObjHeader* h = stGetHeader(vm, obj);
  if (!h) {
    snprintf(outBuf, maxLen, "<invalid>");
    return;
  }
  if (h->format == FMT_STRING) {
    snprintf(outBuf, maxLen, "'%s'", (const char*)stByteData(vm, obj));
    return;
  }
  if (h->format == FMT_SYMBOL) {
    snprintf(outBuf, maxLen, "#%s", (const char*)stByteData(vm, obj));
    return;
  }
  if (h->classIdx == CLS_FLOAT) {
    float fv = stAsFloat(vm, obj);
    int32_t iv = (int32_t)fv;
    float frac = fabsf(fv - (float)iv);
    int32_t dec4 = (int32_t)(frac * 10000.0f + 0.5f);
    if (dec4 >= 10000) {
      iv += (fv >= 0.0f) ? 1 : -1;
      dec4 -= 10000;
    }
    char fbuf[32];
    if (fv < 0.0f && iv == 0) {
      snprintf(fbuf, sizeof(fbuf), "-0.%04ld", (long)dec4);
    } else {
      snprintf(fbuf, sizeof(fbuf), "%ld.%04ld", (long)iv, (long)dec4);
    }
    // Trim trailing zeros after decimal point (keeping at least 1 digit after '.')
    size_t flen = strlen(fbuf);
    while (flen > 2 && fbuf[flen - 1] == '0' && fbuf[flen - 2] != '.') {
      fbuf[--flen] = '\0';
    }
    snprintf(outBuf, maxLen, "%s", fbuf);
    return;
  }
  if (h->flags & OBJ_FLAG_CLASS_OBJ) {
    Oop nm = stSlotAt(vm, obj, SLOT_CLS_NAME);
    const char* s = (const char*)stByteData(vm, nm);
    snprintf(outBuf, maxLen, "%s", s ? s : "Class");
    return;
  }
  if (h->classIdx == CLS_POINT) {
    char xb[32], yb[32];
    stFormatOop(vm, stSlotAt(vm, obj, SLOT_POINT_X), xb, sizeof(xb));
    stFormatOop(vm, stSlotAt(vm, obj, SLOT_POINT_Y), yb, sizeof(yb));
    snprintf(outBuf, maxLen, "%s@%s", xb, yb);
    return;
  }
  if (h->classIdx == CLS_ARRAY) {
    size_t pos = 0;
    pos += snprintf(outBuf + pos, maxLen - pos, "#(");
    for (uint16_t i = 0; i < h->size && pos + 8 < maxLen; i++) {
      if (i > 0) pos += snprintf(outBuf + pos, maxLen - pos, " ");
      char elem[48];
      stFormatOop(vm, stSlotAt(vm, obj, i), elem, sizeof(elem));
      pos += snprintf(outBuf + pos, maxLen - pos, "%s", elem);
    }
    snprintf(outBuf + pos, maxLen - pos, ")");
    return;
  }
  if (h->classIdx == CLS_ORDERED_COLLECTION) {
    Oop arr = stSlotAt(vm, obj, SLOT_OC_ARRAY);
    Oop szOop = stSlotAt(vm, obj, SLOT_OC_SIZE);
    uint16_t sz = isSmallInt(szOop) ? (uint16_t)asSmallInt(szOop) : 0;
    size_t pos = 0;
    pos += snprintf(outBuf + pos, maxLen - pos, "OrderedCollection(");
    for (uint16_t i = 0; i < sz && pos + 8 < maxLen; i++) {
      if (i > 0) pos += snprintf(outBuf + pos, maxLen - pos, " ");
      char elem[48];
      stFormatOop(vm, stSlotAt(vm, arr, i), elem, sizeof(elem));
      pos += snprintf(outBuf + pos, maxLen - pos, "%s", elem);
    }
    snprintf(outBuf + pos, maxLen - pos, ")");
    return;
  }
  Oop cls = stGetClassOop(vm, obj);
  Oop clsName = stSlotAt(vm, cls, SLOT_CLS_NAME);
  const char* cname = (const char*)stByteData(vm, clsName);
  if (!cname) cname = "Object";
  bool vowel = (cname[0] == 'A' || cname[0] == 'E' || cname[0] == 'I' ||
                cname[0] == 'O' || cname[0] == 'U');
  snprintf(outBuf, maxLen, "%s %s", vowel ? "an" : "a", cname);
}

const char* stGetMethodSourceCStr(VMState* vm, Oop methOop, char* scratchBuf, size_t scratchCap) {
  if (!isHeapOop(methOop) || methOop == kOopNil) return "";
  Oop srcOop = stSlotAt(vm, methOop, SLOT_METH_SOURCE);
  if (isSmallInt(srcOop)) {
    const char* ro = stGetBootstrapSource((int)asSmallInt(srcOop));
    if (ro) return ro;
  }
  if (isHeapOop(srcOop)) {
    stCopyCString(vm, srcOop, scratchBuf, scratchCap);
    return scratchBuf;
  }
  return "";
}

// ---------------------------------------------------------------------------
// Primitive Execution Table (`<primitive: N>`)
// ---------------------------------------------------------------------------
enum PrimOutcome {
  PRIM_SUCCESS      = 0,  // Push `outResult` onto current context stack
  PRIM_ACTIVATE_CTX = 1,  // Primitive pushed a new `vm->activeContext`
  PRIM_FAIL         = 2,  // Fallback to Smalltalk method body
};

static PrimOutcome executePrimitive(VMState* vm, uint16_t primId, Oop rcvr,
                                    const Oop* args, uint8_t numArgs, Oop* outResult) {
  switch (primId) {
    // --- 1..19: SmallInteger & Float Arithmetic / Comparisons ---
    case 1: {  // +
      if (numArgs < 1) return PRIM_FAIL;
      if (isSmallInt(rcvr) && isSmallInt(args[0])) {
        int64_t r = (int64_t)asSmallInt(rcvr) + (int64_t)asSmallInt(args[0]);
        *outResult = makeSmallInt((int32_t)r);
        return PRIM_SUCCESS;
      }
      if (stIsNumber(vm, rcvr) && stIsNumber(vm, args[0])) {
        *outResult = stNewFloat(vm, stToFloat(vm, rcvr) + stToFloat(vm, args[0]));
        return PRIM_SUCCESS;
      }
      return PRIM_FAIL;
    }
    case 2: {  // -
      if (numArgs < 1) return PRIM_FAIL;
      if (isSmallInt(rcvr) && isSmallInt(args[0])) {
        int64_t r = (int64_t)asSmallInt(rcvr) - (int64_t)asSmallInt(args[0]);
        *outResult = makeSmallInt((int32_t)r);
        return PRIM_SUCCESS;
      }
      if (stIsNumber(vm, rcvr) && stIsNumber(vm, args[0])) {
        *outResult = stNewFloat(vm, stToFloat(vm, rcvr) - stToFloat(vm, args[0]));
        return PRIM_SUCCESS;
      }
      return PRIM_FAIL;
    }
    case 3: {  // <
      if (numArgs < 1) return PRIM_FAIL;
      if (isSmallInt(rcvr) && isSmallInt(args[0])) {
        *outResult = makeBool(asSmallInt(rcvr) < asSmallInt(args[0]));
        return PRIM_SUCCESS;
      }
      if (stIsNumber(vm, rcvr) && stIsNumber(vm, args[0])) {
        *outResult = makeBool(stToFloat(vm, rcvr) < stToFloat(vm, args[0]));
        return PRIM_SUCCESS;
      }
      if (isChar(rcvr) && isChar(args[0])) {
        *outResult = makeBool(asChar(rcvr) < asChar(args[0]));
        return PRIM_SUCCESS;
      }
      return PRIM_FAIL;
    }
    case 4: {  // >
      if (numArgs < 1) return PRIM_FAIL;
      if (isSmallInt(rcvr) && isSmallInt(args[0])) {
        *outResult = makeBool(asSmallInt(rcvr) > asSmallInt(args[0]));
        return PRIM_SUCCESS;
      }
      if (stIsNumber(vm, rcvr) && stIsNumber(vm, args[0])) {
        *outResult = makeBool(stToFloat(vm, rcvr) > stToFloat(vm, args[0]));
        return PRIM_SUCCESS;
      }
      if (isChar(rcvr) && isChar(args[0])) {
        *outResult = makeBool(asChar(rcvr) > asChar(args[0]));
        return PRIM_SUCCESS;
      }
      return PRIM_FAIL;
    }
    case 5: {  // <=
      if (numArgs < 1) return PRIM_FAIL;
      if (isSmallInt(rcvr) && isSmallInt(args[0])) {
        *outResult = makeBool(asSmallInt(rcvr) <= asSmallInt(args[0]));
        return PRIM_SUCCESS;
      }
      if (stIsNumber(vm, rcvr) && stIsNumber(vm, args[0])) {
        *outResult = makeBool(stToFloat(vm, rcvr) <= stToFloat(vm, args[0]));
        return PRIM_SUCCESS;
      }
      return PRIM_FAIL;
    }
    case 6: {  // >=
      if (numArgs < 1) return PRIM_FAIL;
      if (isSmallInt(rcvr) && isSmallInt(args[0])) {
        *outResult = makeBool(asSmallInt(rcvr) >= asSmallInt(args[0]));
        return PRIM_SUCCESS;
      }
      if (stIsNumber(vm, rcvr) && stIsNumber(vm, args[0])) {
        *outResult = makeBool(stToFloat(vm, rcvr) >= stToFloat(vm, args[0]));
        return PRIM_SUCCESS;
      }
      return PRIM_FAIL;
    }
    case 7: {  // =
      if (numArgs < 1) return PRIM_FAIL;
      if (stIsNumber(vm, rcvr) && stIsNumber(vm, args[0])) {
        if (isSmallInt(rcvr) && isSmallInt(args[0])) {
          *outResult = makeBool(rcvr == args[0]);
        } else {
          *outResult = makeBool(fabsf(stToFloat(vm, rcvr) - stToFloat(vm, args[0])) < 1e-6f);
        }
        return PRIM_SUCCESS;
      }
      *outResult = makeBool(stOopEquals(vm, rcvr, args[0]));
      return PRIM_SUCCESS;
    }
    case 8: {  // ~=
      if (numArgs < 1) return PRIM_FAIL;
      Oop eqRes = kOopFalse;
      if (executePrimitive(vm, 7, rcvr, args, numArgs, &eqRes) == PRIM_SUCCESS) {
        *outResult = (eqRes == kOopTrue) ? kOopFalse : kOopTrue;
        return PRIM_SUCCESS;
      }
      return PRIM_FAIL;
    }
    case 9: {  // *
      if (numArgs < 1) return PRIM_FAIL;
      if (isSmallInt(rcvr) && isSmallInt(args[0])) {
        int64_t r = (int64_t)asSmallInt(rcvr) * (int64_t)asSmallInt(args[0]);
        *outResult = makeSmallInt((int32_t)r);
        return PRIM_SUCCESS;
      }
      if (stIsNumber(vm, rcvr) && stIsNumber(vm, args[0])) {
        *outResult = stNewFloat(vm, stToFloat(vm, rcvr) * stToFloat(vm, args[0]));
        return PRIM_SUCCESS;
      }
      return PRIM_FAIL;
    }
    case 10: {  // /
      if (numArgs < 1) return PRIM_FAIL;
      if (isSmallInt(rcvr) && isSmallInt(args[0])) {
        int32_t a = asSmallInt(rcvr);
        int32_t b = asSmallInt(args[0]);
        if (b == 0) return PRIM_FAIL;
        if (a % b == 0) {
          *outResult = makeSmallInt(a / b);
        } else {
          *outResult = stNewFloat(vm, (float)a / (float)b);
        }
        return PRIM_SUCCESS;
      }
      if (stIsNumber(vm, rcvr) && stIsNumber(vm, args[0])) {
        float b = stToFloat(vm, args[0]);
        if (b == 0.0f) return PRIM_FAIL;
        *outResult = stNewFloat(vm, stToFloat(vm, rcvr) / b);
        return PRIM_SUCCESS;
      }
      return PRIM_FAIL;
    }
    case 11: {  // \\ (modulo)
      if (numArgs < 1 || !isSmallInt(rcvr) || !isSmallInt(args[0])) return PRIM_FAIL;
      int32_t a = asSmallInt(rcvr);
      int32_t b = asSmallInt(args[0]);
      if (b == 0) return PRIM_FAIL;
      int32_t r = a % b;
      if (r != 0 && ((r < 0) != (b < 0))) r += b;
      *outResult = makeSmallInt(r);
      return PRIM_SUCCESS;
    }
    case 12: {  // // (floor division)
      if (numArgs < 1 || !isSmallInt(rcvr) || !isSmallInt(args[0])) return PRIM_FAIL;
      int32_t a = asSmallInt(rcvr);
      int32_t b = asSmallInt(args[0]);
      if (b == 0) return PRIM_FAIL;
      int32_t q = a / b;
      int32_t r = a % b;
      if (r != 0 && ((r < 0) != (b < 0))) q--;
      *outResult = makeSmallInt(q);
      return PRIM_SUCCESS;
    }
    case 13: {  // quo: (truncated division)
      if (numArgs < 1 || !isSmallInt(rcvr) || !isSmallInt(args[0])) return PRIM_FAIL;
      int32_t b = asSmallInt(args[0]);
      if (b == 0) return PRIM_FAIL;
      *outResult = makeSmallInt(asSmallInt(rcvr) / b);
      return PRIM_SUCCESS;
    }
    case 14: {  // bitAnd:
      if (numArgs < 1 || !isSmallInt(rcvr) || !isSmallInt(args[0])) return PRIM_FAIL;
      *outResult = makeSmallInt(asSmallInt(rcvr) & asSmallInt(args[0]));
      return PRIM_SUCCESS;
    }
    case 15: {  // bitOr:
      if (numArgs < 1 || !isSmallInt(rcvr) || !isSmallInt(args[0])) return PRIM_FAIL;
      *outResult = makeSmallInt(asSmallInt(rcvr) | asSmallInt(args[0]));
      return PRIM_SUCCESS;
    }
    case 16: {  // bitXor:
      if (numArgs < 1 || !isSmallInt(rcvr) || !isSmallInt(args[0])) return PRIM_FAIL;
      *outResult = makeSmallInt(asSmallInt(rcvr) ^ asSmallInt(args[0]));
      return PRIM_SUCCESS;
    }
    case 17: {  // bitShift:
      if (numArgs < 1 || !isSmallInt(rcvr) || !isSmallInt(args[0])) return PRIM_FAIL;
      int32_t v = asSmallInt(rcvr);
      int32_t sh = asSmallInt(args[0]);
      if (sh >= 0 && sh < 30) *outResult = makeSmallInt(v << sh);
      else if (sh < 0 && sh > -31) *outResult = makeSmallInt(v >> (-sh));
      else *outResult = makeSmallInt(0);
      return PRIM_SUCCESS;
    }
    case 18: {  // asFloat
      if (!stIsNumber(vm, rcvr)) return PRIM_FAIL;
      *outResult = stNewFloat(vm, stToFloat(vm, rcvr));
      return PRIM_SUCCESS;
    }
    case 19: {  // truncated / asInteger
      if (isSmallInt(rcvr)) {
        *outResult = rcvr;
        return PRIM_SUCCESS;
      }
      if (stIsNumber(vm, rcvr)) {
        *outResult = makeSmallInt((int32_t)stToFloat(vm, rcvr));
        return PRIM_SUCCESS;
      }
      if (isChar(rcvr)) {
        *outResult = makeSmallInt((int32_t)asChar(rcvr));
        return PRIM_SUCCESS;
      }
      return PRIM_FAIL;
    }

    // --- 20..25: Float Math ---
    case 20:  // sin
      *outResult = stNewFloat(vm, sinf(stToFloat(vm, rcvr)));
      return PRIM_SUCCESS;
    case 21:  // cos
      *outResult = stNewFloat(vm, cosf(stToFloat(vm, rcvr)));
      return PRIM_SUCCESS;
    case 22:  // sqrt
      *outResult = stNewFloat(vm, sqrtf(fabsf(stToFloat(vm, rcvr))));
      return PRIM_SUCCESS;
    case 23:  // rounded
      *outResult = makeSmallInt((int32_t)roundf(stToFloat(vm, rcvr)));
      return PRIM_SUCCESS;
    case 24:  // floor
      *outResult = makeSmallInt((int32_t)floorf(stToFloat(vm, rcvr)));
      return PRIM_SUCCESS;
    case 25:  // ceiling
      *outResult = makeSmallInt((int32_t)ceilf(stToFloat(vm, rcvr)));
      return PRIM_SUCCESS;

    // --- 30..42: Object, Indexing, Reflection ---
    case 30:  // ==
      if (numArgs < 1) return PRIM_FAIL;
      *outResult = makeBool(rcvr == args[0]);
      return PRIM_SUCCESS;
    case 31:  // class
      *outResult = stGetClassOop(vm, rcvr);
      return PRIM_SUCCESS;
    case 32: {  // size / basicSize
      ObjHeader* h = stGetHeader(vm, rcvr);
      *outResult = makeSmallInt(h ? (int32_t)h->size : 0);
      return PRIM_SUCCESS;
    }
    case 33: {  // at: (1-based indexing)
      if (numArgs < 1 || !isSmallInt(args[0])) return PRIM_FAIL;
      int32_t idx1 = asSmallInt(args[0]);
      ObjHeader* h = stGetHeader(vm, rcvr);
      if (!h || idx1 < 1 || idx1 > (int32_t)h->size) return PRIM_FAIL;
      uint16_t idx0 = (uint16_t)(idx1 - 1);
      if (h->format == FMT_OOP_SLOTS) {
        *outResult = stSlotAt(vm, rcvr, idx0);
      } else if (h->format == FMT_STRING || h->format == FMT_SYMBOL) {
        *outResult = makeChar(stByteData(vm, rcvr)[idx0]);
      } else {
        *outResult = makeSmallInt(stByteData(vm, rcvr)[idx0]);
      }
      return PRIM_SUCCESS;
    }
    case 34: {  // at:put: (1-based indexing)
      if (numArgs < 2 || !isSmallInt(args[0])) return PRIM_FAIL;
      int32_t idx1 = asSmallInt(args[0]);
      ObjHeader* h = stGetHeader(vm, rcvr);
      if (!h || idx1 < 1 || idx1 > (int32_t)h->size) return PRIM_FAIL;
      uint16_t idx0 = (uint16_t)(idx1 - 1);
      if (h->format == FMT_OOP_SLOTS) {
        stSlotAtPut(vm, rcvr, idx0, args[1]);
      } else if (h->format == FMT_STRING) {
        if (isChar(args[1])) {
          stByteData(vm, rcvr)[idx0] = asChar(args[1]);
        } else if (isSmallInt(args[1])) {
          stByteData(vm, rcvr)[idx0] = (uint8_t)asSmallInt(args[1]);
        } else {
          return PRIM_FAIL;
        }
      } else if (h->format == FMT_BYTES) {
        if (!isSmallInt(args[1])) return PRIM_FAIL;
        stByteData(vm, rcvr)[idx0] = (uint8_t)asSmallInt(args[1]);
      } else {
        return PRIM_FAIL;
      }
      *outResult = args[1];
      return PRIM_SUCCESS;
    }
    case 35: {  // instVarAt: (1-based)
      if (numArgs < 1 || !isSmallInt(args[0])) return PRIM_FAIL;
      int32_t idx1 = asSmallInt(args[0]);
      ObjHeader* h = stGetHeader(vm, rcvr);
      if (!h || h->format != FMT_OOP_SLOTS || idx1 < 1 || idx1 > (int32_t)h->size) return PRIM_FAIL;
      *outResult = stSlotAt(vm, rcvr, (uint16_t)(idx1 - 1));
      return PRIM_SUCCESS;
    }
    case 36: {  // instVarAt:put: (1-based)
      if (numArgs < 2 || !isSmallInt(args[0])) return PRIM_FAIL;
      int32_t idx1 = asSmallInt(args[0]);
      ObjHeader* h = stGetHeader(vm, rcvr);
      if (!h || h->format != FMT_OOP_SLOTS || idx1 < 1 || idx1 > (int32_t)h->size) return PRIM_FAIL;
      stSlotAtPut(vm, rcvr, (uint16_t)(idx1 - 1), args[1]);
      *outResult = args[1];
      return PRIM_SUCCESS;
    }
    case 37: {  // shallowCopy
      ObjHeader* h = stGetHeader(vm, rcvr);
      if (!h) {
        *outResult = rcvr;
        return PRIM_SUCCESS;
      }
      if (h->format == FMT_OOP_SLOTS) {
        uint16_t saved = vm->tempRootCount;
        stPushRoot(vm, rcvr);
        Oop cp = stNewOopSlots(vm, h->classIdx, h->size, h->flags & ~OBJ_FLAG_MARKED);
        uint16_t sz = stObjSize(vm, rcvr);
        for (uint16_t i = 0; i < sz; i++) {
          stSlotAtPut(vm, cp, i, stSlotAt(vm, rcvr, i));
        }
        stPopRoots(vm, saved);
        *outResult = cp;
      } else {
        uint16_t saved = vm->tempRootCount;
        stPushRoot(vm, rcvr);
        Oop cp = stNewBytes(vm, h->classIdx, (ObjFormat)h->format, stByteData(vm, rcvr), h->size);
        stPopRoots(vm, saved);
        *outResult = cp;
      }
      return PRIM_SUCCESS;
    }
    case 38:    // perform: aSelector
    case 39: {  // perform: aSelector with: arg1
      if (numArgs < 1) return PRIM_FAIL;
      Oop sel = args[0];
      Oop meth = stLookupMethod(vm, rcvr, sel, kOopNull);
      if (meth == kOopNull || meth == kOopNil) return PRIM_FAIL;
      uint8_t callArgs = (primId == 39 && numArgs >= 2) ? 1 : 0;
      const Oop* callArgPtr = (callArgs > 0) ? &args[1] : nullptr;
      int32_t mhdr = asSmallInt(stSlotAt(vm, meth, SLOT_METH_HEADER));
      uint16_t mprim = unpackMethodPrimId(mhdr);
      if (mprim > 0) {
        Oop pres = kOopNil;
        PrimOutcome po = executePrimitive(vm, mprim, rcvr, callArgPtr, callArgs, &pres);
        if (po == PRIM_SUCCESS) {
          *outResult = pres;
          return PRIM_SUCCESS;
        }
        if (po == PRIM_ACTIVATE_CTX) {
          return PRIM_ACTIVATE_CTX;
        }
      }
      // Pop `rcvr` and `perform:` arguments from current context before pushing new MethodContext
      for (uint8_t i = 0; i <= numArgs; i++) ctxPop(vm, vm->activeContext);
      Oop newCtx = createMethodContext(vm, vm->activeContext, rcvr, meth, callArgPtr, callArgs);
      vm->activeContext = newCtx;
      return PRIM_ACTIVATE_CTX;
    }
    case 41: {  // printString
      char buf[192];
      stFormatOop(vm, rcvr, buf, sizeof(buf));
      *outResult = stNewString(vm, buf);
      return PRIM_SUCCESS;
    }
    case 42: {  // error: aString
      char msg[96] = "Error";
      if (numArgs >= 1) stCopyCString(vm, args[0], msg, sizeof(msg));
      snprintf(vm->lastError, sizeof(vm->lastError), "Error: %s", msg);
      *outResult = rcvr;
      return PRIM_SUCCESS;
    }

    // --- 50..70: Class, Method, String, Character, Dictionary ---
    case 50: {  // Class>>basicNew / new
      uint16_t cid = CLS_OBJECT;
      for (uint16_t i = 1; i < kMaxClasses; i++) {
        if (vm->classes[i] == rcvr) {
          cid = i;
          break;
        }
      }
      if (cid == CLS_STRING) {
        *outResult = stNewBytes(vm, CLS_STRING, FMT_STRING, nullptr, 0);
      } else if (cid == CLS_BYTE_ARRAY) {
        *outResult = stNewByteArray(vm, 0);
      } else {
        uint16_t instSz = stClassInstSize(vm, rcvr);
        *outResult = stNewOopSlots(vm, cid, instSz, 0);
      }
      return PRIM_SUCCESS;
    }
    case 51: {  // Class>>basicNew: / new: size
      if (numArgs < 1 || !isSmallInt(args[0])) return PRIM_FAIL;
      int32_t sz = asSmallInt(args[0]);
      if (sz < 0 || sz > 2048) return PRIM_FAIL;
      uint16_t cid = CLS_ARRAY;
      for (uint16_t i = 1; i < kMaxClasses; i++) {
        if (vm->classes[i] == rcvr) {
          cid = i;
          break;
        }
      }
      if (cid == CLS_STRING) {
        *outResult = stNewBytes(vm, CLS_STRING, FMT_STRING, nullptr, (uint16_t)sz);
      } else if (cid == CLS_BYTE_ARRAY) {
        *outResult = stNewByteArray(vm, (uint16_t)sz);
      } else {
        *outResult = stNewOopSlots(vm, cid, (uint16_t)sz, 0);
      }
      return PRIM_SUCCESS;
    }
    case 52:  // Class>>name
      *outResult = stSlotAt(vm, rcvr, SLOT_CLS_NAME);
      return PRIM_SUCCESS;
    case 53:  // Class>>superclass
      *outResult = stSlotAt(vm, rcvr, SLOT_CLS_SUPERCLASS);
      return PRIM_SUCCESS;
    case 54: {  // Class>>subclasses
      Oop subs[32];
      uint16_t count = 0;
      for (uint16_t i = 1; i < vm->classCount && i < kMaxClasses; i++) {
        Oop c = vm->classes[i];
        if (isHeapOop(c) && c != kOopNil && stSlotAt(vm, c, SLOT_CLS_SUPERCLASS) == rcvr) {
          if (count < 32) subs[count++] = c;
        }
      }
      Oop arr = stNewArray(vm, count);
      for (uint16_t i = 0; i < count; i++) stSlotAtPut(vm, arr, i, subs[i]);
      *outResult = arr;
      return PRIM_SUCCESS;
    }
    case 55:    // Class>>selectors
    case 56: {  // Class>>classSelectors
      Oop dict = stSlotAt(vm, rcvr, (primId == 56) ? SLOT_CLS_CLASS_METHODS : SLOT_CLS_INST_METHODS);
      Oop keys = stSlotAt(vm, dict, SLOT_DICT_KEYS);
      Oop tallyOop = stSlotAt(vm, dict, SLOT_DICT_TALLY);
      uint16_t tally = isSmallInt(tallyOop) ? (uint16_t)asSmallInt(tallyOop) : 0;
      uint16_t saved = vm->tempRootCount;
      stPushRoot(vm, keys);
      Oop arr = stNewArray(vm, tally);
      for (uint16_t i = 0; i < tally; i++) {
        stSlotAtPut(vm, arr, i, stSlotAt(vm, keys, i));
      }
      stPopRoots(vm, saved);
      *outResult = arr;
      return PRIM_SUCCESS;
    }
    case 57: {  // Class>>sourceCodeAt: selector
      if (numArgs < 1) return PRIM_FAIL;
      Oop sel = args[0];
      if (stGetHeader(vm, sel) && stGetHeader(vm, sel)->format == FMT_STRING) {
        char sbuf[96];
        stCopyCString(vm, sel, sbuf, sizeof(sbuf));
        sel = stInternSymbol(vm, sbuf);
      }
      Oop idict = stSlotAt(vm, rcvr, SLOT_CLS_INST_METHODS);
      Oop m = stDictAt(vm, idict, sel, kOopNull);
      if (m == kOopNull || m == kOopNil) {
        Oop cdict = stSlotAt(vm, rcvr, SLOT_CLS_CLASS_METHODS);
        m = stDictAt(vm, cdict, sel, kOopNull);
      }
      if (m == kOopNull || m == kOopNil) {
        *outResult = stNewString(vm, "");
        return PRIM_SUCCESS;
      }
      char scratch[512];
      const char* src = stGetMethodSourceCStr(vm, m, scratch, sizeof(scratch));
      *outResult = stNewString(vm, src);
      return PRIM_SUCCESS;
    }
    case 58:    // Class>>compile: sourceString
    case 59: {  // Class>>compileClassMethod: sourceString
      if (numArgs < 1) return PRIM_FAIL;
      char srcBuf[512];
      stCopyCString(vm, args[0], srcBuf, sizeof(srcBuf));
      Oop m = stCompileMethod(vm, rcvr, (primId == 59), srcBuf, -1);
      if (m == kOopNil) return PRIM_FAIL;
      *outResult = stSlotAt(vm, m, SLOT_METH_SELECTOR);
      return PRIM_SUCCESS;
    }
    case 60:    // Class>>subclass: sym instanceVariableNames: str
    case 61: {  // Class>>subclass: sym instanceVariableNames: str classVariableNames: cstr
      if (numArgs < 2) return PRIM_FAIL;
      char cname[64], ivars[128];
      stCopyCString(vm, args[0], cname, sizeof(cname));
      stCopyCString(vm, args[1], ivars, sizeof(ivars));
      if (cname[0] == '\0') return PRIM_FAIL;
      Oop newCls = stCreateClass(vm, 0, cname, rcvr, ivars);
      *outResult = newCls;
      return PRIM_SUCCESS;
    }
    case 62: {  // String/Array `,` concatenation
      if (numArgs < 1) return PRIM_FAIL;
      ObjHeader* ha = stGetHeader(vm, rcvr);
      ObjHeader* hb = stGetHeader(vm, args[0]);
      if (!ha || !hb) return PRIM_FAIL;
      uint16_t saved = vm->tempRootCount;
      stPushRoot(vm, rcvr);
      stPushRoot(vm, args[0]);
      if ((ha->format == FMT_STRING || ha->format == FMT_SYMBOL) &&
          (hb->format == FMT_STRING || hb->format == FMT_SYMBOL)) {
        uint16_t n1 = stObjSize(vm, rcvr);
        uint16_t n2 = stObjSize(vm, args[0]);
        Oop res = stNewBytes(vm, CLS_STRING, FMT_STRING, nullptr, n1 + n2);
        if (res != kOopNil) {
          uint8_t* dst = stByteData(vm, res);
          memcpy(dst, stByteData(vm, rcvr), n1);
          memcpy(dst + n1, stByteData(vm, args[0]), n2);
          dst[n1 + n2] = '\0';
        }
        stPopRoots(vm, saved);
        *outResult = res;
        return PRIM_SUCCESS;
      }
      if (ha->format == FMT_OOP_SLOTS && hb->format == FMT_OOP_SLOTS) {
        uint16_t n1 = stObjSize(vm, rcvr);
        uint16_t n2 = stObjSize(vm, args[0]);
        Oop res = stNewArray(vm, n1 + n2);
        for (uint16_t i = 0; i < n1; i++) stSlotAtPut(vm, res, i, stSlotAt(vm, rcvr, i));
        for (uint16_t i = 0; i < n2; i++) stSlotAtPut(vm, res, n1 + i, stSlotAt(vm, args[0], i));
        stPopRoots(vm, saved);
        *outResult = res;
        return PRIM_SUCCESS;
      }
      stPopRoots(vm, saved);
      return PRIM_FAIL;
    }
    case 63: {  // copyFrom: start to: stop (1-based)
      if (numArgs < 2 || !isSmallInt(args[0]) || !isSmallInt(args[1])) return PRIM_FAIL;
      int32_t start1 = asSmallInt(args[0]);
      int32_t stop1  = asSmallInt(args[1]);
      ObjHeader* h = stGetHeader(vm, rcvr);
      if (!h) return PRIM_FAIL;
      if (start1 < 1) start1 = 1;
      if (stop1 > (int32_t)h->size) stop1 = (int32_t)h->size;
      int32_t len = (stop1 >= start1) ? (stop1 - start1 + 1) : 0;
      uint16_t saved = vm->tempRootCount;
      stPushRoot(vm, rcvr);
      if (h->format == FMT_STRING || h->format == FMT_SYMBOL) {
        Oop res = stNewBytes(vm, CLS_STRING, FMT_STRING,
                             stByteData(vm, rcvr) + (start1 - 1), (uint16_t)len);
        stPopRoots(vm, saved);
        *outResult = res;
        return PRIM_SUCCESS;
      }
      if (h->format == FMT_OOP_SLOTS) {
        Oop res = stNewArray(vm, (uint16_t)len);
        for (int32_t i = 0; i < len; i++) {
          stSlotAtPut(vm, res, (uint16_t)i, stSlotAt(vm, rcvr, (uint16_t)(start1 - 1 + i)));
        }
        stPopRoots(vm, saved);
        *outResult = res;
        return PRIM_SUCCESS;
      }
      stPopRoots(vm, saved);
      return PRIM_FAIL;
    }
    case 64: {  // asSymbol
      char buf[96];
      stCopyCString(vm, rcvr, buf, sizeof(buf));
      *outResult = stInternSymbol(vm, buf);
      return PRIM_SUCCESS;
    }
    case 65: {  // Character>>value / asInteger
      if (isChar(rcvr)) {
        *outResult = makeSmallInt((int32_t)asChar(rcvr));
        return PRIM_SUCCESS;
      }
      return PRIM_FAIL;
    }
    case 66: {  // Character class>>value: anInt
      if (numArgs < 1 || !isSmallInt(args[0])) return PRIM_FAIL;
      *outResult = makeChar((uint8_t)(asSmallInt(args[0]) & 0xFF));
      return PRIM_SUCCESS;
    }
    case 67: {  // Dictionary>>at: key ifAbsent: aBlock
      if (numArgs < 1) return PRIM_FAIL;
      Oop found = stDictAt(vm, rcvr, args[0], kOopNull);
      if (found != kOopNull) {
        *outResult = found;
        return PRIM_SUCCESS;
      }
      if (numArgs >= 2) {
        // Evaluate `ifAbsent:` block if it's a BlockClosure, or return value directly
        ObjHeader* bh = stGetHeader(vm, args[1]);
        if (bh && bh->classIdx == CLS_BLOCK_CLOSURE) {
          for (uint8_t i = 0; i <= numArgs; i++) ctxPop(vm, vm->activeContext);
          vm->activeContext = createBlockContext(vm, vm->activeContext, args[1], nullptr, 0);
          return PRIM_ACTIVATE_CTX;
        }
        *outResult = args[1];
        return PRIM_SUCCESS;
      }
      *outResult = kOopNil;
      return PRIM_SUCCESS;
    }
    case 68: {  // Dictionary>>at: key put: val
      if (numArgs < 2) return PRIM_FAIL;
      stDictAtPut(vm, rcvr, args[0], args[1]);
      *outResult = args[1];
      return PRIM_SUCCESS;
    }
    case 69: {  // Dictionary>>keys
      Oop keys = stSlotAt(vm, rcvr, SLOT_DICT_KEYS);
      Oop tallyOop = stSlotAt(vm, rcvr, SLOT_DICT_TALLY);
      uint16_t tally = isSmallInt(tallyOop) ? (uint16_t)asSmallInt(tallyOop) : 0;
      uint16_t saved = vm->tempRootCount;
      stPushRoot(vm, keys);
      Oop arr = stNewArray(vm, tally);
      for (uint16_t i = 0; i < tally; i++) {
        stSlotAtPut(vm, arr, i, stSlotAt(vm, keys, i));
      }
      stPopRoots(vm, saved);
      *outResult = arr;
      return PRIM_SUCCESS;
    }
    case 70: {  // Dictionary>>removeKey: key
      if (numArgs < 1) return PRIM_FAIL;
      stDictRemoveKey(vm, rcvr, args[0]);
      *outResult = rcvr;
      return PRIM_SUCCESS;
    }

    // --- 71..74: OrderedCollection Primitives ---
    case 71: {  // OrderedCollection>>add: anObject
      if (numArgs < 1) return PRIM_FAIL;
      uint16_t saved = vm->tempRootCount;
      stPushRoot(vm, rcvr);
      stPushRoot(vm, args[0]);
      Oop arr = stSlotAt(vm, rcvr, SLOT_OC_ARRAY);
      Oop szOop = stSlotAt(vm, rcvr, SLOT_OC_SIZE);
      uint16_t sz = isSmallInt(szOop) ? (uint16_t)asSmallInt(szOop) : 0;
      uint16_t cap = stObjSize(vm, arr);
      if (sz >= cap) {
        uint16_t newCap = (cap < 4) ? 4 : (cap * 2);
        Oop newArr = stNewArray(vm, newCap);
        for (uint16_t i = 0; i < sz && i < cap; i++) {
          stSlotAtPut(vm, newArr, i, stSlotAt(vm, arr, i));
        }
        arr = newArr;
        stSlotAtPut(vm, rcvr, SLOT_OC_ARRAY, arr);
      }
      stSlotAtPut(vm, arr, sz, args[0]);
      stSlotAtPut(vm, rcvr, SLOT_OC_SIZE, makeSmallInt(sz + 1));
      stPopRoots(vm, saved);
      *outResult = args[0];
      return PRIM_SUCCESS;
    }
    case 72: {  // OrderedCollection>>removeLast
      Oop arr = stSlotAt(vm, rcvr, SLOT_OC_ARRAY);
      Oop szOop = stSlotAt(vm, rcvr, SLOT_OC_SIZE);
      uint16_t sz = isSmallInt(szOop) ? (uint16_t)asSmallInt(szOop) : 0;
      if (sz == 0) return PRIM_FAIL;
      Oop v = stSlotAt(vm, arr, sz - 1);
      stSlotAtPut(vm, arr, sz - 1, kOopNil);
      stSlotAtPut(vm, rcvr, SLOT_OC_SIZE, makeSmallInt(sz - 1));
      *outResult = v;
      return PRIM_SUCCESS;
    }

    // --- 80..83: BlockClosure Activation ---
    case 80:
    case 81:
    case 82:
    case 83: {
      ObjHeader* bh = stGetHeader(vm, rcvr);
      if (!bh || bh->classIdx != CLS_BLOCK_CLOSURE) return PRIM_FAIL;
      uint8_t expectedArgs = (uint8_t)asSmallInt(stSlotAt(vm, rcvr, SLOT_BLK_NUM_ARGS));
      if (numArgs != expectedArgs) return PRIM_FAIL;
      // Pop receiver (closure) and arguments from current context stack before pushing BlockContext
      for (uint8_t i = 0; i <= numArgs; i++) {
        ctxPop(vm, vm->activeContext);
      }
      Oop bctx = createBlockContext(vm, vm->activeContext, rcvr, args, numArgs);
      vm->activeContext = bctx;
      return PRIM_ACTIVATE_CTX;
    }

    // --- 90..99: Transcript & System ---
    case 90: {  // Transcript class>>show: aStringOrObject
      if (numArgs < 1) return PRIM_FAIL;
      ObjHeader* h = stGetHeader(vm, args[0]);
      if (h && (h->format == FMT_STRING || h->format == FMT_SYMBOL)) {
        stTranscriptAppend(vm, (const char*)stByteData(vm, args[0]), h->size);
      } else {
        char buf[128];
        stFormatOop(vm, args[0], buf, sizeof(buf));
        stTranscriptAppendCStr(vm, buf);
      }
      *outResult = rcvr;
      return PRIM_SUCCESS;
    }
    case 91:  // Transcript class>>cr
      stTranscriptAppend(vm, "\n", 1);
      *outResult = rcvr;
      return PRIM_SUCCESS;
    case 92:  // Transcript class>>clear
      stTranscriptClear(vm);
      *outResult = rcvr;
      return PRIM_SUCCESS;
    case 93:  // Transcript class>>contents
      *outResult = stNewString(vm, vm->transcript, (int)vm->transcriptLen);
      return PRIM_SUCCESS;
    case 94: {  // System class>>gc
      stRunGC(vm);
      int32_t freeBytes = (int32_t)((kPayloadWords - vm->payloadUsedWords) * sizeof(uint32_t));
      *outResult = makeSmallInt(freeBytes);
      return PRIM_SUCCESS;
    }
    case 95: {  // System class>>freeHeap
      int32_t freeB = vm->os && vm->os->free_heap_bytes
                          ? (int32_t)vm->os->free_heap_bytes()
                          : (int32_t)((kPayloadWords - vm->payloadUsedWords) * 4);
      *outResult = makeSmallInt(freeB);
      return PRIM_SUCCESS;
    }
    case 96: {  // System class>>millis
      int32_t ms = (vm->os && vm->os->millis) ? (int32_t)vm->os->millis() : 0;
      *outResult = makeSmallInt(ms);
      return PRIM_SUCCESS;
    }
    case 97: {  // System class>>delayMs:
      if (numArgs >= 1 && isSmallInt(args[0]) && vm->os && vm->os->delay_ms) {
        int32_t ms = asSmallInt(args[0]);
        if (ms > 0 && ms <= 5000) vm->os->delay_ms((uint32_t)ms);
      }
      *outResult = rcvr;
      return PRIM_SUCCESS;
    }
    case 98: {  // System class>>ledR:g:b:ms:
      if (numArgs >= 4 && vm->os && vm->os->set_rgb_led) {
        bool r = (args[0] == kOopTrue) || (isSmallInt(args[0]) && asSmallInt(args[0]) != 0);
        bool g = (args[1] == kOopTrue) || (isSmallInt(args[1]) && asSmallInt(args[1]) != 0);
        bool b = (args[2] == kOopTrue) || (isSmallInt(args[2]) && asSmallInt(args[2]) != 0);
        uint32_t ms = isSmallInt(args[3]) ? (uint32_t)asSmallInt(args[3]) : 0;
        vm->os->set_rgb_led(r, g, b, ms);
      }
      *outResult = rcvr;
      return PRIM_SUCCESS;
    }
    case 99: {  // System class>>classes
      Oop clsList[kMaxClasses];
      uint16_t count = 0;
      for (uint16_t i = 1; i < vm->classCount && i < kMaxClasses; i++) {
        if (isHeapOop(vm->classes[i]) && vm->classes[i] != kOopNil) {
          clsList[count++] = vm->classes[i];
        }
      }
      Oop arr = stNewArray(vm, count);
      for (uint16_t i = 0; i < count; i++) stSlotAtPut(vm, arr, i, clsList[i]);
      *outResult = arr;
      return PRIM_SUCCESS;
    }

    // --- 100..106: Lakatos Unified VFS (`File` class) ---
    case 100: {  // File class>>ls: path
      if (numArgs < 1 || !vm->os || !vm->os->fs_ls) return PRIM_FAIL;
      char path[128];
      stCopyCString(vm, args[0], path, sizeof(path));
      LakatosFsEntry entries[24];
      int n = vm->os->fs_ls(path, entries, 24);
      if (n < 0) n = 0;
      uint16_t saved = vm->tempRootCount;
      Oop arr = stNewArray(vm, (uint16_t)n);
      stPushRoot(vm, arr);
      for (int i = 0; i < n; i++) {
        char item[72];
        if (entries[i].isDir) {
          snprintf(item, sizeof(item), "%s/", entries[i].name);
        } else {
          snprintf(item, sizeof(item), "%s", entries[i].name);
        }
        stSlotAtPut(vm, arr, (uint16_t)i, stNewString(vm, item));
      }
      stPopRoots(vm, saved);
      *outResult = arr;
      return PRIM_SUCCESS;
    }
    case 101: {  // File class>>read: path
      if (numArgs < 1 || !vm->os || !vm->os->fs_read) return PRIM_FAIL;
      char path[128];
      stCopyCString(vm, args[0], path, sizeof(path));
      char buf[1024];
      int32_t got = vm->os->fs_read(path, 0, buf, sizeof(buf) - 1);
      if (got < 0) {
        *outResult = kOopNil;
        return PRIM_SUCCESS;
      }
      buf[got] = '\0';
      *outResult = stNewString(vm, buf, got);
      return PRIM_SUCCESS;
    }
    case 102: {  // File class>>write: path contents: str
      if (numArgs < 2 || !vm->os || !vm->os->fs_write) return PRIM_FAIL;
      char path[128];
      stCopyCString(vm, args[0], path, sizeof(path));
      ObjHeader* ch = stGetHeader(vm, args[1]);
      if (!ch || (ch->format != FMT_STRING && ch->format != FMT_SYMBOL && ch->format != FMT_BYTES)) {
        return PRIM_FAIL;
      }
      bool ok = vm->os->fs_write(path, stByteData(vm, args[1]), ch->size);
      *outResult = makeBool(ok);
      return PRIM_SUCCESS;
    }
    case 103: {  // File class>>exists: path
      if (numArgs < 1 || !vm->os || !vm->os->fs_stat) return PRIM_FAIL;
      char path[128];
      stCopyCString(vm, args[0], path, sizeof(path));
      LakatosFsEntry st;
      bool ok = vm->os->fs_stat(path, &st);
      *outResult = makeBool(ok);
      return PRIM_SUCCESS;
    }
    case 104: {  // File class>>delete: path
      if (numArgs < 1 || !vm->os || !vm->os->fs_rm) return PRIM_FAIL;
      char path[128];
      stCopyCString(vm, args[0], path, sizeof(path));
      bool ok = vm->os->fs_rm(path);
      *outResult = makeBool(ok);
      return PRIM_SUCCESS;
    }
    case 105: {  // File class>>mkdir: path
      if (numArgs < 1 || !vm->os || !vm->os->fs_mkdir) return PRIM_FAIL;
      char path[128];
      stCopyCString(vm, args[0], path, sizeof(path));
      bool ok = vm->os->fs_mkdir(path, true);
      *outResult = makeBool(ok);
      return PRIM_SUCCESS;
    }

    // --- 110..114: Display & Turtle Pen ---
    case 110: {  // Display class>>clear: color
      if (numArgs >= 1 && isSmallInt(args[0]) && vm->os && vm->os->tft_fill_screen) {
        vm->os->tft_fill_screen((uint16_t)asSmallInt(args[0]));
      }
      *outResult = rcvr;
      return PRIM_SUCCESS;
    }
    case 111: {  // Display class>>fillRectX:y:w:h:color:
      if (numArgs >= 5 && vm->os && vm->os->tft_fill_rect) {
        vm->os->tft_fill_rect(asSmallInt(args[0]), asSmallInt(args[1]),
                              asSmallInt(args[2]), asSmallInt(args[3]),
                              (uint16_t)asSmallInt(args[4]));
      }
      *outResult = rcvr;
      return PRIM_SUCCESS;
    }
    case 112: {  // Display class>>drawLineX0:y0:x1:y1:color:
      if (numArgs >= 5 && vm->os && vm->os->tft_draw_line) {
        vm->os->tft_draw_line(asSmallInt(args[0]), asSmallInt(args[1]),
                              asSmallInt(args[2]), asSmallInt(args[3]),
                              (uint16_t)asSmallInt(args[4]));
      }
      *outResult = rcvr;
      return PRIM_SUCCESS;
    }
    case 113: {  // Display class>>drawString:x:y:color:
      if (numArgs >= 4 && vm->os && vm->os->tft_draw_string) {
        char sbuf[96];
        stCopyCString(vm, args[0], sbuf, sizeof(sbuf));
        vm->os->tft_set_text_color((uint16_t)asSmallInt(args[3]), LAKATOS_BLACK);
        vm->os->tft_draw_string(sbuf, asSmallInt(args[1]), asSmallInt(args[2]), 1);
      }
      *outResult = rcvr;
      return PRIM_SUCCESS;
    }
    case 114: {  // Pen>>go: distance
      if (numArgs < 1 || !stIsNumber(vm, args[0])) return PRIM_FAIL;
      float dist  = stToFloat(vm, args[0]);
      float x0    = stToFloat(vm, stSlotAt(vm, rcvr, SLOT_PEN_X));
      float y0    = stToFloat(vm, stSlotAt(vm, rcvr, SLOT_PEN_Y));
      float deg   = stToFloat(vm, stSlotAt(vm, rcvr, SLOT_PEN_ANGLE));
      float rad   = deg * (3.14159265f / 180.0f);
      float x1    = x0 + dist * cosf(rad);
      float y1    = y0 + dist * sinf(rad);
      Oop isDown  = stSlotAt(vm, rcvr, SLOT_PEN_DOWN);
      Oop colOop  = stSlotAt(vm, rcvr, SLOT_PEN_COLOR);
      uint16_t col = isSmallInt(colOop) ? (uint16_t)asSmallInt(colOop) : LAKATOS_CYAN;
      if (col >= 1 && col <= 7) {
        static const uint16_t kPalette[8] = {
          0xFFFF, 0xF800, 0xFD20, 0xFFE0, 0x07E0, 0x07FF, 0x3B9F, 0xF81F
        };
        col = kPalette[col];
      }

      if (isDown == kOopTrue && vm->os && vm->os->tft_draw_line) {
        vm->os->tft_draw_line((int)roundf(x0), (int)roundf(y0),
                              (int)roundf(x1), (int)roundf(y1), col);
      }
      stSlotAtPut(vm, rcvr, SLOT_PEN_X, makeSmallInt((int32_t)roundf(x1)));
      stSlotAtPut(vm, rcvr, SLOT_PEN_Y, makeSmallInt((int32_t)roundf(y1)));
      *outResult = rcvr;
      return PRIM_SUCCESS;
    }

    // --- 120..122: Visual Tool Window Instantiation ---
    case 120: {  // Workspace>>open
      if (vm->onOpenWindow) {
        vm->onOpenWindow(ST_WIN_WORKSPACE, (numArgs >= 1) ? args[0] : kOopNil, vm->uiUser);
      }
      *outResult = rcvr;
      return PRIM_SUCCESS;
    }
    case 121: {  // ClassBrowser>>open / openOn:
      if (vm->onOpenWindow) {
        vm->onOpenWindow(ST_WIN_CLASS_BROWSER, (numArgs >= 1) ? args[0] : kOopNil, vm->uiUser);
      }
      *outResult = rcvr;
      return PRIM_SUCCESS;
    }
    case 122: {  // FileBrowser>>open / openOn:
      if (vm->onOpenWindow) {
        vm->onOpenWindow(ST_WIN_FILE_BROWSER, (numArgs >= 1) ? args[0] : kOopNil, vm->uiUser);
      }
      *outResult = rcvr;
      return PRIM_SUCCESS;
    }

    default:
      return PRIM_FAIL;
  }
}

// ---------------------------------------------------------------------------
// Main Context-Stepping Bytecode Interpreter Loop
// ---------------------------------------------------------------------------
static Oop runActiveContext(VMState* vm, uint32_t maxSteps) {
  uint32_t steps = 0;

  while (vm->activeContext != kOopNil && vm->activeContext != kOopNull) {
    if (++steps > maxSteps) {
      snprintf(vm->lastError, sizeof(vm->lastError), "Execution step limit (%lu) exceeded",
               (unsigned long)maxSteps);
      vm->activeContext = kOopNil;
      return kOopNil;
    }
    if ((steps & 0x3FFu) == 0 && vm->os && vm->os->yield) {
      vm->os->yield();
    }

    Oop ctx        = vm->activeContext;
    Oop meth       = stSlotAt(vm, ctx, SLOT_CTX_METHOD);
    Oop rcvr       = stSlotAt(vm, ctx, SLOT_CTX_RECEIVER);
    Oop closure    = stSlotAt(vm, ctx, SLOT_CTX_CLOSURE);
    Oop homeCtx    = stSlotAt(vm, ctx, SLOT_CTX_HOME);
    Oop litsArr    = stSlotAt(vm, meth, SLOT_METH_LITERALS);
    Oop bcArr      = stSlotAt(vm, meth, SLOT_METH_BYTECODES);
    uint8_t* bc    = stByteData(vm, bcArr);
    uint16_t bcLen = stObjSize(vm, bcArr);
    int32_t pc     = asSmallInt(stSlotAt(vm, ctx, SLOT_CTX_PC));

    if (!bc || pc < 0 || pc >= (int32_t)bcLen) {
      // Implicit end of bytecode
      Oop sender = stSlotAt(vm, ctx, SLOT_CTX_SENDER);
      stSlotAtPut(vm, ctx, SLOT_CTX_HOME, kOopNil);
      vm->activeContext = sender;
      if (sender == kOopNil) return rcvr;
      ctxPush(vm, sender, rcvr);
      continue;
    }

    uint8_t op = bc[pc++];
    stSlotAtPut(vm, ctx, SLOT_CTX_PC, makeSmallInt(pc));

    switch (op) {
      case OP_NOP:
        break;

      case OP_PUSH_SELF:
        ctxPush(vm, ctx, rcvr);
        break;

      case OP_PUSH_NIL:
        ctxPush(vm, ctx, kOopNil);
        break;

      case OP_PUSH_TRUE:
        ctxPush(vm, ctx, kOopTrue);
        break;

      case OP_PUSH_FALSE:
        ctxPush(vm, ctx, kOopFalse);
        break;

      case OP_PUSH_CONTEXT:
        ctxPush(vm, ctx, ctx);
        break;

      case OP_PUSH_SMALLINT_8: {
        int8_t v = (int8_t)bc[pc++];
        stSlotAtPut(vm, ctx, SLOT_CTX_PC, makeSmallInt(pc));
        ctxPush(vm, ctx, makeSmallInt((int32_t)v));
        break;
      }

      case OP_PUSH_SMALLINT_16: {
        uint16_t lo = bc[pc++];
        uint16_t hi = bc[pc++];
        stSlotAtPut(vm, ctx, SLOT_CTX_PC, makeSmallInt(pc));
        int16_t v = (int16_t)(lo | (hi << 8));
        ctxPush(vm, ctx, makeSmallInt((int32_t)v));
        break;
      }

      case OP_PUSH_LITERAL: {
        uint8_t litIdx = bc[pc++];
        stSlotAtPut(vm, ctx, SLOT_CTX_PC, makeSmallInt(pc));
        ctxPush(vm, ctx, stSlotAt(vm, litsArr, litIdx));
        break;
      }

      case OP_PUSH_TEMP: {
        uint8_t tempIdx = bc[pc++];
        stSlotAtPut(vm, ctx, SLOT_CTX_PC, makeSmallInt(pc));
        Oop targetCtx = (closure != kOopNil) ? homeCtx : ctx;
        ctxPush(vm, ctx, stSlotAt(vm, targetCtx, SLOT_CTX_TEMPS_AND_STACK + tempIdx));
        break;
      }

      case OP_STORE_TEMP: {
        uint8_t tempIdx = bc[pc++];
        stSlotAtPut(vm, ctx, SLOT_CTX_PC, makeSmallInt(pc));
        Oop targetCtx = (closure != kOopNil) ? homeCtx : ctx;
        stSlotAtPut(vm, targetCtx, SLOT_CTX_TEMPS_AND_STACK + tempIdx, ctxTop(vm, ctx));
        break;
      }

      case OP_PUSH_INST: {
        uint8_t instIdx = bc[pc++];
        stSlotAtPut(vm, ctx, SLOT_CTX_PC, makeSmallInt(pc));
        ctxPush(vm, ctx, stSlotAt(vm, rcvr, instIdx));
        break;
      }

      case OP_STORE_INST: {
        uint8_t instIdx = bc[pc++];
        stSlotAtPut(vm, ctx, SLOT_CTX_PC, makeSmallInt(pc));
        stSlotAtPut(vm, rcvr, instIdx, ctxTop(vm, ctx));
        break;
      }

      case OP_PUSH_GLOBAL: {
        uint8_t litIdx = bc[pc++];
        stSlotAtPut(vm, ctx, SLOT_CTX_PC, makeSmallInt(pc));
        Oop sym = stSlotAt(vm, litsArr, litIdx);
        Oop val = stDictAt(vm, vm->globalsDict, sym, kOopNil);
        ctxPush(vm, ctx, val);
        break;
      }

      case OP_STORE_GLOBAL: {
        uint8_t litIdx = bc[pc++];
        stSlotAtPut(vm, ctx, SLOT_CTX_PC, makeSmallInt(pc));
        Oop sym = stSlotAt(vm, litsArr, litIdx);
        Oop val = ctxTop(vm, ctx);
        stDictAtPut(vm, vm->globalsDict, sym, val);
        break;
      }

      case OP_POP:
        ctxPop(vm, ctx);
        break;

      case OP_DUP:
        ctxPush(vm, ctx, ctxTop(vm, ctx));
        break;

      case OP_MAKE_ARRAY: {
        uint8_t count = bc[pc++];
        stSlotAtPut(vm, ctx, SLOT_CTX_PC, makeSmallInt(pc));
        Oop items[32];
        for (int i = (int)count - 1; i >= 0; i--) {
          items[i] = ctxPop(vm, ctx);
        }
        uint16_t saved = vm->tempRootCount;
        for (uint8_t i = 0; i < count; i++) stPushRoot(vm, items[i]);
        Oop arr = stNewArray(vm, count);
        for (uint8_t i = 0; i < count; i++) stSlotAtPut(vm, arr, i, items[i]);
        stPopRoots(vm, saved);
        ctxPush(vm, ctx, arr);
        break;
      }

      case OP_JUMP: {
        uint16_t lo = bc[pc++];
        uint16_t hi = bc[pc++];
        int16_t rel = (int16_t)(lo | (hi << 8));
        pc += rel;
        stSlotAtPut(vm, ctx, SLOT_CTX_PC, makeSmallInt(pc));
        break;
      }

      case OP_JUMP_IF_FALSE: {
        uint16_t lo = bc[pc++];
        uint16_t hi = bc[pc++];
        int16_t rel = (int16_t)(lo | (hi << 8));
        Oop cond = ctxPop(vm, ctx);
        if (cond == kOopFalse || cond == kOopNil) {
          pc += rel;
        }
        stSlotAtPut(vm, ctx, SLOT_CTX_PC, makeSmallInt(pc));
        break;
      }

      case OP_JUMP_IF_TRUE: {
        uint16_t lo = bc[pc++];
        uint16_t hi = bc[pc++];
        int16_t rel = (int16_t)(lo | (hi << 8));
        Oop cond = ctxPop(vm, ctx);
        if (cond == kOopTrue) {
          pc += rel;
        }
        stSlotAtPut(vm, ctx, SLOT_CTX_PC, makeSmallInt(pc));
        break;
      }

      case OP_BLOCK_COPY: {
        uint8_t numBlkArgs = bc[pc++];
        uint8_t tempBase   = bc[pc++];
        uint16_t lo        = bc[pc++];
        uint16_t hi        = bc[pc++];
        uint16_t bodyLen   = lo | (hi << 8);
        int32_t startPc    = pc;
        pc += bodyLen;
        stSlotAtPut(vm, ctx, SLOT_CTX_PC, makeSmallInt(pc));

        Oop actualHome = (closure != kOopNil) ? homeCtx : ctx;
        Oop blk = stNewOopSlots(vm, CLS_BLOCK_CLOSURE, SLOT_BLK_COUNT, 0);
        if (blk != kOopNil) {
          stSlotAtPut(vm, blk, SLOT_BLK_HOME_CONTEXT,  actualHome);
          stSlotAtPut(vm, blk, SLOT_BLK_OUTER_CONTEXT, ctx);
          stSlotAtPut(vm, blk, SLOT_BLK_METHOD,        meth);
          stSlotAtPut(vm, blk, SLOT_BLK_START_PC,      makeSmallInt(startPc));
          stSlotAtPut(vm, blk, SLOT_BLK_NUM_ARGS,      makeSmallInt(numBlkArgs));
          stSlotAtPut(vm, blk, SLOT_BLK_TEMP_BASE,     makeSmallInt(tempBase));
        }
        ctxPush(vm, vm->activeContext, blk);
        break;
      }

      case OP_RETURN_SELF: {
        Oop sender = stSlotAt(vm, ctx, SLOT_CTX_SENDER);
        stSlotAtPut(vm, ctx, SLOT_CTX_HOME, kOopNil);
        vm->activeContext = sender;
        if (sender == kOopNil) return rcvr;
        ctxPush(vm, sender, rcvr);
        break;
      }

      case OP_RETURN_TOP: {
        Oop retVal = ctxPop(vm, ctx);
        if (closure == kOopNil) {
          // Normal return from MethodContext
          Oop sender = stSlotAt(vm, ctx, SLOT_CTX_SENDER);
          stSlotAtPut(vm, ctx, SLOT_CTX_HOME, kOopNil);
          vm->activeContext = sender;
          if (sender == kOopNil) return retVal;
          ctxPush(vm, sender, retVal);
        } else {
          // Non-Local Return (`^ expr`) from inside a BlockClosure!
          // Verify `homeCtx` is still active on the sender chain.
          bool homeAlive = isHeapOop(homeCtx) &&
                           (stSlotAt(vm, homeCtx, SLOT_CTX_HOME) == homeCtx);
          bool foundOnChain = false;
          if (homeAlive) {
            Oop walk = ctx;
            int depth = 0;
            while (isHeapOop(walk) && walk != kOopNil && depth++ < 128) {
              if (walk == homeCtx) {
                foundOnChain = true;
                break;
              }
              walk = stSlotAt(vm, walk, SLOT_CTX_SENDER);
            }
          }
          if (!foundOnChain) {
            snprintf(vm->lastError, sizeof(vm->lastError),
                     "BlockCannotReturn: home method context already returned");
            vm->activeContext = kOopNil;
            return kOopNil;
          }
          // Unwind sender chain down to `homeCtx`
          Oop walk = ctx;
          while (walk != homeCtx && isHeapOop(walk) && walk != kOopNil) {
            Oop next = stSlotAt(vm, walk, SLOT_CTX_SENDER);
            stSlotAtPut(vm, walk, SLOT_CTX_HOME, kOopNil);
            walk = next;
          }
          Oop targetSender = stSlotAt(vm, homeCtx, SLOT_CTX_SENDER);
          stSlotAtPut(vm, homeCtx, SLOT_CTX_HOME, kOopNil);
          vm->activeContext = targetSender;
          if (targetSender == kOopNil) return retVal;
          ctxPush(vm, targetSender, retVal);
        }
        break;
      }

      case OP_BLOCK_RETURN: {
        Oop retVal = ctxPop(vm, ctx);
        if (closure != kOopNil) {
          // Restore saved outer block parameters for recursive block safety
          uint8_t numBlkArgs = (uint8_t)asSmallInt(stSlotAt(vm, closure, SLOT_BLK_NUM_ARGS));
          uint8_t tempBase   = (uint8_t)asSmallInt(stSlotAt(vm, closure, SLOT_BLK_TEMP_BASE));
          for (uint8_t i = 0; i < numBlkArgs; i++) {
            Oop prevVal = stSlotAt(vm, ctx, SLOT_CTX_TEMPS_AND_STACK + i);
            stSlotAtPut(vm, homeCtx, SLOT_CTX_TEMPS_AND_STACK + tempBase + i, prevVal);
          }
        }
        Oop sender = stSlotAt(vm, ctx, SLOT_CTX_SENDER);
        vm->activeContext = sender;
        if (sender == kOopNil) return retVal;
        ctxPush(vm, sender, retVal);
        break;
      }

      case OP_SEND:
      case OP_SUPER_SEND: {
        uint8_t selLitIdx = bc[pc++];
        uint8_t numArgs   = bc[pc++];
        stSlotAtPut(vm, ctx, SLOT_CTX_PC, makeSmallInt(pc));
        Oop selector = stSlotAt(vm, litsArr, selLitIdx);

        // Peek receiver and arguments on current context stack
        int32_t sp = asSmallInt(stSlotAt(vm, ctx, SLOT_CTX_SP));
        uint16_t sbase = ctxStackBase(vm, ctx);
        if (sp < (int32_t)(numArgs + 1)) {
          snprintf(vm->lastError, sizeof(vm->lastError), "Stack underflow on SEND");
          vm->activeContext = kOopNil;
          return kOopNil;
        }
        uint16_t rcvrSlot = sbase + (uint16_t)(sp - 1 - numArgs);
        Oop sendRcvr = stSlotAt(vm, ctx, rcvrSlot);
        Oop sendArgs[16];
        for (uint8_t i = 0; i < numArgs && i < 16; i++) {
          sendArgs[i] = stSlotAt(vm, ctx, rcvrSlot + 1 + i);
        }

        // Fast-path inline SmallInteger binary operations
        if (op == OP_SEND && numArgs == 1 && isSmallInt(sendRcvr) && isSmallInt(sendArgs[0])) {
          int32_t a = asSmallInt(sendRcvr);
          int32_t b = asSmallInt(sendArgs[0]);
          Oop fastRes = kOopNull;
          if (selector == vm->symPlus)       fastRes = makeSmallInt(a + b);
          else if (selector == vm->symMinus) fastRes = makeSmallInt(a - b);
          else if (selector == vm->symMul)   fastRes = makeSmallInt(a * b);
          else if (selector == vm->symLT)    fastRes = makeBool(a < b);
          else if (selector == vm->symLE)    fastRes = makeBool(a <= b);
          else if (selector == vm->symGT)    fastRes = makeBool(a > b);
          else if (selector == vm->symGE)    fastRes = makeBool(a >= b);
          else if (selector == vm->symEQ || selector == vm->symIdentEQ) fastRes = makeBool(a == b);
          else if (selector == vm->symNE)    fastRes = makeBool(a != b);

          if (fastRes != kOopNull) {
            ctxPop(vm, ctx);
            ctxPop(vm, ctx);
            ctxPush(vm, ctx, fastRes);
            break;
          }
        }

        Oop startCls = kOopNull;
        if (op == OP_SUPER_SEND) {
          Oop ownerCls = stSlotAt(vm, meth, SLOT_METH_OWNER_CLASS);
          startCls = stSlotAt(vm, ownerCls, SLOT_CLS_SUPERCLASS);
        }

        Oop targetMeth = stLookupMethod(vm, sendRcvr, selector, startCls);
        if (targetMeth == kOopNull || targetMeth == kOopNil) {
          // Try `doesNotUnderstand:` with a 2-element Array `#(selector #(args...))`
          Oop dnuMeth = stLookupMethod(vm, sendRcvr, vm->symDoesNotUnderstand, kOopNull);
          if (dnuMeth != kOopNull && dnuMeth != kOopNil) {
            for (uint8_t i = 0; i <= numArgs; i++) ctxPop(vm, ctx);
            uint16_t saved = vm->tempRootCount;
            stPushRoot(vm, sendRcvr);
            stPushRoot(vm, selector);
            for (uint8_t i = 0; i < numArgs; i++) stPushRoot(vm, sendArgs[i]);
            Oop argArr = stNewArray(vm, numArgs);
            stPushRoot(vm, argArr);
            for (uint8_t i = 0; i < numArgs; i++) stSlotAtPut(vm, argArr, i, sendArgs[i]);
            Oop msgPair = stNewArray(vm, 2);
            stSlotAtPut(vm, msgPair, 0, selector);
            stSlotAtPut(vm, msgPair, 1, argArr);
            stPopRoots(vm, saved);

            Oop dnuArgs[1] = {msgPair};
            vm->activeContext = createMethodContext(vm, vm->activeContext, sendRcvr, dnuMeth, dnuArgs, 1);
            break;
          }

          char rcvrName[48], selName[64];
          stFormatOop(vm, stGetClassOop(vm, sendRcvr), rcvrName, sizeof(rcvrName));
          stCopyCString(vm, selector, selName, sizeof(selName));
          snprintf(vm->lastError, sizeof(vm->lastError),
                   "MessageNotUnderstood: %s>>#%s", rcvrName, selName);
          vm->activeContext = kOopNil;
          return kOopNil;
        }

        int32_t mhdr = asSmallInt(stSlotAt(vm, targetMeth, SLOT_METH_HEADER));
        uint16_t primId = unpackMethodPrimId(mhdr);
        if (primId > 0) {
          Oop primRes = kOopNil;
          PrimOutcome po = executePrimitive(vm, primId, sendRcvr, sendArgs, numArgs, &primRes);
          if (po == PRIM_SUCCESS) {
            for (uint8_t i = 0; i <= numArgs; i++) ctxPop(vm, vm->activeContext);
            ctxPush(vm, vm->activeContext, primRes);
            break;
          }
          if (po == PRIM_ACTIVATE_CTX) {
            break;
          }
          // PRIM_FAIL falls through to execute the Smalltalk fallback method body!
        }

        for (uint8_t i = 0; i <= numArgs; i++) ctxPop(vm, vm->activeContext);
        Oop newCtx = createMethodContext(vm, vm->activeContext, sendRcvr, targetMeth, sendArgs, numArgs);
        vm->activeContext = newCtx;
        break;
      }

      default:
        snprintf(vm->lastError, sizeof(vm->lastError), "Unknown bytecode 0x%02X", (unsigned)op);
        vm->activeContext = kOopNil;
        return kOopNil;
    }
  }
  return kOopNil;
}

void stVmInit(VMState* vm, const LakatosApi* os) {
  if (!stMemInit(vm, os)) return;

  // Cache common selector symbols
  vm->symPlus              = stInternSymbol(vm, "+");
  vm->symMinus             = stInternSymbol(vm, "-");
  vm->symMul               = stInternSymbol(vm, "*");
  vm->symDiv               = stInternSymbol(vm, "/");
  vm->symMod               = stInternSymbol(vm, "\\\\");
  vm->symLT                = stInternSymbol(vm, "<");
  vm->symLE                = stInternSymbol(vm, "<=");
  vm->symGT                = stInternSymbol(vm, ">");
  vm->symGE                = stInternSymbol(vm, ">=");
  vm->symEQ                = stInternSymbol(vm, "=");
  vm->symNE                = stInternSymbol(vm, "~=");
  vm->symIdentEQ           = stInternSymbol(vm, "==");
  vm->symAt                = stInternSymbol(vm, "at:");
  vm->symAtPut             = stInternSymbol(vm, "at:put:");
  vm->symSize              = stInternSymbol(vm, "size");
  vm->symValue             = stInternSymbol(vm, "value");
  vm->symValueArg          = stInternSymbol(vm, "value:");
  vm->symValueArg2         = stInternSymbol(vm, "value:value:");
  vm->symDoesNotUnderstand = stInternSymbol(vm, "doesNotUnderstand:");
  vm->symPrintString       = stInternSymbol(vm, "printString");

  // Create global `Smalltalk` dictionary
  vm->globalsDict = stNewDictionary(vm, 64);
  stDictAtPut(vm, vm->globalsDict, stInternSymbol(vm, "Smalltalk"), vm->globalsDict);

  // Install standard classes and bootstrap Smalltalk-80 library
  stBootstrapInstall(vm);
}

Oop stEval(VMState* vm, const char* exprSrc, Oop receiver, uint32_t maxSteps) {
  if (!vm || !exprSrc) return kOopNil;
  Oop rcvrCls = stGetClassOop(vm, receiver);
  Oop meth = stCompileExpression(vm, exprSrc, rcvrCls);
  if (meth == kOopNil) {
    return kOopNil;
  }
  vm->activeContext = createMethodContext(vm, kOopNil, receiver, meth, nullptr, 0);
  return runActiveContext(vm, maxSteps);
}
