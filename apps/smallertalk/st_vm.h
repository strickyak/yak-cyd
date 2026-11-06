#pragma once

#include "st_memory.h"

// Window instantiation IDs passed to `vm->onOpenWindow(winType, argOop, vm->uiUser)`
enum StUiWindowType {
  ST_WIN_WORKSPACE     = 1,
  ST_WIN_CLASS_BROWSER = 2,
  ST_WIN_FILE_BROWSER  = 3,
};

// Initializes the VM, interning core symbols, creating standard classes,
// and compiling the Smalltalk-80 bootstrap library from `.rodata`.
void stVmInit(VMState* vm, const LakatosApi* os);

// Evaluates a Smalltalk-80 expression string (e.g. from Workspace or File-In)
// and returns the resulting `Oop` (or `kOopNil` on error, setting `vm->lastError`).
Oop  stEval(VMState* vm, const char* exprSrc, Oop receiver = kOopNil, uint32_t maxSteps = 500000);

// Formats any `Oop` into a human-readable Smalltalk `printString` C-string buffer.
void stFormatOop(VMState* vm, Oop obj, char* outBuf, size_t maxLen);

// Retrieves the C-string source code of a `CompiledMethod` (either from `.rodata`
// bootstrap table or from a heap `String` Oop).
const char* stGetMethodSourceCStr(VMState* vm, Oop methOop, char* scratchBuf, size_t scratchCap);
