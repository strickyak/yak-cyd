package lakat

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"image/jpeg"
	"image/png"
	"io"
	"math"
	"math/rand"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
	"time"
)

const (
	NullGuardSize   = 256
	StringPoolBase  = 0x0100
	OsScratchSize   = 512
	MaxFrameBytes   = 4096
	HeapBlockHdrLen = 8
)

type FunctionMeta struct {
	Index        int
	Name         string
	ArgCount     int
	LocalCount   int
	FrameSize    int
	CodeOffset   int
	CodeSize     int
	VarSizes     []int
	LocalOffsets []int
}

type CallFrame struct {
	Func      *FunctionMeta
	ReturnPC  int
	FrameAddr int
}

type TouchEvent struct {
	Touched bool
	X       int
	Y       int
}

type VM struct {
	AppTitle         string
	Memory           []byte
	Code             []byte
	Stack            []uint32
	CallStack        []CallFrame
	CurrentFrame     *CallFrame
	PC               int
	Running          bool
	ExitCode         int
	InstructionCount int
	MaxInstructions  int

	Functions      []*FunctionMeta
	EntryFuncIndex int
	StringPoolEnd  int
	OsScratchBase  int
	OsScratchPtr   int
	GlobalsBase    int
	GlobalSizes    []int
	GlobalOffsets  []int
	FrameBase      int
	FrameSP        int
	HeapBase       int

	// Emulated 320x240 RGB565 TFT Display
	Screen    [240][320]uint16
	TextFg    uint16
	TextBg    uint16
	TextDatum uint8

	// Emulated Hardware & Config State
	StartTime    time.Time
	VirtualMs    uint32
	AirportCode  string
	DeviceName   string
	SerialNumber string
	ShUrl        string
	TouchQueue   []TouchEvent
	CurrentTouch TouchEvent
	SerialIn           []byte
	Stdout             io.Writer
	VfsRoot            string
	PrefetchStatus     int
	PrefetchBytes      uint32
	PrefetchBuf        []byte
	CornerExitDisabled bool
	AppArg             string
	JoinNativeBin      string
}

func NewVM(lappOrLcod []byte) (*VM, error) {
	payload, title, err := UnwrapLapp(lappOrLcod)
	if err != nil {
		return nil, err
	}
	vm := &VM{
		AppTitle:     title,
		Memory:       make([]byte, LakatosDefaultArenaLen),
		Stack:        make([]uint32, 0, 1024),
		CallStack:    make([]CallFrame, 0, 128),
		TextFg:       0xFFFF,
		TextBg:       0x0000,
		TextDatum:    0,
		StartTime:    time.Now(),
		AirportCode:  "PDX",
		DeviceName:   "test",
		SerialNumber: "ing",
		ShUrl:        "http://pizga.net:1908/",
		Stdout:       os.Stdout,
		VfsRoot:      "build",
	}
	if err := vm.loadModule(payload); err != nil {
		return nil, err
	}
	return vm, nil
}

func align4(n int) int {
	return (n + 3) & ^3
}

func align8(n int) int {
	return (n + 7) & ^7
}

func (vm *VM) loadModule(payload []byte) error {
	if len(payload) < LCodeHeaderSz || !bytes.Equal(payload[0:4], LCodeMagic) {
		return fmt.Errorf("invalid LCOD module header")
	}

	strPoolSize := int(binary.LittleEndian.Uint32(payload[8:12]))
	globalsCount := int(binary.LittleEndian.Uint16(payload[12:14]))
	funcCount := int(binary.LittleEndian.Uint16(payload[14:16]))
	entryFunc := int(binary.LittleEndian.Uint16(payload[16:18]))
	codeSize := int(binary.LittleEndian.Uint32(payload[20:24]))

	vm.EntryFuncIndex = entryFunc

	offset := LCodeHeaderSz
	vm.GlobalSizes = make([]int, globalsCount)
	vm.GlobalOffsets = make([]int, globalsCount)
	for i := 0; i < globalsCount; i++ {
		if offset+4 > len(payload) {
			return fmt.Errorf("truncated globals table")
		}
		vm.GlobalSizes[i] = int(binary.LittleEndian.Uint32(payload[offset : offset+4]))
		offset += 4
	}

	if offset+strPoolSize > len(payload) {
		return fmt.Errorf("truncated string pool")
	}
	copy(vm.Memory[StringPoolBase:StringPoolBase+strPoolSize], payload[offset:offset+strPoolSize])
	offset += strPoolSize

	vm.StringPoolEnd = align4(StringPoolBase + strPoolSize)
	vm.OsScratchBase = vm.StringPoolEnd
	vm.OsScratchPtr = 0
	vm.GlobalsBase = align4(vm.OsScratchBase + OsScratchSize)

	gCursor := vm.GlobalsBase
	for i, gSz := range vm.GlobalSizes {
		vm.GlobalOffsets[i] = gCursor
		gCursor += align4(gSz)
	}
	vm.FrameBase = align8(gCursor)
	vm.FrameSP = vm.FrameBase
	vm.HeapBase = align8(vm.FrameBase + MaxFrameBytes)
	if vm.HeapBase+64 > len(vm.Memory) {
		return fmt.Errorf("globals + strings exceed VM arena (%d bytes)", vm.HeapBase)
	}
	vm.initHeap()

	vm.Functions = make([]*FunctionMeta, funcCount)
	maxMetaEnd := offset + funcCount*LCodeFuncEntSz
	for i := 0; i < funcCount; i++ {
		if offset+LCodeFuncEntSz > len(payload) {
			return fmt.Errorf("truncated function table")
		}
		ent := payload[offset : offset+LCodeFuncEntSz]
		offset += LCodeFuncEntSz

		argCount := int(binary.LittleEndian.Uint16(ent[0:2]))
		localCount := int(binary.LittleEndian.Uint16(ent[2:4]))
		frameSize := int(binary.LittleEndian.Uint32(ent[4:8]))
		codeOffset := int(binary.LittleEndian.Uint32(ent[8:12]))
		fnCodeSize := int(binary.LittleEndian.Uint32(ent[12:16]))
		metaOffset := int(binary.LittleEndian.Uint32(ent[16:20]))

		varSizes := make([]int, localCount)
		localOffsets := make([]int, localCount)
		mPos := metaOffset
		lOff := 0
		for v := 0; v < localCount; v++ {
			if mPos+4 > len(payload) {
				return fmt.Errorf("truncated function var sizes")
			}
			sz := int(binary.LittleEndian.Uint32(payload[mPos : mPos+4]))
			varSizes[v] = sz
			localOffsets[v] = lOff
			lOff += align4(sz)
			mPos += 4
		}
		if lOff > frameSize {
			frameSize = lOff
		}
		fnName := fmt.Sprintf("fn_%d", i)
		if mPos < len(payload) {
			nLen := int(payload[mPos])
			mPos++
			if mPos+nLen <= len(payload) && nLen > 0 {
				fnName = string(payload[mPos : mPos+nLen])
				mPos += nLen
			}
		}
		if mPos > maxMetaEnd {
			maxMetaEnd = mPos
		}
		vm.Functions[i] = &FunctionMeta{
			Index:        i,
			Name:         fnName,
			ArgCount:     argCount,
			LocalCount:   localCount,
			FrameSize:    align4(frameSize),
			CodeOffset:   codeOffset,
			CodeSize:     fnCodeSize,
			VarSizes:     varSizes,
			LocalOffsets: localOffsets,
		}
	}

	if maxMetaEnd+codeSize > len(payload) {
		return fmt.Errorf("truncated code section")
	}
	vm.Code = make([]byte, codeSize)
	copy(vm.Code, payload[maxMetaEnd:maxMetaEnd+codeSize])
	return nil
}

// ---------------------------------------------------------------------------
// Coalescing Free-List Heap Allocator inside vm.Memory[HeapBase .. len(Memory)]
// Block header (8 bytes): [uint32 payloadSize (aligned 8)][uint32 used (0 or 1)]
// ---------------------------------------------------------------------------
func (vm *VM) initHeap() {
	totalAvail := len(vm.Memory) - vm.HeapBase - HeapBlockHdrLen
	binary.LittleEndian.PutUint32(vm.Memory[vm.HeapBase:vm.HeapBase+4], uint32(totalAvail))
	binary.LittleEndian.PutUint32(vm.Memory[vm.HeapBase+4:vm.HeapBase+8], 0)
}

func (vm *VM) heapMalloc(reqSize int) uint32 {
	if reqSize <= 0 {
		reqSize = 8
	}
	need := align8(reqSize)
	pos := vm.HeapBase
	limit := len(vm.Memory)

	for pos+HeapBlockHdrLen <= limit {
		blkSz := int(binary.LittleEndian.Uint32(vm.Memory[pos : pos+4]))
		used := binary.LittleEndian.Uint32(vm.Memory[pos+4 : pos+8])
		if blkSz <= 0 || pos+HeapBlockHdrLen+blkSz > limit {
			break
		}
		if used == 0 && blkSz >= need {
			rem := blkSz - need
			if rem >= HeapBlockHdrLen+8 {
				// Split block
				binary.LittleEndian.PutUint32(vm.Memory[pos:pos+4], uint32(need))
				binary.LittleEndian.PutUint32(vm.Memory[pos+4:pos+8], 1)
				nextPos := pos + HeapBlockHdrLen + need
				binary.LittleEndian.PutUint32(vm.Memory[nextPos:nextPos+4], uint32(rem-HeapBlockHdrLen))
				binary.LittleEndian.PutUint32(vm.Memory[nextPos+4:nextPos+8], 0)
			} else {
				binary.LittleEndian.PutUint32(vm.Memory[pos+4:pos+8], 1)
			}
			return uint32(pos + HeapBlockHdrLen)
		}
		pos += HeapBlockHdrLen + blkSz
	}
	return 0
}

func (vm *VM) heapFree(addr uint32) error {
	if addr == 0 {
		return nil
	}
	hdrPos := int(addr) - HeapBlockHdrLen
	if hdrPos < vm.HeapBase || hdrPos+HeapBlockHdrLen > len(vm.Memory) {
		return fmt.Errorf("invalid free() address 0x%X", addr)
	}
	binary.LittleEndian.PutUint32(vm.Memory[hdrPos+4:hdrPos+8], 0)

	// Coalesce adjacent free blocks
	pos := vm.HeapBase
	limit := len(vm.Memory)
	for pos+HeapBlockHdrLen <= limit {
		blkSz := int(binary.LittleEndian.Uint32(vm.Memory[pos : pos+4]))
		used := binary.LittleEndian.Uint32(vm.Memory[pos+4 : pos+8])
		if blkSz <= 0 || pos+HeapBlockHdrLen+blkSz > limit {
			break
		}
		nextPos := pos + HeapBlockHdrLen + blkSz
		if used == 0 && nextPos+HeapBlockHdrLen <= limit {
			nextSz := int(binary.LittleEndian.Uint32(vm.Memory[nextPos : nextPos+4]))
			nextUsed := binary.LittleEndian.Uint32(vm.Memory[nextPos+4 : nextPos+8])
			if nextUsed == 0 && nextSz > 0 && nextPos+HeapBlockHdrLen+nextSz <= limit {
				combined := blkSz + HeapBlockHdrLen + nextSz
				binary.LittleEndian.PutUint32(vm.Memory[pos:pos+4], uint32(combined))
				continue
			}
		}
		pos = nextPos
	}
	return nil
}

func (vm *VM) heapBytesFree() uint32 {
	var total uint32
	pos := vm.HeapBase
	limit := len(vm.Memory)
	for pos+HeapBlockHdrLen <= limit {
		blkSz := int(binary.LittleEndian.Uint32(vm.Memory[pos : pos+4]))
		used := binary.LittleEndian.Uint32(vm.Memory[pos+4 : pos+8])
		if blkSz <= 0 || pos+HeapBlockHdrLen+blkSz > limit {
			break
		}
		if used == 0 {
			total += uint32(blkSz)
		}
		pos += HeapBlockHdrLen + blkSz
	}
	return total
}

// ---------------------------------------------------------------------------
// Stack & Bounds-Checked Memory Helpers
// ---------------------------------------------------------------------------
func (vm *VM) push(v uint32) {
	vm.Stack = append(vm.Stack, v)
}

func (vm *VM) pop() (uint32, error) {
	n := len(vm.Stack)
	if n == 0 {
		return 0, vm.errf("stack underflow")
	}
	v := vm.Stack[n-1]
	vm.Stack = vm.Stack[:n-1]
	return v, nil
}

func (vm *VM) checkAddr(addr uint32, size int) error {
	if addr < NullGuardSize {
		return vm.errf("null/low pointer dereference at 0x%04X", addr)
	}
	if int(addr)+size > len(vm.Memory) || int(addr) < 0 {
		return vm.errf("memory access out of bounds at 0x%05X (size %d)", addr, size)
	}
	return nil
}

func (vm *VM) errf(format string, args ...any) error {
	fnName := "<top>"
	if vm.CurrentFrame != nil && vm.CurrentFrame.Func != nil {
		fnName = vm.CurrentFrame.Func.Name
	}
	return fmt.Errorf("LCode VM Error in %s at PC 0x%04X: %s", fnName, vm.PC, fmt.Sprintf(format, args...))
}

func (vm *VM) writeOsScratchString(s string) (base uint32, length uint32) {
	b := []byte(s)
	if len(b) > 120 {
		b = b[:120]
	}
	if vm.OsScratchPtr+len(b)+1 > OsScratchSize {
		vm.OsScratchPtr = 0
	}
	addr := vm.OsScratchBase + vm.OsScratchPtr
	copy(vm.Memory[addr:addr+len(b)], b)
	vm.Memory[addr+len(b)] = 0
	vm.OsScratchPtr += len(b) + 1
	return uint32(addr), uint32(len(b))
}

func (vm *VM) readSliceString(base, length uint32) (string, error) {
	if length == 0 {
		return "", nil
	}
	if err := vm.checkAddr(base, int(length)); err != nil {
		return "", err
	}
	return string(vm.Memory[base : base+length]), nil
}

func (vm *VM) readCStr(addr uint32) (string, error) {
	if err := vm.checkAddr(addr, 1); err != nil {
		return "", err
	}
	end := int(addr)
	for end < len(vm.Memory) && vm.Memory[end] != 0 {
		end++
	}
	return string(vm.Memory[addr:end]), nil
}

// ---------------------------------------------------------------------------
// Main Execution Loop
// ---------------------------------------------------------------------------
func (vm *VM) Run() (int, error) {
	if vm.EntryFuncIndex < 0 || vm.EntryFuncIndex >= len(vm.Functions) {
		return -1, fmt.Errorf("invalid entry function index %d", vm.EntryFuncIndex)
	}
	entryFn := vm.Functions[vm.EntryFuncIndex]
	vm.FrameSP = vm.FrameBase + entryFn.FrameSize
	for i := vm.FrameBase; i < vm.FrameSP; i++ {
		vm.Memory[i] = 0
	}
	vm.CallStack = append(vm.CallStack[:0], CallFrame{
		Func:      entryFn,
		ReturnPC:  -1,
		FrameAddr: vm.FrameBase,
	})
	vm.CurrentFrame = &vm.CallStack[0]
	vm.PC = entryFn.CodeOffset
	vm.Running = true
	vm.ExitCode = 0

	for vm.Running {
		if vm.MaxInstructions > 0 && vm.InstructionCount >= vm.MaxInstructions {
			vm.Running = false
			break
		}
		if vm.PC < 0 || vm.PC >= len(vm.Code) {
			return -1, vm.errf("PC out of bounds")
		}
		op := vm.Code[vm.PC]
		vm.PC++
		vm.InstructionCount++

		if err := vm.stepOp(op); err != nil {
			return -1, err
		}
	}
	return vm.ExitCode, nil
}

func (vm *VM) fetchU8() byte {
	b := vm.Code[vm.PC]
	vm.PC++
	return b
}

func (vm *VM) fetchU16() uint16 {
	v := binary.LittleEndian.Uint16(vm.Code[vm.PC : vm.PC+2])
	vm.PC += 2
	return v
}

func (vm *VM) fetchU32() uint32 {
	v := binary.LittleEndian.Uint32(vm.Code[vm.PC : vm.PC+4])
	vm.PC += 4
	return v
}

func (vm *VM) loadLocalSlot(slot int) error {
	fn := vm.CurrentFrame.Func
	if slot < 0 || slot >= fn.LocalCount {
		return vm.errf("local slot %d out of range", slot)
	}
	addr := vm.CurrentFrame.FrameAddr + fn.LocalOffsets[slot]
	sz := fn.VarSizes[slot]
	if sz == 12 {
		vm.push(binary.LittleEndian.Uint32(vm.Memory[addr : addr+4]))
		vm.push(binary.LittleEndian.Uint32(vm.Memory[addr+4 : addr+8]))
		vm.push(binary.LittleEndian.Uint32(vm.Memory[addr+8 : addr+12]))
	} else {
		vm.push(binary.LittleEndian.Uint32(vm.Memory[addr : addr+4]))
	}
	return nil
}

func (vm *VM) storeLocalSlot(slot int) error {
	fn := vm.CurrentFrame.Func
	if slot < 0 || slot >= fn.LocalCount {
		return vm.errf("local slot %d out of range", slot)
	}
	addr := vm.CurrentFrame.FrameAddr + fn.LocalOffsets[slot]
	sz := fn.VarSizes[slot]
	if sz == 12 {
		ln, err := vm.pop()
		if err != nil {
			return err
		}
		cp, err := vm.pop()
		if err != nil {
			return err
		}
		bs, err := vm.pop()
		if err != nil {
			return err
		}
		binary.LittleEndian.PutUint32(vm.Memory[addr:addr+4], bs)
		binary.LittleEndian.PutUint32(vm.Memory[addr+4:addr+8], cp)
		binary.LittleEndian.PutUint32(vm.Memory[addr+8:addr+12], ln)
	} else {
		v, err := vm.pop()
		if err != nil {
			return err
		}
		binary.LittleEndian.PutUint32(vm.Memory[addr:addr+4], v)
	}
	return nil
}

func (vm *VM) stepOp(op byte) error {
	switch op {
	case OpNOP:
		return nil
	case OpPUSH_NIL, OpPUSH_FALSE, OpPUSH_0:
		vm.push(0)
	case OpPUSH_NIL_SLICE:
		vm.push(0)
		vm.push(0)
		vm.push(0)
	case OpPUSH_TRUE, OpPUSH_1:
		vm.push(1)
	case OpPUSH_NEG1:
		vm.push(0xFFFFFFFF)
	case OpPUSH_I8:
		v := int32(int8(vm.fetchU8()))
		vm.push(uint32(v))
	case OpPUSH_U8:
		vm.push(uint32(vm.fetchU8()))
	case OpPUSH_I16:
		v := int32(int16(vm.fetchU16()))
		vm.push(uint32(v))
	case OpPUSH_U16:
		vm.push(uint32(vm.fetchU16()))
	case OpPUSH_I32:
		vm.push(vm.fetchU32())
	case OpPUSH_STR:
		off := int(vm.fetchU16())
		strAddr := StringPoolBase + off
		sLen := uint32(binary.LittleEndian.Uint16(vm.Memory[strAddr : strAddr+2]))
		base := uint32(strAddr + 2)
		vm.push(base)
		vm.push(sLen)
		vm.push(sLen)
	case OpPUSH_CSTR:
		off := int(vm.fetchU16())
		strAddr := StringPoolBase + off
		vm.push(uint32(strAddr + 2))
	case OpPOP:
		_, err := vm.pop()
		return err
	case OpPOP_SLICE:
		for i := 0; i < 3; i++ {
			if _, err := vm.pop(); err != nil {
				return err
			}
		}
	case OpDUP:
		v, err := vm.pop()
		if err != nil {
			return err
		}
		vm.push(v)
		vm.push(v)
	case OpSWAP:
		b, err := vm.pop()
		if err != nil {
			return err
		}
		a, err := vm.pop()
		if err != nil {
			return err
		}
		vm.push(b)
		vm.push(a)
	case OpDUP2:
		b, err := vm.pop()
		if err != nil {
			return err
		}
		a, err := vm.pop()
		if err != nil {
			return err
		}
		vm.push(a)
		vm.push(b)
		vm.push(a)
		vm.push(b)
	case OpROT:
		c, err := vm.pop()
		if err != nil {
			return err
		}
		b, err := vm.pop()
		if err != nil {
			return err
		}
		a, err := vm.pop()
		if err != nil {
			return err
		}
		vm.push(b)
		vm.push(c)
		vm.push(a)
	case OpOVER:
		b, err := vm.pop()
		if err != nil {
			return err
		}
		a, err := vm.pop()
		if err != nil {
			return err
		}
		vm.push(a)
		vm.push(b)
		vm.push(a)

	// Locals & Globals
	case OpLOAD_LOCAL_0, OpLOAD_LOCAL_1, OpLOAD_LOCAL_2, OpLOAD_LOCAL_3:
		return vm.loadLocalSlot(int(op - OpLOAD_LOCAL_0))
	case OpSTORE_LOCAL_0, OpSTORE_LOCAL_1, OpSTORE_LOCAL_2, OpSTORE_LOCAL_3:
		return vm.storeLocalSlot(int(op - OpSTORE_LOCAL_0))
	case OpLOAD_LOCAL:
		return vm.loadLocalSlot(int(vm.fetchU8()))
	case OpSTORE_LOCAL:
		return vm.storeLocalSlot(int(vm.fetchU8()))
	case OpLOAD_GLOBAL:
		gIdx := int(vm.fetchU16())
		if gIdx < 0 || gIdx >= len(vm.GlobalOffsets) {
			return vm.errf("global index %d out of range", gIdx)
		}
		addr := vm.GlobalOffsets[gIdx]
		if vm.GlobalSizes[gIdx] == 12 {
			vm.push(binary.LittleEndian.Uint32(vm.Memory[addr : addr+4]))
			vm.push(binary.LittleEndian.Uint32(vm.Memory[addr+4 : addr+8]))
			vm.push(binary.LittleEndian.Uint32(vm.Memory[addr+8 : addr+12]))
		} else {
			vm.push(binary.LittleEndian.Uint32(vm.Memory[addr : addr+4]))
		}
	case OpSTORE_GLOBAL:
		gIdx := int(vm.fetchU16())
		if gIdx < 0 || gIdx >= len(vm.GlobalOffsets) {
			return vm.errf("global index %d out of range", gIdx)
		}
		addr := vm.GlobalOffsets[gIdx]
		if vm.GlobalSizes[gIdx] == 12 {
			ln, err := vm.pop()
			if err != nil {
				return err
			}
			cp, err := vm.pop()
			if err != nil {
				return err
			}
			bs, err := vm.pop()
			if err != nil {
				return err
			}
			binary.LittleEndian.PutUint32(vm.Memory[addr:addr+4], bs)
			binary.LittleEndian.PutUint32(vm.Memory[addr+4:addr+8], cp)
			binary.LittleEndian.PutUint32(vm.Memory[addr+8:addr+12], ln)
		} else {
			v, err := vm.pop()
			if err != nil {
				return err
			}
			binary.LittleEndian.PutUint32(vm.Memory[addr:addr+4], v)
		}
	case OpADDR_OF_GLOBAL:
		gIdx := int(vm.fetchU16())
		if gIdx < 0 || gIdx >= len(vm.GlobalOffsets) {
			return vm.errf("global index %d out of range", gIdx)
		}
		vm.push(uint32(vm.GlobalOffsets[gIdx]))
	case OpADDR_OF_LOCAL:
		slot := int(vm.fetchU8())
		fn := vm.CurrentFrame.Func
		if slot < 0 || slot >= fn.LocalCount {
			return vm.errf("local slot %d out of range", slot)
		}
		vm.push(uint32(vm.CurrentFrame.FrameAddr + fn.LocalOffsets[slot]))

	// Memory PEEK / POKE & Indexing
	case OpPEEK1:
		addr, err := vm.pop()
		if err != nil {
			return err
		}
		if err := vm.checkAddr(addr, 1); err != nil {
			return err
		}
		vm.push(uint32(vm.Memory[addr]))
	case OpPEEKS1:
		addr, err := vm.pop()
		if err != nil {
			return err
		}
		if err := vm.checkAddr(addr, 1); err != nil {
			return err
		}
		vm.push(uint32(int32(int8(vm.Memory[addr]))))
	case OpPOKE1:
		val, err := vm.pop()
		if err != nil {
			return err
		}
		addr, err := vm.pop()
		if err != nil {
			return err
		}
		if err := vm.checkAddr(addr, 1); err != nil {
			return err
		}
		vm.Memory[addr] = byte(val)
	case OpPEEK2:
		addr, err := vm.pop()
		if err != nil {
			return err
		}
		if err := vm.checkAddr(addr, 2); err != nil {
			return err
		}
		vm.push(uint32(binary.LittleEndian.Uint16(vm.Memory[addr : addr+2])))
	case OpPEEKS2:
		addr, err := vm.pop()
		if err != nil {
			return err
		}
		if err := vm.checkAddr(addr, 2); err != nil {
			return err
		}
		vm.push(uint32(int32(int16(binary.LittleEndian.Uint16(vm.Memory[addr : addr+2])))))
	case OpPOKE2:
		val, err := vm.pop()
		if err != nil {
			return err
		}
		addr, err := vm.pop()
		if err != nil {
			return err
		}
		if err := vm.checkAddr(addr, 2); err != nil {
			return err
		}
		binary.LittleEndian.PutUint16(vm.Memory[addr:addr+2], uint16(val))
	case OpPEEK4:
		addr, err := vm.pop()
		if err != nil {
			return err
		}
		if err := vm.checkAddr(addr, 4); err != nil {
			return err
		}
		vm.push(binary.LittleEndian.Uint32(vm.Memory[addr : addr+4]))
	case OpPOKE4:
		val, err := vm.pop()
		if err != nil {
			return err
		}
		addr, err := vm.pop()
		if err != nil {
			return err
		}
		if err := vm.checkAddr(addr, 4); err != nil {
			return err
		}
		binary.LittleEndian.PutUint32(vm.Memory[addr:addr+4], val)
	case OpSHL1_ADD:
		idx, err := vm.pop()
		if err != nil {
			return err
		}
		base, err := vm.pop()
		if err != nil {
			return err
		}
		vm.push(base + idx*2)
	case OpSHL2_ADD:
		idx, err := vm.pop()
		if err != nil {
			return err
		}
		base, err := vm.pop()
		if err != nil {
			return err
		}
		vm.push(base + idx*4)
	case OpINDEX_ADD:
		elemSz := uint32(vm.fetchU16())
		idx, err := vm.pop()
		if err != nil {
			return err
		}
		base, err := vm.pop()
		if err != nil {
			return err
		}
		vm.push(base + idx*elemSz)
	case OpMEM_COPY:
		cnt, err := vm.pop()
		if err != nil {
			return err
		}
		src, err := vm.pop()
		if err != nil {
			return err
		}
		dst, err := vm.pop()
		if err != nil {
			return err
		}
		if cnt > 0 {
			if err := vm.checkAddr(src, int(cnt)); err != nil {
				return err
			}
			if err := vm.checkAddr(dst, int(cnt)); err != nil {
				return err
			}
			copy(vm.Memory[dst:dst+cnt], vm.Memory[src:src+cnt])
		}
	case OpMEM_SET:
		cnt, err := vm.pop()
		if err != nil {
			return err
		}
		val, err := vm.pop()
		if err != nil {
			return err
		}
		dst, err := vm.pop()
		if err != nil {
			return err
		}
		if cnt > 0 {
			if err := vm.checkAddr(dst, int(cnt)); err != nil {
				return err
			}
			b := byte(val)
			for i := uint32(0); i < cnt; i++ {
				vm.Memory[dst+i] = b
			}
		}
	case OpMEM_CMP:
		cnt, err := vm.pop()
		if err != nil {
			return err
		}
		bAddr, err := vm.pop()
		if err != nil {
			return err
		}
		aAddr, err := vm.pop()
		if err != nil {
			return err
		}
		if cnt == 0 {
			vm.push(0)
		} else {
			if err := vm.checkAddr(aAddr, int(cnt)); err != nil {
				return err
			}
			if err := vm.checkAddr(bAddr, int(cnt)); err != nil {
				return err
			}
			res := bytes.Compare(vm.Memory[aAddr:aAddr+cnt], vm.Memory[bAddr:bAddr+cnt])
			vm.push(uint32(int32(res)))
		}

	// 32-bit ALU
	case OpADD, OpSUB, OpMUL, OpDIV, OpMOD, OpUDIV, OpUMOD,
		OpBIT_AND, OpBIT_OR, OpBIT_XOR, OpSHL, OpSHR, OpUSHR,
		OpCMP_EQ, OpCMP_NE, OpCMP_LT, OpCMP_LE, OpCMP_GT, OpCMP_GE,
		OpUCMP_LT, OpUCMP_LE, OpUCMP_GT, OpUCMP_GE:
		b, err := vm.pop()
		if err != nil {
			return err
		}
		a, err := vm.pop()
		if err != nil {
			return err
		}
		switch op {
		case OpADD:
			vm.push(a + b)
		case OpSUB:
			vm.push(a - b)
		case OpMUL:
			vm.push(a * b)
		case OpDIV:
			if b == 0 {
				return vm.errf("division by zero")
			}
			vm.push(uint32(int32(a) / int32(b)))
		case OpMOD:
			if b == 0 {
				return vm.errf("modulo by zero")
			}
			vm.push(uint32(int32(a) % int32(b)))
		case OpUDIV:
			if b == 0 {
				return vm.errf("unsigned division by zero")
			}
			vm.push(a / b)
		case OpUMOD:
			if b == 0 {
				return vm.errf("unsigned modulo by zero")
			}
			vm.push(a % b)
		case OpBIT_AND:
			vm.push(a & b)
		case OpBIT_OR:
			vm.push(a | b)
		case OpBIT_XOR:
			vm.push(a ^ b)
		case OpSHL:
			vm.push(a << (b & 31))
		case OpSHR:
			vm.push(uint32(int32(a) >> (b & 31)))
		case OpUSHR:
			vm.push(a >> (b & 31))
		case OpCMP_EQ:
			if a == b {
				vm.push(1)
			} else {
				vm.push(0)
			}
		case OpCMP_NE:
			if a != b {
				vm.push(1)
			} else {
				vm.push(0)
			}
		case OpCMP_LT:
			if int32(a) < int32(b) {
				vm.push(1)
			} else {
				vm.push(0)
			}
		case OpCMP_LE:
			if int32(a) <= int32(b) {
				vm.push(1)
			} else {
				vm.push(0)
			}
		case OpCMP_GT:
			if int32(a) > int32(b) {
				vm.push(1)
			} else {
				vm.push(0)
			}
		case OpCMP_GE:
			if int32(a) >= int32(b) {
				vm.push(1)
			} else {
				vm.push(0)
			}
		case OpUCMP_LT:
			if a < b {
				vm.push(1)
			} else {
				vm.push(0)
			}
		case OpUCMP_LE:
			if a <= b {
				vm.push(1)
			} else {
				vm.push(0)
			}
		case OpUCMP_GT:
			if a > b {
				vm.push(1)
			} else {
				vm.push(0)
			}
		case OpUCMP_GE:
			if a >= b {
				vm.push(1)
			} else {
				vm.push(0)
			}
		}

	case OpNEG:
		a, err := vm.pop()
		if err != nil {
			return err
		}
		vm.push(uint32(-int32(a)))
	case OpBIT_NOT:
		a, err := vm.pop()
		if err != nil {
			return err
		}
		vm.push(^a)
	case OpNOT:
		a, err := vm.pop()
		if err != nil {
			return err
		}
		if a == 0 {
			vm.push(1)
		} else {
			vm.push(0)
		}

	// IEEE-754 32-Bit Floating Point ALU & Math
	case OpFADD, OpFSUB, OpFMUL, OpFDIV, OpFATAN2,
		OpFCMP_EQ, OpFCMP_NE, OpFCMP_LT, OpFCMP_LE, OpFCMP_GT, OpFCMP_GE:
		bBits, err := vm.pop()
		if err != nil {
			return err
		}
		aBits, err := vm.pop()
		if err != nil {
			return err
		}
		af := math.Float32frombits(aBits)
		bf := math.Float32frombits(bBits)
		switch op {
		case OpFADD:
			vm.push(math.Float32bits(af + bf))
		case OpFSUB:
			vm.push(math.Float32bits(af - bf))
		case OpFMUL:
			vm.push(math.Float32bits(af * bf))
		case OpFDIV:
			vm.push(math.Float32bits(af / bf))
		case OpFATAN2:
			vm.push(math.Float32bits(float32(math.Atan2(float64(af), float64(bf)))))
		case OpFCMP_EQ:
			if af == bf {
				vm.push(1)
			} else {
				vm.push(0)
			}
		case OpFCMP_NE:
			if af != bf {
				vm.push(1)
			} else {
				vm.push(0)
			}
		case OpFCMP_LT:
			if af < bf {
				vm.push(1)
			} else {
				vm.push(0)
			}
		case OpFCMP_LE:
			if af <= bf {
				vm.push(1)
			} else {
				vm.push(0)
			}
		case OpFCMP_GT:
			if af > bf {
				vm.push(1)
			} else {
				vm.push(0)
			}
		case OpFCMP_GE:
			if af >= bf {
				vm.push(1)
			} else {
				vm.push(0)
			}
		}

	case OpFNEG, OpFABS, OpFSQRT, OpFSIN, OpFCOS, OpFFLOOR, OpFCEIL, OpI32_TO_F32, OpF32_TO_I32:
		aBits, err := vm.pop()
		if err != nil {
			return err
		}
		switch op {
		case OpFNEG:
			vm.push(math.Float32bits(-math.Float32frombits(aBits)))
		case OpFABS:
			vm.push(math.Float32bits(float32(math.Abs(float64(math.Float32frombits(aBits))))))
		case OpFSQRT:
			vm.push(math.Float32bits(float32(math.Sqrt(float64(math.Float32frombits(aBits))))))
		case OpFSIN:
			vm.push(math.Float32bits(float32(math.Sin(float64(math.Float32frombits(aBits))))))
		case OpFCOS:
			vm.push(math.Float32bits(float32(math.Cos(float64(math.Float32frombits(aBits))))))
		case OpFFLOOR:
			vm.push(math.Float32bits(float32(math.Floor(float64(math.Float32frombits(aBits))))))
		case OpFCEIL:
			vm.push(math.Float32bits(float32(math.Ceil(float64(math.Float32frombits(aBits))))))
		case OpI32_TO_F32:
			vm.push(math.Float32bits(float32(int32(aBits))))
		case OpF32_TO_I32:
			vm.push(uint32(int32(math.Float32frombits(aBits))))
		}

	// Control Flow
	case OpJUMP:
		rel := int(int16(vm.fetchU16()))
		vm.PC += rel
	case OpJUMP_IF_TRUE:
		rel := int(int16(vm.fetchU16()))
		cond, err := vm.pop()
		if err != nil {
			return err
		}
		if cond != 0 {
			vm.PC += rel
		}
	case OpJUMP_IF_FALSE:
		rel := int(int16(vm.fetchU16()))
		cond, err := vm.pop()
		if err != nil {
			return err
		}
		if cond == 0 {
			vm.PC += rel
		}
	case OpCALL:
		fIdx := int(vm.fetchU16())
		if fIdx < 0 || fIdx >= len(vm.Functions) {
			return vm.errf("call function index %d out of range", fIdx)
		}
		callee := vm.Functions[fIdx]
		newFrameAddr := vm.FrameSP
		if newFrameAddr+callee.FrameSize > vm.HeapBase {
			return vm.errf("call frame stack overflow calling %s", callee.Name)
		}
		// Zero frame first
		for i := newFrameAddr; i < newFrameAddr+callee.FrameSize; i++ {
			vm.Memory[i] = 0
		}
		// Pop arguments in reverse order
		for a := callee.ArgCount - 1; a >= 0; a-- {
			pAddr := newFrameAddr + callee.LocalOffsets[a]
			if callee.VarSizes[a] == 12 {
				ln, err := vm.pop()
				if err != nil {
					return err
				}
				cp, err := vm.pop()
				if err != nil {
					return err
				}
				bs, err := vm.pop()
				if err != nil {
					return err
				}
				binary.LittleEndian.PutUint32(vm.Memory[pAddr:pAddr+4], bs)
				binary.LittleEndian.PutUint32(vm.Memory[pAddr+4:pAddr+8], cp)
				binary.LittleEndian.PutUint32(vm.Memory[pAddr+8:pAddr+12], ln)
			} else {
				v, err := vm.pop()
				if err != nil {
					return err
				}
				binary.LittleEndian.PutUint32(vm.Memory[pAddr:pAddr+4], v)
			}
		}
		vm.FrameSP = newFrameAddr + callee.FrameSize
		vm.CallStack = append(vm.CallStack, CallFrame{
			Func:      callee,
			ReturnPC:  vm.PC,
			FrameAddr: newFrameAddr,
		})
		vm.CurrentFrame = &vm.CallStack[len(vm.CallStack)-1]
		vm.PC = callee.CodeOffset

	case OpRET, OpRET_SLICE, OpRET_VOID:
		vm.FrameSP = vm.CurrentFrame.FrameAddr
		retPC := vm.CurrentFrame.ReturnPC
		vm.CallStack = vm.CallStack[:len(vm.CallStack)-1]
		if len(vm.CallStack) == 0 || retPC < 0 {
			vm.Running = false
			return nil
		}
		vm.CurrentFrame = &vm.CallStack[len(vm.CallStack)-1]
		vm.PC = retPC

	case OpHALT:
		vm.ExitCode = 0
		vm.Running = false
	case OpPANIC:
		ln, _ := vm.pop()
		_, _ = vm.pop()
		bs, _ := vm.pop()
		msg, _ := vm.readSliceString(bs, ln)
		return vm.errf("PANIC: %s", msg)

	// Slices & Strings
	case OpSLICE_NEW:
		// Stack already has (base, cap, len) -> leave on stack as 3-word slice!
		return nil
	case OpSLICE_LEN:
		ln, err := vm.pop()
		if err != nil {
			return err
		}
		_, _ = vm.pop()
		_, _ = vm.pop()
		vm.push(ln)
	case OpSLICE_CAP:
		_, err := vm.pop()
		if err != nil {
			return err
		}
		cp, err := vm.pop()
		if err != nil {
			return err
		}
		_, _ = vm.pop()
		vm.push(cp)
	case OpSLICE_DATA:
		_, err := vm.pop()
		if err != nil {
			return err
		}
		_, _ = vm.pop()
		bs, err := vm.pop()
		if err != nil {
			return err
		}
		vm.push(bs)
	case OpSLICE_SUB:
		elemSz, err := vm.pop()
		if err != nil {
			return err
		}
		limit, err := vm.pop()
		if err != nil {
			return err
		}
		start, err := vm.pop()
		if err != nil {
			return err
		}
		_, err = vm.pop()
		if err != nil {
			return err
		}
		cp, err := vm.pop()
		if err != nil {
			return err
		}
		bs, err := vm.pop()
		if err != nil {
			return err
		}
		if start > limit || limit > cp {
			return vm.errf("slice bounds out of range [%d:%d] with cap %d", start, limit, cp)
		}
		vm.push(bs + start*elemSz)
		vm.push(cp - start)
		vm.push(limit - start)
	case OpSTR_CMP:
		l2, err := vm.pop()
		if err != nil {
			return err
		}
		_, _ = vm.pop()
		b2, err := vm.pop()
		if err != nil {
			return err
		}
		l1, err := vm.pop()
		if err != nil {
			return err
		}
		_, _ = vm.pop()
		b1, err := vm.pop()
		if err != nil {
			return err
		}
		s1, err := vm.readSliceString(b1, l1)
		if err != nil {
			return err
		}
		s2, err := vm.readSliceString(b2, l2)
		if err != nil {
			return err
		}
		if s1 < s2 {
			vm.push(0xFFFFFFFF)
		} else if s1 > s2 {
			vm.push(1)
		} else {
			vm.push(0)
		}

	// Managed Heap
	case OpMALLOC:
		sz, err := vm.pop()
		if err != nil {
			return err
		}
		vm.push(vm.heapMalloc(int(sz)))
	case OpZALLOC:
		sz, err := vm.pop()
		if err != nil {
			return err
		}
		addr := vm.heapMalloc(int(sz))
		if addr != 0 {
			for i := uint32(0); i < sz; i++ {
				vm.Memory[addr+i] = 0
			}
		}
		vm.push(addr)
	case OpREALLOC:
		newSz, err := vm.pop()
		if err != nil {
			return err
		}
		oldAddr, err := vm.pop()
		if err != nil {
			return err
		}
		if oldAddr == 0 {
			vm.push(vm.heapMalloc(int(newSz)))
		} else {
			newAddr := vm.heapMalloc(int(newSz))
			if newAddr != 0 {
				oldSz := binary.LittleEndian.Uint32(vm.Memory[oldAddr-8 : oldAddr-4])
				copySz := oldSz
				if newSz < copySz {
					copySz = newSz
				}
				copy(vm.Memory[newAddr:newAddr+copySz], vm.Memory[oldAddr:oldAddr+copySz])
				_ = vm.heapFree(oldAddr)
			}
			vm.push(newAddr)
		}
	case OpFREE:
		addr, err := vm.pop()
		if err != nil {
			return err
		}
		return vm.heapFree(addr)
	case OpHEAP_FREE:
		vm.push(vm.heapBytesFree())

	// Lakatos OS Syscalls
	case OpSYS_EXIT:
		code, err := vm.pop()
		if err != nil {
			return err
		}
		vm.ExitCode = int(int32(code))
		vm.Running = false
	case OpIO_PRINT:
		ln, err := vm.pop()
		if err != nil {
			return err
		}
		_, _ = vm.pop()
		bs, err := vm.pop()
		if err != nil {
			return err
		}
		s, err := vm.readSliceString(bs, ln)
		if err != nil {
			return err
		}
		fmt.Fprintln(vm.Stdout, s)
	case OpSYSCALL:
		sysId := vm.fetchU8()
		return vm.execSyscall(sysId)

	default:
		return vm.errf("unknown opcode 0x%02X", op)
	}
	return nil
}

func (vm *VM) popSliceString() (string, error) {
	ln, err := vm.pop()
	if err != nil {
		return "", err
	}
	_, err = vm.pop()
	if err != nil {
		return "", err
	}
	bs, err := vm.pop()
	if err != nil {
		return "", err
	}
	return vm.readSliceString(bs, ln)
}

func (vm *VM) pushScratchString(s string) {
	base, ln := vm.writeOsScratchString(s)
	vm.push(base)
	vm.push(ln)
	vm.push(ln)
}

func (vm *VM) execSyscall(sysId byte) error {
	switch sysId {
	case SysExit:
		code, err := vm.pop()
		if err != nil {
			return err
		}
		vm.ExitCode = int(int32(code))
		vm.Running = false
	case SysYield:
		vm.VirtualMs += 1
	case SysMillis:
		vm.VirtualMs += 16
		vm.push(vm.VirtualMs)
	case SysMicros:
		vm.VirtualMs += 1
		vm.push(vm.VirtualMs * 1000)
	case SysDelayMs:
		ms, err := vm.pop()
		if err != nil {
			return err
		}
		vm.VirtualMs += ms
	case SysRand:
		mx, err := vm.pop()
		if err != nil {
			return err
		}
		if mx == 0 {
			vm.push(rand.Uint32())
		} else if mx == 1 {
			vm.push(0)
		} else {
			vm.push(rand.Uint32() % mx)
		}
	case SysSetCornerExit:
		en, err := vm.pop()
		if err != nil {
			return err
		}
		vm.CornerExitDisabled = (en == 0)

	case SysGetAirport:
		vm.pushScratchString(vm.AirportCode)
	case SysGetAirportInfo:
		outAddr, err := vm.pop()
		if err != nil {
			return err
		}
		if err := vm.checkAddr(outAddr, 68); err != nil {
			return err
		}
		for i := 0; i < 68; i++ {
			vm.Memory[int(outAddr)+i] = 0
		}
		copy(vm.Memory[outAddr:outAddr+3], []byte(vm.AirportCode))
		copy(vm.Memory[outAddr+4:outAddr+27], []byte("Portland, OR"))
		copy(vm.Memory[outAddr+28:outAddr+59], []byte("PST8PDT,M3.2.0,M11.1.0"))
		binary.LittleEndian.PutUint32(vm.Memory[outAddr+60:outAddr+64], math.Float32bits(45.5898))
		binary.LittleEndian.PutUint32(vm.Memory[outAddr+64:outAddr+68], math.Float32bits(-122.5951))
		vm.push(1)
	case SysGetDeviceName:
		vm.pushScratchString(vm.DeviceName)
	case SysGetSerialNumber:
		vm.pushScratchString(vm.SerialNumber)
	case SysGetShUrl:
		vm.pushScratchString(vm.ShUrl)
	case SysGetUnixTime:
		vm.push(uint32(time.Now().Unix()))
	case SysFormatLocalTime:
		maxLen, err := vm.pop()
		if err != nil {
			return err
		}
		bufAddr, err := vm.pop()
		if err != nil {
			return err
		}
		_, err = vm.popSliceString()
		if err != nil {
			return err
		}
		nowStr := time.Now().Format("15:04")
		if int(maxLen) > 0 {
			if err := vm.checkAddr(bufAddr, int(maxLen)); err != nil {
				return err
			}
			b := []byte(nowStr)
			if len(b) >= int(maxLen) {
				b = b[:maxLen-1]
			}
			copy(vm.Memory[bufAddr:], b)
			vm.Memory[int(bufAddr)+len(b)] = 0
		}
		vm.push(1)
	case SysCityHash32:
		ln, err := vm.pop()
		if err != nil {
			return err
		}
		bufAddr, err := vm.pop()
		if err != nil {
			return err
		}
		var data []byte
		if ln > 0 {
			if err := vm.checkAddr(bufAddr, int(ln)); err != nil {
				return err
			}
			data = vm.Memory[bufAddr : bufAddr+ln]
		}
		vm.push(CityHash32(data))

	// TFT Display Primitives
	case SysTftFillScreen:
		c, err := vm.pop()
		if err != nil {
			return err
		}
		vm.tftFillRect(0, 0, 320, 240, uint16(c))
	case SysTftFillRect:
		c, _ := vm.pop()
		h, _ := vm.pop()
		w, _ := vm.pop()
		y, _ := vm.pop()
		x, err := vm.pop()
		if err != nil {
			return err
		}
		vm.tftFillRect(int(int32(x)), int(int32(y)), int(int32(w)), int(int32(h)), uint16(c))
	case SysTftDrawRect, SysTftDrawRoundRect:
		c, _ := vm.pop()
		if sysId == SysTftDrawRoundRect {
			_, _ = vm.pop() // radius
		}
		h, _ := vm.pop()
		w, _ := vm.pop()
		y, _ := vm.pop()
		x, err := vm.pop()
		if err != nil {
			return err
		}
		vm.tftDrawRect(int(int32(x)), int(int32(y)), int(int32(w)), int(int32(h)), uint16(c))
	case SysTftFillRoundRect:
		c, _ := vm.pop()
		_, _ = vm.pop() // radius
		h, _ := vm.pop()
		w, _ := vm.pop()
		y, _ := vm.pop()
		x, err := vm.pop()
		if err != nil {
			return err
		}
		vm.tftFillRect(int(int32(x)), int(int32(y)), int(int32(w)), int(int32(h)), uint16(c))
	case SysTftDrawLine:
		c, _ := vm.pop()
		y1, _ := vm.pop()
		x1, _ := vm.pop()
		y0, _ := vm.pop()
		x0, err := vm.pop()
		if err != nil {
			return err
		}
		vm.tftDrawLine(int(int32(x0)), int(int32(y0)), int(int32(x1)), int(int32(y1)), uint16(c))
	case SysTftDrawFastHLine:
		c, _ := vm.pop()
		w, _ := vm.pop()
		y, _ := vm.pop()
		x, err := vm.pop()
		if err != nil {
			return err
		}
		vm.tftFillRect(int(int32(x)), int(int32(y)), int(int32(w)), 1, uint16(c))
	case SysTftDrawFastVLine:
		c, _ := vm.pop()
		h, _ := vm.pop()
		y, _ := vm.pop()
		x, err := vm.pop()
		if err != nil {
			return err
		}
		vm.tftFillRect(int(int32(x)), int(int32(y)), 1, int(int32(h)), uint16(c))
	case SysTftFillCircle, SysTftDrawCircle:
		c, _ := vm.pop()
		r, _ := vm.pop()
		y, _ := vm.pop()
		x, err := vm.pop()
		if err != nil {
			return err
		}
		vm.tftCircle(int(int32(x)), int(int32(y)), int(int32(r)), uint16(c), sysId == SysTftFillCircle)
	case SysTftFillTriangle:
		c, _ := vm.pop()
		y2, _ := vm.pop()
		x2, _ := vm.pop()
		y1, _ := vm.pop()
		x1, _ := vm.pop()
		y0, _ := vm.pop()
		x0, err := vm.pop()
		if err != nil {
			return err
		}
		vm.tftFillTriangle(int(int32(x0)), int(int32(y0)), int(int32(x1)), int(int32(y1)), int(int32(x2)), int(int32(y2)), uint16(c))
	case SysTftDrawChar:
		sz, _ := vm.pop()
		bg, _ := vm.pop()
		fg, _ := vm.pop()
		ch, _ := vm.pop()
		y, _ := vm.pop()
		x, err := vm.pop()
		if err != nil {
			return err
		}
		vm.tftDrawGlyph(int(int32(x)), int(int32(y)), byte(ch), uint16(fg), uint16(bg), int(sz))
	case SysTftSetTextColor:
		bg, _ := vm.pop()
		fg, err := vm.pop()
		if err != nil {
			return err
		}
		vm.TextFg = uint16(fg)
		vm.TextBg = uint16(bg)
	case SysTftSetTextDatum:
		d, err := vm.pop()
		if err != nil {
			return err
		}
		vm.TextDatum = uint8(d)
	case SysTftDrawString:
		font, _ := vm.pop()
		y, _ := vm.pop()
		x, _ := vm.pop()
		s, err := vm.popSliceString()
		if err != nil {
			return err
		}
		w := vm.tftDrawText(s, int(int32(x)), int(int32(y)), int(font))
		vm.push(uint32(w))
	case SysTftDrawCStr:
		font, _ := vm.pop()
		y, _ := vm.pop()
		x, _ := vm.pop()
		cstrAddr, err := vm.pop()
		if err != nil {
			return err
		}
		s, err := vm.readCStr(cstrAddr)
		if err != nil {
			return err
		}
		w := vm.tftDrawText(s, int(int32(x)), int(int32(y)), int(font))
		vm.push(uint32(w))
	case SysTftDrawInt:
		font, _ := vm.pop()
		y, _ := vm.pop()
		x, _ := vm.pop()
		val, err := vm.pop()
		if err != nil {
			return err
		}
		s := strconv.Itoa(int(int32(val)))
		w := vm.tftDrawText(s, int(int32(x)), int(int32(y)), int(font))
		vm.push(uint32(w))
	case SysFmtIntToStr:
		maxLen, _ := vm.pop()
		bufAddr, _ := vm.pop()
		val, err := vm.pop()
		if err != nil {
			return err
		}
		isUnsigned := (maxLen & 0x80000000) != 0
		maxLen &= 0x7FFFFFFF
		var s string
		if isUnsigned {
			s = strconv.FormatUint(uint64(val), 10)
		} else {
			s = strconv.Itoa(int(int32(val)))
		}
		if bufAddr != 0 && maxLen > 0 {
			if err := vm.checkAddr(bufAddr, int(maxLen)); err != nil {
				return err
			}
			b := []byte(s)
			if len(b) >= int(maxLen) {
				b = b[:maxLen-1]
			}
			copy(vm.Memory[bufAddr:], b)
			vm.Memory[int(bufAddr)+len(b)] = 0
			vm.push(bufAddr)
			vm.push(maxLen)
			vm.push(uint32(len(b)))
		} else {
			vm.pushScratchString(s)
		}
	case SysFmtFloatToStr:
		decimals, _ := vm.pop()
		valBits, err := vm.pop()
		if err != nil {
			return err
		}
		f := math.Float32frombits(valBits)
		var s string
		if int32(decimals) < 0 {
			s = strconv.FormatFloat(float64(f), 'g', -1, 32)
		} else {
			prec := int(int32(decimals))
			if prec > 9 {
				prec = 9
			}
			s = strconv.FormatFloat(float64(f), 'f', prec, 32)
		}
		vm.pushScratchString(s)
	case SysTftDrawFloat:
		font, _ := vm.pop()
		y, _ := vm.pop()
		x, _ := vm.pop()
		decimals, _ := vm.pop()
		valBits, err := vm.pop()
		if err != nil {
			return err
		}
		f := math.Float32frombits(valBits)
		prec := int(int32(decimals))
		if prec < 0 {
			prec = 2
		} else if prec > 9 {
			prec = 9
		}
		s := strconv.FormatFloat(float64(f), 'f', prec, 32)
		w := vm.tftDrawText(s, int(int32(x)), int(int32(y)), int(font))
		vm.push(uint32(w))
	case SysTftPushRGB565:
		pixAddr, _ := vm.pop()
		h, _ := vm.pop()
		w, _ := vm.pop()
		y, _ := vm.pop()
		x, err := vm.pop()
		if err != nil {
			return err
		}
		totalBytes := int(w) * int(h) * 2
		if err := vm.checkAddr(pixAddr, totalBytes); err != nil {
			return err
		}
		idx := int(pixAddr)
		for row := 0; row < int(h); row++ {
			for col := 0; col < int(w); col++ {
				px := int(int32(x)) + col
				py := int(int32(y)) + row
				c := binary.LittleEndian.Uint16(vm.Memory[idx : idx+2])
				idx += 2
				if px >= 0 && px < 320 && py >= 0 && py < 240 {
					vm.Screen[py][px] = c
				}
			}
		}
	case SysTftDrawPngFile:
		y, _ := vm.pop()
		x, _ := vm.pop()
		path, err := vm.popSliceString()
		if err != nil {
			return err
		}
		hostPath := filepath.Join(vm.VfsRoot, filepath.Clean("/"+path))
		data, err := os.ReadFile(hostPath)
		if err != nil {
			vm.push(0)
		} else if vm.drawPngBytes(data, int(int32(x)), int(int32(y))) {
			vm.push(1)
		} else {
			vm.push(0)
		}
	case SysTftDrawPngMem:
		y, _ := vm.pop()
		x, _ := vm.pop()
		ln, _ := vm.pop()
		bufAddr, err := vm.pop()
		if err != nil {
			return err
		}
		var src []byte
		if bufAddr == 0 {
			if ln > 0 && int(ln) <= len(vm.PrefetchBuf) {
				src = vm.PrefetchBuf[:ln]
			}
		} else if ln > 0 && vm.checkAddr(bufAddr, int(ln)) == nil {
			src = vm.Memory[bufAddr : bufAddr+ln]
		}
		if len(src) == 0 {
			vm.push(0)
		} else if vm.drawPngBytes(src, int(int32(x)), int(int32(y))) {
			vm.push(1)
		} else {
			vm.push(0)
		}
	case SysPngInfo:
		path, err := vm.popSliceString()
		if err != nil {
			return err
		}
		hostPath := filepath.Join(vm.VfsRoot, filepath.Clean("/"+path))
		data, err := os.ReadFile(hostPath)
		if err != nil {
			vm.push(0)
			vm.push(0)
			vm.push(0)
		} else if cfg, err := png.DecodeConfig(bytes.NewReader(data)); err != nil {
			vm.push(0)
			vm.push(0)
			vm.push(0)
		} else {
			vm.push(1)
			vm.push(uint32(cfg.Width))
			vm.push(uint32(cfg.Height))
		}
	case SysTftDrawJpgFile:
		scale, _ := vm.pop()
		y, _ := vm.pop()
		x, _ := vm.pop()
		path, err := vm.popSliceString()
		if err != nil {
			return err
		}
		hostPath := filepath.Join(vm.VfsRoot, filepath.Clean("/"+path))
		data, err := os.ReadFile(hostPath)
		if err != nil {
			vm.push(0)
		} else if vm.drawJpgBytes(data, int(int32(x)), int(int32(y)), int(int32(scale))) {
			vm.push(1)
		} else {
			vm.push(0)
		}
	case SysTftDrawJpgMem:
		scale, _ := vm.pop()
		y, _ := vm.pop()
		x, _ := vm.pop()
		ln, _ := vm.pop()
		bufAddr, err := vm.pop()
		if err != nil {
			return err
		}
		var src []byte
		if bufAddr == 0 {
			if ln > 0 && int(ln) <= len(vm.PrefetchBuf) {
				src = vm.PrefetchBuf[:ln]
			}
		} else if ln > 0 && vm.checkAddr(bufAddr, int(ln)) == nil {
			src = vm.Memory[bufAddr : bufAddr+ln]
		}
		if len(src) == 0 {
			vm.push(0)
		} else if vm.drawJpgBytes(src, int(int32(x)), int(int32(y)), int(int32(scale))) {
			vm.push(1)
		} else {
			vm.push(0)
		}
	case SysJpgInfo:
		path, err := vm.popSliceString()
		if err != nil {
			return err
		}
		hostPath := filepath.Join(vm.VfsRoot, filepath.Clean("/"+path))
		data, err := os.ReadFile(hostPath)
		if err != nil {
			vm.push(0)
			vm.push(0)
			vm.push(0)
			return nil
		}
		cfg, err := jpeg.DecodeConfig(bytes.NewReader(data))
		if err != nil {
			vm.push(0)
			vm.push(0)
			vm.push(0)
		} else {
			vm.push(1)
			vm.push(uint32(cfg.Width))
			vm.push(uint32(cfg.Height))
		}
	case SysPrefetchStart:
		maxLen, _ := vm.pop()
		bufAddr, _ := vm.pop()
		path, err := vm.popSliceString()
		if err != nil {
			return err
		}
		if maxLen == 0 || (bufAddr != 0 && vm.checkAddr(bufAddr, int(maxLen)) != nil) {
			vm.PrefetchStatus = -1
			vm.PrefetchBytes = 0
			vm.push(0)
			return nil
		}
		hostPath := filepath.Join(vm.VfsRoot, filepath.Clean("/"+path))
		data, err := os.ReadFile(hostPath)
		if err != nil || len(data) == 0 {
			vm.PrefetchStatus = -1
			vm.PrefetchBytes = 0
			vm.push(0)
			return nil
		}
		n := len(data)
		if n > int(maxLen) {
			n = int(maxLen)
		}
		if bufAddr == 0 {
			if cap(vm.PrefetchBuf) < int(maxLen) {
				vm.PrefetchBuf = make([]byte, maxLen)
			} else {
				vm.PrefetchBuf = vm.PrefetchBuf[:maxLen]
			}
			copy(vm.PrefetchBuf[:n], data[:n])
		} else {
			copy(vm.Memory[bufAddr:int(bufAddr)+n], data[:n])
		}
		vm.PrefetchBytes = uint32(n)
		vm.PrefetchStatus = 2 // READY
		vm.push(1)
	case SysPrefetchStatus:
		vm.push(uint32(int32(vm.PrefetchStatus)))
		vm.push(vm.PrefetchBytes)
	case SysPrefetchCancel:
		vm.PrefetchStatus = 0
		vm.PrefetchBytes = 0
	case SysTftDrawPal4:
		pixAddr, _ := vm.pop()
		palAddr, _ := vm.pop()
		h, _ := vm.pop()
		w, _ := vm.pop()
		y, _ := vm.pop()
		x, err := vm.pop()
		if err != nil {
			return err
		}
		if w == 0 || h == 0 || (w&1) != 0 {
			return nil
		}
		if err := vm.checkAddr(palAddr, 16*4); err != nil {
			return err
		}
		totalBytes := int(w*h) / 2
		if err := vm.checkAddr(pixAddr, totalBytes); err != nil {
			return err
		}
		var pal [16]uint16
		for i := 0; i < 16; i++ {
			pal[i] = uint16(binary.LittleEndian.Uint32(vm.Memory[palAddr+uint32(i*4):]))
		}
		rowBytes := int(w) / 2
		for row := 0; row < int(h); row++ {
			py := int(int32(y)) + row
			if py < 0 || py >= 240 {
				continue
			}
			rowOff := int(pixAddr) + row*rowBytes
			for col := 0; col < int(w); col++ {
				px := int(int32(x)) + col
				if px < 0 || px >= 320 {
					continue
				}
				b := vm.Memory[rowOff+(col>>1)]
				var nib uint8
				if (col & 1) == 0 {
					nib = (b >> 4) & 0x0F
				} else {
					nib = b & 0x0F
				}
				vm.Screen[py][px] = pal[nib]
			}
		}
	case SysSavePngPal4:
		pixAddr, _ := vm.pop()
		palAddr, _ := vm.pop()
		h, _ := vm.pop()
		w, _ := vm.pop()
		path, err := vm.popSliceString()
		if err != nil {
			return err
		}
		if w == 0 || h == 0 || (w&1) != 0 ||
			vm.checkAddr(palAddr, 16*4) != nil ||
			vm.checkAddr(pixAddr, int(w*h)/2) != nil {
			vm.push(0)
			return nil
		}
		var pal [16]uint16
		for i := 0; i < 16; i++ {
			pal[i] = uint16(binary.LittleEndian.Uint32(vm.Memory[palAddr+uint32(i*4):]))
		}
		totalBytes := int(w*h) / 2
		pngBytes := encodePngPal4(int(w), int(h), pal, vm.Memory[pixAddr:int(pixAddr)+totalBytes])
		hostPath := filepath.Join(vm.VfsRoot, filepath.Clean("/"+path))
		_ = os.MkdirAll(filepath.Dir(hostPath), 0o755)
		if err := os.WriteFile(hostPath, pngBytes, 0o644); err != nil {
			vm.push(0)
		} else {
			vm.push(1)
		}

	// Touch, Keyboard, LED & Sensors
	case SysReadTouch, SysReadClick:
		if len(vm.TouchQueue) > 0 {
			ev := vm.TouchQueue[0]
			vm.TouchQueue = vm.TouchQueue[1:]
			vm.CurrentTouch = ev
		}
		if vm.CurrentTouch.Touched {
			if !vm.CornerExitDisabled && vm.CurrentTouch.X >= 0 && vm.CurrentTouch.X < 10 && vm.CurrentTouch.Y >= 0 && vm.CurrentTouch.Y < 10 {
				vm.CurrentTouch.Touched = false
				vm.ExitCode = 0
				vm.Running = false
				vm.push(0)
				vm.push(0)
				vm.push(0)
				return nil
			}
			vm.push(1)
			vm.push(uint32(vm.CurrentTouch.X))
			vm.push(uint32(vm.CurrentTouch.Y))
			if sysId == SysReadClick {
				vm.CurrentTouch.Touched = false
			}
		} else {
			vm.push(0)
			vm.push(0)
			vm.push(0)
		}
	case SysReadTouchRaw:
		if vm.CurrentTouch.Touched {
			vm.push(1)
			vm.push(uint32(vm.CurrentTouch.X * 12))
			vm.push(uint32(vm.CurrentTouch.Y * 16))
			vm.push(1200)
		} else {
			vm.push(0)
			vm.push(0)
			vm.push(0)
			vm.push(0)
		}
	case SysPromptKeyboard:
		_, _ = vm.pop() // allowCtrlC
		_, _ = vm.pop() // maxLen
		_, _ = vm.pop() // bufAddr
		_, err := vm.popSliceString()
		if err != nil {
			return err
		}
		vm.push(0)
	case SysSetRgbLed:
		_, _ = vm.pop()
		_, _ = vm.pop()
		_, _ = vm.pop()
		_, err := vm.pop()
		if err != nil {
			return err
		}
	case SysReadLdr:
		vm.push(950)
	case SysReadCpuTemp:
		vm.push(425) // 42.5 C

	// VFS & Serial
	case SysFsLs:
		maxEnt, _ := vm.pop()
		entAddr, _ := vm.pop()
		path, err := vm.popSliceString()
		if err != nil {
			return err
		}
		const entryStride = 88 // sizeof(LakatosFsEntry) on Xtensa ESP32
		if maxEnt > 0 {
			if err := vm.checkAddr(entAddr, int(maxEnt)*entryStride); err != nil {
				return err
			}
		}
		if vm.VfsRoot == "" {
			vm.push(0xFFFFFFFF)
			return nil
		}
		hostPath := filepath.Join(vm.VfsRoot, filepath.Clean("/"+path))
		entries, err := os.ReadDir(hostPath)
		if err != nil {
			vm.push(0xFFFFFFFF)
			return nil
		}
		count := 0
		for _, de := range entries {
			if count >= int(maxEnt) {
				break
			}
			base := int(entAddr) + count*entryStride
			for i := 0; i < entryStride; i++ {
				vm.Memory[base+i] = 0
			}
			nm := []byte(de.Name())
			if len(nm) > 63 {
				nm = nm[:63]
			}
			copy(vm.Memory[base:base+63], nm)
			var sz int64
			var mt int64
			if info, err := de.Info(); err == nil {
				sz = info.Size()
				mt = info.ModTime().Unix()
			}
			binary.LittleEndian.PutUint64(vm.Memory[base+64:base+72], uint64(sz))
			binary.LittleEndian.PutUint64(vm.Memory[base+72:base+80], uint64(mt))
			if de.IsDir() {
				vm.Memory[base+80] = 1
			}
			count++
		}
		vm.push(uint32(count))
	case SysFsStat:
		entAddr, _ := vm.pop()
		path, err := vm.popSliceString()
		if err != nil {
			return err
		}
		const entryStride = 88
		if err := vm.checkAddr(entAddr, entryStride); err != nil {
			return err
		}
		if vm.VfsRoot == "" {
			vm.push(0)
			return nil
		}
		hostPath := filepath.Join(vm.VfsRoot, filepath.Clean("/"+path))
		info, err := os.Stat(hostPath)
		if err != nil {
			vm.push(0)
			return nil
		}
		base := int(entAddr)
		for i := 0; i < entryStride; i++ {
			vm.Memory[base+i] = 0
		}
		nm := []byte(info.Name())
		if len(nm) > 63 {
			nm = nm[:63]
		}
		copy(vm.Memory[base:base+63], nm)
		binary.LittleEndian.PutUint64(vm.Memory[base+64:base+72], uint64(info.Size()))
		binary.LittleEndian.PutUint64(vm.Memory[base+72:base+80], uint64(info.ModTime().Unix()))
		if info.IsDir() {
			vm.Memory[base+80] = 1
		}
		vm.push(1)
	case SysFsRead:
		ln, _ := vm.pop()
		bufAddr, _ := vm.pop()
		off, _ := vm.pop()
		path, err := vm.popSliceString()
		if err != nil {
			return err
		}
		if err := vm.checkAddr(bufAddr, int(ln)); err != nil {
			return err
		}
		hostPath := filepath.Join(vm.VfsRoot, filepath.Clean("/"+path))
		data, err := os.ReadFile(hostPath)
		if err != nil || int(off) >= len(data) {
			vm.push(0xFFFFFFFF)
		} else {
			rem := data[off:]
			if len(rem) > int(ln) {
				rem = rem[:ln]
			}
			copy(vm.Memory[bufAddr:], rem)
			vm.push(uint32(len(rem)))
		}
	case SysFsWrite:
		ln, _ := vm.pop()
		bufAddr, _ := vm.pop()
		path, err := vm.popSliceString()
		if err != nil {
			return err
		}
		if err := vm.checkAddr(bufAddr, int(ln)); err != nil {
			return err
		}
		if vm.VfsRoot != "" {
			hostPath := filepath.Join(vm.VfsRoot, filepath.Clean("/"+path))
			_ = os.MkdirAll(filepath.Dir(hostPath), 0o755)
			if err := os.WriteFile(hostPath, vm.Memory[bufAddr:bufAddr+ln], 0o644); err == nil {
				vm.push(1)
				return nil
			}
		}
		vm.push(0)
	case SysFsMkdir:
		parents, _ := vm.pop()
		path, err := vm.popSliceString()
		if err != nil {
			return err
		}
		if vm.VfsRoot != "" {
			hostPath := filepath.Join(vm.VfsRoot, filepath.Clean("/"+path))
			var mkErr error
			if parents != 0 {
				mkErr = os.MkdirAll(hostPath, 0o755)
			} else {
				mkErr = os.Mkdir(hostPath, 0o755)
			}
			if mkErr == nil {
				vm.push(1)
				return nil
			}
		}
		vm.push(0)
	case SysFsRmdir, SysFsRm:
		path, err := vm.popSliceString()
		if err != nil {
			return err
		}
		if vm.VfsRoot != "" {
			hostPath := filepath.Join(vm.VfsRoot, filepath.Clean("/"+path))
			if err := os.Remove(hostPath); err == nil {
				vm.push(1)
				return nil
			}
		}
		vm.push(0)
	case SysWifiConnected:
		vm.push(1)
	case SysHttpGet:
		maxLen, _ := vm.pop()
		bufAddr, _ := vm.pop()
		urlStr, err := vm.popSliceString()
		if err != nil {
			return err
		}
		if maxLen == 0 {
			vm.push(0)
			return nil
		}
		if err := vm.checkAddr(bufAddr, int(maxLen)); err != nil {
			return err
		}
		client := &http.Client{Timeout: 6 * time.Second}
		resp, err := client.Get(urlStr)
		if err != nil {
			vm.push(0xFFFFFFFF)
			return nil
		}
		defer resp.Body.Close()
		if resp.StatusCode < 200 || resp.StatusCode >= 300 {
			vm.push(uint32(int32(-resp.StatusCode)))
			return nil
		}
		body, err := io.ReadAll(io.LimitReader(resp.Body, int64(maxLen)))
		if err != nil {
			vm.push(0xFFFFFFFF)
			return nil
		}
		copy(vm.Memory[bufAddr:int(bufAddr)+len(body)], body)
		if len(body) < int(maxLen) {
			vm.Memory[int(bufAddr)+len(body)] = 0
		}
		vm.push(uint32(len(body)))
	case SysHttpGetCached:
		maxAgeSec, _ := vm.pop()
		maxLen, _ := vm.pop()
		bufAddr, _ := vm.pop()
		urlStr, err := vm.popSliceString()
		if err != nil {
			return err
		}
		if maxLen == 0 {
			vm.push(0)
			return nil
		}
		if err := vm.checkAddr(bufAddr, int(maxLen)); err != nil {
			return err
		}
		hash := CityHash32([]byte(urlStr))
		var cacheHostPath string
		if vm.VfsRoot != "" {
			cacheRel := fmt.Sprintf("/net/cache/%d", hash)
			cacheHostPath = filepath.Join(vm.VfsRoot, filepath.Clean(cacheRel))
			if data, err := os.ReadFile(cacheHostPath); err == nil && len(data) > 0 {
				if nl := bytes.IndexByte(data, '\n'); nl > 0 && nl < 24 {
					line := string(bytes.TrimRight(data[:nl], "\r"))
					if cachedTime, err := strconv.ParseUint(line, 10, 32); err == nil && cachedTime > 0 {
						nowSec := uint64(time.Now().Unix())
						var age uint64
						if nowSec >= cachedTime {
							age = nowSec - cachedTime
						}
						if age <= uint64(maxAgeSec) {
							payload := data[nl+1:]
							if len(payload) > int(maxLen) {
								payload = payload[:maxLen]
							}
							copy(vm.Memory[bufAddr:int(bufAddr)+len(payload)], payload)
							if len(payload) < int(maxLen) {
								vm.Memory[int(bufAddr)+len(payload)] = 0
							}
							vm.push(uint32(len(payload)))
							return nil
						}
					}
				}
			}
		}
		client := &http.Client{Timeout: 6 * time.Second}
		resp, err := client.Get(urlStr)
		if err != nil {
			vm.push(0xFFFFFFFF)
			return nil
		}
		defer resp.Body.Close()
		if resp.StatusCode < 200 || resp.StatusCode >= 300 {
			vm.push(uint32(int32(-resp.StatusCode)))
			return nil
		}
		body, err := io.ReadAll(io.LimitReader(resp.Body, int64(maxLen)))
		if err != nil {
			vm.push(0xFFFFFFFF)
			return nil
		}
		copy(vm.Memory[bufAddr:int(bufAddr)+len(body)], body)
		if len(body) < int(maxLen) {
			vm.Memory[int(bufAddr)+len(body)] = 0
		}
		if cacheHostPath != "" {
			_ = os.MkdirAll(filepath.Dir(cacheHostPath), 0o755)
			hdr := fmt.Sprintf("%d\n", uint32(time.Now().Unix()))
			cacheData := append([]byte(hdr), body...)
			_ = os.WriteFile(cacheHostPath, cacheData, 0o644)
		}
		vm.push(uint32(len(body)))
	case SysSerialAvail:
		vm.push(uint32(len(vm.SerialIn)))
	case SysSerialRead:
		if len(vm.SerialIn) == 0 {
			vm.push(0xFFFFFFFF)
		} else {
			ch := vm.SerialIn[0]
			vm.SerialIn = vm.SerialIn[1:]
			vm.push(uint32(ch))
		}
	case SysSerialWrite:
		ln, _ := vm.pop()
		bufAddr, err := vm.pop()
		if err != nil {
			return err
		}
		if err := vm.checkAddr(bufAddr, int(ln)); err != nil {
			return err
		}
		n, _ := vm.Stdout.Write(vm.Memory[bufAddr : bufAddr+ln])
		vm.push(uint32(n))
	case SysSetAppArg:
		arg, err := vm.popSliceString()
		if err != nil {
			return err
		}
		vm.AppArg = arg
	case SysJoinNative:
		binName, err := vm.popSliceString()
		if err != nil {
			return err
		}
		vm.JoinNativeBin = binName
		vm.ExitCode = 0
		vm.Running = false

	default:
		return vm.errf("unimplemented syscall 0x%02X", sysId)
	}
	return nil
}

// ---------------------------------------------------------------------------
// Emulated 320x240 RGB565 Framebuffer Drawing & Export
// ---------------------------------------------------------------------------
func (vm *VM) tftPlot(x, y int, c uint16) {
	if x >= 0 && x < 320 && y >= 0 && y < 240 {
		vm.Screen[y][x] = c
	}
}

func (vm *VM) tftFillRect(x, y, w, h int, c uint16) {
	for py := y; py < y+h; py++ {
		if py < 0 || py >= 240 {
			continue
		}
		for px := x; px < x+w; px++ {
			if px >= 0 && px < 320 {
				vm.Screen[py][px] = c
			}
		}
	}
}

func (vm *VM) tftDrawRect(x, y, w, h int, c uint16) {
	vm.tftFillRect(x, y, w, 1, c)
	vm.tftFillRect(x, y+h-1, w, 1, c)
	vm.tftFillRect(x, y, 1, h, c)
	vm.tftFillRect(x+w-1, y, 1, h, c)
}

func (vm *VM) tftDrawLine(x0, y0, x1, y1 int, c uint16) {
	dx := x1 - x0
	if dx < 0 {
		dx = -dx
	}
	dy := y1 - y0
	if dy < 0 {
		dy = -dy
	}
	sx := -1
	if x0 < x1 {
		sx = 1
	}
	sy := -1
	if y0 < y1 {
		sy = 1
	}
	err := dx - dy
	for {
		vm.tftPlot(x0, y0, c)
		if x0 == x1 && y0 == y1 {
			break
		}
		e2 := 2 * err
		if e2 > -dy {
			err -= dy
			x0 += sx
		}
		if e2 < dx {
			err += dx
			y0 += sy
		}
	}
}

func (vm *VM) tftCircle(cx, cy, r int, c uint16, fill bool) {
	for dy := -r; dy <= r; dy++ {
		for dx := -r; dx <= r; dx++ {
			d2 := dx*dx + dy*dy
			if fill {
				if d2 <= r*r {
					vm.tftPlot(cx+dx, cy+dy, c)
				}
			} else {
				if d2 >= (r-1)*(r-1) && d2 <= r*r {
					vm.tftPlot(cx+dx, cy+dy, c)
				}
			}
		}
	}
}

func (vm *VM) tftFillTriangle(x0, y0, x1, y1, x2, y2 int, c uint16) {
	if y0 > y1 {
		x0, y0, x1, y1 = x1, y1, x0, y0
	}
	if y1 > y2 {
		x1, y1, x2, y2 = x2, y2, x1, y1
	}
	if y0 > y1 {
		x0, y0, x1, y1 = x1, y1, x0, y0
	}
	if y0 == y2 {
		minX, maxX := x0, x0
		if x1 < minX {
			minX = x1
		}
		if x2 < minX {
			minX = x2
		}
		if x1 > maxX {
			maxX = x1
		}
		if x2 > maxX {
			maxX = x2
		}
		vm.tftFillRect(minX, y0, maxX-minX+1, 1, c)
		return
	}
	totalH := y2 - y0
	for y := y0; y <= y2; y++ {
		secondHalf := y > y1 || y1 == y0
		segH := y1 - y0
		if secondHalf {
			segH = y2 - y1
		}
		if segH == 0 {
			continue
		}
		ax := x0 + (x2-x0)*(y-y0)/totalH
		var bx int
		if secondHalf {
			bx = x1 + (x2-x1)*(y-y1)/segH
		} else {
			bx = x0 + (x1-x0)*(y-y0)/segH
		}
		if ax > bx {
			ax, bx = bx, ax
		}
		vm.tftFillRect(ax, y, bx-ax+1, 1, c)
	}
}

func (vm *VM) tftDrawGlyph(x, y int, ch byte, fg, bg uint16, scale int) {
	if scale < 1 {
		scale = 1
	}
	// Simple 5x7 block glyph for emulator visualization
	for row := 0; row < 7*scale; row++ {
		for col := 0; col < 5*scale; col++ {
			on := false
			if ch > 32 && ch <= 126 {
				r := row / scale
				c := col / scale
				on = ((int(ch)+r*3+c*5)%3 != 0) || r == 0 || r == 6 || c == 0
			}
			if on {
				vm.tftPlot(x+col, y+row, fg)
			} else if bg != fg {
				vm.tftPlot(x+col, y+row, bg)
			}
		}
	}
}

func (vm *VM) tftDrawText(s string, x, y, font int) int {
	scale := 1
	if font >= 4 {
		scale = 2
	}
	charW := 6 * scale
	charH := 8 * scale
	totalW := len(s) * charW

	switch vm.TextDatum {
	case 1: // TC_DATUM
		x -= totalW / 2
	case 2: // TR_DATUM
		x -= totalW
	case 3: // ML_DATUM
		y -= charH / 2
	case 4: // MC_DATUM
		x -= totalW / 2
		y -= charH / 2
	case 5: // MR_DATUM
		x -= totalW
		y -= charH / 2
	case 6: // BL_DATUM
		y -= charH
	case 7: // BC_DATUM
		x -= totalW / 2
		y -= charH
	case 8: // BR_DATUM
		x -= totalW
		y -= charH
	}

	for i := 0; i < len(s); i++ {
		vm.tftDrawGlyph(x+i*charW, y, s[i], vm.TextFg, vm.TextBg, scale)
	}
	return totalW
}

// SavePPM exports the 320x240 RGB565 screen buffer as a standard P6 PPM image.
func (vm *VM) SavePPM(filename string) error {
	var buf bytes.Buffer
	fmt.Fprintf(&buf, "P6\n320 240\n255\n")
	for y := 0; y < 240; y++ {
		for x := 0; x < 320; x++ {
			c := vm.Screen[y][x]
			r := byte(((c >> 11) & 0x1F) * 255 / 31)
			g := byte(((c >> 5) & 0x3F) * 255 / 63)
			b := byte((c & 0x1F) * 255 / 31)
			buf.WriteByte(r)
			buf.WriteByte(g)
			buf.WriteByte(b)
		}
	}
	return os.WriteFile(filename, buf.Bytes(), 0o644)
}

// RenderASCII returns an 80x30 ASCII preview of the 320x240 screen (4x8 pixel cells).
func (vm *VM) RenderASCII() string {
	var out bytes.Buffer
	out.WriteString("+--------------------------------------------------------------------------------+\n")
	for row := 0; row < 30; row++ {
		out.WriteByte('|')
		for col := 0; col < 80; col++ {
			lit := 0
			for dy := 0; dy < 8; dy++ {
				for dx := 0; dx < 4; dx++ {
					if vm.Screen[row*8+dy][col*4+dx] != 0 {
						lit++
					}
				}
			}
			if lit == 0 {
				out.WriteByte(' ')
			} else if lit < 8 {
				out.WriteByte('.')
			} else if lit < 20 {
				out.WriteByte('*')
			} else {
				out.WriteByte('#')
			}
		}
		out.WriteString("|\n")
	}
	out.WriteString("+--------------------------------------------------------------------------------+\n")
	return out.String()
}

func (vm *VM) drawPngBytes(data []byte, dstX, dstY int) bool {
	img, err := png.Decode(bytes.NewReader(data))
	if err != nil {
		return false
	}
	b := img.Bounds()
	w := b.Dx()
	h := b.Dy()
	step := 1
	if dstX < 0 {
		boxY := dstY
		if boxY < 0 || boxY >= 240 {
			boxY = 0
		}
		boxW := 320
		boxH := 240 - boxY
		for step < 8 && (w/step > boxW || h/step > boxH) {
			step++
		}
		sw := w / step
		sh := h / step
		dstX = 0
		if boxW > sw {
			dstX = (boxW - sw) / 2
		}
		dstY = boxY
		if boxH > sh {
			dstY = boxY + (boxH-sh)/2
		}
	}
	for oy := 0; oy*step < h; oy++ {
		sy := dstY + oy
		if sy < 0 || sy >= 240 {
			continue
		}
		py := b.Min.Y + oy*step
		for ox := 0; ox*step < w; ox++ {
			sx := dstX + ox
			if sx < 0 || sx >= 320 {
				continue
			}
			px := b.Min.X + ox*step
			r16, g16, b16, a16 := img.At(px, py).RGBA()
			if a16 >= 0x8000 {
				r8 := uint16((r16 * 255) / a16)
				g8 := uint16((g16 * 255) / a16)
				b8 := uint16((b16 * 255) / a16)
				c565 := ((r8 & 0xF8) << 8) | ((g8 & 0xFC) << 3) | (b8 >> 3)
				vm.Screen[sy][sx] = c565
			}
		}
	}
	return true
}

func (vm *VM) drawJpgBytes(data []byte, dstX, dstY, scale int) bool {
	if scale < 0 {
		scale = 0
	} else if scale > 3 {
		scale = 3
	}
	img, err := jpeg.Decode(bytes.NewReader(data))
	if err != nil {
		return false
	}
	b := img.Bounds()
	if dstX < 0 {
		boxY := dstY
		if boxY < 0 || boxY >= 240 {
			boxY = 0
		}
		boxW := 320
		boxH := 240 - boxY
		scale = 0
		for scale < 3 && ((b.Dx()>>scale) > boxW || (b.Dy()>>scale) > boxH) {
			scale++
		}
		sw := b.Dx() >> scale
		sh := b.Dy() >> scale
		dstX = 0
		if boxW > sw {
			dstX = (boxW - sw) / 2
		}
		dstY = boxY
		if boxH > sh {
			dstY = boxY + (boxH-sh)/2
		}
	}
	w := b.Dx() >> scale
	h := b.Dy() >> scale
	step := 1 << scale
	for oy := 0; oy < h; oy++ {
		sy := dstY + oy
		if sy < 0 || sy >= 240 {
			continue
		}
		py := b.Min.Y + oy*step
		for ox := 0; ox < w; ox++ {
			sx := dstX + ox
			if sx < 0 || sx >= 320 {
				continue
			}
			px := b.Min.X + ox*step
			r16, g16, b16, _ := img.At(px, py).RGBA()
			r8 := uint16(r16 >> 8)
			g8 := uint16(g16 >> 8)
			b8 := uint16(b16 >> 8)
			c565 := ((r8 & 0xF8) << 8) | ((g8 & 0xFC) << 3) | (b8 >> 3)
			vm.Screen[sy][sx] = c565
		}
	}
	return true
}

func cityRotate32(val uint32, shift uint) uint32 {
	if shift == 0 {
		return val
	}
	return (val >> shift) | (val << (32 - shift))
}

func cityBswap32(x uint32) uint32 {
	return ((x & 0x000000FF) << 24) |
		((x & 0x0000FF00) << 8) |
		((x & 0x00FF0000) >> 8) |
		((x & 0xFF000000) >> 24)
}

func cityFmix(h uint32) uint32 {
	h ^= h >> 16
	h *= 0x85ebca6b
	h ^= h >> 13
	h *= 0xc2b2ae35
	h ^= h >> 16
	return h
}

func cityMur(a, h uint32) uint32 {
	const c1 uint32 = 0xcc9e2d51
	const c2 uint32 = 0x1b873593
	a *= c1
	a = cityRotate32(a, 17)
	a *= c2
	h ^= a
	h = cityRotate32(h, 19)
	return h*5 + 0xe6546b64
}

func cityHash32Len0to4(s []byte) uint32 {
	const c1 uint32 = 0xcc9e2d51
	var b uint32 = 0
	var c uint32 = 9
	for i := 0; i < len(s); i++ {
		v := int8(s[i])
		b = b*c1 + uint32(int32(v))
		c ^= b
	}
	return cityFmix(cityMur(b, cityMur(uint32(len(s)), c)))
}

func cityHash32Len5to12(s []byte) uint32 {
	n := uint32(len(s))
	a := n
	b := n * 5
	c := uint32(9)
	d := b
	a += binary.LittleEndian.Uint32(s[0:4])
	b += binary.LittleEndian.Uint32(s[len(s)-4:])
	off := (len(s) >> 1) & 4
	c += binary.LittleEndian.Uint32(s[off : off+4])
	return cityFmix(cityMur(c, cityMur(b, cityMur(a, d))))
}

func cityHash32Len13to24(s []byte) uint32 {
	n := len(s)
	a := binary.LittleEndian.Uint32(s[(n>>1)-4:])
	b := binary.LittleEndian.Uint32(s[4:])
	c := binary.LittleEndian.Uint32(s[n-8:])
	d := binary.LittleEndian.Uint32(s[n>>1:])
	e := binary.LittleEndian.Uint32(s[0:])
	f := binary.LittleEndian.Uint32(s[n-4:])
	h := uint32(n)
	return cityFmix(cityMur(f, cityMur(e, cityMur(d, cityMur(c, cityMur(b, cityMur(a, h)))))))
}

// CityHash32 computes Google's 32-bit CityHash of s.
func CityHash32(s []byte) uint32 {
	n := len(s)
	if n <= 24 {
		if n <= 12 {
			if n <= 4 {
				return cityHash32Len0to4(s)
			}
			return cityHash32Len5to12(s)
		}
		return cityHash32Len13to24(s)
	}
	const c1 uint32 = 0xcc9e2d51
	const c2 uint32 = 0x1b873593
	h := uint32(n)
	g := c1 * h
	f := g
	a0 := cityRotate32(binary.LittleEndian.Uint32(s[n-4:])*c1, 17) * c2
	a1 := cityRotate32(binary.LittleEndian.Uint32(s[n-8:])*c1, 17) * c2
	a2 := cityRotate32(binary.LittleEndian.Uint32(s[n-16:])*c1, 17) * c2
	a3 := cityRotate32(binary.LittleEndian.Uint32(s[n-12:])*c1, 17) * c2
	a4 := cityRotate32(binary.LittleEndian.Uint32(s[n-20:])*c1, 17) * c2
	h ^= a0
	h = cityRotate32(h, 19)
	h = h*5 + 0xe6546b64
	h ^= a2
	h = cityRotate32(h, 19)
	h = h*5 + 0xe6546b64
	g ^= a1
	g = cityRotate32(g, 19)
	g = g*5 + 0xe6546b64
	g ^= a3
	g = cityRotate32(g, 19)
	g = g*5 + 0xe6546b64
	f += a4
	f = cityRotate32(f, 19) + 113
	iters := (n - 1) / 20
	pos := 0
	for {
		b0 := cityRotate32(binary.LittleEndian.Uint32(s[pos:])*c1, 17) * c2
		b1 := binary.LittleEndian.Uint32(s[pos+4:])
		b2 := cityRotate32(binary.LittleEndian.Uint32(s[pos+8:])*c1, 17) * c2
		b3 := cityRotate32(binary.LittleEndian.Uint32(s[pos+12:])*c1, 17) * c2
		b4 := binary.LittleEndian.Uint32(s[pos+16:])
		h ^= b0
		h = cityRotate32(h, 18)
		h = h*5 + 0xe6546b64
		f += b1
		f = cityRotate32(f, 19)
		f = f * c1
		g += b2
		g = cityRotate32(g, 18)
		g = g*5 + 0xe6546b64
		h ^= b3 + b1
		h = cityRotate32(h, 19)
		h = h*5 + 0xe6546b64
		g ^= b4
		g = cityBswap32(g) * 5
		h += b4 * 5
		h = cityBswap32(h)
		f += b0
		f, g, h = g, h, f
		pos += 20
		iters--
		if iters == 0 {
			break
		}
	}
	g = cityRotate32(g, 11) * c1
	g = cityRotate32(g, 17) * c1
	f = cityRotate32(f, 11) * c1
	f = cityRotate32(f, 17) * c1
	h = cityRotate32(h+g, 19)
	h = h*5 + 0xe6546b64
	h = cityRotate32(h, 17) * c1
	h = cityRotate32(h+f, 19)
	h = h*5 + 0xe6546b64
	h = cityRotate32(h, 17) * c1
	return h
}

// encodePngPal4 encodes a 4-bit indexed-color PNG (colorType=3, bitDepth=4)
// using Filter Type 2 (Up) and RFC 1951 Fixed-Huffman RLE (dist=1) compression,
// matching the exact algorithm in src/lakatos/lakatos_png.cpp.
func encodePngPal4(w, h int, pal565 [16]uint16, pix4bpp []byte) []byte {
	var bitBuf uint32
	var bitCnt uint
	var idat bytes.Buffer

	// Zlib header (CMF=0x78, FLG=0x01)
	idat.WriteByte(0x78)
	idat.WriteByte(0x01)

	emitBits := func(val uint32, nbits uint) {
		bitBuf |= (val & ((1 << nbits) - 1)) << bitCnt
		bitCnt += nbits
		for bitCnt >= 8 {
			idat.WriteByte(byte(bitBuf & 0xFF))
			bitBuf >>= 8
			bitCnt -= 8
		}
	}
	emitHuff := func(code uint32, nbits uint) {
		var rev uint32
		for i := uint(0); i < nbits; i++ {
			rev = (rev << 1) | ((code >> i) & 1)
		}
		emitBits(rev, nbits)
	}
	emitLit := func(b int) {
		if b < 144 {
			emitHuff(uint32(0x30+b), 8)
		} else {
			emitHuff(uint32(0x190+(b-144)), 9)
		}
	}
	baseLen := [29]int{
		3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
		35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258,
	}
	extraBits := [29]uint{
		0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
		3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0,
	}
	emitLenDist1 := func(length int) {
		idx := 28
		for i := 0; i < 28; i++ {
			if length >= baseLen[i] && length < baseLen[i+1] {
				idx = i
				break
			}
		}
		sym := 257 + idx
		if sym <= 279 {
			emitHuff(uint32(sym-256), 7)
		} else {
			emitHuff(uint32(0xC0+(sym-280)), 8)
		}
		if extraBits[idx] > 0 {
			emitBits(uint32(length-baseLen[idx]), extraBits[idx])
		}
		// Distance = 1 -> distance code 0 (5 zero bits)
		emitBits(0, 5)
	}

	// Block header: BFINAL=1 (1 bit), BTYPE=01 Fixed Huffman (2 bits)
	emitBits(1, 1)
	emitBits(1, 2)

	var s1 uint32 = 1
	var s2 uint32 = 0
	prevByte := -1
	runLen := 0

	flushRun := func() {
		for runLen >= 3 {
			take := runLen
			if take > 258 {
				take = 258
			}
			emitLenDist1(take)
			runLen -= take
		}
		for runLen > 0 {
			emitLit(prevByte)
			runLen--
		}
	}
	feedByte := func(b byte) {
		s1 = (s1 + uint32(b)) % 65521
		s2 = (s2 + s1) % 65521
		if int(b) == prevByte {
			runLen++
			if runLen == 258 {
				emitLenDist1(258)
				runLen = 0
			}
		} else {
			flushRun()
			emitLit(int(b))
			prevByte = int(b)
			runLen = 0
		}
	}

	rowBytes := w / 2
	for y := 0; y < h; y++ {
		curOff := y * rowBytes
		if y == 0 {
			feedByte(0) // Filter 0 (None)
			for i := 0; i < rowBytes; i++ {
				feedByte(pix4bpp[curOff+i])
			}
		} else {
			prevOff := (y - 1) * rowBytes
			feedByte(2) // Filter 2 (Up)
			for i := 0; i < rowBytes; i++ {
				feedByte(pix4bpp[curOff+i] - pix4bpp[prevOff+i])
			}
		}
	}
	flushRun()
	// EOB (256)
	emitHuff(0, 7)
	if bitCnt > 0 {
		idat.WriteByte(byte(bitBuf & 0xFF))
	}
	adler := (s2 << 16) | s1
	var adlerBytes [4]byte
	binary.BigEndian.PutUint32(adlerBytes[:], adler)
	idat.Write(adlerBytes[:])

	pngCrc32 := func(data []byte) uint32 {
		c := uint32(0xFFFFFFFF)
		for _, b := range data {
			c ^= uint32(b)
			for k := 0; k < 8; k++ {
				if (c & 1) != 0 {
					c = (c >> 1) ^ 0xEDB88320
				} else {
					c >>= 1
				}
			}
		}
		return ^c
	}

	var out bytes.Buffer
	out.Write([]byte{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A})
	writeChunk := func(typ string, payload []byte) {
		var lenBuf [4]byte
		binary.BigEndian.PutUint32(lenBuf[:], uint32(len(payload)))
		out.Write(lenBuf[:])
		var body bytes.Buffer
		body.WriteString(typ)
		body.Write(payload)
		out.Write(body.Bytes())
		var crcBuf [4]byte
		binary.BigEndian.PutUint32(crcBuf[:], pngCrc32(body.Bytes()))
		out.Write(crcBuf[:])
	}

	var ihdr [13]byte
	binary.BigEndian.PutUint32(ihdr[0:4], uint32(w))
	binary.BigEndian.PutUint32(ihdr[4:8], uint32(h))
	ihdr[8] = 4 // bit depth = 4
	ihdr[9] = 3 // color type = 3 (Indexed-color)
	writeChunk("IHDR", ihdr[:])

	var plte [16 * 3]byte
	for i := 0; i < 16; i++ {
		c := pal565[i]
		r5 := (c >> 11) & 0x1F
		g6 := (c >> 5) & 0x3F
		b5 := c & 0x1F
		plte[i*3+0] = byte((r5 << 3) | (r5 >> 2))
		plte[i*3+1] = byte((g6 << 2) | (g6 >> 4))
		plte[i*3+2] = byte((b5 << 3) | (b5 >> 2))
	}
	writeChunk("PLTE", plte[:])
	writeChunk("IDAT", idat.Bytes())
	writeChunk("IEND", nil)
	return out.Bytes()
}
