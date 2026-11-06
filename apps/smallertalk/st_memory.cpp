#include "st_memory.h"
#include <string.h>

static inline uint32_t wordsForFormat(ObjFormat fmt, uint16_t size) {
  if (fmt == FMT_OOP_SLOTS) {
    return (uint32_t)size;
  }
  // Byte-oriented formats: round up to 4-byte words (+1 byte for NUL terminator on strings/symbols)
  uint32_t bytes = (uint32_t)size + ((fmt == FMT_STRING || fmt == FMT_SYMBOL) ? 1u : 0u);
  return (bytes + 3u) >> 2;
}

bool stMemInit(VMState* vm, const LakatosApi* os) {
  memset(vm, 0, sizeof(VMState));
  vm->os               = os;
  vm->freeObjHint      = 1;
  vm->payloadUsedWords = 0;
  vm->globalsDict      = kOopNil;
  vm->activeContext    = kOopNil;
  vm->pinNewObjects    = false;

  vm->objects = (ObjHeader*)os->malloc(sizeof(ObjHeader) * kMaxObjects);
  vm->payload = (uint32_t*)os->malloc(sizeof(uint32_t) * kPayloadWords);
  if (!vm->objects || !vm->payload) {
    if (vm->objects) { os->free(vm->objects); vm->objects = nullptr; }
    if (vm->payload) { os->free(vm->payload); vm->payload = nullptr; }
    return false;
  }
  memset(vm->objects, 0, sizeof(ObjHeader) * kMaxObjects);
  memset(vm->payload, 0, sizeof(uint32_t) * kPayloadWords);
  return true;
}

void stMemDestroy(VMState* vm) {
  if (!vm || !vm->os) return;
  if (vm->payload) {
    vm->os->free(vm->payload);
    vm->payload = nullptr;
  }
  if (vm->objects) {
    vm->os->free(vm->objects);
    vm->objects = nullptr;
  }
}

ObjHeader* stGetHeader(VMState* vm, Oop o) {
  if (!isHeapOop(o)) return nullptr;
  uint16_t idx = asObjIndex(o);
  if (idx == 0 || idx >= kMaxObjects) return nullptr;
  if (vm->objects[idx].format == FMT_FREE) return nullptr;
  return &vm->objects[idx];
}

uint16_t stGetClassIndex(VMState* vm, Oop o) {
  if (isSmallInt(o)) return CLS_SMALL_INTEGER;
  if (isChar(o))     return CLS_CHARACTER;
  if (o == kOopNil)  return CLS_UNDEFINED_OBJECT;
  if (o == kOopTrue) return CLS_TRUE;
  if (o == kOopFalse)return CLS_FALSE;
  ObjHeader* h = stGetHeader(vm, o);
  return h ? h->classIdx : CLS_OBJECT;
}

Oop stGetClassOop(VMState* vm, Oop o) {
  uint16_t cid = stGetClassIndex(vm, o);
  if (cid > 0 && cid < kMaxClasses && vm->classes[cid] != kOopNull) {
    return vm->classes[cid];
  }
  return vm->classes[CLS_OBJECT];
}

Oop* stOopSlots(VMState* vm, Oop o) {
  ObjHeader* h = stGetHeader(vm, o);
  if (!h || h->format != FMT_OOP_SLOTS) return nullptr;
  return (Oop*)(&vm->payload[h->payloadOff]);
}

uint8_t* stByteData(VMState* vm, Oop o) {
  ObjHeader* h = stGetHeader(vm, o);
  if (!h || h->format == FMT_OOP_SLOTS || h->format == FMT_FREE) return nullptr;
  return (uint8_t*)(&vm->payload[h->payloadOff]);
}

uint16_t stObjSize(VMState* vm, Oop o) {
  ObjHeader* h = stGetHeader(vm, o);
  return h ? h->size : 0;
}

Oop stSlotAt(VMState* vm, Oop o, uint16_t idx) {
  ObjHeader* h = stGetHeader(vm, o);
  if (!h || h->format != FMT_OOP_SLOTS || idx >= h->size) return kOopNil;
  return vm->payload[h->payloadOff + idx];
}

void stSlotAtPut(VMState* vm, Oop o, uint16_t idx, Oop val) {
  ObjHeader* h = stGetHeader(vm, o);
  if (!h || h->format != FMT_OOP_SLOTS || idx >= h->size) return;
  vm->payload[h->payloadOff + idx] = val;
}

// ---------------------------------------------------------------------------
// Mark & Compact Garbage Collector
// ---------------------------------------------------------------------------
static void markOop(VMState* vm, Oop root) {
  if (!isHeapOop(root)) return;

  // Iterative mark stack using VMState scratch indices to avoid C++ stack pressure on ESP32
  uint16_t* worklist = vm->gcWorklist;
  int sp = 0;
  uint16_t startIdx = asObjIndex(root);
  if (startIdx == 0 || startIdx >= kMaxObjects) return;
  if (vm->objects[startIdx].format == FMT_FREE) return;
  if (vm->objects[startIdx].flags & OBJ_FLAG_MARKED) return;

  vm->objects[startIdx].flags |= OBJ_FLAG_MARKED;
  worklist[sp++] = startIdx;

  while (sp > 0) {
    uint16_t idx = worklist[--sp];
    ObjHeader& h = vm->objects[idx];

    // Also mark the class object if valid
    if (h.classIdx > 0 && h.classIdx < kMaxClasses) {
      Oop clsOop = vm->classes[h.classIdx];
      if (isHeapOop(clsOop)) {
        uint16_t cIdx = asObjIndex(clsOop);
        if (cIdx > 0 && cIdx < kMaxObjects &&
            vm->objects[cIdx].format != FMT_FREE &&
            !(vm->objects[cIdx].flags & OBJ_FLAG_MARKED)) {
          vm->objects[cIdx].flags |= OBJ_FLAG_MARKED;
          if (sp < 256) worklist[sp++] = cIdx;
        }
      }
    }

    if (h.format == FMT_OOP_SLOTS) {
      Oop* slots = &vm->payload[h.payloadOff];
      for (uint16_t i = 0; i < h.size; i++) {
        Oop child = slots[i];
        if (isHeapOop(child)) {
          uint16_t chIdx = asObjIndex(child);
          if (chIdx > 0 && chIdx < kMaxObjects &&
              vm->objects[chIdx].format != FMT_FREE &&
              !(vm->objects[chIdx].flags & OBJ_FLAG_MARKED)) {
            vm->objects[chIdx].flags |= OBJ_FLAG_MARKED;
            if (sp < 256) {
              worklist[sp++] = chIdx;
            } else {
              // Fallback recursion if worklist overflows
              markOop(vm, child);
            }
          }
        }
      }
    }
  }
}

void stRunGC(VMState* vm) {
  // 1. If pinNewObjects is NOT active, clear existing mark bits;
  //    if pinNewObjects IS active (e.g. inside a primitive or compiler),
  //    preserve any pre-marked newly allocated objects!
  if (!vm->pinNewObjects) {
    for (uint16_t i = 1; i < kMaxObjects; i++) {
      vm->objects[i].flags &= ~OBJ_FLAG_MARKED;
    }
  } else {
    // Expand transitive closure of already-marked objects
    for (uint16_t i = 1; i < kMaxObjects; i++) {
      if (vm->objects[i].format != FMT_FREE && (vm->objects[i].flags & OBJ_FLAG_MARKED)) {
        ObjHeader& h = vm->objects[i];
        if (h.format == FMT_OOP_SLOTS) {
          Oop* slots = &vm->payload[h.payloadOff];
          for (uint16_t k = 0; k < h.size; k++) {
            markOop(vm, slots[k]);
          }
        }
      }
    }
  }

  // 2. Mark all VM roots
  for (uint16_t i = 0; i < vm->symbolCount; i++) {
    markOop(vm, vm->symbols[i]);
  }
  for (uint16_t i = 1; i < kMaxClasses; i++) {
    if (vm->classes[i] != kOopNull) {
      markOop(vm, vm->classes[i]);
    }
  }
  markOop(vm, vm->globalsDict);
  markOop(vm, vm->activeContext);
  for (uint16_t i = 0; i < vm->tempRootCount; i++) {
    markOop(vm, vm->tempRoots[i]);
  }

  // 3. Sweep unmarked objects; build sorted list of live objects by payloadOff for sliding compaction
  // Since payloadOff is monotonic on allocation, we can compact cleanly by sorting live indices by payloadOff
  // or copying through a 2-pass word move.
  //Wait: can live objects be out of payloadOff order? Only if an object was allocated after a previous GC,
  // which STILL appends to the end of `payloadUsedWords`!
  // Let's verify: every allocation appends at `payloadUsedWords`, and every compaction slides live objects
  // in ascending `payloadOff` order! Therefore, for all live objects with `words > 0`, sorting them by
  // `payloadOff` is guaranteed to never overwrite unread words when sliding down!
  uint16_t liveCount = 0;
  uint16_t firstFree = kMaxObjects;
  for (uint16_t i = 1; i < kMaxObjects; i++) {
    ObjHeader& h = vm->objects[i];
    if (h.format == FMT_FREE) {
      if (i < firstFree) firstFree = i;
      continue;
    }
    if (!(h.flags & OBJ_FLAG_MARKED)) {
      h.format     = FMT_FREE;
      h.flags      = 0;
      h.classIdx   = 0;
      h.size       = 0;
      h.payloadOff = 0;
      if (i < firstFree) firstFree = i;
    } else {
      if (!vm->pinNewObjects) {
        h.flags &= ~OBJ_FLAG_MARKED;
      }
      liveCount++;
    }
  }

  // Compact payload words in ascending payloadOff order
  uint16_t* liveIndices = vm->gcLiveIndices;
  uint16_t nLiveWithPayload = 0;
  for (uint16_t i = 1; i < kMaxObjects; i++) {
    ObjHeader& h = vm->objects[i];
    if (h.format != FMT_FREE) {
      uint32_t w = wordsForFormat((ObjFormat)h.format, h.size);
      if (w > 0) {
        liveIndices[nLiveWithPayload++] = i;
      } else {
        h.payloadOff = 0;
      }
    }
  }

  // Insertion sort `liveIndices` by `payloadOff` (it is already nearly 100% sorted!)
  for (uint16_t i = 1; i < nLiveWithPayload; i++) {
    uint16_t keyIdx = liveIndices[i];
    uint16_t keyOff = vm->objects[keyIdx].payloadOff;
    int j = (int)i - 1;
    while (j >= 0 && vm->objects[liveIndices[j]].payloadOff > keyOff) {
      liveIndices[j + 1] = liveIndices[j];
      j--;
    }
    liveIndices[j + 1] = keyIdx;
  }

  uint32_t writeOff = 0;
  for (uint16_t i = 0; i < nLiveWithPayload; i++) {
    ObjHeader& h = vm->objects[liveIndices[i]];
    uint32_t w = wordsForFormat((ObjFormat)h.format, h.size);
    if (h.payloadOff != writeOff) {
      memmove(&vm->payload[writeOff], &vm->payload[h.payloadOff], w * sizeof(uint32_t));
      h.payloadOff = (uint16_t)writeOff;
    }
    writeOff += w;
  }

  vm->payloadUsedWords = writeOff;
  vm->liveObjCount     = liveCount;
  vm->freeObjHint      = (firstFree < kMaxObjects) ? firstFree : 1;
  vm->gcCount++;
}

uint16_t stAllocObj(VMState* vm, uint16_t classIdx, ObjFormat fmt, uint16_t size, uint8_t flags) {
  uint32_t wordsNeeded = wordsForFormat(fmt, size);
  if (vm->payloadUsedWords + wordsNeeded > kPayloadWords || vm->liveObjCount + 1 >= kMaxObjects) {
    stRunGC(vm);
  }
  if (vm->payloadUsedWords + wordsNeeded > kPayloadWords) {
    return 0;  // Out of payload memory
  }

  uint16_t foundIdx = 0;
  for (uint16_t pass = 0; pass < 2 && foundIdx == 0; pass++) {
    uint16_t start = (pass == 0) ? vm->freeObjHint : 1;
    for (uint16_t i = start; i < kMaxObjects; i++) {
      if (vm->objects[i].format == FMT_FREE) {
        foundIdx = i;
        vm->freeObjHint = i + 1;
        break;
      }
    }
    if (foundIdx == 0 && pass == 0) {
      stRunGC(vm);
    }
  }
  if (foundIdx == 0) return 0;

  uint16_t payOff = (uint16_t)vm->payloadUsedWords;
  vm->payloadUsedWords += wordsNeeded;
  vm->liveObjCount++;

  ObjHeader& h = vm->objects[foundIdx];
  h.classIdx   = classIdx;
  h.format     = (uint8_t)fmt;
  h.flags      = flags | (vm->pinNewObjects ? OBJ_FLAG_MARKED : 0u);
  h.size       = size;
  h.payloadOff = payOff;

  if (wordsNeeded > 0) {
    if (fmt == FMT_OOP_SLOTS) {
      for (uint32_t i = 0; i < wordsNeeded; i++) {
        vm->payload[payOff + i] = kOopNil;
      }
    } else {
      memset(&vm->payload[payOff], 0, wordsNeeded * sizeof(uint32_t));
    }
  }
  return foundIdx;
}

Oop stNewOopSlots(VMState* vm, uint16_t classIdx, uint16_t numSlots, uint8_t flags) {
  uint16_t idx = stAllocObj(vm, classIdx, FMT_OOP_SLOTS, numSlots, flags);
  return idx ? makeHeapOop(idx) : kOopNil;
}

Oop stNewBytes(VMState* vm, uint16_t classIdx, ObjFormat fmt, const void* src, uint16_t byteLen) {
  uint16_t idx = stAllocObj(vm, classIdx, fmt, byteLen, 0);
  if (!idx) return kOopNil;
  Oop o = makeHeapOop(idx);
  uint8_t* dst = stByteData(vm, o);
  if (src && byteLen > 0 && dst) {
    memcpy(dst, src, byteLen);
  }
  if ((fmt == FMT_STRING || fmt == FMT_SYMBOL) && dst) {
    dst[byteLen] = '\0';
  }
  return o;
}

Oop stNewString(VMState* vm, const char* str, int len) {
  if (!str) str = "";
  if (len < 0) len = (int)strlen(str);
  return stNewBytes(vm, CLS_STRING, FMT_STRING, str, (uint16_t)len);
}

Oop stInternSymbol(VMState* vm, const char* str, int len) {
  if (!str) str = "";
  if (len < 0) len = (int)strlen(str);
  for (uint16_t i = 0; i < vm->symbolCount; i++) {
    Oop sym = vm->symbols[i];
    ObjHeader* h = stGetHeader(vm, sym);
    if (h && h->size == (uint16_t)len) {
      const char* sdata = (const char*)stByteData(vm, sym);
      if (sdata && memcmp(sdata, str, (size_t)len) == 0) {
        return sym;
      }
    }
  }
  Oop newSym = stNewBytes(vm, CLS_SYMBOL, FMT_SYMBOL, str, (uint16_t)len);
  if (newSym != kOopNil && vm->symbolCount < kMaxSymbols) {
    vm->symbols[vm->symbolCount++] = newSym;
  }
  return newSym;
}

Oop stNewFloat(VMState* vm, float val) {
  return stNewBytes(vm, CLS_FLOAT, FMT_BYTES, &val, sizeof(float));
}

float stAsFloat(VMState* vm, Oop o) {
  if (isSmallInt(o)) return (float)asSmallInt(o);
  ObjHeader* h = stGetHeader(vm, o);
  if (h && h->classIdx == CLS_FLOAT && h->size == sizeof(float)) {
    float v = 0.0f;
    memcpy(&v, stByteData(vm, o), sizeof(float));
    return v;
  }
  return 0.0f;
}

bool stIsNumber(VMState* vm, Oop o) {
  if (isSmallInt(o)) return true;
  ObjHeader* h = stGetHeader(vm, o);
  return h && h->classIdx == CLS_FLOAT;
}

float stToFloat(VMState* vm, Oop o) {
  return stAsFloat(vm, o);
}

Oop stNewArray(VMState* vm, uint16_t len) {
  return stNewOopSlots(vm, CLS_ARRAY, len, 0);
}

Oop stNewByteArray(VMState* vm, uint16_t len) {
  return stNewBytes(vm, CLS_BYTE_ARRAY, FMT_BYTES, nullptr, len);
}

Oop stNewPoint(VMState* vm, Oop x, Oop y) {
  uint16_t saved = vm->tempRootCount;
  stPushRoot(vm, x);
  stPushRoot(vm, y);
  Oop pt = stNewOopSlots(vm, CLS_POINT, SLOT_POINT_COUNT, 0);
  if (pt != kOopNil) {
    stSlotAtPut(vm, pt, SLOT_POINT_X, x);
    stSlotAtPut(vm, pt, SLOT_POINT_Y, y);
  }
  stPopRoots(vm, saved);
  return pt;
}

bool stCStringEquals(VMState* vm, Oop strOrSym, const char* cstr) {
  if (!cstr) return false;
  ObjHeader* h = stGetHeader(vm, strOrSym);
  if (!h || (h->format != FMT_STRING && h->format != FMT_SYMBOL)) return false;
  size_t clen = strlen(cstr);
  if (h->size != clen) return false;
  const uint8_t* b = stByteData(vm, strOrSym);
  return b && memcmp(b, cstr, clen) == 0;
}

void stCopyCString(VMState* vm, Oop strOrSym, char* outBuf, size_t maxLen) {
  if (!outBuf || maxLen == 0) return;
  outBuf[0] = '\0';
  ObjHeader* h = stGetHeader(vm, strOrSym);
  if (!h || (h->format != FMT_STRING && h->format != FMT_SYMBOL)) return;
  size_t n = h->size;
  if (n >= maxLen) n = maxLen - 1;
  const uint8_t* b = stByteData(vm, strOrSym);
  if (b && n > 0) {
    memcpy(outBuf, b, n);
  }
  outBuf[n] = '\0';
}

bool stOopEquals(VMState* vm, Oop a, Oop b) {
  if (a == b) return true;
  if (isSmallInt(a) && isSmallInt(b)) return a == b;
  ObjHeader* ha = stGetHeader(vm, a);
  ObjHeader* hb = stGetHeader(vm, b);
  if (!ha || !hb) return false;
  if ((ha->format == FMT_STRING || ha->format == FMT_SYMBOL) &&
      (hb->format == FMT_STRING || hb->format == FMT_SYMBOL)) {
    if (ha->size != hb->size) return false;
    return memcmp(stByteData(vm, a), stByteData(vm, b), ha->size) == 0;
  }
  if (ha->classIdx == CLS_FLOAT && hb->classIdx == CLS_FLOAT) {
    return stAsFloat(vm, a) == stAsFloat(vm, b);
  }
  return false;
}

// ---------------------------------------------------------------------------
// Dictionary Operations
// ---------------------------------------------------------------------------
Oop stNewDictionary(VMState* vm, uint16_t initialCap) {
  if (initialCap < 4) initialCap = 4;
  uint16_t saved = vm->tempRootCount;
  Oop dict = stNewOopSlots(vm, CLS_DICTIONARY, SLOT_DICT_COUNT, 0);
  stPushRoot(vm, dict);
  Oop keys = stNewArray(vm, initialCap);
  stPushRoot(vm, keys);
  Oop vals = stNewArray(vm, initialCap);
  stSlotAtPut(vm, dict, SLOT_DICT_KEYS, keys);
  stSlotAtPut(vm, dict, SLOT_DICT_VALUES, vals);
  stSlotAtPut(vm, dict, SLOT_DICT_TALLY, makeSmallInt(0));
  stPopRoots(vm, saved);
  return dict;
}

Oop stDictAt(VMState* vm, Oop dict, Oop key, Oop defaultVal) {
  ObjHeader* hd = stGetHeader(vm, dict);
  if (!hd || hd->format != FMT_OOP_SLOTS || hd->size < SLOT_DICT_COUNT) return defaultVal;
  Oop keys = stSlotAt(vm, dict, SLOT_DICT_KEYS);
  Oop vals = stSlotAt(vm, dict, SLOT_DICT_VALUES);
  Oop tallyOop = stSlotAt(vm, dict, SLOT_DICT_TALLY);
  uint16_t tally = isSmallInt(tallyOop) ? (uint16_t)asSmallInt(tallyOop) : 0;
  uint16_t cap = stObjSize(vm, keys);
  if (tally > cap) tally = cap;

  Oop* kSlots = stOopSlots(vm, keys);
  Oop* vSlots = stOopSlots(vm, vals);
  if (!kSlots || !vSlots) return defaultVal;

  for (uint16_t i = 0; i < tally; i++) {
    if (stOopEquals(vm, kSlots[i], key)) {
      return vSlots[i];
    }
  }
  return defaultVal;
}

void stDictAtPut(VMState* vm, Oop dict, Oop key, Oop val) {
  ObjHeader* hd = stGetHeader(vm, dict);
  if (!hd || hd->format != FMT_OOP_SLOTS || hd->size < SLOT_DICT_COUNT) return;

  uint16_t saved = vm->tempRootCount;
  stPushRoot(vm, dict);
  stPushRoot(vm, key);
  stPushRoot(vm, val);

  Oop keys = stSlotAt(vm, dict, SLOT_DICT_KEYS);
  Oop vals = stSlotAt(vm, dict, SLOT_DICT_VALUES);
  Oop tallyOop = stSlotAt(vm, dict, SLOT_DICT_TALLY);
  uint16_t tally = isSmallInt(tallyOop) ? (uint16_t)asSmallInt(tallyOop) : 0;
  uint16_t cap = stObjSize(vm, keys);

  Oop* kSlots = stOopSlots(vm, keys);
  Oop* vSlots = stOopSlots(vm, vals);
  if (kSlots && vSlots) {
    for (uint16_t i = 0; i < tally && i < cap; i++) {
      if (stOopEquals(vm, kSlots[i], key)) {
        vSlots[i] = val;
        stPopRoots(vm, saved);
        return;
      }
    }
  }

  if (tally >= cap) {
    uint16_t newCap = (cap == 0) ? 8 : (cap * 2);
    Oop newKeys = stNewArray(vm, newCap);
    stPushRoot(vm, newKeys);
    Oop newVals = stNewArray(vm, newCap);
    stPushRoot(vm, newVals);
    for (uint16_t i = 0; i < tally && i < cap; i++) {
      stSlotAtPut(vm, newKeys, i, stSlotAt(vm, keys, i));
      stSlotAtPut(vm, newVals, i, stSlotAt(vm, vals, i));
    }
    keys = newKeys;
    vals = newVals;
    stSlotAtPut(vm, dict, SLOT_DICT_KEYS, keys);
    stSlotAtPut(vm, dict, SLOT_DICT_VALUES, vals);
  }

  stSlotAtPut(vm, keys, tally, key);
  stSlotAtPut(vm, vals, tally, val);
  stSlotAtPut(vm, dict, SLOT_DICT_TALLY, makeSmallInt(tally + 1));
  stPopRoots(vm, saved);
}

bool stDictRemoveKey(VMState* vm, Oop dict, Oop key) {
  ObjHeader* hd = stGetHeader(vm, dict);
  if (!hd || hd->format != FMT_OOP_SLOTS || hd->size < SLOT_DICT_COUNT) return false;
  Oop keys = stSlotAt(vm, dict, SLOT_DICT_KEYS);
  Oop vals = stSlotAt(vm, dict, SLOT_DICT_VALUES);
  Oop tallyOop = stSlotAt(vm, dict, SLOT_DICT_TALLY);
  uint16_t tally = isSmallInt(tallyOop) ? (uint16_t)asSmallInt(tallyOop) : 0;
  uint16_t cap = stObjSize(vm, keys);
  if (tally > cap) tally = cap;

  for (uint16_t i = 0; i < tally; i++) {
    if (stOopEquals(vm, stSlotAt(vm, keys, i), key)) {
      for (uint16_t j = i; j + 1 < tally; j++) {
        stSlotAtPut(vm, keys, j, stSlotAt(vm, keys, j + 1));
        stSlotAtPut(vm, vals, j, stSlotAt(vm, vals, j + 1));
      }
      stSlotAtPut(vm, keys, tally - 1, kOopNil);
      stSlotAtPut(vm, vals, tally - 1, kOopNil);
      stSlotAtPut(vm, dict, SLOT_DICT_TALLY, makeSmallInt(tally - 1));
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Class Creation & Simplified Metaclass Method Lookup
// ---------------------------------------------------------------------------
static Oop parseSpaceSeparatedSymbols(VMState* vm, const char* str) {
  if (!str) return stNewArray(vm, 0);
  Oop tempSyms[32];
  uint16_t count = 0;
  const char* p = str;
  while (*p && count < 32) {
    while (*p == ' ' || *p == '\t' || *p == ',' || *p == '\r' || *p == '\n') p++;
    if (!*p) break;
    const char* start = p;
    while (*p && *p != ' ' && *p != '\t' && *p != ',' && *p != '\r' && *p != '\n') p++;
    tempSyms[count++] = stInternSymbol(vm, start, (int)(p - start));
  }
  Oop arr = stNewArray(vm, count);
  for (uint16_t i = 0; i < count; i++) {
    stSlotAtPut(vm, arr, i, tempSyms[i]);
  }
  return arr;
}

uint16_t stClassInstSize(VMState* vm, Oop clsOop) {
  if (!isHeapOop(clsOop) || clsOop == kOopNil) return 0;
  Oop szOop = stSlotAt(vm, clsOop, SLOT_CLS_INST_SIZE);
  return isSmallInt(szOop) ? (uint16_t)asSmallInt(szOop) : 0;
}

Oop stCreateClass(VMState* vm, uint16_t fixedClsIdx, const char* name,
                  Oop superCls, const char* instVarNamesCsv) {
  uint16_t saved = vm->tempRootCount;
  stPushRoot(vm, superCls);

  Oop nameSym = stInternSymbol(vm, name);
  stPushRoot(vm, nameSym);

  // Check if class with this name already exists in Smalltalk globals
  uint16_t clsId = fixedClsIdx;
  Oop clsOop = kOopNil;
  if (clsId == 0) {
    // Check if updating an existing class
    if (vm->globalsDict != kOopNil) {
      Oop existing = stDictAt(vm, vm->globalsDict, nameSym, kOopNull);
      if (existing != kOopNull && isHeapOop(existing)) {
        ObjHeader* eh = stGetHeader(vm, existing);
        if (eh && (eh->flags & OBJ_FLAG_CLASS_OBJ)) {
          clsOop = existing;
          for (uint16_t i = 1; i < kMaxClasses; i++) {
            if (vm->classes[i] == existing) {
              clsId = i;
              break;
            }
          }
        }
      }
    }
    if (clsId == 0) {
      if (vm->classCount < CLS_FIRST_USER) vm->classCount = CLS_FIRST_USER;
      if (vm->classCount < kMaxClasses) {
        clsId = vm->classCount++;
      } else {
        stPopRoots(vm, saved);
        return kOopNil;
      }
    }
  } else {
    if (clsId >= vm->classCount) vm->classCount = clsId + 1;
  }

  Oop ivarArr = parseSpaceSeparatedSymbols(vm, instVarNamesCsv);
  stPushRoot(vm, ivarArr);
  uint16_t superInstSize = stClassInstSize(vm, superCls);
  uint16_t totalInstSize = superInstSize + stObjSize(vm, ivarArr);

  if (clsOop == kOopNil) {
    clsOop = stNewOopSlots(vm, CLS_CLASS, SLOT_CLS_COUNT, OBJ_FLAG_CLASS_OBJ);
    stPushRoot(vm, clsOop);
    Oop instMeths = stNewDictionary(vm, 12);
    stPushRoot(vm, instMeths);
    Oop clsMeths  = stNewDictionary(vm, 4);
    stPushRoot(vm, clsMeths);
    Oop clsVars   = stNewDictionary(vm, 4);
    stPushRoot(vm, clsVars);

    stSlotAtPut(vm, clsOop, SLOT_CLS_NAME,           nameSym);
    stSlotAtPut(vm, clsOop, SLOT_CLS_SUPERCLASS,     superCls);
    stSlotAtPut(vm, clsOop, SLOT_CLS_INST_METHODS,   instMeths);
    stSlotAtPut(vm, clsOop, SLOT_CLS_CLASS_METHODS,  clsMeths);
    stSlotAtPut(vm, clsOop, SLOT_CLS_INST_VAR_NAMES, ivarArr);
    stSlotAtPut(vm, clsOop, SLOT_CLS_INST_SIZE,      makeSmallInt(totalInstSize));
    stSlotAtPut(vm, clsOop, SLOT_CLS_CLASS_VARS,     clsVars);
  } else {
    stSlotAtPut(vm, clsOop, SLOT_CLS_SUPERCLASS,     superCls);
    stSlotAtPut(vm, clsOop, SLOT_CLS_INST_VAR_NAMES, ivarArr);
    stSlotAtPut(vm, clsOop, SLOT_CLS_INST_SIZE,      makeSmallInt(totalInstSize));
  }

  vm->classes[clsId] = clsOop;
  if (vm->globalsDict != kOopNil) {
    stDictAtPut(vm, vm->globalsDict, nameSym, clsOop);
  }
  stPopRoots(vm, saved);
  return clsOop;
}

int stFindInstVarIndex(VMState* vm, Oop clsOop, Oop symName) {
  if (!isHeapOop(clsOop) || clsOop == kOopNil) return -1;
  Oop superCls = stSlotAt(vm, clsOop, SLOT_CLS_SUPERCLASS);
  int superIdx = stFindInstVarIndex(vm, superCls, symName);
  if (superIdx >= 0) return superIdx;

  uint16_t baseOffset = stClassInstSize(vm, superCls);
  Oop ivars = stSlotAt(vm, clsOop, SLOT_CLS_INST_VAR_NAMES);
  uint16_t n = stObjSize(vm, ivars);
  for (uint16_t i = 0; i < n; i++) {
    if (stSlotAt(vm, ivars, i) == symName) {
      return (int)(baseOffset + i);
    }
  }
  return -1;
}

Oop stLookupMethod(VMState* vm, Oop rcvr, Oop selector, Oop startClsOop) {
  ObjHeader* rh = stGetHeader(vm, rcvr);
  bool isClassReceiver = (rh != nullptr) && ((rh->flags & OBJ_FLAG_CLASS_OBJ) != 0);

  if (isClassReceiver) {
    // 1. Search classMethods up the class hierarchy
    Oop cur = (startClsOop != kOopNull) ? startClsOop : rcvr;
    int depth = 0;
    while (isHeapOop(cur) && cur != kOopNil && depth++ < 32) {
      Oop cdict = stSlotAt(vm, cur, SLOT_CLS_CLASS_METHODS);
      Oop m = stDictAt(vm, cdict, selector, kOopNull);
      if (m != kOopNull && m != kOopNil) return m;
      cur = stSlotAt(vm, cur, SLOT_CLS_SUPERCLASS);
    }
    // 2. Fallback to Class and Object instanceMethods (so `new`, `name`, `==`, `printString` work on classes)
    cur = vm->classes[CLS_CLASS];
    depth = 0;
    while (isHeapOop(cur) && cur != kOopNil && depth++ < 32) {
      Oop idict = stSlotAt(vm, cur, SLOT_CLS_INST_METHODS);
      Oop m = stDictAt(vm, idict, selector, kOopNull);
      if (m != kOopNull && m != kOopNil) return m;
      cur = stSlotAt(vm, cur, SLOT_CLS_SUPERCLASS);
    }
    return kOopNull;
  }

  // Regular instance (or immediate SmallInteger / Character / Boolean / UndefinedObject)
  Oop cur = (startClsOop != kOopNull) ? startClsOop : stGetClassOop(vm, rcvr);
  int depth = 0;
  while (isHeapOop(cur) && cur != kOopNil && depth++ < 32) {
    Oop idict = stSlotAt(vm, cur, SLOT_CLS_INST_METHODS);
    Oop m = stDictAt(vm, idict, selector, kOopNull);
    if (m != kOopNull && m != kOopNil) return m;
    cur = stSlotAt(vm, cur, SLOT_CLS_SUPERCLASS);
  }
  return kOopNull;
}

// ---------------------------------------------------------------------------
// Transcript Output Buffer
// ---------------------------------------------------------------------------
void stTranscriptAppend(VMState* vm, const char* text, size_t len) {
  if (!text || len == 0) return;
  if (len >= kTranscriptCap) {
    text += (len - (kTranscriptCap - 1));
    len = kTranscriptCap - 1;
  }
  if (vm->transcriptLen + len >= kTranscriptCap) {
    // Drop oldest lines (~1/3 of buffer) to make room
    size_t drop = (vm->transcriptLen + len) - (kTranscriptCap - 512);
    if (drop > vm->transcriptLen) drop = vm->transcriptLen;
    while (drop < vm->transcriptLen && vm->transcript[drop] != '\n') {
      drop++;
    }
    if (drop < vm->transcriptLen && vm->transcript[drop] == '\n') {
      drop++;
    }
    size_t keep = vm->transcriptLen - drop;
    memmove(vm->transcript, vm->transcript + drop, keep);
    vm->transcriptLen = keep;
  }
  memcpy(vm->transcript + vm->transcriptLen, text, len);
  vm->transcriptLen += len;
  vm->transcript[vm->transcriptLen] = '\0';
  vm->transcriptDirty = true;
}

void stTranscriptAppendCStr(VMState* vm, const char* text) {
  if (!text) return;
  stTranscriptAppend(vm, text, strlen(text));
}

void stTranscriptClear(VMState* vm) {
  vm->transcriptLen = 0;
  vm->transcript[0] = '\0';
  vm->transcriptDirty = true;
}
