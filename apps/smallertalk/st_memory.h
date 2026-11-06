#pragma once

#include "st_types.h"
#include "lakatos_api.h"

static constexpr uint16_t kMaxObjects    = 2048;
static constexpr uint32_t kPayloadWords  = 8192;   // 32,768 bytes (32 KB)
static constexpr uint16_t kMaxClasses    = 64;
static constexpr uint16_t kMaxSymbols    = 512;
static constexpr uint16_t kMaxTempRoots  = 64;
static constexpr size_t   kTranscriptCap = 2048;

struct VMState {
  const LakatosApi* os;

  // Object Table & Contiguous Payload Store (heap-allocated in separate chunks for ESP32)
  ObjHeader* objects;
  uint32_t*  payload;
  uint32_t   payloadUsedWords;
  uint16_t   freeObjHint;
  uint16_t   liveObjCount;
  uint32_t   gcCount;

  // Interned Symbol Table (Oop of each Symbol)
  Oop       symbols[kMaxSymbols];
  uint16_t  symbolCount;

  // Class Table (ClassId -> Class Oop)
  Oop       classes[kMaxClasses];
  uint16_t  classCount;

  // Global System Dictionary (`Smalltalk`)
  Oop       globalsDict;

  // Active Execution Context (`thisContext`)
  Oop       activeContext;

  // Temporary GC roots during compilation / primitives
  Oop       tempRoots[kMaxTempRoots];
  uint16_t  tempRootCount;
  bool      pinNewObjects;

  // Cached Common Selector Symbols for Fast Dispatch
  Oop       symPlus;
  Oop       symMinus;
  Oop       symMul;
  Oop       symDiv;
  Oop       symMod;
  Oop       symLT;
  Oop       symLE;
  Oop       symGT;
  Oop       symGE;
  Oop       symEQ;
  Oop       symNE;
  Oop       symIdentEQ;
  Oop       symAt;
  Oop       symAtPut;
  Oop       symSize;
  Oop       symValue;
  Oop       symValueArg;
  Oop       symValueArg2;
  Oop       symDoesNotUnderstand;
  Oop       symUnknownSelector;
  Oop       symPrintString;

  // Transcript Ring/Text Buffer
  char      transcript[kTranscriptCap];
  size_t    transcriptLen;
  bool      transcriptDirty;

  // Last Error / Status Message
  char      lastError[128];

  // UI Hook Callbacks (registered by Hybrid Window Manager)
  void      (*onOpenWindow)(int winType, Oop argOop, void* uiUser);
  void*     uiUser;

  // Scratch buffers for Mark-Compact GC (avoids large stack arrays on ESP32)
  uint16_t  gcWorklist[256];
  uint16_t  gcLiveIndices[kMaxObjects];
};

// ---------------------------------------------------------------------------
// Memory & Object Manipulation API
// ---------------------------------------------------------------------------
bool      stMemInit(VMState* vm, const LakatosApi* os);
void      stMemDestroy(VMState* vm);
void      stRunGC(VMState* vm);

uint16_t  stAllocObj(VMState* vm, uint16_t classIdx, ObjFormat fmt, uint16_t size, uint8_t flags = 0);
Oop       stNewOopSlots(VMState* vm, uint16_t classIdx, uint16_t numSlots, uint8_t flags = 0);
Oop       stNewBytes(VMState* vm, uint16_t classIdx, ObjFormat fmt, const void* src, uint16_t byteLen);
Oop       stNewString(VMState* vm, const char* str, int len = -1);
Oop       stInternSymbol(VMState* vm, const char* str, int len = -1);
Oop       stNewFloat(VMState* vm, float val);
float     stAsFloat(VMState* vm, Oop o);
bool      stIsNumber(VMState* vm, Oop o);
float     stToFloat(VMState* vm, Oop o);
Oop       stNewArray(VMState* vm, uint16_t len);
Oop       stNewByteArray(VMState* vm, uint16_t len);
Oop       stNewPoint(VMState* vm, Oop x, Oop y);

// Safe Slot & Byte Accessors
ObjHeader* stGetHeader(VMState* vm, Oop o);
uint16_t   stGetClassIndex(VMState* vm, Oop o);
Oop        stGetClassOop(VMState* vm, Oop o);

Oop*       stOopSlots(VMState* vm, Oop o);
uint8_t*   stByteData(VMState* vm, Oop o);
uint16_t   stObjSize(VMState* vm, Oop o);

Oop        stSlotAt(VMState* vm, Oop o, uint16_t idx);
void       stSlotAtPut(VMState* vm, Oop o, uint16_t idx, Oop val);

bool       stCStringEquals(VMState* vm, Oop strOrSym, const char* cstr);
void       stCopyCString(VMState* vm, Oop strOrSym, char* outBuf, size_t maxLen);

// Dictionary Helpers (used for MethodDictionaries, ClassVars, and Smalltalk globals)
Oop        stNewDictionary(VMState* vm, uint16_t initialCap = 8);
Oop        stDictAt(VMState* vm, Oop dict, Oop key, Oop defaultVal = kOopNull);
void       stDictAtPut(VMState* vm, Oop dict, Oop key, Oop val);
bool       stDictRemoveKey(VMState* vm, Oop dict, Oop key);
bool       stOopEquals(VMState* vm, Oop a, Oop b);

// Class Creation & Method Installation Helpers
Oop        stCreateClass(VMState* vm, uint16_t fixedClsIdx, const char* name,
                         Oop superCls, const char* instVarNamesCsv);
int        stFindInstVarIndex(VMState* vm, Oop clsOop, Oop symName);
uint16_t   stClassInstSize(VMState* vm, Oop clsOop);
Oop        stLookupMethod(VMState* vm, Oop rcvr, Oop selector, Oop startClsOop = kOopNull);

// Transcript Helpers
void       stTranscriptAppend(VMState* vm, const char* text, size_t len);
void       stTranscriptAppendCStr(VMState* vm, const char* text);
void       stTranscriptClear(VMState* vm);

// Temp Root Guard
static inline void stPushRoot(VMState* vm, Oop o) {
  if (vm->tempRootCount < kMaxTempRoots) {
    vm->tempRoots[vm->tempRootCount++] = o;
  }
}
static inline void stPopRoots(VMState* vm, uint16_t savedCount) {
  if (savedCount <= kMaxTempRoots) {
    vm->tempRootCount = savedCount;
  }
}
