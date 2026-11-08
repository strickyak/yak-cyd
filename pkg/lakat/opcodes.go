package lakat

import (
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"fmt"
)

// Lakatos .lapp Outer Container Constants (128 bytes)
const (
	LakatosAppMagic        = uint32(0x414B414C) // "LAKA" little-endian
	LakatosHeaderVersion   = uint32(1)
	LakatosAbiVersion      = uint32(1)
	LakatosAppHeaderSize   = 128
	LakatosAppFlagLCode    = uint32(0x00000001)
	LakatosDefaultArenaLen = 131072 // 128 KB flat VM RAM arena
)

// LCode Module Header Constants (24 bytes at offset 0 of payload, or offset 128 of .lapp)
var (
	LCodeMagic     = []byte("LCOD")
	LCodeFormatVer = uint16(1)
	LCodeHeaderSz  = 24
	LCodeFuncEntSz = 20
)

// 32-Bit LCode Opcodes (extended from Par4)
const (
	// Group 1: Stack & Literals (0x00 .. 0x1F)
	OpNOP            = byte(0x00)
	OpPUSH_NIL       = byte(0x01)
	OpPUSH_NIL_SLICE = byte(0x02)
	OpPUSH_TRUE      = byte(0x03)
	OpPUSH_FALSE     = byte(0x04)
	OpPUSH_0         = byte(0x05)
	OpPUSH_1         = byte(0x06)
	OpPUSH_NEG1      = byte(0x07)
	OpPUSH_I8        = byte(0x08)
	OpPUSH_U8        = byte(0x09)
	OpPUSH_I16       = byte(0x0A)
	OpPUSH_STR       = byte(0x0B)
	OpPOP            = byte(0x0C)
	OpPOP_SLICE      = byte(0x0D)
	OpDUP            = byte(0x0E)
	OpSWAP           = byte(0x0F)
	OpDUP2           = byte(0x10)
	OpROT            = byte(0x11)
	OpPUSH_U16       = byte(0x12)
	OpPUSH_I32       = byte(0x13)
	OpPUSH_CSTR      = byte(0x14)
	OpOVER           = byte(0x15)

	// Group 2: Variables, Pointers & Memory (0x20 .. 0x3F)
	OpLOAD_LOCAL_0   = byte(0x20)
	OpLOAD_LOCAL_1   = byte(0x21)
	OpLOAD_LOCAL_2   = byte(0x22)
	OpLOAD_LOCAL_3   = byte(0x23)
	OpSTORE_LOCAL_0  = byte(0x24)
	OpSTORE_LOCAL_1  = byte(0x25)
	OpSTORE_LOCAL_2  = byte(0x26)
	OpSTORE_LOCAL_3  = byte(0x27)
	OpLOAD_LOCAL     = byte(0x28)
	OpSTORE_LOCAL    = byte(0x29)
	OpLOAD_GLOBAL    = byte(0x2A)
	OpSTORE_GLOBAL   = byte(0x2B)
	OpADDR_OF_GLOBAL = byte(0x30)
	OpPEEK2          = byte(0x31) // Read unsigned 16-bit halfword
	OpPOKE2          = byte(0x32) // Write 16-bit halfword
	OpPEEK1          = byte(0x33) // Read unsigned 8-bit byte
	OpPOKE1          = byte(0x34) // Write 8-bit byte
	OpADDR_OF_LOCAL  = byte(0x35)
	OpSHL1_ADD       = byte(0x36) // base + idx * 2
	OpMEM_COPY       = byte(0x37) // memcpy(dst, src, count)
	OpMEM_SET        = byte(0x38) // memset(dst, val, count)
	OpPEEK4          = byte(0x39) // Read 32-bit word
	OpPOKE4          = byte(0x3A) // Write 32-bit word
	OpPEEKS1         = byte(0x3B) // Read signed 8-bit byte (sign-extended to 32-bit)
	OpPEEKS2         = byte(0x3C) // Read signed 16-bit halfword (sign-extended to 32-bit)
	OpSHL2_ADD       = byte(0x3D) // base + idx * 4
	OpINDEX_ADD      = byte(0x3E) // base + idx * imm16u
	OpMEM_CMP        = byte(0x3F) // memcmp(a, b, count) -> -1, 0, 1

	// Group 3: 32-Bit Arithmetic, Logic & Comparisons (0x40 .. 0x5F)
	OpADD     = byte(0x40)
	OpSUB     = byte(0x41)
	OpMUL     = byte(0x42)
	OpDIV     = byte(0x43) // Signed 32-bit division
	OpMOD     = byte(0x44) // Signed 32-bit modulo
	OpNEG     = byte(0x45)
	OpBIT_AND = byte(0x46)
	OpBIT_OR  = byte(0x47)
	OpBIT_XOR = byte(0x48)
	OpBIT_NOT = byte(0x49)
	OpSHL     = byte(0x4A)
	OpSHR     = byte(0x4B) // Arithmetic shift right (signed)
	OpCMP_EQ  = byte(0x4C)
	OpCMP_NE  = byte(0x4D)
	OpCMP_LT  = byte(0x4E) // Signed <
	OpCMP_LE  = byte(0x4F) // Signed <=
	OpCMP_GT  = byte(0x50) // Signed >
	OpCMP_GE  = byte(0x51) // Signed >=
	OpNOT     = byte(0x52)
	OpUDIV    = byte(0x53) // Unsigned 32-bit division
	OpUMOD    = byte(0x54) // Unsigned 32-bit modulo
	OpUSHR    = byte(0x55) // Logical shift right (unsigned)
	OpUCMP_LT = byte(0x56) // Unsigned <
	OpUCMP_LE = byte(0x57) // Unsigned <=
	OpUCMP_GT = byte(0x58) // Unsigned >
	OpUCMP_GE = byte(0x59) // Unsigned >=

	// Group 4: Control Flow & Calls (0x60 .. 0x6F)
	OpJUMP          = byte(0x60)
	OpJUMP_IF_TRUE  = byte(0x61)
	OpJUMP_IF_FALSE = byte(0x62)
	OpCALL          = byte(0x63)
	OpRET           = byte(0x64)
	OpRET_SLICE     = byte(0x65)
	OpRET_VOID      = byte(0x66)
	OpHALT          = byte(0x67)
	OpPANIC         = byte(0x68)

	// Group 5: Slices & Strings (0x70 .. 0x7F)
	OpSLICE_NEW  = byte(0x70) // (base cap len -- slice)
	OpSLICE_LEN  = byte(0x71) // (slice -- len)
	OpSLICE_CAP  = byte(0x72) // (slice -- cap)
	OpSLICE_SUB  = byte(0x73) // (slice start limit elemSz -- subslice)
	OpSLICE_DATA = byte(0x74) // (slice -- base)
	OpSTR_CMP    = byte(0x77) // (s1 s2 -- cmp)

	// Group 6: Managed Heap (0x80 .. 0x87)
	OpMALLOC    = byte(0x80) // (size -- addr)
	OpZALLOC    = byte(0x81) // (size -- addr)
	OpREALLOC   = byte(0x82) // (addr newSize -- newAddr)
	OpFREE      = byte(0x83) // (addr -- )
	OpHEAP_FREE = byte(0x84) // ( -- freeBytes)

	// Group 8: IEEE-754 32-Bit Floating Point (0x90 .. 0xA3)
	OpFADD       = byte(0x90) // (f1 f2 -- f1+f2)
	OpFSUB       = byte(0x91) // (f1 f2 -- f1-f2)
	OpFMUL       = byte(0x92) // (f1 f2 -- f1*f2)
	OpFDIV       = byte(0x93) // (f1 f2 -- f1/f2)
	OpFNEG       = byte(0x94) // (f -- -f)
	OpFABS       = byte(0x95) // (f -- |f|)
	OpFSQRT      = byte(0x96) // (f -- sqrt(f))
	OpFSIN       = byte(0x97) // (rad -- sin(rad))
	OpFCOS       = byte(0x98) // (rad -- cos(rad))
	OpFATAN2     = byte(0x99) // (y x -- atan2(y,x))
	OpFFLOOR     = byte(0x9A) // (f -- floor(f))
	OpFCEIL      = byte(0x9B) // (f -- ceil(f))
	OpFCMP_EQ    = byte(0x9C) // (f1 f2 -- f1==f2)
	OpFCMP_NE    = byte(0x9D) // (f1 f2 -- f1!=f2)
	OpFCMP_LT    = byte(0x9E) // (f1 f2 -- f1<f2)
	OpFCMP_LE    = byte(0x9F) // (f1 f2 -- f1<=f2)
	OpFCMP_GT    = byte(0xA0) // (f1 f2 -- f1>f2)
	OpFCMP_GE    = byte(0xA1) // (f1 f2 -- f1>=f2)
	OpI32_TO_F32 = byte(0xA2) // (i32 -- f32)
	OpF32_TO_I32 = byte(0xA3) // (f32 -- i32)

	// Group 7: Lakatos OS Syscalls & I/O (0xC0 .. 0xCF)
	OpSYSCALL  = byte(0xC0) // 1-byte Syscall ID
	OpSYS_EXIT = byte(0xC8) // (code -- )
	OpIO_PRINT = byte(0xC9) // (strSlice -- )
)

// Lakatos OS Syscall Numbers for OpSYSCALL
const (
	// Lifecycle & Timing (0x00 .. 0x07)
	SysExit    = byte(0x00) // (code -- )
	SysYield   = byte(0x01) // ( -- )
	SysMillis  = byte(0x02) // ( -- ms)
	SysMicros  = byte(0x03) // ( -- us)
	SysDelayMs       = byte(0x04) // (ms -- )
	SysRand          = byte(0x05) // (max -- r)
	SysSetCornerExit = byte(0x06) // (enable -- )

	// Identity, Config & Time (0x08 .. 0x0F)
	SysGetAirport       = byte(0x08) // ( -- strSlice)
	SysGetAirportInfo   = byte(0x09) // (outStructAddr -- ok)
	SysGetDeviceName    = byte(0x0A) // ( -- strSlice)
	SysGetSerialNumber  = byte(0x0B) // ( -- strSlice)
	SysGetShUrl         = byte(0x0C) // ( -- strSlice)
	SysGetUnixTime      = byte(0x0D) // ( -- unixSecs)
	SysFormatLocalTime  = byte(0x0E) // (fmtSlice bufAddr maxLen -- ok)
	SysCityHash32       = byte(0x0F) // (bufAddr len -- hash32)

	// TFT Display (0x10 .. 0x24)
	SysTftFillScreen    = byte(0x10) // (color -- )
	SysTftFillRect      = byte(0x11) // (x y w h color -- )
	SysTftDrawRect      = byte(0x12) // (x y w h color -- )
	SysTftFillRoundRect = byte(0x13) // (x y w h r color -- )
	SysTftDrawRoundRect = byte(0x14) // (x y w h r color -- )
	SysTftDrawLine      = byte(0x15) // (x0 y0 x1 y1 color -- )
	SysTftDrawFastHLine = byte(0x16) // (x y w color -- )
	SysTftDrawFastVLine = byte(0x17) // (x y h color -- )
	SysTftFillCircle    = byte(0x18) // (x y r color -- )
	SysTftDrawCircle    = byte(0x19) // (x y r color -- )
	SysTftFillTriangle  = byte(0x1A) // (x0 y0 x1 y1 x2 y2 color -- )
	SysTftDrawChar      = byte(0x1B) // (x y ch fg bg size -- )
	SysTftSetTextColor  = byte(0x1C) // (fg bg -- )
	SysTftSetTextDatum  = byte(0x1D) // (datum -- )
	SysTftDrawString    = byte(0x1E) // (strSlice x y font -- width)
	SysTftDrawInt       = byte(0x1F) // (val x y font -- width)
	SysTftPushRGB565    = byte(0x20) // (x y w h pixelsAddr -- )
	SysTftDrawCStr      = byte(0x21) // (cstrAddr x y font -- width)
	SysFmtIntToStr      = byte(0x22) // (val bufAddr maxLen -- strSlice)
	SysFmtFloatToStr    = byte(0x23) // (f32 decimals -- strSlice)
	SysTftDrawFloat     = byte(0x24) // (f32 decimals x y font -- width)
	SysTftDrawPngFile   = byte(0x25) // (pathSlice x y -- ok)
	SysTftDrawPngMem    = byte(0x26) // (bufAddr len x y -- ok)
	SysPngInfo          = byte(0x27) // (pathSlice -- ok width height)

	// Touch, Stylus Keyboard, LED & Sensors (0x28 .. 0x2F)
	SysReadTouch      = byte(0x28) // ( -- touched x y)
	SysReadTouchRaw   = byte(0x29) // ( -- touched rawX rawY rawZ)
	SysPromptKeyboard = byte(0x2A) // (titleSlice bufAddr maxLen allowCtrlC -- res)
	SysSetRgbLed      = byte(0x2B) // (r g b durationMs -- )
	SysReadLdr        = byte(0x2C) // ( -- rawLdr)
	SysReadCpuTemp    = byte(0x2D) // ( -- deciCelsius)
	SysReadClick      = byte(0x2E) // ( -- clicked x y)

	// Unified VFS (/flash, /sd, /net), Network & Serial (0x30 .. 0x3F)
	SysFsLs          = byte(0x30) // (pathSlice entriesAddr maxEntries -- count)
	SysFsStat        = byte(0x31) // (pathSlice entryAddr -- ok)
	SysFsRead        = byte(0x32) // (pathSlice offset bufAddr len -- bytesRead)
	SysFsWrite       = byte(0x33) // (pathSlice bufAddr len -- ok)
	SysFsWriteAt     = byte(0x34) // (pathSlice offset bufAddr len -- ok)
	SysFsMkdir       = byte(0x35) // (pathSlice parents -- ok)
	SysFsRmdir       = byte(0x36) // (pathSlice -- ok)
	SysFsRm          = byte(0x37) // (pathSlice -- ok)
	SysFsDu          = byte(0x38) // (pathSlice -- totalBytes)
	SysWifiConnected = byte(0x39) // ( -- connected)
	SysHttpGet       = byte(0x3A) // (urlSlice bufAddr maxLen -- bytesRead)
	SysSerialAvail   = byte(0x3B) // ( -- count)
	SysSerialRead    = byte(0x3C) // ( -- ch)
	SysSerialWrite   = byte(0x3D) // (bufAddr len -- written)
	SysHttpGetCached = byte(0x3E) // (urlSlice bufAddr maxLen maxAgeSec -- bytesRead)

	// JPEG Image Decoder & Renderer (0x40 .. 0x42)
	SysTftDrawJpgFile = byte(0x40) // (pathSlice x y scale -- ok)
	SysTftDrawJpgMem  = byte(0x41) // (bufAddr len x y scale -- ok)
	SysJpgInfo        = byte(0x42) // (pathSlice -- ok width height)

	// Core 0 Background File Prefetcher (0x43 .. 0x45)
	SysPrefetchStart  = byte(0x43) // (pathSlice bufAddr maxLen -- ok)
	SysPrefetchStatus = byte(0x44) // ( -- status bytesGot)
	SysPrefetchCancel = byte(0x45) // ( -- )

	// 4-Bit Indexed Palette Rendering & PNG Encoder (0x46 .. 0x47)
	SysTftDrawPal4 = byte(0x46) // (x y w h palAddr pix4bppAddr -- )
	SysSavePngPal4 = byte(0x47) // (pathSlice w h palAddr pix4bppAddr -- ok)

	// Multi-Head Native Binary Chaining (0x48 .. 0x49)
	SysSetAppArg  = byte(0x48) // (argSlice -- )
	SysJoinNative = byte(0x49) // (binNameSlice -- )

	// Core 0 16-Channel Audio Synthesizer & Mixer (0x4A .. 0x4C)
	SysAudioStart    = byte(0x4A) // ( -- )
	SysAudioStop     = byte(0x4B) // ( -- )
	SysAudioSetVoice = byte(0x4C) // (ch pcmAddr len loopStart loopLen stepFp16 vol resetPos -- )

	// WebSocket Client & Off-Screen 4-Bit (pal4) 3D Rasterizer (0x4D .. 0x54)
	SysWsOpen       = byte(0x4D) // (urlOrPathSlice -- ok)
	SysWsSend       = byte(0x4E) // (msgSlice -- ok)
	SysWsRecv       = byte(0x4F) // (bufAddr maxLen -- bytesRead)
	SysWsClose      = byte(0x50) // ( -- )
	SysPal4Clear    = byte(0x51) // (bufAddr w h topRows topCol botCol -- )
	SysPal4FillTri  = byte(0x52) // (bufAddr w h x0 y0 x1 y1 x2 y2 col -- )
	SysPal4FillCirc = byte(0x53) // (bufAddr w h cx cy r col -- )
	SysPal4DrawLine = byte(0x54) // (bufAddr w h x0 y0 x1 y1 col -- )
)

type ArgType string

const (
	ArgNone     ArgType = "none"
	ArgImm8S    ArgType = "imm8_s"
	ArgImm8U    ArgType = "imm8_u"
	ArgImm16S   ArgType = "imm16_s"
	ArgImm16U   ArgType = "imm16_u"
	ArgImm32    ArgType = "imm32"
	ArgRel16    ArgType = "rel16"
	ArgStr16    ArgType = "str16"
	ArgFunc16   ArgType = "func16"
	ArgLocal8   ArgType = "local8"
	ArgGlobal16 ArgType = "global16"
	ArgSys8     ArgType = "sys8"
)

type OpcodeInfo struct {
	Mnemonic string
	ArgBytes int
	ArgType  ArgType
	Desc     string
}

var OpcodeTable = map[byte]OpcodeInfo{
	OpNOP:            {"NOP", 0, ArgNone, "No operation"},
	OpPUSH_NIL:       {"PUSH_NIL", 0, ArgNone, "Push scalar nil (0)"},
	OpPUSH_NIL_SLICE: {"PUSH_NIL_SLICE", 0, ArgNone, "Push 3-word nil slice (0, 0, 0)"},
	OpPUSH_TRUE:      {"PUSH_TRUE", 0, ArgNone, "Push boolean true (1)"},
	OpPUSH_FALSE:     {"PUSH_FALSE", 0, ArgNone, "Push boolean false (0)"},
	OpPUSH_0:         {"PUSH_0", 0, ArgNone, "Push integer 0"},
	OpPUSH_1:         {"PUSH_1", 0, ArgNone, "Push integer 1"},
	OpPUSH_NEG1:      {"PUSH_NEG1", 0, ArgNone, "Push integer -1"},
	OpPUSH_I8:        {"PUSH_I8", 1, ArgImm8S, "Push signed 8-bit integer"},
	OpPUSH_U8:        {"PUSH_U8", 1, ArgImm8U, "Push unsigned 8-bit integer"},
	OpPUSH_I16:       {"PUSH_I16", 2, ArgImm16S, "Push signed 16-bit integer"},
	OpPUSH_U16:       {"PUSH_U16", 2, ArgImm16U, "Push unsigned 16-bit integer"},
	OpPUSH_I32:       {"PUSH_I32", 4, ArgImm32, "Push 32-bit integer"},
	OpPUSH_STR:       {"PUSH_STR", 2, ArgStr16, "Push 3-word string slice {Base, Len, Len}"},
	OpPUSH_CSTR:      {"PUSH_CSTR", 2, ArgStr16, "Push null-terminated C-string address"},
	OpPOP:            {"POP", 0, ArgNone, "Pop 32-bit word"},
	OpPOP_SLICE:      {"POP_SLICE", 0, ArgNone, "Pop 3-word slice"},
	OpDUP:            {"DUP", 0, ArgNone, "Duplicate top word"},
	OpSWAP:           {"SWAP", 0, ArgNone, "Swap top two words"},
	OpDUP2:           {"DUP2", 0, ArgNone, "Duplicate top two words"},
	OpROT:            {"ROT", 0, ArgNone, "Rotate top three words (a b c -- b c a)"},
	OpOVER:           {"OVER", 0, ArgNone, "Copy second word to top (a b -- a b a)"},

	OpLOAD_LOCAL_0:   {"LOAD_LOCAL_0", 0, ArgNone, "Fast load local 0"},
	OpLOAD_LOCAL_1:   {"LOAD_LOCAL_1", 0, ArgNone, "Fast load local 1"},
	OpLOAD_LOCAL_2:   {"LOAD_LOCAL_2", 0, ArgNone, "Fast load local 2"},
	OpLOAD_LOCAL_3:   {"LOAD_LOCAL_3", 0, ArgNone, "Fast load local 3"},
	OpSTORE_LOCAL_0:  {"STORE_LOCAL_0", 0, ArgNone, "Fast store local 0"},
	OpSTORE_LOCAL_1:  {"STORE_LOCAL_1", 0, ArgNone, "Fast store local 1"},
	OpSTORE_LOCAL_2:  {"STORE_LOCAL_2", 0, ArgNone, "Fast store local 2"},
	OpSTORE_LOCAL_3:  {"STORE_LOCAL_3", 0, ArgNone, "Fast store local 3"},
	OpLOAD_LOCAL:     {"LOAD_LOCAL", 1, ArgLocal8, "Load local variable"},
	OpSTORE_LOCAL:    {"STORE_LOCAL", 1, ArgLocal8, "Store local variable"},
	OpLOAD_GLOBAL:    {"LOAD_GLOBAL", 2, ArgGlobal16, "Load global variable"},
	OpSTORE_GLOBAL:   {"STORE_GLOBAL", 2, ArgGlobal16, "Store global variable"},
	OpADDR_OF_GLOBAL: {"ADDR_OF_GLOBAL", 2, ArgGlobal16, "Byte address of global variable"},
	OpPEEK2:          {"PEEK2", 0, ArgNone, "Read unsigned 16-bit halfword from memory"},
	OpPOKE2:          {"POKE2", 0, ArgNone, "Write 16-bit halfword to memory"},
	OpPEEK1:          {"PEEK1", 0, ArgNone, "Read unsigned 8-bit byte from memory"},
	OpPOKE1:          {"POKE1", 0, ArgNone, "Write 8-bit byte to memory"},
	OpADDR_OF_LOCAL:  {"ADDR_OF_LOCAL", 1, ArgLocal8, "Byte address of local variable"},
	OpSHL1_ADD:       {"SHL1_ADD", 0, ArgNone, "base + idx * 2"},
	OpMEM_COPY:       {"MEM_COPY", 0, ArgNone, "Copy count bytes from src to dst"},
	OpMEM_SET:        {"MEM_SET", 0, ArgNone, "Set count bytes at dst to val"},
	OpPEEK4:          {"PEEK4", 0, ArgNone, "Read 32-bit word from memory"},
	OpPOKE4:          {"POKE4", 0, ArgNone, "Write 32-bit word to memory"},
	OpPEEKS1:         {"PEEKS1", 0, ArgNone, "Read signed 8-bit byte from memory"},
	OpPEEKS2:         {"PEEKS2", 0, ArgNone, "Read signed 16-bit halfword from memory"},
	OpSHL2_ADD:       {"SHL2_ADD", 0, ArgNone, "base + idx * 4"},
	OpINDEX_ADD:      {"INDEX_ADD", 2, ArgImm16U, "base + idx * elemSize"},
	OpMEM_CMP:        {"MEM_CMP", 0, ArgNone, "Compare count bytes at a and b"},

	OpADD:     {"ADD", 0, ArgNone, "32-bit addition"},
	OpSUB:     {"SUB", 0, ArgNone, "32-bit subtraction"},
	OpMUL:     {"MUL", 0, ArgNone, "32-bit multiplication"},
	OpDIV:     {"DIV", 0, ArgNone, "32-bit signed division"},
	OpMOD:     {"MOD", 0, ArgNone, "32-bit signed modulo"},
	OpNEG:     {"NEG", 0, ArgNone, "32-bit two's complement negation"},
	OpBIT_AND: {"BIT_AND", 0, ArgNone, "32-bit bitwise AND"},
	OpBIT_OR:  {"BIT_OR", 0, ArgNone, "32-bit bitwise OR"},
	OpBIT_XOR: {"BIT_XOR", 0, ArgNone, "32-bit bitwise XOR"},
	OpBIT_NOT: {"BIT_NOT", 0, ArgNone, "32-bit bitwise NOT"},
	OpSHL:     {"SHL", 0, ArgNone, "32-bit shift left"},
	OpSHR:     {"SHR", 0, ArgNone, "32-bit arithmetic shift right"},
	OpCMP_EQ:  {"CMP_EQ", 0, ArgNone, "32-bit equal (==)"},
	OpCMP_NE:  {"CMP_NE", 0, ArgNone, "32-bit not equal (!=)"},
	OpCMP_LT:  {"CMP_LT", 0, ArgNone, "32-bit signed less than (<)"},
	OpCMP_LE:  {"CMP_LE", 0, ArgNone, "32-bit signed less than or equal (<=)"},
	OpCMP_GT:  {"CMP_GT", 0, ArgNone, "32-bit signed greater than (>)"},
	OpCMP_GE:  {"CMP_GE", 0, ArgNone, "32-bit signed greater than or equal (>=)"},
	OpNOT:     {"NOT", 0, ArgNone, "Logical NOT"},
	OpUDIV:    {"UDIV", 0, ArgNone, "32-bit unsigned division"},
	OpUMOD:    {"UMOD", 0, ArgNone, "32-bit unsigned modulo"},
	OpUSHR:    {"USHR", 0, ArgNone, "32-bit logical shift right"},
	OpUCMP_LT: {"UCMP_LT", 0, ArgNone, "32-bit unsigned less than (<)"},
	OpUCMP_LE: {"UCMP_LE", 0, ArgNone, "32-bit unsigned less than or equal (<=)"},
	OpUCMP_GT: {"UCMP_GT", 0, ArgNone, "32-bit unsigned greater than (>)"},
	OpUCMP_GE: {"UCMP_GE", 0, ArgNone, "32-bit unsigned greater than or equal (>=)"},

	OpJUMP:          {"JUMP", 2, ArgRel16, "Unconditional relative branch"},
	OpJUMP_IF_TRUE:  {"JUMP_IF_TRUE", 2, ArgRel16, "Branch if condition is non-zero"},
	OpJUMP_IF_FALSE: {"JUMP_IF_FALSE", 2, ArgRel16, "Branch if condition is zero"},
	OpCALL:          {"CALL", 2, ArgFunc16, "Call function by index"},
	OpRET:           {"RET", 0, ArgNone, "Return 32-bit word to caller"},
	OpRET_SLICE:     {"RET_SLICE", 0, ArgNone, "Return 3-word slice to caller"},
	OpRET_VOID:      {"RET_VOID", 0, ArgNone, "Return void to caller"},
	OpHALT:          {"HALT", 0, ArgNone, "Halt VM cleanly (exit 0)"},
	OpPANIC:         {"PANIC", 0, ArgNone, "Panic with message slice and exit"},

	OpSLICE_NEW:  {"SLICE_NEW", 0, ArgNone, "Create 3-word slice (base cap len)"},
	OpSLICE_LEN:  {"SLICE_LEN", 0, ArgNone, "Extract length from slice"},
	OpSLICE_CAP:  {"SLICE_CAP", 0, ArgNone, "Extract capacity from slice"},
	OpSLICE_SUB:  {"SLICE_SUB", 0, ArgNone, "Sub-slice view"},
	OpSLICE_DATA: {"SLICE_DATA", 0, ArgNone, "Extract Base pointer from slice"},
	OpSTR_CMP:    {"STR_CMP", 0, ArgNone, "Compare two string slices"},

	OpMALLOC:    {"MALLOC", 0, ArgNone, "Allocate size bytes from VM heap"},
	OpZALLOC:    {"ZALLOC", 0, ArgNone, "Allocate & zero size bytes from VM heap"},
	OpREALLOC:   {"REALLOC", 0, ArgNone, "Resize heap allocation (addr newSize)"},
	OpFREE:      {"FREE", 0, ArgNone, "Free heap allocation (addr)"},
	OpHEAP_FREE: {"HEAP_FREE", 0, ArgNone, "Push free bytes remaining in VM heap"},

	OpFADD:       {"FADD", 0, ArgNone, "float32 addition"},
	OpFSUB:       {"FSUB", 0, ArgNone, "float32 subtraction"},
	OpFMUL:       {"FMUL", 0, ArgNone, "float32 multiplication"},
	OpFDIV:       {"FDIV", 0, ArgNone, "float32 division"},
	OpFNEG:       {"FNEG", 0, ArgNone, "float32 negation"},
	OpFABS:       {"FABS", 0, ArgNone, "float32 absolute value"},
	OpFSQRT:      {"FSQRT", 0, ArgNone, "float32 square root"},
	OpFSIN:       {"FSIN", 0, ArgNone, "float32 sine (radians)"},
	OpFCOS:       {"FCOS", 0, ArgNone, "float32 cosine (radians)"},
	OpFATAN2:     {"FATAN2", 0, ArgNone, "float32 atan2(y, x) in radians"},
	OpFFLOOR:     {"FFLOOR", 0, ArgNone, "float32 floor"},
	OpFCEIL:      {"FCEIL", 0, ArgNone, "float32 ceil"},
	OpFCMP_EQ:    {"FCMP_EQ", 0, ArgNone, "float32 equal (==)"},
	OpFCMP_NE:    {"FCMP_NE", 0, ArgNone, "float32 not equal (!=)"},
	OpFCMP_LT:    {"FCMP_LT", 0, ArgNone, "float32 less than (<)"},
	OpFCMP_LE:    {"FCMP_LE", 0, ArgNone, "float32 less than or equal (<=)"},
	OpFCMP_GT:    {"FCMP_GT", 0, ArgNone, "float32 greater than (>)"},
	OpFCMP_GE:    {"FCMP_GE", 0, ArgNone, "float32 greater than or equal (>=)"},
	OpI32_TO_F32: {"I32_TO_F32", 0, ArgNone, "Convert signed int32 to float32"},
	OpF32_TO_I32: {"F32_TO_I32", 0, ArgNone, "Convert float32 to signed int32"},

	OpSYSCALL:  {"SYSCALL", 1, ArgSys8, "Invoke Lakatos OS syscall"},
	OpSYS_EXIT: {"SYS_EXIT", 0, ArgNone, "Exit app with status code"},
	OpIO_PRINT: {"IO_PRINT", 0, ArgNone, "Print string slice with newline"},
}

var MnemonicToOpcode = map[string]byte{}
var SyscallByName = map[string]byte{
	"SYS_EXIT":               SysExit,
	"SYS_YIELD":              SysYield,
	"SYS_MILLIS":             SysMillis,
	"SYS_MICROS":             SysMicros,
	"SYS_DELAY_MS":           SysDelayMs,
	"SYS_RAND":               SysRand,
	"SYS_SET_CORNER_EXIT":    SysSetCornerExit,
	"SYS_GET_AIRPORT":        SysGetAirport,
	"SYS_GET_AIRPORT_INFO":   SysGetAirportInfo,
	"SYS_GET_DEVICE_NAME":    SysGetDeviceName,
	"SYS_GET_SERIAL_NUMBER":  SysGetSerialNumber,
	"SYS_GET_SH_URL":         SysGetShUrl,
	"SYS_GET_UNIX_TIME":      SysGetUnixTime,
	"SYS_FORMAT_LOCAL_TIME":  SysFormatLocalTime,
	"SYS_CITY_HASH32":        SysCityHash32,
	"SYS_TFT_FILL_SCREEN":    SysTftFillScreen,
	"SYS_TFT_FILL_RECT":      SysTftFillRect,
	"SYS_TFT_DRAW_RECT":      SysTftDrawRect,
	"SYS_TFT_FILL_ROUND_RECT": SysTftFillRoundRect,
	"SYS_TFT_DRAW_ROUND_RECT": SysTftDrawRoundRect,
	"SYS_TFT_DRAW_LINE":      SysTftDrawLine,
	"SYS_TFT_DRAW_FAST_HLINE": SysTftDrawFastHLine,
	"SYS_TFT_DRAW_FAST_VLINE": SysTftDrawFastVLine,
	"SYS_TFT_FILL_CIRCLE":    SysTftFillCircle,
	"SYS_TFT_DRAW_CIRCLE":    SysTftDrawCircle,
	"SYS_TFT_FILL_TRIANGLE":  SysTftFillTriangle,
	"SYS_TFT_DRAW_CHAR":      SysTftDrawChar,
	"SYS_TFT_SET_TEXT_COLOR": SysTftSetTextColor,
	"SYS_TFT_SET_TEXT_DATUM": SysTftSetTextDatum,
	"SYS_TFT_DRAW_STRING":    SysTftDrawString,
	"SYS_TFT_DRAW_INT":       SysTftDrawInt,
	"SYS_TFT_PUSH_RGB565":    SysTftPushRGB565,
	"SYS_TFT_DRAW_CSTR":       SysTftDrawCStr,
	"SYS_FMT_INT_TO_STR":      SysFmtIntToStr,
	"SYS_FMT_FLOAT_TO_STR":    SysFmtFloatToStr,
	"SYS_TFT_DRAW_FLOAT":      SysTftDrawFloat,
	"SYS_TFT_DRAW_PNG_FILE":   SysTftDrawPngFile,
	"SYS_TFT_DRAW_PNG_MEM":    SysTftDrawPngMem,
	"SYS_PNG_INFO":            SysPngInfo,
	"SYS_TFT_DRAW_JPG_FILE":   SysTftDrawJpgFile,
	"SYS_TFT_DRAW_JPG_MEM":    SysTftDrawJpgMem,
	"SYS_JPG_INFO":            SysJpgInfo,
	"SYS_PREFETCH_START":      SysPrefetchStart,
	"SYS_PREFETCH_STATUS":     SysPrefetchStatus,
	"SYS_PREFETCH_CANCEL":     SysPrefetchCancel,
	"SYS_TFT_DRAW_PAL4":       SysTftDrawPal4,
	"SYS_SAVE_PNG_PAL4":       SysSavePngPal4,
	"SYS_SET_APP_ARG":         SysSetAppArg,
	"SYS_JOIN_NATIVE":         SysJoinNative,
	"SYS_AUDIO_START":         SysAudioStart,
	"SYS_AUDIO_STOP":          SysAudioStop,
	"SYS_AUDIO_SET_VOICE":     SysAudioSetVoice,
	"SYS_WS_OPEN":             SysWsOpen,
	"SYS_WS_SEND":             SysWsSend,
	"SYS_WS_RECV":             SysWsRecv,
	"SYS_WS_CLOSE":            SysWsClose,
	"SYS_PAL4_CLEAR":          SysPal4Clear,
	"SYS_PAL4_FILL_TRI":       SysPal4FillTri,
	"SYS_PAL4_FILL_CIRC":      SysPal4FillCirc,
	"SYS_PAL4_DRAW_LINE":      SysPal4DrawLine,
	"SYS_READ_TOUCH":         SysReadTouch,
	"SYS_READ_CLICK":         SysReadClick,
	"SYS_READ_TOUCH_RAW":     SysReadTouchRaw,
	"SYS_PROMPT_KEYBOARD":    SysPromptKeyboard,
	"SYS_SET_RGB_LED":        SysSetRgbLed,
	"SYS_READ_LDR":           SysReadLdr,
	"SYS_READ_CPU_TEMP":      SysReadCpuTemp,
	"SYS_FS_LS":              SysFsLs,
	"SYS_FS_STAT":            SysFsStat,
	"SYS_FS_READ":            SysFsRead,
	"SYS_FS_WRITE":           SysFsWrite,
	"SYS_FS_WRITE_AT":        SysFsWriteAt,
	"SYS_FS_MKDIR":           SysFsMkdir,
	"SYS_FS_RMDIR":           SysFsRmdir,
	"SYS_FS_RM":              SysFsRm,
	"SYS_FS_DU":              SysFsDu,
	"SYS_WIFI_CONNECTED":     SysWifiConnected,
	"SYS_HTTP_GET":           SysHttpGet,
	"SYS_SERIAL_AVAIL":       SysSerialAvail,
	"SYS_SERIAL_READ":        SysSerialRead,
	"SYS_SERIAL_WRITE":       SysSerialWrite,
	"SYS_HTTP_GET_CACHED":    SysHttpGetCached,
}

var SyscallToName = map[byte]string{}

func init() {
	for op, info := range OpcodeTable {
		MnemonicToOpcode[info.Mnemonic] = op
	}
	MnemonicToOpcode["PEEK"] = OpPEEK4
	MnemonicToOpcode["POKE"] = OpPOKE4
	MnemonicToOpcode["PEEKW"] = OpPEEK2
	MnemonicToOpcode["POKEW"] = OpPOKE2
	MnemonicToOpcode["PEEKB"] = OpPEEK1
	MnemonicToOpcode["POKEB"] = OpPOKE1

	for name, id := range SyscallByName {
		SyscallToName[id] = name
	}
}

// WrapLapp packages a raw LCOD bytecode module into a 128-byte Lakatos .lapp container
// with SHA-256 integrity hash and LAKATOS_APP_FLAG_LCODE set.
func WrapLapp(lcodPayload []byte, appTitle string) []byte {
	totalSize := uint32(LakatosAppHeaderSize + len(lcodPayload))
	sum := sha256.Sum256(lcodPayload)

	hdr := make([]byte, LakatosAppHeaderSize)
	binary.LittleEndian.PutUint32(hdr[0:4], LakatosAppMagic)       // magic "LAKA"
	binary.LittleEndian.PutUint32(hdr[4:8], LakatosHeaderVersion)  // header_version = 1
	binary.LittleEndian.PutUint32(hdr[8:12], LakatosAbiVersion)    // abi_version = 1
	binary.LittleEndian.PutUint32(hdr[12:16], 0)                   // entry_addr (0 for LCode VM)
	binary.LittleEndian.PutUint32(hdr[16:20], LakatosAppHeaderSize) // text_offset = 128
	binary.LittleEndian.PutUint32(hdr[20:24], uint32(len(lcodPayload))) // text_size = payload len
	binary.LittleEndian.PutUint32(hdr[52:56], totalSize)           // total_file_size
	copy(hdr[56:88], sum[:])                                       // sha256[32]

	titleBytes := []byte(appTitle)
	if len(titleBytes) > 31 {
		titleBytes = titleBytes[:31]
	}
	copy(hdr[88:120], titleBytes)
	binary.LittleEndian.PutUint32(hdr[120:124], LakatosAppFlagLCode) // flags = LAKATOS_APP_FLAG_LCODE (1)
	binary.LittleEndian.PutUint32(hdr[124:128], 0)                   // reserved

	out := make([]byte, 0, int(totalSize))
	out = append(out, hdr...)
	out = append(out, lcodPayload...)
	return out
}

// UnwrapLapp accepts either a .lapp container (starting with "LAKA") or a raw "LCOD" payload,
// validates the SHA-256 if wrapped in .lapp, and returns the LCOD payload and app title.
func UnwrapLapp(data []byte) (payload []byte, title string, err error) {
	if len(data) >= 4 && bytes.Equal(data[0:4], LCodeMagic) {
		return data, "LCodeApp", nil
	}
	if len(data) < LakatosAppHeaderSize {
		return nil, "", fmt.Errorf("binary too small (%d bytes)", len(data))
	}
	magic := binary.LittleEndian.Uint32(data[0:4])
	if magic != LakatosAppMagic {
		return nil, "", fmt.Errorf("invalid .lapp magic: 0x%08X", magic)
	}
	flags := binary.LittleEndian.Uint32(data[120:124])
	if (flags & LakatosAppFlagLCode) == 0 {
		return nil, "", fmt.Errorf(".lapp does not have LAKATOS_APP_FLAG_LCODE set (flags=0x%X)", flags)
	}
	totalSize := binary.LittleEndian.Uint32(data[52:56])
	if int(totalSize) > len(data) || totalSize < LakatosAppHeaderSize {
		return nil, "", fmt.Errorf("invalid .lapp total_file_size %d (file len %d)", totalSize, len(data))
	}
	payload = data[LakatosAppHeaderSize:totalSize]
	sum := sha256.Sum256(payload)
	if !bytes.Equal(sum[:], data[56:88]) {
		return nil, "", fmt.Errorf(".lapp SHA-256 verification failed")
	}
	rawTitle := data[88:120]
	if idx := bytes.IndexByte(rawTitle, 0); idx >= 0 {
		rawTitle = rawTitle[:idx]
	}
	return payload, string(rawTitle), nil
}
