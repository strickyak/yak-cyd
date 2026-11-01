package lcode

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"strings"
)

// Disassemble converts a .lapp or raw LCOD binary back into round-trippable .lasm text.
func Disassemble(data []byte) (string, error) {
	payload, title, err := UnwrapLapp(data)
	if err != nil {
		return "", err
	}
	if len(payload) < LCodeHeaderSz || !bytes.Equal(payload[0:4], LCodeMagic) {
		return "", fmt.Errorf("invalid LCOD module header")
	}

	strPoolSize := int(binary.LittleEndian.Uint32(payload[8:12]))
	globalsCount := int(binary.LittleEndian.Uint16(payload[12:14]))
	funcCount := int(binary.LittleEndian.Uint16(payload[14:16]))
	entryFunc := int(binary.LittleEndian.Uint16(payload[16:18]))
	codeSize := int(binary.LittleEndian.Uint32(payload[20:24]))

	var out strings.Builder
	fmt.Fprintf(&out, "; LCode Disassembly (Title: %q, Payload: %d bytes, Code: %d bytes)\n\n",
		title, len(payload), codeSize)

	offset := LCodeHeaderSz
	for i := 0; i < globalsCount; i++ {
		if offset+4 > len(payload) {
			return "", fmt.Errorf("truncated globals table")
		}
		sz := int(binary.LittleEndian.Uint32(payload[offset : offset+4]))
		offset += 4
		fmt.Fprintf(&out, ".global g%d: %d\n", i, sz)
	}
	if globalsCount > 0 {
		out.WriteString("\n")
	}

	strPoolStart := offset
	strPoolEnd := strPoolStart + strPoolSize
	if strPoolEnd > len(payload) {
		return "", fmt.Errorf("truncated string pool")
	}
	strPool := payload[strPoolStart:strPoolEnd]
	offset = strPoolEnd

	type disFunc struct {
		index      int
		name       string
		argCount   int
		localCount int
		frameSize  int
		codeOffset int
		codeSize   int
		varSizes   []int
	}
	funcs := make([]disFunc, funcCount)
	maxMetaEnd := offset + funcCount*LCodeFuncEntSz

	for i := 0; i < funcCount; i++ {
		if offset+LCodeFuncEntSz > len(payload) {
			return "", fmt.Errorf("truncated function table")
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
		mPos := metaOffset
		for v := 0; v < localCount; v++ {
			if mPos+4 > len(payload) {
				return "", fmt.Errorf("truncated function var sizes")
			}
			varSizes[v] = int(binary.LittleEndian.Uint32(payload[mPos : mPos+4]))
			mPos += 4
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
		funcs[i] = disFunc{
			index:      i,
			name:       fnName,
			argCount:   argCount,
			localCount: localCount,
			frameSize:  frameSize,
			codeOffset: codeOffset,
			codeSize:   fnCodeSize,
			varSizes:   varSizes,
		}
	}

	codeStart := maxMetaEnd
	if codeStart+codeSize > len(payload) {
		return "", fmt.Errorf("truncated code section")
	}
	code := payload[codeStart : codeStart+codeSize]

	readPoolString := func(off int) string {
		if off+2 > len(strPool) {
			return `""`
		}
		sLen := int(binary.LittleEndian.Uint16(strPool[off : off+2]))
		if off+2+sLen > len(strPool) {
			return `""`
		}
		return fmt.Sprintf("%q", string(strPool[off+2:off+2+sLen]))
	}

	for _, fn := range funcs {
		entryTag := ""
		if fn.index == entryFunc {
			entryTag = " entry"
		}
		fmt.Fprintf(&out, ".function %s%s\n", fn.name, entryTag)
		for v := 0; v < fn.localCount; v++ {
			if v < fn.argCount {
				fmt.Fprintf(&out, "    .param p%d: %d\n", v, fn.varSizes[v])
			} else {
				fmt.Fprintf(&out, "    .local l%d: %d\n", v, fn.varSizes[v])
			}
		}

		fnCode := code[fn.codeOffset : fn.codeOffset+fn.codeSize]

		// First pass over function bytecode to collect branch target PCs
		branchTargets := make(map[int]string)
		pc := 0
		for pc < len(fnCode) {
			op := fnCode[pc]
			info, ok := OpcodeTable[op]
			if !ok {
				break
			}
			if info.ArgType == ArgRel16 && pc+3 <= len(fnCode) {
				rel := int(int16(binary.LittleEndian.Uint16(fnCode[pc+1 : pc+3])))
				target := pc + 3 + rel
				branchTargets[target] = fmt.Sprintf(".L_%04X", target)
			}
			pc += 1 + info.ArgBytes
		}

		pc = 0
		for pc < len(fnCode) {
			if lbl, ok := branchTargets[pc]; ok {
				fmt.Fprintf(&out, "%s:\n", lbl)
			}
			op := fnCode[pc]
			info, ok := OpcodeTable[op]
			if !ok {
				fmt.Fprintf(&out, "    ; unknown opcode 0x%02X\n", op)
				pc++
				continue
			}
			argBytes := fnCode[pc+1 : pc+1+info.ArgBytes]
			switch info.ArgType {
			case ArgNone:
				fmt.Fprintf(&out, "    %s\n", info.Mnemonic)
			case ArgImm8S:
				fmt.Fprintf(&out, "    %s %d\n", info.Mnemonic, int8(argBytes[0]))
			case ArgImm8U:
				fmt.Fprintf(&out, "    %s %d\n", info.Mnemonic, argBytes[0])
			case ArgSys8:
				if sName, ok := SyscallToName[argBytes[0]]; ok {
					fmt.Fprintf(&out, "    %s %s\n", info.Mnemonic, sName)
				} else {
					fmt.Fprintf(&out, "    %s %d\n", info.Mnemonic, argBytes[0])
				}
			case ArgImm16S:
				v := int16(binary.LittleEndian.Uint16(argBytes))
				fmt.Fprintf(&out, "    %s %d\n", info.Mnemonic, v)
			case ArgImm16U:
				v := binary.LittleEndian.Uint16(argBytes)
				fmt.Fprintf(&out, "    %s %d\n", info.Mnemonic, v)
			case ArgImm32:
				v := int32(binary.LittleEndian.Uint32(argBytes))
				fmt.Fprintf(&out, "    %s %d\n", info.Mnemonic, v)
			case ArgStr16:
				off := int(binary.LittleEndian.Uint16(argBytes))
				fmt.Fprintf(&out, "    %s %s\n", info.Mnemonic, readPoolString(off))
			case ArgLocal8:
				slot := int(argBytes[0])
				if slot < fn.argCount {
					fmt.Fprintf(&out, "    %s p%d\n", info.Mnemonic, slot)
				} else {
					fmt.Fprintf(&out, "    %s l%d\n", info.Mnemonic, slot)
				}
			case ArgGlobal16:
				slot := int(binary.LittleEndian.Uint16(argBytes))
				fmt.Fprintf(&out, "    %s g%d\n", info.Mnemonic, slot)
			case ArgFunc16:
				fIdx := int(binary.LittleEndian.Uint16(argBytes))
				if fIdx >= 0 && fIdx < len(funcs) {
					fmt.Fprintf(&out, "    %s %s\n", info.Mnemonic, funcs[fIdx].name)
				} else {
					fmt.Fprintf(&out, "    %s %d\n", info.Mnemonic, fIdx)
				}
			case ArgRel16:
				rel := int(int16(binary.LittleEndian.Uint16(argBytes)))
				target := pc + 3 + rel
				if lbl, ok := branchTargets[target]; ok {
					fmt.Fprintf(&out, "    %s %s\n", info.Mnemonic, lbl)
				} else {
					fmt.Fprintf(&out, "    %s %d\n", info.Mnemonic, rel)
				}
			}
			pc += 1 + info.ArgBytes
		}
		if lbl, ok := branchTargets[len(fnCode)]; ok {
			fmt.Fprintf(&out, "%s:\n", lbl)
		}
		out.WriteString(".endfunction\n\n")
	}

	return out.String(), nil
}
