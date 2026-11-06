#pragma once

#include "st_memory.h"

// Compiles a Smalltalk-80 method source string and installs it into `clsOop`
// (either in `instanceMethods` or `classMethods` depending on `isClassMethod`).
// If `bootstrapSrcIdx >= 0`, stores the compact integer index in SLOT_METH_SOURCE
// instead of allocating a duplicate heap String for `.rodata` bootstrap methods.
// Returns the new `CompiledMethod` Oop, or `kOopNil` on syntax error (setting `vm->lastError`).
Oop stCompileMethod(VMState* vm, Oop clsOop, bool isClassMethod,
                    const char* src, int bootstrapSrcIdx = -1);

// Compiles a standalone Workspace / File-In Smalltalk-80 expression or statement list
// into an anonymous `CompiledMethod` targeting `receiverClsOop` (default `UndefinedObject`).
// Returns the `CompiledMethod` Oop, or `kOopNil` on syntax error (setting `vm->lastError`).
Oop stCompileExpression(VMState* vm, const char* src, Oop receiverClsOop = kOopNull);
