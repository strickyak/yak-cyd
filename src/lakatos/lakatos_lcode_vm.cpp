#pragma GCC optimize("O3")

#include "lakatos_state.h"

#include <esp_heap_caps.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// LCode 32-Bit Bytecode Opcodes & Syscall IDs (matching pkg/lakat)
// ---------------------------------------------------------------------------
namespace {

enum LCodeOp : uint8_t {
  // Group 1: Stack & Literals
  OpNOP            = 0x00,
  OpPUSH_NIL       = 0x01,
  OpPUSH_NIL_SLICE = 0x02,
  OpPUSH_TRUE      = 0x03,
  OpPUSH_FALSE     = 0x04,
  OpPUSH_0         = 0x05,
  OpPUSH_1         = 0x06,
  OpPUSH_NEG1      = 0x07,
  OpPUSH_I8        = 0x08,
  OpPUSH_U8        = 0x09,
  OpPUSH_I16       = 0x0A,
  OpPUSH_STR       = 0x0B,
  OpPOP            = 0x0C,
  OpPOP_SLICE      = 0x0D,
  OpDUP            = 0x0E,
  OpSWAP           = 0x0F,
  OpDUP2           = 0x10,
  OpROT            = 0x11,
  OpPUSH_U16       = 0x12,
  OpPUSH_I32       = 0x13,
  OpPUSH_CSTR      = 0x14,
  OpOVER           = 0x15,

  // Group 2: Variables, Pointers & Memory
  OpLOAD_LOCAL_0   = 0x20,
  OpLOAD_LOCAL_1   = 0x21,
  OpLOAD_LOCAL_2   = 0x22,
  OpLOAD_LOCAL_3   = 0x23,
  OpSTORE_LOCAL_0  = 0x24,
  OpSTORE_LOCAL_1  = 0x25,
  OpSTORE_LOCAL_2  = 0x26,
  OpSTORE_LOCAL_3  = 0x27,
  OpLOAD_LOCAL     = 0x28,
  OpSTORE_LOCAL    = 0x29,
  OpLOAD_GLOBAL    = 0x2A,
  OpSTORE_GLOBAL   = 0x2B,
  OpADDR_OF_GLOBAL = 0x30,
  OpPEEK2          = 0x31,
  OpPOKE2          = 0x32,
  OpPEEK1          = 0x33,
  OpPOKE1          = 0x34,
  OpADDR_OF_LOCAL  = 0x35,
  OpSHL1_ADD       = 0x36,
  OpMEM_COPY       = 0x37,
  OpMEM_SET        = 0x38,
  OpPEEK4          = 0x39,
  OpPOKE4          = 0x3A,
  OpPEEKS1         = 0x3B,
  OpPEEKS2         = 0x3C,
  OpSHL2_ADD       = 0x3D,
  OpINDEX_ADD      = 0x3E,
  OpMEM_CMP        = 0x3F,

  // Group 3: 32-Bit ALU
  OpADD     = 0x40,
  OpSUB     = 0x41,
  OpMUL     = 0x42,
  OpDIV     = 0x43,
  OpMOD     = 0x44,
  OpNEG     = 0x45,
  OpBIT_AND = 0x46,
  OpBIT_OR  = 0x47,
  OpBIT_XOR = 0x48,
  OpBIT_NOT = 0x49,
  OpSHL     = 0x4A,
  OpSHR     = 0x4B,
  OpCMP_EQ  = 0x4C,
  OpCMP_NE  = 0x4D,
  OpCMP_LT  = 0x4E,
  OpCMP_LE  = 0x4F,
  OpCMP_GT  = 0x50,
  OpCMP_GE  = 0x51,
  OpNOT     = 0x52,
  OpUDIV    = 0x53,
  OpUMOD    = 0x54,
  OpUSHR    = 0x55,
  OpUCMP_LT = 0x56,
  OpUCMP_LE = 0x57,
  OpUCMP_GT = 0x58,
  OpUCMP_GE = 0x59,

  // Group 4: Control Flow & Calls
  OpJUMP          = 0x60,
  OpJUMP_IF_TRUE  = 0x61,
  OpJUMP_IF_FALSE = 0x62,
  OpCALL          = 0x63,
  OpRET           = 0x64,
  OpRET_SLICE     = 0x65,
  OpRET_VOID      = 0x66,
  OpHALT          = 0x67,
  OpPANIC         = 0x68,

  // Group 5: Slices & Strings
  OpSLICE_NEW  = 0x70,
  OpSLICE_LEN  = 0x71,
  OpSLICE_CAP  = 0x72,
  OpSLICE_SUB  = 0x73,
  OpSLICE_DATA = 0x74,
  OpSTR_CMP    = 0x77,

  // Group 6: Managed Heap
  OpMALLOC    = 0x80,
  OpZALLOC    = 0x81,
  OpREALLOC   = 0x82,
  OpFREE      = 0x83,
  OpHEAP_FREE = 0x84,

  // Group 8: IEEE-754 32-Bit Floating Point (Hardware FPU)
  OpFADD       = 0x90,
  OpFSUB       = 0x91,
  OpFMUL       = 0x92,
  OpFDIV       = 0x93,
  OpFNEG       = 0x94,
  OpFABS       = 0x95,
  OpFSQRT      = 0x96,
  OpFSIN       = 0x97,
  OpFCOS       = 0x98,
  OpFATAN2     = 0x99,
  OpFFLOOR     = 0x9A,
  OpFCEIL      = 0x9B,
  OpFCMP_EQ    = 0x9C,
  OpFCMP_NE    = 0x9D,
  OpFCMP_LT    = 0x9E,
  OpFCMP_LE    = 0x9F,
  OpFCMP_GT    = 0xA0,
  OpFCMP_GE    = 0xA1,
  OpI32_TO_F32 = 0xA2,
  OpF32_TO_I32 = 0xA3,

  // Group 7: Lakatos OS Syscalls
  OpSYSCALL  = 0xC0,
  OpSYS_EXIT = 0xC8,
  OpIO_PRINT = 0xC9,
};

enum LCodeSyscall : uint8_t {
  SysExit            = 0x00,
  SysYield           = 0x01,
  SysMillis          = 0x02,
  SysMicros          = 0x03,
  SysDelayMs         = 0x04,
  SysRand            = 0x05,

  SysGetAirport      = 0x08,
  SysGetAirportInfo  = 0x09,
  SysGetDeviceName   = 0x0A,
  SysGetSerialNumber = 0x0B,
  SysGetShUrl        = 0x0C,
  SysGetUnixTime     = 0x0D,
  SysFormatLocalTime = 0x0E,
  SysCityHash32      = 0x0F,

  SysTftFillScreen    = 0x10,
  SysTftFillRect      = 0x11,
  SysTftDrawRect      = 0x12,
  SysTftFillRoundRect = 0x13,
  SysTftDrawRoundRect = 0x14,
  SysTftDrawLine      = 0x15,
  SysTftDrawFastHLine = 0x16,
  SysTftDrawFastVLine = 0x17,
  SysTftFillCircle    = 0x18,
  SysTftDrawCircle    = 0x19,
  SysTftFillTriangle  = 0x1A,
  SysTftDrawChar      = 0x1B,
  SysTftSetTextColor  = 0x1C,
  SysTftSetTextDatum  = 0x1D,
  SysTftDrawString    = 0x1E,
  SysTftDrawInt       = 0x1F,
  SysTftPushRGB565    = 0x20,
  SysTftDrawCStr      = 0x21,
  SysFmtIntToStr      = 0x22,
  SysFmtFloatToStr    = 0x23,
  SysTftDrawFloat     = 0x24,
  SysTftDrawPngFile   = 0x25,
  SysTftDrawPngMem    = 0x26,
  SysPngInfo          = 0x27,

  SysReadTouch      = 0x28,
  SysReadTouchRaw   = 0x29,
  SysPromptKeyboard = 0x2A,
  SysSetRgbLed      = 0x2B,
  SysReadLdr        = 0x2C,
  SysReadCpuTemp    = 0x2D,
  SysReadClick      = 0x2E,

  SysFsLs          = 0x30,
  SysFsStat        = 0x31,
  SysFsRead        = 0x32,
  SysFsWrite       = 0x33,
  SysFsWriteAt     = 0x34,
  SysFsMkdir       = 0x35,
  SysFsRmdir       = 0x36,
  SysFsRm          = 0x37,
  SysFsDu          = 0x38,
  SysWifiConnected = 0x39,
  SysHttpGet       = 0x3A,
  SysSerialAvail   = 0x3B,
  SysSerialRead    = 0x3C,
  SysSerialWrite   = 0x3D,
  SysHttpGetCached = 0x3E,

  SysTftDrawJpgFile = 0x40,
  SysTftDrawJpgMem  = 0x41,
  SysJpgInfo        = 0x42,

  SysPrefetchStart  = 0x43,
  SysPrefetchStatus = 0x44,
  SysPrefetchCancel = 0x45,
};

static constexpr uint32_t kNullGuardSize   = 256;
static constexpr uint32_t kStringPoolBase  = 0x0100;
static constexpr uint32_t kOsScratchSize   = 512;
static constexpr uint32_t kHeapBlockHdrLen = 8;
static constexpr int      kMaxEvalStack    = 96;
static constexpr int      kMaxCallDepth    = 32;
static constexpr int      kMaxFuncs        = 96;
static constexpr int      kMaxGlobals      = 96;
static constexpr int      kMaxVarPool      = 384;

static inline __attribute__((always_inline)) uint16_t readU16LE(const uint8_t* p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline __attribute__((always_inline)) uint32_t readU32LE(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline __attribute__((always_inline)) uint32_t readAlignedU32(const uint8_t* p) {
  return *(const uint32_t*)p;
}

static inline __attribute__((always_inline)) void writeU16LE(uint8_t* p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static inline __attribute__((always_inline)) void writeU32LE(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
  p[2] = (uint8_t)((v >> 16) & 0xFF);
  p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static inline __attribute__((always_inline)) void writeAlignedU32(uint8_t* p, uint32_t v) {
  *(uint32_t*)p = v;
}

static inline uint32_t align4(uint32_t n) { return (n + 3u) & ~3u; }
static inline uint32_t align8(uint32_t n) { return (n + 7u) & ~7u; }

static inline float u32ToF32(uint32_t u) {
  float f;
  memcpy(&f, &u, sizeof(float));
  return f;
}

static inline uint32_t f32ToU32(float f) {
  uint32_t u;
  memcpy(&u, &f, sizeof(uint32_t));
  return u;
}

struct LCodeFuncMeta {
  uint32_t argCount;
  uint32_t localCount;
  uint32_t frameSize;
  uint32_t codeOffset;
  uint32_t codeSize;
  uint32_t varSizeStart; // Index into varSizesPool
};

struct LCodeCallFrame {
  uint32_t funcIdx;
  int32_t  returnPC;
  uint32_t frameAddr;
};

struct LCodeVM {
  uint8_t*       mem = nullptr;
  uint32_t       memSize = 0;
  uint8_t*       codeBuf = nullptr;
  const uint8_t* code = nullptr;
  uint32_t       codeSize = 0;

  uint32_t       stack[kMaxEvalStack];
  int32_t        sp = 0;

  LCodeCallFrame callStack[kMaxCallDepth];
  int32_t        callDepth = 0;

  LCodeFuncMeta  funcs[kMaxFuncs];
  int32_t        funcCount = 0;
  int32_t        entryFuncIdx = 0;

  uint32_t       varSizesPool[kMaxVarPool];
  uint32_t       localOffsetsPool[kMaxVarPool];
  int32_t        varPoolUsed = 0;

  uint32_t       globalSizes[kMaxGlobals];
  uint32_t       globalOffsets[kMaxGlobals];
  int32_t        globalCount = 0;

  uint32_t       stringPoolEnd = 0;
  uint32_t       osScratchBase = 0;
  uint32_t       osScratchPtr  = 0;
  uint32_t       globalsBase   = 0;
  uint32_t       frameBase     = 0;
  uint32_t       frameSP       = 0;
  uint32_t       heapBase      = 0;

  int32_t        pc = 0;
  uint32_t       running = 0;
  int32_t        exitCode = 0;
  const char*    errReason = nullptr;
  int32_t        errPC = 0;

  void setErr(const char* msg) {
    errReason = msg;
    errPC = pc;
    running = 0;
  }

  inline __attribute__((always_inline)) bool push(uint32_t v) {
    if (__builtin_expect(sp >= kMaxEvalStack, 0)) {
      setErr("Eval stack overflow");
      return false;
    }
    stack[sp++] = v;
    return true;
  }

  inline __attribute__((always_inline)) bool pop(uint32_t& out) {
    if (__builtin_expect(sp <= 0, 0)) {
      setErr("Eval stack underflow");
      return false;
    }
    out = stack[--sp];
    return true;
  }

  inline __attribute__((always_inline)) bool checkAddr(uint32_t addr, uint32_t sz) {
    if (__builtin_expect(addr < kNullGuardSize || addr + sz > memSize || addr + sz < addr, 0)) {
      setErr("Bad mem access");
      return false;
    }
    return true;
  }

  void initHeap() {
    if (heapBase + kHeapBlockHdrLen >= memSize) return;
    uint32_t avail = memSize - heapBase - kHeapBlockHdrLen;
    writeU32LE(mem + heapBase, avail);
    writeU32LE(mem + heapBase + 4, 0);
  }

  uint32_t heapMalloc(uint32_t reqSize) {
    if (reqSize == 0) reqSize = 8;
    uint32_t need = align8(reqSize);
    uint32_t cur = heapBase;
    while (cur + kHeapBlockHdrLen <= memSize) {
      uint32_t blkSz = readU32LE(mem + cur);
      uint32_t used  = readU32LE(mem + cur + 4);
      if (blkSz == 0) break;
      if (used == 0 && blkSz >= need) {
        if (blkSz >= need + kHeapBlockHdrLen + 8) {
          uint32_t nextHdr = cur + kHeapBlockHdrLen + need;
          uint32_t remSz   = blkSz - need - kHeapBlockHdrLen;
          writeU32LE(mem + nextHdr, remSz);
          writeU32LE(mem + nextHdr + 4, 0);
          writeU32LE(mem + cur, need);
        }
        writeU32LE(mem + cur + 4, 1);
        return cur + kHeapBlockHdrLen;
      }
      cur += kHeapBlockHdrLen + blkSz;
    }
    setErr("Out of LCode heap");
    return 0;
  }

  bool heapFree(uint32_t addr) {
    if (addr == 0) return true;
    if (addr < heapBase + kHeapBlockHdrLen || addr >= memSize) {
      setErr("Invalid free() pointer");
      return false;
    }
    uint32_t hdr = addr - kHeapBlockHdrLen;
    if (readU32LE(mem + hdr + 4) == 0) {
      setErr("Double free()");
      return false;
    }
    writeU32LE(mem + hdr + 4, 0);

    // Coalesce adjacent free blocks
    uint32_t cur = heapBase;
    while (cur + kHeapBlockHdrLen <= memSize) {
      uint32_t sz   = readU32LE(mem + cur);
      uint32_t used = readU32LE(mem + cur + 4);
      if (sz == 0) break;
      uint32_t nxt = cur + kHeapBlockHdrLen + sz;
      if (used == 0 && nxt + kHeapBlockHdrLen <= memSize) {
        uint32_t nxtSz   = readU32LE(mem + nxt);
        uint32_t nxtUsed = readU32LE(mem + nxt + 4);
        if (nxtSz > 0 && nxtUsed == 0) {
          writeU32LE(mem + cur, sz + kHeapBlockHdrLen + nxtSz);
          continue;
        }
      }
      cur = nxt;
    }
    return true;
  }

  uint32_t heapBytesFree() {
    uint32_t total = 0;
    uint32_t cur = heapBase;
    while (cur + kHeapBlockHdrLen <= memSize) {
      uint32_t sz   = readU32LE(mem + cur);
      uint32_t used = readU32LE(mem + cur + 4);
      if (sz == 0) break;
      if (used == 0) total += sz;
      cur += kHeapBlockHdrLen + sz;
    }
    return total;
  }

  void pushScratchString(const char* s) {
    if (!s) s = "";
    uint32_t len = (uint32_t)strlen(s);
    if (len > 240) len = 240;
    if (osScratchPtr + len + 1 > kOsScratchSize) {
      osScratchPtr = 0;
    }
    uint32_t dst = osScratchBase + osScratchPtr;
    memcpy(mem + dst, s, len);
    mem[dst + len] = 0;
    osScratchPtr = align4(osScratchPtr + len + 1);
    push(dst);
    push(len);
    push(len);
  }

  bool popSliceAsCStr(char* outBuf, size_t maxOut) {
    uint32_t ln = 0, cp = 0, bs = 0;
    if (!pop(ln) || !pop(cp) || !pop(bs)) return false;
    (void)cp;
    if (ln == 0) {
      outBuf[0] = '\0';
      return true;
    }
    if (!checkAddr(bs, ln)) return false;
    size_t copyLen = (ln < maxOut - 1) ? ln : (maxOut - 1);
    memcpy(outBuf, mem + bs, copyLen);
    outBuf[copyLen] = '\0';
    return true;
  }

  inline __attribute__((always_inline)) bool loadLocalSlot(uint32_t slot) {
    const LCodeCallFrame& cf = callStack[callDepth - 1];
    const LCodeFuncMeta& fn  = funcs[cf.funcIdx];
    if (__builtin_expect(slot >= fn.localCount, 0)) {
      setErr("Local slot out of range");
      return false;
    }
    uint32_t poolIdx = fn.varSizeStart + slot;
    uint32_t addr    = cf.frameAddr + localOffsetsPool[poolIdx];
    uint32_t sz      = varSizesPool[poolIdx];
    if (__builtin_expect(sz == 12, 0)) {
      return push(readAlignedU32(mem + addr)) &&
             push(readAlignedU32(mem + addr + 4)) &&
             push(readAlignedU32(mem + addr + 8));
    }
    return push(readAlignedU32(mem + addr));
  }

  inline __attribute__((always_inline)) bool storeLocalSlot(uint32_t slot) {
    const LCodeCallFrame& cf = callStack[callDepth - 1];
    const LCodeFuncMeta& fn  = funcs[cf.funcIdx];
    if (__builtin_expect(slot >= fn.localCount, 0)) {
      setErr("Local slot out of range");
      return false;
    }
    uint32_t poolIdx = fn.varSizeStart + slot;
    uint32_t addr    = cf.frameAddr + localOffsetsPool[poolIdx];
    uint32_t sz      = varSizesPool[poolIdx];
    if (__builtin_expect(sz == 12, 0)) {
      uint32_t ln = 0, cp = 0, bs = 0;
      if (!pop(ln) || !pop(cp) || !pop(bs)) return false;
      writeAlignedU32(mem + addr, bs);
      writeAlignedU32(mem + addr + 4, cp);
      writeAlignedU32(mem + addr + 8, ln);
      return true;
    }
    uint32_t v = 0;
    if (!pop(v)) return false;
    writeAlignedU32(mem + addr, v);
    return true;
  }

  bool execSyscall(uint8_t sysId) {
    switch (sysId) {
      case SysExit: {
        uint32_t code = 0;
        if (!pop(code)) return false;
        exitCode = (int)(int32_t)code;
        running = false;
        return true;
      }
      case SysYield: {
        updateLakatosConfigBackground();
        lakatosUpdateRgbLed();
        yield();
        return true;
      }
      case SysMillis:
        return push(millis());
      case SysMicros:
        return push(micros());
      case SysDelayMs: {
        uint32_t ms = 0;
        if (!pop(ms)) return false;
        uint32_t start = millis();
        while ((uint32_t)(millis() - start) < ms) {
          updateLakatosConfigBackground();
          lakatosUpdateRgbLed();
          uint32_t rem = ms - (uint32_t)(millis() - start);
          delay(rem > 5 ? 5 : rem);
        }
        return true;
      }
      case SysRand: {
        uint32_t mx = 0;
        if (!pop(mx)) return false;
        return push(mx <= 1 ? 0 : (uint32_t)random(mx));
      }

      // Identity, Config & Time
      case SysGetAirport:
        pushScratchString(lakatosGetAirportCode());
        return running;
      case SysGetAirportInfo: {
        uint32_t outAddr = 0;
        if (!pop(outAddr)) return false;
        if (!checkAddr(outAddr, sizeof(LakatosAirportInfo))) return false;
        LakatosAirportInfo info;
        bool ok = lakatosGetAirportInfo(&info);
        memcpy(mem + outAddr, &info, sizeof(info));
        return push(ok ? 1 : 0);
      }
      case SysGetDeviceName:
        pushScratchString(lakatosGetDeviceName());
        return running;
      case SysGetSerialNumber:
        pushScratchString(lakatosGetSerialNumber());
        return running;
      case SysGetShUrl:
        pushScratchString(lakatosGetSuperhighwayUrl());
        return running;
      case SysGetUnixTime:
        return push((uint32_t)lakatosGetUnixTime());
      case SysFormatLocalTime: {
        uint32_t maxLen = 0, bufAddr = 0;
        if (!pop(maxLen) || !pop(bufAddr)) return false;
        char fmtStr[64];
        if (!popSliceAsCStr(fmtStr, sizeof(fmtStr))) return false;
        if (maxLen == 0 || !checkAddr(bufAddr, maxLen)) return false;
        bool ok = lakatosFormatLocalTime((char*)(mem + bufAddr), maxLen, fmtStr);
        return push(ok ? 1 : 0);
      }
      case SysCityHash32: {
        uint32_t ln = 0, bufAddr = 0;
        if (!pop(ln) || !pop(bufAddr)) return false;
        if (ln > 0 && !checkAddr(bufAddr, ln)) return false;
        uint32_t h = lakatosCityHash32(ln > 0 ? (mem + bufAddr) : nullptr, ln);
        return push(h);
      }

      // TFT Display
      case SysTftFillScreen: {
        uint32_t c = 0;
        if (!pop(c)) return false;
        LakatosTftGuard tftGuard;
        tft.fillScreen((uint16_t)c);
        return true;
      }
      case SysTftFillRect: {
        uint32_t c = 0, h = 0, w = 0, y = 0, x = 0;
        if (!pop(c) || !pop(h) || !pop(w) || !pop(y) || !pop(x)) return false;
        LakatosTftGuard tftGuard;
        tft.fillRect((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h, (uint16_t)c);
        return true;
      }
      case SysTftDrawRect: {
        uint32_t c = 0, h = 0, w = 0, y = 0, x = 0;
        if (!pop(c) || !pop(h) || !pop(w) || !pop(y) || !pop(x)) return false;
        LakatosTftGuard tftGuard;
        tft.drawRect((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h, (uint16_t)c);
        return true;
      }
      case SysTftFillRoundRect: {
        uint32_t c = 0, r = 0, h = 0, w = 0, y = 0, x = 0;
        if (!pop(c) || !pop(r) || !pop(h) || !pop(w) || !pop(y) || !pop(x)) return false;
        LakatosTftGuard tftGuard;
        tft.fillRoundRect((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h, (int32_t)r, (uint16_t)c);
        return true;
      }
      case SysTftDrawRoundRect: {
        uint32_t c = 0, r = 0, h = 0, w = 0, y = 0, x = 0;
        if (!pop(c) || !pop(r) || !pop(h) || !pop(w) || !pop(y) || !pop(x)) return false;
        LakatosTftGuard tftGuard;
        tft.drawRoundRect((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h, (int32_t)r, (uint16_t)c);
        return true;
      }
      case SysTftDrawLine: {
        uint32_t c = 0, y1 = 0, x1 = 0, y0 = 0, x0 = 0;
        if (!pop(c) || !pop(y1) || !pop(x1) || !pop(y0) || !pop(x0)) return false;
        LakatosTftGuard tftGuard;
        tft.drawLine((int32_t)x0, (int32_t)y0, (int32_t)x1, (int32_t)y1, (uint16_t)c);
        return true;
      }
      case SysTftDrawFastHLine: {
        uint32_t c = 0, w = 0, y = 0, x = 0;
        if (!pop(c) || !pop(w) || !pop(y) || !pop(x)) return false;
        LakatosTftGuard tftGuard;
        tft.drawFastHLine((int32_t)x, (int32_t)y, (int32_t)w, (uint16_t)c);
        return true;
      }
      case SysTftDrawFastVLine: {
        uint32_t c = 0, h = 0, y = 0, x = 0;
        if (!pop(c) || !pop(h) || !pop(y) || !pop(x)) return false;
        LakatosTftGuard tftGuard;
        tft.drawFastVLine((int32_t)x, (int32_t)y, (int32_t)h, (uint16_t)c);
        return true;
      }
      case SysTftFillCircle: {
        uint32_t c = 0, r = 0, y = 0, x = 0;
        if (!pop(c) || !pop(r) || !pop(y) || !pop(x)) return false;
        LakatosTftGuard tftGuard;
        tft.fillCircle((int32_t)x, (int32_t)y, (int32_t)r, (uint16_t)c);
        return true;
      }
      case SysTftDrawCircle: {
        uint32_t c = 0, r = 0, y = 0, x = 0;
        if (!pop(c) || !pop(r) || !pop(y) || !pop(x)) return false;
        LakatosTftGuard tftGuard;
        tft.drawCircle((int32_t)x, (int32_t)y, (int32_t)r, (uint16_t)c);
        return true;
      }
      case SysTftFillTriangle: {
        uint32_t c = 0, y2 = 0, x2 = 0, y1 = 0, x1 = 0, y0 = 0, x0 = 0;
        if (!pop(c) || !pop(y2) || !pop(x2) || !pop(y1) || !pop(x1) || !pop(y0) || !pop(x0))
          return false;
        LakatosTftGuard tftGuard;
        tft.fillTriangle((int32_t)x0, (int32_t)y0, (int32_t)x1, (int32_t)y1,
                         (int32_t)x2, (int32_t)y2, (uint16_t)c);
        return true;
      }
      case SysTftDrawChar: {
        uint32_t sz = 0, bg = 0, fg = 0, ch = 0, y = 0, x = 0;
        if (!pop(sz) || !pop(bg) || !pop(fg) || !pop(ch) || !pop(y) || !pop(x)) return false;
        LakatosTftGuard tftGuard;
        tft.drawChar((int32_t)x, (int32_t)y, (char)ch, (uint16_t)fg, (uint16_t)bg, (uint8_t)sz);
        return true;
      }
      case SysTftSetTextColor: {
        uint32_t bg = 0, fg = 0;
        if (!pop(bg) || !pop(fg)) return false;
        LakatosTftGuard tftGuard;
        tft.setTextColor((uint16_t)fg, (uint16_t)bg);
        return true;
      }
      case SysTftSetTextDatum: {
        uint32_t d = 0;
        if (!pop(d)) return false;
        LakatosTftGuard tftGuard;
        tft.setTextDatum((uint8_t)d);
        return true;
      }
      case SysTftDrawString: {
        uint32_t font = 0, y = 0, x = 0;
        if (!pop(font) || !pop(y) || !pop(x)) return false;
        char strBuf[128];
        if (!popSliceAsCStr(strBuf, sizeof(strBuf))) return false;
        int w;
        {
          LakatosTftGuard tftGuard;
          w = tft.drawString(strBuf, (int32_t)x, (int32_t)y, (uint8_t)font);
        }
        return push((uint32_t)w);
      }
      case SysTftDrawCStr: {
        uint32_t font = 0, y = 0, x = 0, cstrAddr = 0;
        if (!pop(font) || !pop(y) || !pop(x) || !pop(cstrAddr)) return false;
        if (!checkAddr(cstrAddr, 1)) return false;
        int w;
        {
          LakatosTftGuard tftGuard;
          w = tft.drawString((const char*)(mem + cstrAddr), (int32_t)x, (int32_t)y, (uint8_t)font);
        }
        return push((uint32_t)w);
      }
      case SysTftDrawInt: {
        uint32_t font = 0, y = 0, x = 0, val = 0;
        if (!pop(font) || !pop(y) || !pop(x) || !pop(val)) return false;
        char numBuf[24];
        snprintf(numBuf, sizeof(numBuf), "%ld", (long)(int32_t)val);
        int w;
        {
          LakatosTftGuard tftGuard;
          w = tft.drawString(numBuf, (int32_t)x, (int32_t)y, (uint8_t)font);
        }
        return push((uint32_t)w);
      }
      case SysFmtIntToStr: {
        uint32_t maxLen = 0, bufAddr = 0, val = 0;
        if (!pop(maxLen) || !pop(bufAddr) || !pop(val)) return false;
        bool isUnsigned = (maxLen & 0x80000000u) != 0;
        maxLen &= 0x7FFFFFFFu;
        char numBuf[24];
        if (isUnsigned) {
          snprintf(numBuf, sizeof(numBuf), "%lu", (unsigned long)val);
        } else {
          snprintf(numBuf, sizeof(numBuf), "%ld", (long)(int32_t)val);
        }
        uint32_t sLen = (uint32_t)strlen(numBuf);
        if (bufAddr != 0 && maxLen > 0) {
          if (!checkAddr(bufAddr, maxLen)) return false;
          if (sLen >= maxLen) sLen = maxLen - 1;
          memcpy(mem + bufAddr, numBuf, sLen);
          mem[bufAddr + sLen] = 0;
          return push(bufAddr) && push(maxLen) && push(sLen);
        }
        pushScratchString(numBuf);
        return running;
      }
      case SysFmtFloatToStr: {
        uint32_t decimals = 0, valBits = 0;
        if (!pop(decimals) || !pop(valBits)) return false;
        float f = u32ToF32(valBits);
        char numBuf[32];
        int32_t dec = (int32_t)decimals;
        if (dec < 0) {
          snprintf(numBuf, sizeof(numBuf), "%g", (double)f);
        } else {
          if (dec > 9) dec = 9;
          snprintf(numBuf, sizeof(numBuf), "%.*f", (int)dec, (double)f);
        }
        pushScratchString(numBuf);
        return running;
      }
      case SysTftDrawFloat: {
        uint32_t font = 0, y = 0, x = 0, decimals = 0, valBits = 0;
        if (!pop(font) || !pop(y) || !pop(x) || !pop(decimals) || !pop(valBits)) return false;
        float f = u32ToF32(valBits);
        int32_t dec = (int32_t)decimals;
        if (dec < 0) dec = 2;
        if (dec > 9) dec = 9;
        char numBuf[32];
        snprintf(numBuf, sizeof(numBuf), "%.*f", (int)dec, (double)f);
        int w;
        {
          LakatosTftGuard tftGuard;
          w = tft.drawString(numBuf, (int32_t)x, (int32_t)y, (uint8_t)font);
        }
        return push((uint32_t)w);
      }
      case SysTftPushRGB565: {
        uint32_t pixAddr = 0, h = 0, w = 0, y = 0, x = 0;
        if (!pop(pixAddr) || !pop(h) || !pop(w) || !pop(y) || !pop(x)) return false;
        uint32_t totalBytes = w * h * 2;
        if (!checkAddr(pixAddr, totalBytes)) return false;
        LakatosTftGuard tftGuard;
        tft.pushImage((int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h,
                      (uint16_t*)(mem + pixAddr));
        return true;
      }
      case SysTftDrawPngFile: {
        uint32_t y = 0, x = 0;
        if (!pop(y) || !pop(x)) return false;
        char pathBuf[128];
        if (!popSliceAsCStr(pathBuf, sizeof(pathBuf))) return false;
        bool ok = lakatosDrawPngFile(pathBuf, (int)(int32_t)x, (int)(int32_t)y);
        return push(ok ? 1 : 0);
      }
      case SysTftDrawPngMem: {
        uint32_t y = 0, x = 0, len = 0, bufAddr = 0;
        if (!pop(y) || !pop(x) || !pop(len) || !pop(bufAddr)) return false;
        if (len == 0) return push(0);
        const uint8_t* srcPtr = nullptr;
        if (bufAddr == 0) {
          uint32_t avail = 0;
          srcPtr = lakatosPrefetchBuffer(&avail);
          if (!srcPtr || len > avail) return push(0);
        } else {
          if (!checkAddr(bufAddr, len)) return push(0);
          srcPtr = mem + bufAddr;
        }
        bool ok = lakatosDrawPngMem(srcPtr, len, (int)(int32_t)x, (int)(int32_t)y);
        return push(ok ? 1 : 0);
      }
      case SysPngInfo: {
        char pathBuf[128];
        if (!popSliceAsCStr(pathBuf, sizeof(pathBuf))) return false;
        int w = 0, h = 0;
        bool ok = lakatosPngInfo(pathBuf, &w, &h);
        return push(ok ? 1 : 0) &&
               push((uint32_t)(int32_t)w) &&
               push((uint32_t)(int32_t)h);
      }
      case SysTftDrawJpgFile: {
        uint32_t scale = 0, y = 0, x = 0;
        if (!pop(scale) || !pop(y) || !pop(x)) return false;
        char pathBuf[128];
        if (!popSliceAsCStr(pathBuf, sizeof(pathBuf))) return false;
        bool ok = lakatosDrawJpgFile(pathBuf, (int)(int32_t)x, (int)(int32_t)y,
                                     (int)(int32_t)scale);
        return push(ok ? 1 : 0);
      }
      case SysTftDrawJpgMem: {
        uint32_t scale = 0, y = 0, x = 0, len = 0, bufAddr = 0;
        if (!pop(scale) || !pop(y) || !pop(x) || !pop(len) || !pop(bufAddr)) return false;
        if (len == 0) return push(0);
        const uint8_t* srcPtr = nullptr;
        if (bufAddr == 0) {
          uint32_t avail = 0;
          srcPtr = lakatosPrefetchBuffer(&avail);
          if (!srcPtr || len > avail) return push(0);
        } else {
          if (!checkAddr(bufAddr, len)) return push(0);
          srcPtr = mem + bufAddr;
        }
        bool ok = lakatosDrawJpgMem(srcPtr, len, (int)(int32_t)x,
                                    (int)(int32_t)y, (int)(int32_t)scale);
        return push(ok ? 1 : 0);
      }
      case SysJpgInfo: {
        char pathBuf[128];
        if (!popSliceAsCStr(pathBuf, sizeof(pathBuf))) return false;
        int w = 0, h = 0;
        bool ok = lakatosJpgInfo(pathBuf, &w, &h);
        return push(ok ? 1 : 0) &&
               push((uint32_t)(int32_t)w) &&
               push((uint32_t)(int32_t)h);
      }

      // Core 0 Background File Prefetcher
      case SysPrefetchStart: {
        uint32_t maxLen = 0, bufAddr = 0;
        if (!pop(maxLen) || !pop(bufAddr)) return false;
        char pathBuf[128];
        if (!popSliceAsCStr(pathBuf, sizeof(pathBuf))) return false;
        if (maxLen == 0) return push(0);
        uint8_t* dstPtr = nullptr;
        if (bufAddr != 0) {
          if (!checkAddr(bufAddr, maxLen)) return push(0);
          dstPtr = mem + bufAddr;
        }
        bool ok = lakatosPrefetchStart(pathBuf, dstPtr, maxLen);
        return push(ok ? 1 : 0);
      }
      case SysPrefetchStatus: {
        uint32_t bytesGot = 0;
        int st = lakatosPrefetchStatus(&bytesGot);
        return push((uint32_t)(int32_t)st) &&
               push(bytesGot);
      }
      case SysPrefetchCancel: {
        lakatosPrefetchCancel();
        return true;
      }

      // Touch, Keyboard, LED & Sensors
      case SysReadTouch: {
        LakatosTouchSample ts = lakatosReadTouchLatched();
        return push(ts.touched ? 1 : 0) &&
               push((uint32_t)ts.x) &&
               push((uint32_t)ts.y);
      }
      case SysReadClick: {
        LakatosTouchSample ts = lakatosReadClick();
        return push(ts.touched ? 1 : 0) &&
               push((uint32_t)ts.x) &&
               push((uint32_t)ts.y);
      }
      case SysReadTouchRaw: {
        LakatosTouchSample ts = lakatosReadTouchLatched();
        return push(ts.touched ? 1 : 0) &&
               push((uint32_t)ts.rawX) &&
               push((uint32_t)ts.rawY) &&
               push((uint32_t)ts.rawZ);
      }
      case SysPromptKeyboard: {
        uint32_t allowCtrlC = 0, maxLen = 0, bufAddr = 0;
        if (!pop(allowCtrlC) || !pop(maxLen) || !pop(bufAddr)) return false;
        char titleBuf[64];
        if (!popSliceAsCStr(titleBuf, sizeof(titleBuf))) return false;
        if (maxLen == 0 || !checkAddr(bufAddr, maxLen)) return false;
        int res = lakatosPromptKeyboard(titleBuf, (char*)(mem + bufAddr),
                                        maxLen, allowCtrlC != 0, false);
        tft.setTextDatum(TL_DATUM);
        return push((uint32_t)(int32_t)res);
      }
      case SysSetRgbLed: {
        uint32_t dur = 0, b = 0, g = 0, r = 0;
        if (!pop(dur) || !pop(b) || !pop(g) || !pop(r)) return false;
        lakatosSetRgbLed(r != 0, g != 0, b != 0, dur);
        return true;
      }
      case SysReadLdr:
        return push((uint32_t)lakatosReadLdrRaw());
      case SysReadCpuTemp:
        return push((uint32_t)(int32_t)(lakatosReadCpuTempC() * 10.0f));

      // Unified VFS (/flash, /sd, /net), Network & Serial
      case SysFsLs: {
        uint32_t maxEnt = 0, entAddr = 0;
        if (!pop(maxEnt) || !pop(entAddr)) return false;
        char pathBuf[128];
        if (!popSliceAsCStr(pathBuf, sizeof(pathBuf))) return false;
        if (maxEnt > 0 && !checkAddr(entAddr, maxEnt * sizeof(LakatosFsEntry))) return false;
        int n = lakatosFsLs(pathBuf, (LakatosFsEntry*)(mem + entAddr), (int)maxEnt);
        return push((uint32_t)(int32_t)n);
      }
      case SysFsStat: {
        uint32_t entAddr = 0;
        if (!pop(entAddr)) return false;
        char pathBuf[128];
        if (!popSliceAsCStr(pathBuf, sizeof(pathBuf))) return false;
        if (!checkAddr(entAddr, sizeof(LakatosFsEntry))) return false;
        bool ok = lakatosFsStat(pathBuf, (LakatosFsEntry*)(mem + entAddr));
        return push(ok ? 1 : 0);
      }
      case SysFsRead: {
        uint32_t ln = 0, bufAddr = 0, off = 0;
        if (!pop(ln) || !pop(bufAddr) || !pop(off)) return false;
        char pathBuf[128];
        if (!popSliceAsCStr(pathBuf, sizeof(pathBuf))) return false;
        if (ln > 0 && !checkAddr(bufAddr, ln)) return false;
        int32_t got = lakatosFsRead(pathBuf, off, mem + bufAddr, ln);
        return push((uint32_t)got);
      }
      case SysFsWrite: {
        uint32_t ln = 0, bufAddr = 0;
        if (!pop(ln) || !pop(bufAddr)) return false;
        char pathBuf[128];
        if (!popSliceAsCStr(pathBuf, sizeof(pathBuf))) return false;
        if (ln > 0 && !checkAddr(bufAddr, ln)) return false;
        bool ok = lakatosFsWrite(pathBuf, mem + bufAddr, ln);
        return push(ok ? 1 : 0);
      }
      case SysFsWriteAt: {
        uint32_t ln = 0, bufAddr = 0, off = 0;
        if (!pop(ln) || !pop(bufAddr) || !pop(off)) return false;
        char pathBuf[128];
        if (!popSliceAsCStr(pathBuf, sizeof(pathBuf))) return false;
        if (ln > 0 && !checkAddr(bufAddr, ln)) return false;
        bool ok = lakatosFsWriteAt(pathBuf, off, mem + bufAddr, ln);
        return push(ok ? 1 : 0);
      }
      case SysFsMkdir: {
        uint32_t parents = 0;
        if (!pop(parents)) return false;
        char pathBuf[128];
        if (!popSliceAsCStr(pathBuf, sizeof(pathBuf))) return false;
        return push(lakatosFsMkdir(pathBuf, parents != 0) ? 1 : 0);
      }
      case SysFsRmdir: {
        char pathBuf[128];
        if (!popSliceAsCStr(pathBuf, sizeof(pathBuf))) return false;
        return push(lakatosFsRmdir(pathBuf) ? 1 : 0);
      }
      case SysFsRm: {
        char pathBuf[128];
        if (!popSliceAsCStr(pathBuf, sizeof(pathBuf))) return false;
        return push(lakatosFsRm(pathBuf) ? 1 : 0);
      }
      case SysFsDu: {
        char pathBuf[128];
        if (!popSliceAsCStr(pathBuf, sizeof(pathBuf))) return false;
        return push((uint32_t)lakatosFsDu(pathBuf));
      }
      case SysWifiConnected:
        return push(lakatosWifiIsConnected() ? 1 : 0);
      case SysHttpGet: {
        uint32_t maxLen = 0, bufAddr = 0;
        if (!pop(maxLen) || !pop(bufAddr)) return false;
        char urlBuf[256];
        if (!popSliceAsCStr(urlBuf, sizeof(urlBuf))) return false;
        if (maxLen > 0 && !checkAddr(bufAddr, maxLen)) return false;
        int got = lakatosHttpGet(urlBuf, mem + bufAddr, maxLen);
        return push((uint32_t)(int32_t)got);
      }
      case SysHttpGetCached: {
        uint32_t maxAgeSec = 0, maxLen = 0, bufAddr = 0;
        if (!pop(maxAgeSec) || !pop(maxLen) || !pop(bufAddr)) return false;
        char urlBuf[256];
        if (!popSliceAsCStr(urlBuf, sizeof(urlBuf))) return false;
        if (maxLen > 0 && !checkAddr(bufAddr, maxLen)) return false;
        int got = lakatosHttpGetCached(urlBuf, mem + bufAddr, maxLen, maxAgeSec);
        return push((uint32_t)(int32_t)got);
      }
      case SysSerialAvail:
        return push((uint32_t)Serial.available());
      case SysSerialRead:
        return push((uint32_t)(int32_t)Serial.read());
      case SysSerialWrite: {
        uint32_t ln = 0, bufAddr = 0;
        if (!pop(ln) || !pop(bufAddr)) return false;
        if (ln > 0 && !checkAddr(bufAddr, ln)) return false;
        size_t w = Serial.write(mem + bufAddr, ln);
        return push((uint32_t)w);
      }
      default:
        setErr("Unknown syscall");
        return false;
    }
  }
};

} // namespace

// ---------------------------------------------------------------------------
// Google CityHash32 Reference Implementation
// ---------------------------------------------------------------------------
static inline uint32_t cityRotate32(uint32_t val, int shift) {
  return shift == 0 ? val : ((val >> shift) | (val << (32 - shift)));
}

static inline uint32_t cityBswap32(uint32_t x) {
  return __builtin_bswap32(x);
}

static inline uint32_t cityFmix(uint32_t h) {
  h ^= h >> 16;
  h *= 0x85ebca6bu;
  h ^= h >> 13;
  h *= 0xc2b2ae35u;
  h ^= h >> 16;
  return h;
}

static inline uint32_t cityMur(uint32_t a, uint32_t h) {
  constexpr uint32_t c1 = 0xcc9e2d51u;
  constexpr uint32_t c2 = 0x1b873593u;
  a *= c1;
  a = cityRotate32(a, 17);
  a *= c2;
  h ^= a;
  h = cityRotate32(h, 19);
  return h * 5u + 0xe6546b64u;
}

static uint32_t cityHash32Len0to4(const uint8_t* s, size_t len) {
  constexpr uint32_t c1 = 0xcc9e2d51u;
  uint32_t b = 0;
  uint32_t c = 9;
  for (size_t i = 0; i < len; i++) {
    int8_t v = (int8_t)s[i];
    b = b * c1 + (uint32_t)(int32_t)v;
    c ^= b;
  }
  return cityFmix(cityMur(b, cityMur((uint32_t)len, c)));
}

static uint32_t cityHash32Len5to12(const uint8_t* s, size_t len) {
  uint32_t a = (uint32_t)len, b = (uint32_t)len * 5u, c = 9u, d = b;
  a += readU32LE(s);
  b += readU32LE(s + len - 4);
  c += readU32LE(s + ((len >> 1) & 4));
  return cityFmix(cityMur(c, cityMur(b, cityMur(a, d))));
}

static uint32_t cityHash32Len13to24(const uint8_t* s, size_t len) {
  uint32_t a = readU32LE(s - 4 + (len >> 1));
  uint32_t b = readU32LE(s + 4);
  uint32_t c = readU32LE(s + len - 8);
  uint32_t d = readU32LE(s + (len >> 1));
  uint32_t e = readU32LE(s);
  uint32_t f = readU32LE(s + len - 4);
  uint32_t h = (uint32_t)len;
  return cityFmix(cityMur(f, cityMur(e, cityMur(d, cityMur(c, cityMur(b, cityMur(a, h)))))));
}

uint32_t lakatosCityHash32(const void* buf, size_t len) {
  const uint8_t* s = (const uint8_t*)buf;
  if (!s) len = 0;
  if (len <= 24) {
    return len <= 12
               ? (len <= 4 ? cityHash32Len0to4(s, len) : cityHash32Len5to12(s, len))
               : cityHash32Len13to24(s, len);
  }

  constexpr uint32_t c1 = 0xcc9e2d51u;
  constexpr uint32_t c2 = 0x1b873593u;
  uint32_t h = (uint32_t)len, g = c1 * h, f = g;
  uint32_t a0 = cityRotate32(readU32LE(s + len - 4) * c1, 17) * c2;
  uint32_t a1 = cityRotate32(readU32LE(s + len - 8) * c1, 17) * c2;
  uint32_t a2 = cityRotate32(readU32LE(s + len - 16) * c1, 17) * c2;
  uint32_t a3 = cityRotate32(readU32LE(s + len - 12) * c1, 17) * c2;
  uint32_t a4 = cityRotate32(readU32LE(s + len - 20) * c1, 17) * c2;
  h ^= a0;
  h = cityRotate32(h, 19);
  h = h * 5u + 0xe6546b64u;
  h ^= a2;
  h = cityRotate32(h, 19);
  h = h * 5u + 0xe6546b64u;
  g ^= a1;
  g = cityRotate32(g, 19);
  g = g * 5u + 0xe6546b64u;
  g ^= a3;
  g = cityRotate32(g, 19);
  g = g * 5u + 0xe6546b64u;
  f += a4;
  f = cityRotate32(f, 19) + 113u;
  size_t iters = (len - 1) / 20;
  do {
    uint32_t b0 = cityRotate32(readU32LE(s) * c1, 17) * c2;
    uint32_t b1 = readU32LE(s + 4);
    uint32_t b2 = cityRotate32(readU32LE(s + 8) * c1, 17) * c2;
    uint32_t b3 = cityRotate32(readU32LE(s + 12) * c1, 17) * c2;
    uint32_t b4 = readU32LE(s + 16);
    h ^= b0;
    h = cityRotate32(h, 18);
    h = h * 5u + 0xe6546b64u;
    f += b1;
    f = cityRotate32(f, 19);
    f = f * c1;
    g += b2;
    g = cityRotate32(g, 18);
    g = g * 5u + 0xe6546b64u;
    h ^= b3 + b1;
    h = cityRotate32(h, 19);
    h = h * 5u + 0xe6546b64u;
    g ^= b4;
    g = cityBswap32(g) * 5u;
    h += b4 * 5u;
    h = cityBswap32(h);
    f += b0;
    // PERMUTE3(f, h, g): swap(f, h); swap(f, g);
    uint32_t tmp = f;
    f = g;
    g = h;
    h = tmp;
    s += 20;
  } while (--iters != 0);
  g = cityRotate32(g, 11) * c1;
  g = cityRotate32(g, 17) * c1;
  f = cityRotate32(f, 11) * c1;
  f = cityRotate32(f, 17) * c1;
  h = cityRotate32(h + g, 19);
  h = h * 5u + 0xe6546b64u;
  h = cityRotate32(h, 17) * c1;
  h = cityRotate32(h + f, 19);
  h = h * 5u + 0xe6546b64u;
  h = cityRotate32(h, 17) * c1;
  return h;
}

bool lakatosRunLCodeApp(const LakatosAppHeader& hdr, const uint8_t* payload,
                        size_t payloadLen, int* outExitCode, String* outErrMsg) {
  if (!payload || payloadLen < 24) {
    if (outErrMsg) *outErrMsg = "Truncated LCOD module";
    return false;
  }
  if (readU32LE(payload) != LAKATOS_LCODE_MAGIC) {
    if (outErrMsg) *outErrMsg = "Invalid LCOD magic";
    return false;
  }

  // Parse 24-byte LCOD header first (before any heap allocation) so we can allocate
  // vm->mem FIRST from DRAM1 (~109 KB contiguous), leaving DRAM0 (~43 KB) free for
  // LCodeVM (~5 KB), bytecode (~3 KB), Core 0 prefetch worker (~3 KB), and miniz (~11 KB).
  uint32_t strPoolSize  = readU32LE(payload + 8);
  uint16_t globalsCount = readU16LE(payload + 12);
  uint16_t funcCount    = readU16LE(payload + 14);
  uint16_t entryFunc    = readU16LE(payload + 16);
  uint32_t codeSize     = readU32LE(payload + 20);

  if (globalsCount > kMaxGlobals || funcCount > kMaxFuncs) {
    Serial.printf("[Lakatos LCode] Table limits exceeded: globals=%u (max %d), funcs=%u (max %d)\r\n",
                  (unsigned)globalsCount, kMaxGlobals, (unsigned)funcCount, kMaxFuncs);
    if (outErrMsg) *outErrMsg = "LCOD table limits exceeded";
    return false;
  }

  uint32_t offset = 24;
  if (offset + (uint32_t)globalsCount * 4u + strPoolSize > payloadLen) {
    if (outErrMsg) *outErrMsg = "Truncated LCOD globals or string pool";
    return false;
  }

  uint32_t stringPoolEnd = align4(kStringPoolBase + strPoolSize);
  uint32_t osScratchBase = stringPoolEnd;
  uint32_t globalsBase   = align4(osScratchBase + kOsScratchSize);
  uint32_t gCursor       = globalsBase;
  for (uint16_t i = 0; i < globalsCount; i++) {
    uint32_t gSz = readU32LE(payload + offset + (uint32_t)i * 4u);
    gCursor += align4(gSz);
  }
  uint32_t frameBase    = align8(gCursor);
  uint32_t frameReserve = (gCursor > 65536u) ? 512u : 4096u;
  uint32_t heapBase     = align8(frameBase + frameReserve);
  uint32_t minArena     = heapBase + ((gCursor > 65536u) ? 384u : 512u);

  size_t maxContig = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  size_t targetArena = (gCursor <= 16384u) ? 24576u : ((minArena + 255u) & ~255u);
  if (targetArena > maxContig) {
    targetArena = maxContig & ~255u;
  }
  if (targetArena < minArena) {
    Serial.printf("[Lakatos LCode] Arena too small: need=%lu, target=%u, maxContig=%u, freeHeap=%u\r\n",
                  (unsigned long)minArena, (unsigned)targetArena,
                  (unsigned)maxContig, (unsigned)ESP.getFreeHeap());
    if (outErrMsg) *outErrMsg = "Insufficient contiguous RAM for LCode arena";
    return false;
  }

  // Allocate arenaMem FIRST so it claims the large DRAM1 block
  uint8_t* arenaMem = (uint8_t*)malloc(targetArena);
  if (!arenaMem) {
    if (outErrMsg) *outErrMsg = "malloc failed for LCode arena";
    return false;
  }

  // Allocate LCodeVM (~6.8 KB, all 32-bit aligned fields) from 32-bit IRAM first
  // so it consumes 0 bytes of precious 8-bit DRAM heap and 0 bytes of gLakatosPngScratch.
  static_assert(sizeof(LCodeVM) % sizeof(uint32_t) == 0, "LCodeVM must be 32-bit word aligned");
  LCodeVM* vm = (LCodeVM*)heap_caps_malloc(sizeof(LCodeVM), MALLOC_CAP_32BIT);
  if (!vm) {
    vm = (LCodeVM*)malloc(sizeof(LCodeVM));
  }
  if (!vm) {
    free(arenaMem);
    if (outErrMsg) *outErrMsg = "Out of heap for LCodeVM struct";
    return false;
  }
  {
    volatile uint32_t* p32 = (volatile uint32_t*)vm;
    for (size_t w = 0; w < sizeof(LCodeVM) / sizeof(uint32_t); w++) {
      p32[w] = 0;
    }
  }

  vm->mem = arenaMem;
  memset(vm->mem, 0, targetArena);
  vm->memSize = (uint32_t)targetArena;

  vm->globalCount   = globalsCount;
  vm->funcCount     = funcCount;
  vm->entryFuncIdx  = entryFunc;
  vm->stringPoolEnd = stringPoolEnd;
  vm->osScratchBase = osScratchBase;
  vm->osScratchPtr  = 0;
  vm->globalsBase   = globalsBase;

  gCursor = vm->globalsBase;
  for (uint16_t i = 0; i < globalsCount; i++) {
    vm->globalSizes[i]   = readU32LE(payload + offset);
    vm->globalOffsets[i] = gCursor;
    gCursor += align4(vm->globalSizes[i]);
    offset += 4;
  }
  vm->frameBase = frameBase;
  vm->frameSP   = frameBase;
  vm->heapBase  = heapBase;

  memcpy(vm->mem + kStringPoolBase, payload + offset, strPoolSize);
  offset += strPoolSize;

  vm->initHeap();

  uint32_t maxMetaEnd = offset + (uint32_t)funcCount * 20u;
  for (uint16_t i = 0; i < funcCount; i++) {
    if (offset + 20 > payloadLen) {
      free(vm->mem);
      heap_caps_free(vm);
      lakatosPrefetchFree();
      if (outErrMsg) *outErrMsg = "Truncated LCOD function table";
      return false;
    }
    const uint8_t* ent = payload + offset;
    offset += 20;

    uint16_t argCount   = readU16LE(ent + 0);
    uint16_t localCount = readU16LE(ent + 2);
    uint32_t frameSize  = readU32LE(ent + 4);
    uint32_t codeOff    = readU32LE(ent + 8);
    uint32_t fnCodeSz   = readU32LE(ent + 12);
    uint32_t metaOff    = readU32LE(ent + 16);

    if (vm->varPoolUsed + localCount > kMaxVarPool) {
      free(vm->mem);
      heap_caps_free(vm);
      lakatosPrefetchFree();
      if (outErrMsg) *outErrMsg = "Too many local variables in LCOD";
      return false;
    }

    uint32_t poolStart = (uint32_t)vm->varPoolUsed;
    uint32_t mPos = metaOff;
    uint32_t lOff = 0;
    for (uint16_t v = 0; v < localCount; v++) {
      if (mPos + 4 > payloadLen) {
        free(vm->mem);
        heap_caps_free(vm);
        lakatosPrefetchFree();
        if (outErrMsg) *outErrMsg = "Truncated LCOD local metadata";
        return false;
      }
      uint32_t sz = readU32LE(payload + mPos);
      vm->varSizesPool[vm->varPoolUsed]     = sz;
      vm->localOffsetsPool[vm->varPoolUsed] = lOff;
      vm->varPoolUsed++;
      lOff += align4(sz);
      mPos += 4;
    }
    if (lOff > frameSize) frameSize = lOff;
    if (mPos < payloadLen) {
      uint8_t nLen = payload[mPos++];
      mPos += nLen;
    }
    if (mPos > maxMetaEnd) maxMetaEnd = mPos;

    vm->funcs[i].argCount     = argCount;
    vm->funcs[i].localCount   = localCount;
    vm->funcs[i].frameSize    = align4(frameSize);
    vm->funcs[i].codeOffset   = codeOff;
    vm->funcs[i].codeSize     = fnCodeSz;
    vm->funcs[i].varSizeStart = poolStart;
  }

  if (maxMetaEnd + codeSize > payloadLen || codeSize == 0) {
    free(vm->mem);
    heap_caps_free(vm);
    lakatosPrefetchFree();
    if (outErrMsg) *outErrMsg = "Truncated LCOD bytecode section";
    return false;
  }

  // Copy bytecode out of `payload` (which resides in gLakatosAppRamArena and may be
  // overwritten when lakatos_png.cpp uses gLakatosAppRamArena for its 32 KB LZ77 dict).
  // Place bytecode in the upper ~4.5 KB of gLakatosPngScratch (above the 11,008-byte
  // tinfl_decompressor region) when it fits, or allocate from heap otherwise.
  constexpr size_t kTinflReserved = 11008;
  bool codeFromScratch = (gLakatosPngScratch != nullptr) &&
                         (gLakatosPngScratchSize >= kTinflReserved + codeSize);
  if (codeFromScratch) {
    uint8_t* codeDst = gLakatosPngScratch + kTinflReserved;
    memcpy(codeDst, payload + maxMetaEnd, codeSize);
    vm->codeBuf = nullptr;
    vm->code    = codeDst;
  } else {
    vm->codeBuf = (uint8_t*)malloc(codeSize);
    if (!vm->codeBuf) {
      free(vm->mem);
      heap_caps_free(vm);
      lakatosPrefetchFree();
      if (outErrMsg) *outErrMsg = "Out of heap for LCode bytecode";
      return false;
    }
    memcpy(vm->codeBuf, payload + maxMetaEnd, codeSize);
    vm->code = vm->codeBuf;
  }
  vm->codeSize = codeSize;

  if (vm->entryFuncIdx < 0 || vm->entryFuncIdx >= vm->funcCount) {
    if (vm->codeBuf) free(vm->codeBuf);
    free(vm->mem);
    heap_caps_free(vm);
    lakatosPrefetchFree();
    if (outErrMsg) *outErrMsg = "Invalid LCOD entry function";
    return false;
  }

  const LCodeFuncMeta& entryFn = vm->funcs[vm->entryFuncIdx];
  vm->frameSP = vm->frameBase + entryFn.frameSize;
  memset(vm->mem + vm->frameBase, 0, entryFn.frameSize);

  vm->callStack[0].funcIdx   = (uint32_t)vm->entryFuncIdx;
  vm->callStack[0].returnPC  = -1;
  vm->callStack[0].frameAddr = vm->frameBase;
  vm->callDepth    = 1;
  vm->pc           = (int32_t)entryFn.codeOffset;
  vm->running      = 1;
  vm->exitCode     = 0;
  vm->errReason    = nullptr;
  vm->errPC        = 0;

  Serial.printf("[Lakatos LCode] Running '%s' in RAM (code=%lu B, arena=%lu B, heap=%lu B, free8Bit=%u B, freeSys=%u B)\r\n",
                hdr.name,
                (unsigned long)vm->codeSize,
                (unsigned long)vm->memSize,
                (unsigned long)vm->heapBytesFree(),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                (unsigned)ESP.getFreeHeap());

  lakatosWaitTouchRelease();
  {
    LakatosTftGuard tftGuard;
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
  }

  uint32_t stepCounter = 0;
  while (vm->running) {
    if (__builtin_expect((++stepCounter & 0x1FFFu) == 0, 0)) {
      yield();
    }
    if (__builtin_expect(vm->pc < 0 || (uint32_t)vm->pc >= vm->codeSize, 0)) {
      vm->setErr("PC out of bounds");
      break;
    }

    uint8_t op = vm->code[vm->pc++];
    switch (op) {
      case OpNOP:
        break;
      case OpPUSH_NIL:
      case OpPUSH_FALSE:
      case OpPUSH_0:
        vm->push(0);
        break;
      case OpPUSH_NIL_SLICE:
        vm->push(0);
        vm->push(0);
        vm->push(0);
        break;
      case OpPUSH_TRUE:
      case OpPUSH_1:
        vm->push(1);
        break;
      case OpPUSH_NEG1:
        vm->push(0xFFFFFFFFu);
        break;
      case OpPUSH_I8: {
        int8_t v = (int8_t)vm->code[vm->pc++];
        vm->push((uint32_t)(int32_t)v);
        break;
      }
      case OpPUSH_U8: {
        uint8_t v = vm->code[vm->pc++];
        vm->push((uint32_t)v);
        break;
      }
      case OpPUSH_I16: {
        int16_t v = (int16_t)readU16LE(vm->code + vm->pc);
        vm->pc += 2;
        vm->push((uint32_t)(int32_t)v);
        break;
      }
      case OpPUSH_U16: {
        uint16_t v = readU16LE(vm->code + vm->pc);
        vm->pc += 2;
        vm->push((uint32_t)v);
        break;
      }
      case OpPUSH_I32: {
        uint32_t v = readU32LE(vm->code + vm->pc);
        vm->pc += 4;
        vm->push(v);
        break;
      }
      case OpPUSH_STR: {
        uint16_t off = readU16LE(vm->code + vm->pc);
        vm->pc += 2;
        uint32_t strAddr = kStringPoolBase + off;
        uint32_t sLen    = readU16LE(vm->mem + strAddr);
        vm->push(strAddr + 2);
        vm->push(sLen);
        vm->push(sLen);
        break;
      }
      case OpPUSH_CSTR: {
        uint16_t off = readU16LE(vm->code + vm->pc);
        vm->pc += 2;
        vm->push(kStringPoolBase + off + 2);
        break;
      }
      case OpPOP: {
        uint32_t dummy = 0;
        vm->pop(dummy);
        break;
      }
      case OpPOP_SLICE: {
        uint32_t d1 = 0, d2 = 0, d3 = 0;
        vm->pop(d1);
        vm->pop(d2);
        vm->pop(d3);
        break;
      }
      case OpDUP: {
        uint32_t v = 0;
        if (vm->pop(v)) {
          vm->push(v);
          vm->push(v);
        }
        break;
      }
      case OpSWAP: {
        uint32_t b = 0, a = 0;
        if (vm->pop(b) && vm->pop(a)) {
          vm->push(b);
          vm->push(a);
        }
        break;
      }
      case OpDUP2: {
        uint32_t b = 0, a = 0;
        if (vm->pop(b) && vm->pop(a)) {
          vm->push(a);
          vm->push(b);
          vm->push(a);
          vm->push(b);
        }
        break;
      }
      case OpROT: {
        uint32_t c = 0, b = 0, a = 0;
        if (vm->pop(c) && vm->pop(b) && vm->pop(a)) {
          vm->push(b);
          vm->push(c);
          vm->push(a);
        }
        break;
      }
      case OpOVER: {
        uint32_t b = 0, a = 0;
        if (vm->pop(b) && vm->pop(a)) {
          vm->push(a);
          vm->push(b);
          vm->push(a);
        }
        break;
      }

      // Locals & Globals
      case OpLOAD_LOCAL_0:
      case OpLOAD_LOCAL_1:
      case OpLOAD_LOCAL_2:
      case OpLOAD_LOCAL_3:
        vm->loadLocalSlot(op - OpLOAD_LOCAL_0);
        break;
      case OpSTORE_LOCAL_0:
      case OpSTORE_LOCAL_1:
      case OpSTORE_LOCAL_2:
      case OpSTORE_LOCAL_3:
        vm->storeLocalSlot(op - OpSTORE_LOCAL_0);
        break;
      case OpLOAD_LOCAL:
        vm->loadLocalSlot(vm->code[vm->pc++]);
        break;
      case OpSTORE_LOCAL:
        vm->storeLocalSlot(vm->code[vm->pc++]);
        break;
      case OpLOAD_GLOBAL: {
        uint16_t gIdx = readU16LE(vm->code + vm->pc);
        vm->pc += 2;
        if (gIdx >= vm->globalCount) {
          vm->setErr("Global index out of range");
          break;
        }
        uint32_t addr = vm->globalOffsets[gIdx];
        if (vm->globalSizes[gIdx] == 12) {
          vm->push(readU32LE(vm->mem + addr));
          vm->push(readU32LE(vm->mem + addr + 4));
          vm->push(readU32LE(vm->mem + addr + 8));
        } else {
          vm->push(readU32LE(vm->mem + addr));
        }
        break;
      }
      case OpSTORE_GLOBAL: {
        uint16_t gIdx = readU16LE(vm->code + vm->pc);
        vm->pc += 2;
        if (gIdx >= vm->globalCount) {
          vm->setErr("Global index out of range");
          break;
        }
        uint32_t addr = vm->globalOffsets[gIdx];
        if (vm->globalSizes[gIdx] == 12) {
          uint32_t ln = 0, cp = 0, bs = 0;
          if (vm->pop(ln) && vm->pop(cp) && vm->pop(bs)) {
            writeU32LE(vm->mem + addr, bs);
            writeU32LE(vm->mem + addr + 4, cp);
            writeU32LE(vm->mem + addr + 8, ln);
          }
        } else {
          uint32_t v = 0;
          if (vm->pop(v)) {
            writeU32LE(vm->mem + addr, v);
          }
        }
        break;
      }
      case OpADDR_OF_GLOBAL: {
        uint16_t gIdx = readU16LE(vm->code + vm->pc);
        vm->pc += 2;
        if (gIdx >= vm->globalCount) {
          vm->setErr("Global index out of range");
          break;
        }
        vm->push(vm->globalOffsets[gIdx]);
        break;
      }
      case OpADDR_OF_LOCAL: {
        uint8_t slot = vm->code[vm->pc++];
        const LCodeCallFrame& cf = vm->callStack[vm->callDepth - 1];
        const LCodeFuncMeta& fn  = vm->funcs[cf.funcIdx];
        if (slot >= fn.localCount) {
          vm->setErr("Local slot out of range");
          break;
        }
        vm->push(cf.frameAddr + vm->localOffsetsPool[fn.varSizeStart + slot]);
        break;
      }

      // Memory PEEK / POKE & Indexing
      case OpPEEK1: {
        uint32_t addr = 0;
        if (vm->pop(addr) && vm->checkAddr(addr, 1)) {
          vm->push((uint32_t)vm->mem[addr]);
        }
        break;
      }
      case OpPEEKS1: {
        uint32_t addr = 0;
        if (vm->pop(addr) && vm->checkAddr(addr, 1)) {
          vm->push((uint32_t)(int32_t)(int8_t)vm->mem[addr]);
        }
        break;
      }
      case OpPOKE1: {
        uint32_t val = 0, addr = 0;
        if (vm->pop(val) && vm->pop(addr) && vm->checkAddr(addr, 1)) {
          vm->mem[addr] = (uint8_t)val;
        }
        break;
      }
      case OpPEEK2: {
        uint32_t addr = 0;
        if (vm->pop(addr) && vm->checkAddr(addr, 2)) {
          vm->push((uint32_t)readU16LE(vm->mem + addr));
        }
        break;
      }
      case OpPEEKS2: {
        uint32_t addr = 0;
        if (vm->pop(addr) && vm->checkAddr(addr, 2)) {
          vm->push((uint32_t)(int32_t)(int16_t)readU16LE(vm->mem + addr));
        }
        break;
      }
      case OpPOKE2: {
        uint32_t val = 0, addr = 0;
        if (vm->pop(val) && vm->pop(addr) && vm->checkAddr(addr, 2)) {
          writeU16LE(vm->mem + addr, (uint16_t)val);
        }
        break;
      }
      case OpPEEK4: {
        uint32_t addr = 0;
        if (vm->pop(addr) && vm->checkAddr(addr, 4)) {
          vm->push(readU32LE(vm->mem + addr));
        }
        break;
      }
      case OpPOKE4: {
        uint32_t val = 0, addr = 0;
        if (vm->pop(val) && vm->pop(addr) && vm->checkAddr(addr, 4)) {
          writeU32LE(vm->mem + addr, val);
        }
        break;
      }
      case OpSHL1_ADD: {
        uint32_t idx = 0, base = 0;
        if (vm->pop(idx) && vm->pop(base)) vm->push(base + idx * 2u);
        break;
      }
      case OpSHL2_ADD: {
        uint32_t idx = 0, base = 0;
        if (vm->pop(idx) && vm->pop(base)) vm->push(base + idx * 4u);
        break;
      }
      case OpINDEX_ADD: {
        uint16_t elemSz = readU16LE(vm->code + vm->pc);
        vm->pc += 2;
        uint32_t idx = 0, base = 0;
        if (vm->pop(idx) && vm->pop(base)) vm->push(base + idx * (uint32_t)elemSz);
        break;
      }
      case OpMEM_COPY: {
        uint32_t cnt = 0, src = 0, dst = 0;
        if (vm->pop(cnt) && vm->pop(src) && vm->pop(dst)) {
          if (cnt > 0 && vm->checkAddr(src, cnt) && vm->checkAddr(dst, cnt)) {
            memmove(vm->mem + dst, vm->mem + src, cnt);
          }
        }
        break;
      }
      case OpMEM_SET: {
        uint32_t cnt = 0, val = 0, dst = 0;
        if (vm->pop(cnt) && vm->pop(val) && vm->pop(dst)) {
          if (cnt > 0 && vm->checkAddr(dst, cnt)) {
            memset(vm->mem + dst, (uint8_t)val, cnt);
          }
        }
        break;
      }
      case OpMEM_CMP: {
        uint32_t cnt = 0, bAddr = 0, aAddr = 0;
        if (vm->pop(cnt) && vm->pop(bAddr) && vm->pop(aAddr)) {
          if (cnt == 0) {
            vm->push(0);
          } else if (vm->checkAddr(aAddr, cnt) && vm->checkAddr(bAddr, cnt)) {
            int r = memcmp(vm->mem + aAddr, vm->mem + bAddr, cnt);
            vm->push((uint32_t)(int32_t)(r < 0 ? -1 : (r > 0 ? 1 : 0)));
          }
        }
        break;
      }

      // 32-Bit ALU (single-dispatch in-place stack operations)
      case OpADD:
        if (__builtin_expect(vm->sp < 2, 0)) { vm->setErr("Eval stack underflow"); break; }
        vm->sp--;
        vm->stack[vm->sp - 1] += vm->stack[vm->sp];
        break;
      case OpSUB:
        if (__builtin_expect(vm->sp < 2, 0)) { vm->setErr("Eval stack underflow"); break; }
        vm->sp--;
        vm->stack[vm->sp - 1] -= vm->stack[vm->sp];
        break;
      case OpMUL:
        if (__builtin_expect(vm->sp < 2, 0)) { vm->setErr("Eval stack underflow"); break; }
        vm->sp--;
        vm->stack[vm->sp - 1] *= vm->stack[vm->sp];
        break;
      case OpDIV: {
        uint32_t b = 0, a = 0;
        if (!vm->pop(b) || !vm->pop(a)) break;
        if (b == 0) { vm->setErr("Divide by 0"); break; }
        vm->push((uint32_t)((int32_t)a / (int32_t)b));
        break;
      }
      case OpMOD: {
        uint32_t b = 0, a = 0;
        if (!vm->pop(b) || !vm->pop(a)) break;
        if (b == 0) { vm->setErr("Modulo by 0"); break; }
        vm->push((uint32_t)((int32_t)a % (int32_t)b));
        break;
      }
      case OpUDIV: {
        uint32_t b = 0, a = 0;
        if (!vm->pop(b) || !vm->pop(a)) break;
        if (b == 0) { vm->setErr("Unsigned div by 0"); break; }
        vm->push(a / b);
        break;
      }
      case OpUMOD: {
        uint32_t b = 0, a = 0;
        if (!vm->pop(b) || !vm->pop(a)) break;
        if (b == 0) { vm->setErr("Unsigned mod by 0"); break; }
        vm->push(a % b);
        break;
      }
      case OpBIT_AND:
        if (__builtin_expect(vm->sp < 2, 0)) { vm->setErr("Eval stack underflow"); break; }
        vm->sp--;
        vm->stack[vm->sp - 1] &= vm->stack[vm->sp];
        break;
      case OpBIT_OR:
        if (__builtin_expect(vm->sp < 2, 0)) { vm->setErr("Eval stack underflow"); break; }
        vm->sp--;
        vm->stack[vm->sp - 1] |= vm->stack[vm->sp];
        break;
      case OpBIT_XOR:
        if (__builtin_expect(vm->sp < 2, 0)) { vm->setErr("Eval stack underflow"); break; }
        vm->sp--;
        vm->stack[vm->sp - 1] ^= vm->stack[vm->sp];
        break;
      case OpSHL:
        if (__builtin_expect(vm->sp < 2, 0)) { vm->setErr("Eval stack underflow"); break; }
        vm->sp--;
        vm->stack[vm->sp - 1] <<= (vm->stack[vm->sp] & 31u);
        break;
      case OpSHR:
        if (__builtin_expect(vm->sp < 2, 0)) { vm->setErr("Eval stack underflow"); break; }
        vm->sp--;
        vm->stack[vm->sp - 1] = (uint32_t)((int32_t)vm->stack[vm->sp - 1] >> (vm->stack[vm->sp] & 31u));
        break;
      case OpUSHR:
        if (__builtin_expect(vm->sp < 2, 0)) { vm->setErr("Eval stack underflow"); break; }
        vm->sp--;
        vm->stack[vm->sp - 1] >>= (vm->stack[vm->sp] & 31u);
        break;
      case OpCMP_EQ:
        if (__builtin_expect(vm->sp < 2, 0)) { vm->setErr("Eval stack underflow"); break; }
        vm->sp--;
        vm->stack[vm->sp - 1] = (vm->stack[vm->sp - 1] == vm->stack[vm->sp]) ? 1u : 0u;
        break;
      case OpCMP_NE:
        if (__builtin_expect(vm->sp < 2, 0)) { vm->setErr("Eval stack underflow"); break; }
        vm->sp--;
        vm->stack[vm->sp - 1] = (vm->stack[vm->sp - 1] != vm->stack[vm->sp]) ? 1u : 0u;
        break;
      case OpCMP_LT:
        if (__builtin_expect(vm->sp < 2, 0)) { vm->setErr("Eval stack underflow"); break; }
        vm->sp--;
        vm->stack[vm->sp - 1] = ((int32_t)vm->stack[vm->sp - 1] < (int32_t)vm->stack[vm->sp]) ? 1u : 0u;
        break;
      case OpCMP_LE:
        if (__builtin_expect(vm->sp < 2, 0)) { vm->setErr("Eval stack underflow"); break; }
        vm->sp--;
        vm->stack[vm->sp - 1] = ((int32_t)vm->stack[vm->sp - 1] <= (int32_t)vm->stack[vm->sp]) ? 1u : 0u;
        break;
      case OpCMP_GT:
        if (__builtin_expect(vm->sp < 2, 0)) { vm->setErr("Eval stack underflow"); break; }
        vm->sp--;
        vm->stack[vm->sp - 1] = ((int32_t)vm->stack[vm->sp - 1] > (int32_t)vm->stack[vm->sp]) ? 1u : 0u;
        break;
      case OpCMP_GE:
        if (__builtin_expect(vm->sp < 2, 0)) { vm->setErr("Eval stack underflow"); break; }
        vm->sp--;
        vm->stack[vm->sp - 1] = ((int32_t)vm->stack[vm->sp - 1] >= (int32_t)vm->stack[vm->sp]) ? 1u : 0u;
        break;
      case OpUCMP_LT:
        if (__builtin_expect(vm->sp < 2, 0)) { vm->setErr("Eval stack underflow"); break; }
        vm->sp--;
        vm->stack[vm->sp - 1] = (vm->stack[vm->sp - 1] < vm->stack[vm->sp]) ? 1u : 0u;
        break;
      case OpUCMP_LE:
        if (__builtin_expect(vm->sp < 2, 0)) { vm->setErr("Eval stack underflow"); break; }
        vm->sp--;
        vm->stack[vm->sp - 1] = (vm->stack[vm->sp - 1] <= vm->stack[vm->sp]) ? 1u : 0u;
        break;
      case OpUCMP_GT:
        if (__builtin_expect(vm->sp < 2, 0)) { vm->setErr("Eval stack underflow"); break; }
        vm->sp--;
        vm->stack[vm->sp - 1] = (vm->stack[vm->sp - 1] > vm->stack[vm->sp]) ? 1u : 0u;
        break;
      case OpUCMP_GE:
        if (__builtin_expect(vm->sp < 2, 0)) { vm->setErr("Eval stack underflow"); break; }
        vm->sp--;
        vm->stack[vm->sp - 1] = (vm->stack[vm->sp - 1] >= vm->stack[vm->sp]) ? 1u : 0u;
        break;
      case OpNEG: {
        uint32_t a = 0;
        if (vm->pop(a)) vm->push((uint32_t)(-(int32_t)a));
        break;
      }
      case OpBIT_NOT: {
        uint32_t a = 0;
        if (vm->pop(a)) vm->push(~a);
        break;
      }
      case OpNOT: {
        uint32_t a = 0;
        if (vm->pop(a)) vm->push(a == 0 ? 1 : 0);
        break;
      }

      // IEEE-754 32-Bit Floating Point ALU & Math (Xtensa LX6 Hardware FPU)
      case OpFADD:
      case OpFSUB:
      case OpFMUL:
      case OpFDIV:
      case OpFATAN2:
      case OpFCMP_EQ:
      case OpFCMP_NE:
      case OpFCMP_LT:
      case OpFCMP_LE:
      case OpFCMP_GT:
      case OpFCMP_GE: {
        uint32_t bBits = 0, aBits = 0;
        if (!vm->pop(bBits) || !vm->pop(aBits)) break;
        float af = u32ToF32(aBits);
        float bf = u32ToF32(bBits);
        switch (op) {
          case OpFADD:    vm->push(f32ToU32(af + bf)); break;
          case OpFSUB:    vm->push(f32ToU32(af - bf)); break;
          case OpFMUL:    vm->push(f32ToU32(af * bf)); break;
          case OpFDIV:    vm->push(f32ToU32(af / bf)); break;
          case OpFATAN2:  vm->push(f32ToU32(atan2f(af, bf))); break;
          case OpFCMP_EQ: vm->push(af == bf ? 1 : 0); break;
          case OpFCMP_NE: vm->push(af != bf ? 1 : 0); break;
          case OpFCMP_LT: vm->push(af <  bf ? 1 : 0); break;
          case OpFCMP_LE: vm->push(af <= bf ? 1 : 0); break;
          case OpFCMP_GT: vm->push(af >  bf ? 1 : 0); break;
          case OpFCMP_GE: vm->push(af >= bf ? 1 : 0); break;
        }
        break;
      }
      case OpFNEG:
      case OpFABS:
      case OpFSQRT:
      case OpFSIN:
      case OpFCOS:
      case OpFFLOOR:
      case OpFCEIL:
      case OpI32_TO_F32:
      case OpF32_TO_I32: {
        uint32_t aBits = 0;
        if (!vm->pop(aBits)) break;
        switch (op) {
          case OpFNEG:       vm->push(f32ToU32(-u32ToF32(aBits))); break;
          case OpFABS:       vm->push(f32ToU32(fabsf(u32ToF32(aBits)))); break;
          case OpFSQRT:      vm->push(f32ToU32(sqrtf(u32ToF32(aBits)))); break;
          case OpFSIN:       vm->push(f32ToU32(sinf(u32ToF32(aBits)))); break;
          case OpFCOS:       vm->push(f32ToU32(cosf(u32ToF32(aBits)))); break;
          case OpFFLOOR:     vm->push(f32ToU32(floorf(u32ToF32(aBits)))); break;
          case OpFCEIL:      vm->push(f32ToU32(ceilf(u32ToF32(aBits)))); break;
          case OpI32_TO_F32: vm->push(f32ToU32((float)(int32_t)aBits)); break;
          case OpF32_TO_I32: vm->push((uint32_t)(int32_t)u32ToF32(aBits)); break;
        }
        break;
      }

      // Control Flow
      case OpJUMP: {
        int16_t rel = (int16_t)readU16LE(vm->code + vm->pc);
        vm->pc += 2 + rel;
        break;
      }
      case OpJUMP_IF_TRUE: {
        int16_t rel = (int16_t)readU16LE(vm->code + vm->pc);
        vm->pc += 2;
        uint32_t cond = 0;
        if (vm->pop(cond) && cond != 0) vm->pc += rel;
        break;
      }
      case OpJUMP_IF_FALSE: {
        int16_t rel = (int16_t)readU16LE(vm->code + vm->pc);
        vm->pc += 2;
        uint32_t cond = 0;
        if (vm->pop(cond) && cond == 0) vm->pc += rel;
        break;
      }
      case OpCALL: {
        uint16_t fIdx = readU16LE(vm->code + vm->pc);
        vm->pc += 2;
        if (fIdx >= vm->funcCount) {
          vm->setErr("Call func index out of range");
          break;
        }
        if (vm->callDepth >= kMaxCallDepth) {
          vm->setErr("Call stack overflow");
          break;
        }
        const LCodeFuncMeta& callee = vm->funcs[fIdx];
        uint32_t newFrame = vm->frameSP;
        if (newFrame + callee.frameSize > vm->heapBase) {
          vm->setErr("Local frame stack overflow");
          break;
        }
        memset(vm->mem + newFrame, 0, callee.frameSize);
        for (int a = (int)callee.argCount - 1; a >= 0; a--) {
          uint32_t poolIdx = callee.varSizeStart + (uint32_t)a;
          uint32_t pAddr   = newFrame + vm->localOffsetsPool[poolIdx];
          if (vm->varSizesPool[poolIdx] == 12) {
            uint32_t ln = 0, cp = 0, bs = 0;
            if (!vm->pop(ln) || !vm->pop(cp) || !vm->pop(bs)) break;
            writeU32LE(vm->mem + pAddr, bs);
            writeU32LE(vm->mem + pAddr + 4, cp);
            writeU32LE(vm->mem + pAddr + 8, ln);
          } else {
            uint32_t v = 0;
            if (!vm->pop(v)) break;
            writeU32LE(vm->mem + pAddr, v);
          }
        }
        vm->frameSP = newFrame + callee.frameSize;
        {
          int32_t cd = vm->callDepth++;
          vm->callStack[cd].funcIdx   = fIdx;
          vm->callStack[cd].returnPC  = vm->pc;
          vm->callStack[cd].frameAddr = newFrame;
        }
        vm->pc = (int32_t)callee.codeOffset;
        break;
      }
      case OpRET:
      case OpRET_SLICE:
      case OpRET_VOID: {
        const LCodeCallFrame& cf = vm->callStack[vm->callDepth - 1];
        vm->frameSP = cf.frameAddr;
        int32_t retPC = cf.returnPC;
        vm->callDepth--;
        if (vm->callDepth <= 0 || retPC < 0) {
          vm->running = false;
        } else {
          vm->pc = retPC;
        }
        break;
      }
      case OpHALT:
        vm->exitCode = 0;
        vm->running  = false;
        break;
      case OpPANIC: {
        char msg[80];
        vm->popSliceAsCStr(msg, sizeof(msg));
        if (msg[0]) Serial.printf("[Lakatos LCode] PANIC: %s\r\n", msg);
        vm->setErr("PANIC");
        break;
      }

      // Slices & Strings
      case OpSLICE_NEW:
        break;
      case OpSLICE_LEN: {
        uint32_t ln = 0, cp = 0, bs = 0;
        if (vm->pop(ln) && vm->pop(cp) && vm->pop(bs)) vm->push(ln);
        break;
      }
      case OpSLICE_CAP: {
        uint32_t ln = 0, cp = 0, bs = 0;
        if (vm->pop(ln) && vm->pop(cp) && vm->pop(bs)) vm->push(cp);
        break;
      }
      case OpSLICE_DATA: {
        uint32_t ln = 0, cp = 0, bs = 0;
        if (vm->pop(ln) && vm->pop(cp) && vm->pop(bs)) vm->push(bs);
        break;
      }
      case OpSLICE_SUB: {
        uint32_t elemSz = 0, limit = 0, start = 0, ln = 0, cp = 0, bs = 0;
        if (vm->pop(elemSz) && vm->pop(limit) && vm->pop(start) &&
            vm->pop(ln) && vm->pop(cp) && vm->pop(bs)) {
          (void)ln;
          if (start > limit || limit > cp) {
            vm->setErr("Slice bounds out of range");
            break;
          }
          vm->push(bs + start * elemSz);
          vm->push(cp - start);
          vm->push(limit - start);
        }
        break;
      }
      case OpSTR_CMP: {
        uint32_t l2 = 0, c2 = 0, b2 = 0, l1 = 0, c1 = 0, b1 = 0;
        if (vm->pop(l2) && vm->pop(c2) && vm->pop(b2) &&
            vm->pop(l1) && vm->pop(c1) && vm->pop(b1)) {
          (void)c1;
          (void)c2;
          if (l1 > 0 && !vm->checkAddr(b1, l1)) break;
          if (l2 > 0 && !vm->checkAddr(b2, l2)) break;
          uint32_t minL = (l1 < l2) ? l1 : l2;
          int cmp = (minL > 0) ? memcmp(vm->mem + b1, vm->mem + b2, minL) : 0;
          if (cmp == 0) {
            cmp = (l1 < l2) ? -1 : (l1 > l2 ? 1 : 0);
          } else {
            cmp = (cmp < 0) ? -1 : 1;
          }
          vm->push((uint32_t)(int32_t)cmp);
        }
        break;
      }

      // Managed Heap
      case OpMALLOC: {
        uint32_t sz = 0;
        if (vm->pop(sz)) {
          uint32_t addr = vm->heapMalloc(sz);
          if (vm->running) vm->push(addr);
        }
        break;
      }
      case OpZALLOC: {
        uint32_t sz = 0;
        if (vm->pop(sz)) {
          uint32_t addr = vm->heapMalloc(sz);
          if (vm->running && addr != 0) {
            memset(vm->mem + addr, 0, sz);
            vm->push(addr);
          }
        }
        break;
      }
      case OpREALLOC: {
        uint32_t newSz = 0, oldAddr = 0;
        if (vm->pop(newSz) && vm->pop(oldAddr)) {
          if (oldAddr == 0) {
            uint32_t a = vm->heapMalloc(newSz);
            if (vm->running) vm->push(a);
          } else {
            uint32_t newAddr = vm->heapMalloc(newSz);
            if (vm->running && newAddr != 0) {
              uint32_t oldSz = readU32LE(vm->mem + oldAddr - 8);
              uint32_t copySz = (newSz < oldSz) ? newSz : oldSz;
              memcpy(vm->mem + newAddr, vm->mem + oldAddr, copySz);
              vm->heapFree(oldAddr);
              vm->push(newAddr);
            }
          }
        }
        break;
      }
      case OpFREE: {
        uint32_t addr = 0;
        if (vm->pop(addr)) vm->heapFree(addr);
        break;
      }
      case OpHEAP_FREE:
        vm->push(vm->heapBytesFree());
        break;

      // Lakatos Syscalls
      case OpSYS_EXIT: {
        uint32_t code = 0;
        if (vm->pop(code)) {
          vm->exitCode = (int)(int32_t)code;
          vm->running  = false;
        }
        break;
      }
      case OpIO_PRINT: {
        char buf[160];
        if (vm->popSliceAsCStr(buf, sizeof(buf))) {
          Serial.println(buf);
        }
        break;
      }
      case OpSYSCALL: {
        uint8_t sysId = vm->code[vm->pc++];
        vm->execSyscall(sysId);
        break;
      }
      default:
        vm->setErr("Unknown LCode opcode");
        break;
    }
  }

  bool hadErr = (vm->errReason != nullptr);
  if (hadErr) {
    char errBuf[96];
    snprintf(errBuf, sizeof(errBuf), "%s (PC=%ld)", vm->errReason, (long)vm->errPC);
    Serial.printf("[Lakatos LCode] Runtime Error in '%s': %s\r\n", hdr.name, errBuf);
    if (outErrMsg) *outErrMsg = errBuf;
  } else {
    Serial.printf("[Lakatos LCode] '%s' finished (exit=%ld, steps=%lu)\r\n",
                  hdr.name, (long)vm->exitCode, (unsigned long)stepCounter);
  }

  if (outExitCode) *outExitCode = vm->exitCode;
  {
    LakatosTftGuard tftGuard;
    tft.setTextDatum(TL_DATUM);
  }

  lakatosPrefetchFree();
  if (vm->codeBuf) free(vm->codeBuf);
  free(vm->mem);
  heap_caps_free(vm);
  return !hadErr;
}
