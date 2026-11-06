#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Tagged 32-Bit Object Pointer (Oop)
// ---------------------------------------------------------------------------
// Bit 0 == 1       : Signed 31-bit SmallInteger (val = ((int32_t)oop) >> 1)
// Bits 0..2 == 010 : Immediate Character        (ch  = (uint8_t)(oop >> 3))
// Bits 0..2 == 110 : Immediate Special Constant (nil=0x06, true=0x0E, false=0x16)
// Bits 0..1 == 00  : Object Table Index         (idx = oop >> 2; 0 = kOopNull)
// ---------------------------------------------------------------------------
typedef uint32_t Oop;

static constexpr Oop kOopNull  = 0x00u;
static constexpr Oop kOopNil   = 0x06u;
static constexpr Oop kOopTrue  = 0x0Eu;
static constexpr Oop kOopFalse = 0x16u;

static inline bool isSmallInt(Oop o) { return (o & 1u) != 0u; }
static inline bool isChar(Oop o)     { return (o & 7u) == 2u; }
static inline bool isSpecial(Oop o)  { return (o & 7u) == 6u; }
static inline bool isHeapOop(Oop o)  { return (o != 0u) && ((o & 3u) == 0u); }

static inline Oop makeSmallInt(int32_t v) {
  return ((uint32_t)v << 1) | 1u;
}
static inline int32_t asSmallInt(Oop o) {
  return ((int32_t)o) >> 1;
}

static inline Oop makeChar(uint8_t c) {
  return ((uint32_t)c << 3) | 2u;
}
static inline uint8_t asChar(Oop o) {
  return (uint8_t)(o >> 3);
}

static inline Oop makeBool(bool b) {
  return b ? kOopTrue : kOopFalse;
}

static inline Oop makeHeapOop(uint16_t objIdx) {
  return ((uint32_t)objIdx) << 2;
}
static inline uint16_t asObjIndex(Oop o) {
  return (uint16_t)(o >> 2);
}

// ---------------------------------------------------------------------------
// Object Formats & GC Flags
// ---------------------------------------------------------------------------
enum ObjFormat : uint8_t {
  FMT_FREE            = 0,
  FMT_OOP_SLOTS       = 1,  // Array of Oop slots (Object, Array, Dictionary, Context, etc.)
  FMT_BYTES           = 2,  // Raw byte array (ByteArray, Float=4B, CompiledMethod)
  FMT_STRING          = 3,  // UTF-8 / ASCII mutable String
  FMT_SYMBOL          = 4,  // Interned immutable Symbol
};

enum ObjFlags : uint8_t {
  OBJ_FLAG_MARKED     = 1u << 0,
  OBJ_FLAG_CLASS_OBJ  = 1u << 1,  // True if this object is itself a Class
};

struct ObjHeader {
  uint16_t classIdx;    // Class index in ClassTable (or ObjIndex of its Class)
  uint8_t  format;      // ObjFormat
  uint8_t  flags;       // ObjFlags
  uint16_t size;        // Number of Oop slots (FMT_OOP_SLOTS) or byte length (FMT_BYTES/STRING/SYMBOL)
  uint16_t payloadOff;  // 32-bit word offset into VMState::payload[]
};

static_assert(sizeof(ObjHeader) == 8, "ObjHeader must be 8 bytes");

// ---------------------------------------------------------------------------
// Fixed Well-Known Class Indices (ObjIndex in Object Table)
// ---------------------------------------------------------------------------
enum BuiltinClassId : uint16_t {
  CLS_NONE                   = 0,
  CLS_OBJECT                 = 1,
  CLS_UNDEFINED_OBJECT       = 2,
  CLS_BOOLEAN                = 3,
  CLS_TRUE                   = 4,
  CLS_FALSE                  = 5,
  CLS_MAGNITUDE              = 6,
  CLS_NUMBER                 = 7,
  CLS_SMALL_INTEGER          = 8,
  CLS_FLOAT                  = 9,
  CLS_CHARACTER              = 10,
  CLS_COLLECTION             = 11,
  CLS_SEQUENCEABLE_COLLECTION= 12,
  CLS_ARRAY                  = 13,
  CLS_BYTE_ARRAY             = 14,
  CLS_STRING                 = 15,
  CLS_SYMBOL                 = 16,
  CLS_ORDERED_COLLECTION     = 17,
  CLS_DICTIONARY             = 18,
  CLS_STREAM                 = 19,
  CLS_READ_STREAM            = 20,
  CLS_WRITE_STREAM           = 21,
  CLS_CLASS                  = 22,
  CLS_COMPILED_METHOD        = 23,
  CLS_BLOCK_CLOSURE          = 24,
  CLS_CONTEXT                = 25,
  CLS_SYSTEM                 = 26,
  CLS_TRANSCRIPT             = 27,
  CLS_FILE                   = 28,
  CLS_DISPLAY                = 29,
  CLS_PEN                    = 30,
  CLS_WORKSPACE              = 31,
  CLS_CLASS_BROWSER          = 32,
  CLS_FILE_BROWSER           = 33,
  CLS_POINT                  = 34,
  CLS_FIRST_USER             = 35,
};

// ---------------------------------------------------------------------------
// Fixed Slot Layouts for Core Heap Objects (FMT_OOP_SLOTS)
// ---------------------------------------------------------------------------

// Class Object (7 Oop slots)
enum ClassSlots : uint16_t {
  SLOT_CLS_NAME             = 0,  // Symbol
  SLOT_CLS_SUPERCLASS       = 1,  // Class Oop or nil
  SLOT_CLS_INST_METHODS     = 2,  // Dictionary (Symbol -> CompiledMethod)
  SLOT_CLS_CLASS_METHODS    = 3,  // Dictionary (Symbol -> CompiledMethod)
  SLOT_CLS_INST_VAR_NAMES   = 4,  // Array of Symbols
  SLOT_CLS_INST_SIZE        = 5,  // SmallInteger total instance variable count
  SLOT_CLS_CLASS_VARS       = 6,  // Dictionary (Symbol -> Oop)
  SLOT_CLS_COUNT            = 7
};

// Dictionary Object (2 Oop slots: keys Array, values Array, tally SmallInt)
enum DictSlots : uint16_t {
  SLOT_DICT_KEYS            = 0,  // Array of Oop
  SLOT_DICT_VALUES          = 1,  // Array of Oop
  SLOT_DICT_TALLY           = 2,  // SmallInteger count of active entries
  SLOT_DICT_COUNT           = 3
};

// OrderedCollection Object (2 Oop slots)
enum OrdCollSlots : uint16_t {
  SLOT_OC_ARRAY             = 0,  // Backing Array
  SLOT_OC_SIZE              = 1,  // SmallInteger active element count
  SLOT_OC_COUNT             = 2
};

// ReadStream / WriteStream Object (3 Oop slots)
enum StreamSlots : uint16_t {
  SLOT_STREAM_COLLECTION    = 0,  // String or Array
  SLOT_STREAM_POS           = 1,  // SmallInteger 0-based index
  SLOT_STREAM_LIMIT         = 2,  // SmallInteger length/limit
  SLOT_STREAM_COUNT         = 3
};

// Point Object (2 Oop slots)
enum PointSlots : uint16_t {
  SLOT_POINT_X              = 0,
  SLOT_POINT_Y              = 1,
  SLOT_POINT_COUNT          = 2
};

// Pen Object (5 Oop slots)
enum PenSlots : uint16_t {
  SLOT_PEN_X                = 0,  // SmallInteger or Float
  SLOT_PEN_Y                = 1,  // SmallInteger or Float
  SLOT_PEN_ANGLE            = 2,  // SmallInteger degrees
  SLOT_PEN_DOWN             = 3,  // Boolean
  SLOT_PEN_COLOR            = 4,  // SmallInteger RGB565
  SLOT_PEN_COUNT            = 5
};

// CompiledMethod Object (FMT_OOP_SLOTS)
enum MethodSlots : uint16_t {
  SLOT_METH_SELECTOR        = 0,  // Symbol
  SLOT_METH_OWNER_CLASS     = 1,  // Class Oop
  SLOT_METH_LITERALS        = 2,  // Array of literal Oops
  SLOT_METH_BYTECODES       = 3,  // ByteArray of instructions
  SLOT_METH_HEADER          = 4,  // SmallInteger packed: numArgs(4b) | numTemps(6b) | primId(10b) | isClassMeth(1b)
  SLOT_METH_SOURCE          = 5,  // String Oop OR SmallInteger index into kBootstrapSources[]
  SLOT_METH_COUNT           = 6
};

static inline int32_t packMethodHeader(uint8_t numArgs, uint8_t numTemps,
                                       uint16_t primId, bool isClassMeth) {
  return (int32_t)((numArgs & 0x0Fu) |
                   ((uint32_t)(numTemps & 0x3Fu) << 4) |
                   ((uint32_t)(primId & 0x3FFu) << 10) |
                   (isClassMeth ? (1u << 20) : 0u));
}
static inline uint8_t unpackMethodNumArgs(int32_t h)    { return (uint8_t)(h & 0x0F); }
static inline uint8_t unpackMethodNumTemps(int32_t h)   { return (uint8_t)((h >> 4) & 0x3F); }
static inline uint16_t unpackMethodPrimId(int32_t h)    { return (uint16_t)((h >> 10) & 0x3FF); }
static inline bool unpackMethodIsClass(int32_t h)       { return ((h >> 20) & 1) != 0; }

// BlockClosure Object (FMT_OOP_SLOTS)
enum BlockSlots : uint16_t {
  SLOT_BLK_HOME_CONTEXT     = 0,  // MethodContext where block was created
  SLOT_BLK_OUTER_CONTEXT    = 1,  // Enclosing MethodContext or BlockContext
  SLOT_BLK_METHOD           = 2,  // CompiledMethod
  SLOT_BLK_START_PC         = 3,  // SmallInteger bytecode offset
  SLOT_BLK_NUM_ARGS         = 4,  // SmallInteger argument count
  SLOT_BLK_TEMP_BASE        = 5,  // SmallInteger temp slot offset in homeContext
  SLOT_BLK_COUNT            = 6
};

// Context Object (MethodContext / BlockContext, FMT_OOP_SLOTS)
enum ContextSlots : uint16_t {
  SLOT_CTX_SENDER           = 0,  // Caller Context or nil
  SLOT_CTX_RECEIVER         = 1,  // Receiver Oop (`self`)
  SLOT_CTX_METHOD           = 2,  // CompiledMethod Oop
  SLOT_CTX_CLOSURE          = 3,  // BlockClosure Oop (or nil for MethodContext)
  SLOT_CTX_HOME             = 4,  // Home MethodContext (or self for MethodContext; marked dead=nil on return)
  SLOT_CTX_PC               = 5,  // SmallInteger bytecode instruction index
  SLOT_CTX_SP               = 6,  // SmallInteger stack pointer (relative to stack base)
  SLOT_CTX_TEMPS_AND_STACK  = 7   // Inline temps [0..numTemps-1] followed by evaluation stack
};

// ---------------------------------------------------------------------------
// Bytecode Instruction Set
// ---------------------------------------------------------------------------
enum Opcode : uint8_t {
  OP_NOP              = 0,
  OP_PUSH_SELF        = 1,
  OP_PUSH_NIL         = 2,
  OP_PUSH_TRUE        = 3,
  OP_PUSH_FALSE       = 4,
  OP_PUSH_CONTEXT     = 5,   // Push active Context (`thisContext`)
  OP_PUSH_SMALLINT_8  = 6,   // [op, int8_t val]
  OP_PUSH_SMALLINT_16 = 7,   // [op, lo, hi] (int16_t)
  OP_PUSH_LITERAL     = 8,   // [op, litIdx]
  OP_PUSH_TEMP        = 9,   // [op, tempIdx] (from homeContext temps)
  OP_STORE_TEMP       = 10,  // [op, tempIdx] (stores top of stack into homeContext temp, leaves on stack)
  OP_PUSH_INST        = 11,  // [op, instIdx] (from receiver instVars)
  OP_STORE_INST       = 12,  // [op, instIdx] (stores top of stack into receiver instVar, leaves on stack)
  OP_PUSH_GLOBAL      = 13,  // [op, litIdx of Symbol] (looks up in classVars then Smalltalk globals)
  OP_STORE_GLOBAL     = 14,  // [op, litIdx of Symbol]
  OP_POP              = 15,  // Discard top of stack
  OP_DUP              = 16,  // Duplicate top of stack (used for cascades `;`)
  OP_SEND             = 17,  // [op, selectorLitIdx, numArgs]
  OP_SUPER_SEND       = 18,  // [op, selectorLitIdx, numArgs]
  OP_JUMP             = 19,  // [op, offsetLo, offsetHi] (signed 16-bit relative to after instruction)
  OP_JUMP_IF_FALSE    = 20,  // [op, offsetLo, offsetHi] (pops top; jumps if false)
  OP_JUMP_IF_TRUE     = 21,  // [op, offsetLo, offsetHi] (pops top; jumps if true)
  OP_BLOCK_COPY       = 22,  // [op, numArgs, tempBase, bodyLenLo, bodyLenHi] -> pushes BlockClosure & skips body
  OP_RETURN_TOP       = 23,  // Method return (`^ top`); if in BlockClosure, performs NON-LOCAL return from homeContext!
  OP_RETURN_SELF      = 24,  // Implicit method return (`^ self`)
  OP_BLOCK_RETURN     = 25,  // End-of-block local return (returns top of stack to block's caller)
  OP_MAKE_ARRAY       = 26,  // [op, count] pops `count` items and pushes new Array
};
