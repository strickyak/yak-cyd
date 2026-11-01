package lcode

import (
	"encoding/binary"
	"fmt"
	"strconv"
	"strings"
)

type globalVar struct {
	name string
	size int
}

type asmVar struct {
	name string
	size int
	slot int
}

type asmLine struct {
	lineNo  int
	raw     string
	label   string
	opcode  byte
	isOp    bool
	argStr  string
	pc      int
	codeLen int
}

type asmFunction struct {
	index      int
	name       string
	isEntry    bool
	params     []asmVar
	locals     []asmVar
	paramMap   map[string]int
	localMap   map[string]int
	labels     map[string]int
	lines      []asmLine
	codeSize   int
	frameSize  int
	codeOffset int
	metaOffset int
}

// Assembler compiles LCode assembly text (.lasm) into an LCOD binary module.
type Assembler struct {
	symbols        map[string]int
	funcIndices    map[string]int
	globals        []globalVar
	globalSlots    map[string]int
	functions      []*asmFunction
	currentFunc    *asmFunction
	entryFuncIdx   int
	stringPool     []byte
	stringToOffset map[string]int
}

func NewAssembler() *Assembler {
	a := &Assembler{
		symbols:        make(map[string]int),
		funcIndices:    make(map[string]int),
		globalSlots:    make(map[string]int),
		stringToOffset: make(map[string]int),
		entryFuncIdx:   0,
	}
	for name, id := range SyscallByName {
		a.symbols[name] = int(id)
	}
	return a
}

// Assemble translates LCode assembly (.lasm) into a raw LCOD payload.
func Assemble(source string) ([]byte, error) {
	a := NewAssembler()
	return a.Assemble(source)
}

// AssembleLapp translates LCode assembly (.lasm) and wraps it in a 128-byte .lapp container.
func AssembleLapp(source string, title string) ([]byte, error) {
	lcod, err := Assemble(source)
	if err != nil {
		return nil, err
	}
	return WrapLapp(lcod, title), nil
}

func (a *Assembler) Assemble(source string) ([]byte, error) {
	lines := strings.Split(source, "\n")
	if err := a.pass1(lines); err != nil {
		return nil, err
	}
	return a.pass2()
}

func (a *Assembler) addString(s string) int {
	if off, ok := a.stringToOffset[s]; ok {
		return off
	}
	off := len(a.stringPool)
	raw := []byte(s)
	entry := make([]byte, 2+len(raw)+1)
	binary.LittleEndian.PutUint16(entry[0:2], uint16(len(raw)))
	copy(entry[2:], raw)
	entry[len(entry)-1] = 0
	a.stringPool = append(a.stringPool, entry...)
	a.stringToOffset[s] = off
	return off
}

func stripComment(line string) string {
	inQuote := false
	escaped := false
	for i := 0; i < len(line); i++ {
		ch := line[i]
		if inQuote {
			if escaped {
				escaped = false
			} else if ch == '\\' {
				escaped = true
			} else if ch == '"' {
				inQuote = false
			}
		} else {
			if ch == '"' {
				inQuote = true
			} else if ch == ';' || ch == '#' {
				return line[:i]
			} else if ch == '/' && i+1 < len(line) && line[i+1] == '/' {
				return line[:i]
			}
		}
	}
	return line
}

func parseTypeSize(typeSpec string) (int, error) {
	typeSpec = strings.TrimSpace(strings.ToLower(typeSpec))
	switch typeSpec {
	case "word", "int", "int32", "uint32", "scalar", "bool", "byte", "uint16", "int16":
		return 4, nil
	case "slice", "string":
		return 12, nil
	default:
		n, err := strconv.Atoi(typeSpec)
		if err != nil || n <= 0 {
			return 0, fmt.Errorf("invalid variable size/type %q", typeSpec)
		}
		// Align to 4 bytes in frame/globals for clean 32-bit access
		return n, nil
	}
}

func (a *Assembler) pass1(lines []string) error {
	for idx, rawLine := range lines {
		lineNo := idx + 1
		line := strings.TrimSpace(stripComment(rawLine))
		if line == "" {
			continue
		}

		if strings.HasPrefix(line, ".") && !strings.HasSuffix(line, ":") {
			parts := strings.Fields(line)
			dir := strings.ToLower(parts[0])
			switch dir {
			case ".global":
				rest := strings.TrimSpace(line[len(parts[0]):])
				colonIdx := strings.IndexByte(rest, ':')
				var gName, gSizeStr string
				if colonIdx >= 0 {
					gName = strings.TrimSpace(rest[:colonIdx])
					gSizeStr = strings.TrimSpace(rest[colonIdx+1:])
				} else if len(parts) >= 2 {
					gName = parts[1]
					gSizeStr = "4"
				} else {
					return fmt.Errorf("line %d: malformed .global directive", lineNo)
				}
				sz, err := parseTypeSize(gSizeStr)
				if err != nil {
					return fmt.Errorf("line %d: %v", lineNo, err)
				}
				slot := len(a.globals)
				a.globals = append(a.globals, globalVar{name: gName, size: sz})
				a.globalSlots[gName] = slot

			case ".function":
				if len(parts) < 2 {
					return fmt.Errorf("line %d: .function missing name", lineNo)
				}
				fnName := parts[1]
				isEntry := false
				for _, p := range parts[2:] {
					if strings.ToLower(p) == "entry" {
						isEntry = true
					}
				}
				fnIdx := len(a.functions)
				fn := &asmFunction{
					index:    fnIdx,
					name:     fnName,
					isEntry:  isEntry,
					paramMap: make(map[string]int),
					localMap: make(map[string]int),
					labels:   make(map[string]int),
				}
				if isEntry || fnName == "main" {
					a.entryFuncIdx = fnIdx
				}
				a.funcIndices[fnName] = fnIdx
				a.functions = append(a.functions, fn)
				a.currentFunc = fn

			case ".endfunction":
				if a.currentFunc == nil {
					return fmt.Errorf("line %d: .endfunction outside .function", lineNo)
				}
				a.finalizeFuncLayout(a.currentFunc)
				a.currentFunc = nil

			case ".param", ".local":
				if a.currentFunc == nil {
					return fmt.Errorf("line %d: %s outside .function", lineNo, dir)
				}
				rest := strings.TrimSpace(line[len(parts[0]):])
				colonIdx := strings.IndexByte(rest, ':')
				var vName, vSizeStr string
				if colonIdx >= 0 {
					vName = strings.TrimSpace(rest[:colonIdx])
					vSizeStr = strings.TrimSpace(rest[colonIdx+1:])
				} else if len(parts) >= 2 {
					vName = parts[1]
					vSizeStr = "4"
				} else {
					return fmt.Errorf("line %d: malformed %s directive", lineNo, dir)
				}
				sz, err := parseTypeSize(vSizeStr)
				if err != nil {
					return fmt.Errorf("line %d: %v", lineNo, err)
				}
				if dir == ".param" {
					a.currentFunc.params = append(a.currentFunc.params, asmVar{name: vName, size: sz})
				} else {
					a.currentFunc.locals = append(a.currentFunc.locals, asmVar{name: vName, size: sz})
				}
			default:
				return fmt.Errorf("line %d: unknown directive %q", lineNo, dir)
			}
			continue
		}

		if a.currentFunc == nil {
			return fmt.Errorf("line %d: instruction or label outside .function: %q", lineNo, line)
		}

		// Handle labels (e.g. ".L_loop:" or "loop:")
		if strings.HasSuffix(line, ":") {
			lbl := strings.TrimSpace(line[:len(line)-1])
			a.currentFunc.labels[lbl] = a.currentFunc.codeSize
			continue
		}

		fields := strings.Fields(line)
		mnem := strings.ToUpper(fields[0])
		argStr := strings.TrimSpace(line[len(fields[0]):])

		op, ok := MnemonicToOpcode[mnem]
		if !ok {
			return fmt.Errorf("line %d: unknown opcode mnemonic %q", lineNo, mnem)
		}
		info := OpcodeTable[op]
		codeLen := 1 + info.ArgBytes
		a.currentFunc.lines = append(a.currentFunc.lines, asmLine{
			lineNo:  lineNo,
			raw:     line,
			opcode:  op,
			isOp:    true,
			argStr:  argStr,
			pc:      a.currentFunc.codeSize,
			codeLen: codeLen,
		})
		a.currentFunc.codeSize += codeLen
	}

	if a.currentFunc != nil {
		a.finalizeFuncLayout(a.currentFunc)
		a.currentFunc = nil
	}
	if len(a.functions) == 0 {
		return fmt.Errorf("no .function definitions found")
	}
	return nil
}

func (a *Assembler) finalizeFuncLayout(fn *asmFunction) {
	slot := 0
	frameSz := 0
	for i := range fn.params {
		fn.params[i].slot = slot
		fn.paramMap[fn.params[i].name] = slot
		fn.localMap[fn.params[i].name] = slot
		frameSz += fn.params[i].size
		slot++
	}
	for i := range fn.locals {
		fn.locals[i].slot = slot
		fn.localMap[fn.locals[i].name] = slot
		frameSz += fn.locals[i].size
		slot++
	}
	fn.frameSize = frameSz
}

func unquoteAsmString(s string) (string, error) {
	s = strings.TrimSpace(s)
	if len(s) < 2 || s[0] != '"' || s[len(s)-1] != '"' {
		return "", fmt.Errorf("expected quoted string literal, got %q", s)
	}
	var out strings.Builder
	inner := s[1 : len(s)-1]
	for i := 0; i < len(inner); i++ {
		ch := inner[i]
		if ch == '\\' && i+1 < len(inner) {
			i++
			switch inner[i] {
			case 'n':
				out.WriteByte('\n')
			case 'r':
				out.WriteByte('\r')
			case 't':
				out.WriteByte('\t')
			case '0':
				out.WriteByte(0)
			case '\\':
				out.WriteByte('\\')
			case '"':
				out.WriteByte('"')
			case '\'':
				out.WriteByte('\'')
			default:
				out.WriteByte(inner[i])
			}
		} else {
			out.WriteByte(ch)
		}
	}
	return out.String(), nil
}

func (a *Assembler) parseImmediate(s string) (int64, error) {
	s = strings.TrimSpace(s)
	if val, ok := a.symbols[s]; ok {
		return int64(val), nil
	}
	if strings.HasPrefix(s, "$") {
		v, err := strconv.ParseUint(s[1:], 16, 32)
		return int64(v), err
	}
	return strconv.ParseInt(s, 0, 64)
}

func (a *Assembler) pass2() ([]byte, error) {
	// Pre-populate string pool from PUSH_STR / PUSH_CSTR instructions
	for _, fn := range a.functions {
		for _, ln := range fn.lines {
			if ln.opcode == OpPUSH_STR || ln.opcode == OpPUSH_CSTR {
				strVal, err := unquoteAsmString(ln.argStr)
				if err != nil {
					return nil, fmt.Errorf("line %d: %v", ln.lineNo, err)
				}
				a.addString(strVal)
			}
		}
	}

	var codeBuf []byte
	for _, fn := range a.functions {
		fn.codeOffset = len(codeBuf)
		for _, ln := range fn.lines {
			info := OpcodeTable[ln.opcode]
			codeBuf = append(codeBuf, ln.opcode)

			switch info.ArgType {
			case ArgNone:
				// No argument bytes
			case ArgImm8S:
				v, err := a.parseImmediate(ln.argStr)
				if err != nil || v < -128 || v > 127 {
					return nil, fmt.Errorf("line %d: invalid imm8_s %q", ln.lineNo, ln.argStr)
				}
				codeBuf = append(codeBuf, byte(int8(v)))

			case ArgImm8U, ArgSys8:
				v, err := a.parseImmediate(ln.argStr)
				if err != nil || v < 0 || v > 255 {
					return nil, fmt.Errorf("line %d: invalid 8-bit argument %q", ln.lineNo, ln.argStr)
				}
				codeBuf = append(codeBuf, byte(v))

			case ArgImm16S:
				v, err := a.parseImmediate(ln.argStr)
				if err != nil || v < -32768 || v > 32767 {
					return nil, fmt.Errorf("line %d: invalid imm16_s %q", ln.lineNo, ln.argStr)
				}
				var b [2]byte
				binary.LittleEndian.PutUint16(b[:], uint16(int16(v)))
				codeBuf = append(codeBuf, b[:]...)

			case ArgImm16U:
				v, err := a.parseImmediate(ln.argStr)
				if err != nil || v < 0 || v > 65535 {
					return nil, fmt.Errorf("line %d: invalid imm16_u %q", ln.lineNo, ln.argStr)
				}
				var b [2]byte
				binary.LittleEndian.PutUint16(b[:], uint16(v))
				codeBuf = append(codeBuf, b[:]...)

			case ArgImm32:
				v, err := a.parseImmediate(ln.argStr)
				if err != nil {
					return nil, fmt.Errorf("line %d: invalid imm32 %q: %v", ln.lineNo, ln.argStr, err)
				}
				var b [4]byte
				binary.LittleEndian.PutUint32(b[:], uint32(int32(v)))
				codeBuf = append(codeBuf, b[:]...)

			case ArgStr16:
				strVal, err := unquoteAsmString(ln.argStr)
				if err != nil {
					return nil, fmt.Errorf("line %d: %v", ln.lineNo, err)
				}
				off := a.addString(strVal)
				if off > 65535 {
					return nil, fmt.Errorf("line %d: string pool overflow (%d)", ln.lineNo, off)
				}
				var b [2]byte
				binary.LittleEndian.PutUint16(b[:], uint16(off))
				codeBuf = append(codeBuf, b[:]...)

			case ArgLocal8:
				arg := strings.TrimSpace(ln.argStr)
				slot, ok := fn.localMap[arg]
				if !ok {
					v, err := strconv.Atoi(arg)
					if err != nil {
						return nil, fmt.Errorf("line %d: unknown local variable %q in function %s", ln.lineNo, arg, fn.name)
					}
					slot = v
				}
				if slot < 0 || slot > 255 {
					return nil, fmt.Errorf("line %d: local slot %d out of range", ln.lineNo, slot)
				}
				codeBuf = append(codeBuf, byte(slot))

			case ArgGlobal16:
				arg := strings.TrimSpace(ln.argStr)
				slot, ok := a.globalSlots[arg]
				if !ok {
					v, err := strconv.Atoi(arg)
					if err != nil {
						return nil, fmt.Errorf("line %d: unknown global variable %q", ln.lineNo, arg)
					}
					slot = v
				}
				var b [2]byte
				binary.LittleEndian.PutUint16(b[:], uint16(slot))
				codeBuf = append(codeBuf, b[:]...)

			case ArgFunc16:
				arg := strings.TrimSpace(ln.argStr)
				fIdx, ok := a.funcIndices[arg]
				if !ok {
					v, err := strconv.Atoi(arg)
					if err != nil {
						return nil, fmt.Errorf("line %d: unknown function %q", ln.lineNo, arg)
					}
					fIdx = v
				}
				var b [2]byte
				binary.LittleEndian.PutUint16(b[:], uint16(fIdx))
				codeBuf = append(codeBuf, b[:]...)

			case ArgRel16:
				arg := strings.TrimSpace(ln.argStr)
				targetPC, ok := fn.labels[arg]
				if !ok {
					v, err := strconv.Atoi(arg)
					if err != nil {
						return nil, fmt.Errorf("line %d: unknown branch label %q in function %s", ln.lineNo, arg, fn.name)
					}
					targetPC = (ln.pc + ln.codeLen) + v
				}
				rel := targetPC - (ln.pc + ln.codeLen)
				if rel < -32768 || rel > 32767 {
					return nil, fmt.Errorf("line %d: relative jump %d out of 16-bit range", ln.lineNo, rel)
				}
				var b [2]byte
				binary.LittleEndian.PutUint16(b[:], uint16(int16(rel)))
				codeBuf = append(codeBuf, b[:]...)
			}
		}
	}

	// Calculate section offsets
	headerAndGlobalsSz := LCodeHeaderSz + len(a.globals)*4
	funcTableOffset := headerAndGlobalsSz + len(a.stringPool)
	metaStartOffset := funcTableOffset + len(a.functions)*LCodeFuncEntSz

	var metaBuf []byte
	for _, fn := range a.functions {
		fn.metaOffset = metaStartOffset + len(metaBuf)
		totalVars := len(fn.params) + len(fn.locals)
		for _, p := range fn.params {
			var b [4]byte
			binary.LittleEndian.PutUint32(b[:], uint32(p.size))
			metaBuf = append(metaBuf, b[:]...)
		}
		for _, l := range fn.locals {
			var b [4]byte
			binary.LittleEndian.PutUint32(b[:], uint32(l.size))
			metaBuf = append(metaBuf, b[:]...)
		}
		_ = totalVars
		nameBytes := []byte(fn.name)
		if len(nameBytes) > 255 {
			nameBytes = nameBytes[:255]
		}
		metaBuf = append(metaBuf, byte(len(nameBytes)))
		metaBuf = append(metaBuf, nameBytes...)
	}

	out := make([]byte, 0, metaStartOffset+len(metaBuf)+len(codeBuf))

	// 1. 24-byte LCodeModuleHeader
	hdr := make([]byte, LCodeHeaderSz)
	copy(hdr[0:4], LCodeMagic)
	binary.LittleEndian.PutUint16(hdr[4:6], LCodeFormatVer)
	binary.LittleEndian.PutUint16(hdr[6:8], 0)
	binary.LittleEndian.PutUint32(hdr[8:12], uint32(len(a.stringPool)))
	binary.LittleEndian.PutUint16(hdr[12:14], uint16(len(a.globals)))
	binary.LittleEndian.PutUint16(hdr[14:16], uint16(len(a.functions)))
	binary.LittleEndian.PutUint16(hdr[16:18], uint16(a.entryFuncIdx))
	binary.LittleEndian.PutUint16(hdr[18:20], 0)
	binary.LittleEndian.PutUint32(hdr[20:24], uint32(len(codeBuf)))
	out = append(out, hdr...)

	// 2. Global Size Table
	for _, g := range a.globals {
		var b [4]byte
		binary.LittleEndian.PutUint32(b[:], uint32(g.size))
		out = append(out, b[:]...)
	}

	// 3. String Pool
	out = append(out, a.stringPool...)

	// 4. Function Table (20 bytes per function)
	for _, fn := range a.functions {
		ent := make([]byte, LCodeFuncEntSz)
		binary.LittleEndian.PutUint16(ent[0:2], uint16(len(fn.params)))
		binary.LittleEndian.PutUint16(ent[2:4], uint16(len(fn.params)+len(fn.locals)))
		binary.LittleEndian.PutUint32(ent[4:8], uint32(fn.frameSize))
		binary.LittleEndian.PutUint32(ent[8:12], uint32(fn.codeOffset))
		binary.LittleEndian.PutUint32(ent[12:16], uint32(fn.codeSize))
		binary.LittleEndian.PutUint32(ent[16:20], uint32(fn.metaOffset))
		out = append(out, ent...)
	}

	// 5. Function Metadata
	out = append(out, metaBuf...)

	// 6. Bytecode Stream
	out = append(out, codeBuf...)

	return out, nil
}
