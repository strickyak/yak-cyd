package lcode

import (
	"fmt"
	"math"
	"strconv"
	"strings"
	"unicode"
)

// ---------------------------------------------------------------------------
// Lexer for LGolf (Go-subset source language for LCode)
// ---------------------------------------------------------------------------
type TokKind int

const (
	TokEOF TokKind = iota
	TokIdent
	TokInt
	TokFloat
	TokString
	TokPunct
)

type Token struct {
	Kind  TokKind
	Val   string
	Int   int64
	Float float64
	Line  int
}

func LexLGolf(src string) ([]Token, error) {
	var toks []Token
	i := 0
	line := 1
	n := len(src)

	for i < n {
		ch := src[i]
		if ch == '\n' {
			// Insert virtual semicolon if previous token can end a statement
			if len(toks) > 0 {
				prev := toks[len(toks)-1]
				if prev.Kind == TokIdent || prev.Kind == TokInt || prev.Kind == TokFloat || prev.Kind == TokString ||
					(prev.Kind == TokPunct && (prev.Val == ")" || prev.Val == "]" || prev.Val == "}" || prev.Val == "++" || prev.Val == "--")) {
					if prev.Val != "package" && prev.Val != "import" && prev.Val != "var" &&
						prev.Val != "const" && prev.Val != "type" && prev.Val != "func" &&
						prev.Val != "else" {
						toks = append(toks, Token{Kind: TokPunct, Val: ";", Line: line})
					}
				}
			}
			line++
			i++
			continue
		}
		if unicode.IsSpace(rune(ch)) {
			i++
			continue
		}
		// Line comment
		if ch == '/' && i+1 < n && src[i+1] == '/' {
			for i < n && src[i] != '\n' {
				i++
			}
			continue
		}
		// Block comment
		if ch == '/' && i+1 < n && src[i+1] == '*' {
			i += 2
			for i+1 < n && !(src[i] == '*' && src[i+1] == '/') {
				if src[i] == '\n' {
					line++
				}
				i++
			}
			i += 2
			continue
		}
		// Character literal
		if ch == '\'' {
			i++
			if i >= n {
				return nil, fmt.Errorf("line %d: unterminated char literal", line)
			}
			var val byte
			if src[i] == '\\' {
				i++
				if i >= n {
					return nil, fmt.Errorf("line %d: unterminated escape in char literal", line)
				}
				switch src[i] {
				case 'n':
					val = '\n'
				case 'r':
					val = '\r'
				case 't':
					val = '\t'
				case '0':
					val = 0
				case '\'':
					val = '\''
				case '\\':
					val = '\\'
				default:
					val = src[i]
				}
				i++
			} else {
				val = src[i]
				i++
			}
			if i >= n || src[i] != '\'' {
				return nil, fmt.Errorf("line %d: expected closing quote in char literal", line)
			}
			i++
			toks = append(toks, Token{Kind: TokInt, Val: strconv.Itoa(int(val)), Int: int64(val), Line: line})
			continue
		}
		// String literal
		if ch == '"' {
			i++
			var sb strings.Builder
			for i < n && src[i] != '"' {
				if src[i] == '\\' && i+1 < n {
					i++
					switch src[i] {
					case 'n':
						sb.WriteByte('\n')
					case 'r':
						sb.WriteByte('\r')
					case 't':
						sb.WriteByte('\t')
					case '0':
						sb.WriteByte(0)
					case '"':
						sb.WriteByte('"')
					case '\\':
						sb.WriteByte('\\')
					default:
						sb.WriteByte(src[i])
					}
					i++
				} else {
					if src[i] == '\n' {
						line++
					}
					sb.WriteByte(src[i])
					i++
				}
			}
			if i >= n {
				return nil, fmt.Errorf("line %d: unterminated string literal", line)
			}
			i++ // skip closing '"'
			toks = append(toks, Token{Kind: TokString, Val: sb.String(), Line: line})
			continue
		}
		// Number literal (integer or IEEE-754 float32)
		if ch >= '0' && ch <= '9' {
			start := i
			isHexOrBin := ch == '0' && i+1 < n && (src[i+1] == 'x' || src[i+1] == 'X' || src[i+1] == 'b' || src[i+1] == 'B' || src[i+1] == 'o' || src[i+1] == 'O')
			if isHexOrBin {
				for i < n && (unicode.IsLetter(rune(src[i])) || unicode.IsDigit(rune(src[i])) || src[i] == '_') {
					i++
				}
				numStr := strings.ReplaceAll(src[start:i], "_", "")
				v, err := strconv.ParseInt(numStr, 0, 64)
				if err != nil {
					u, err2 := strconv.ParseUint(numStr, 0, 64)
					if err2 != nil {
						return nil, fmt.Errorf("line %d: invalid number %q", line, src[start:i])
					}
					v = int64(u)
				}
				toks = append(toks, Token{Kind: TokInt, Val: numStr, Int: v, Line: line})
				continue
			}

			isFloat := false
			for i < n && (unicode.IsDigit(rune(src[i])) || src[i] == '_') {
				i++
			}
			if i < n && src[i] == '.' && (i+1 >= n || src[i+1] != '.') {
				isFloat = true
				i++
				for i < n && (unicode.IsDigit(rune(src[i])) || src[i] == '_') {
					i++
				}
			}
			if i < n && (src[i] == 'e' || src[i] == 'E') {
				isFloat = true
				i++
				if i < n && (src[i] == '+' || src[i] == '-') {
					i++
				}
				for i < n && (unicode.IsDigit(rune(src[i])) || src[i] == '_') {
					i++
				}
			}
			numStr := strings.ReplaceAll(src[start:i], "_", "")
			if isFloat {
				f, err := strconv.ParseFloat(numStr, 64)
				if err != nil {
					return nil, fmt.Errorf("line %d: invalid float %q", line, src[start:i])
				}
				toks = append(toks, Token{Kind: TokFloat, Val: numStr, Float: f, Line: line})
			} else {
				v, err := strconv.ParseInt(numStr, 0, 64)
				if err != nil {
					u, err2 := strconv.ParseUint(numStr, 0, 64)
					if err2 != nil {
						return nil, fmt.Errorf("line %d: invalid number %q", line, src[start:i])
					}
					v = int64(u)
				}
				toks = append(toks, Token{Kind: TokInt, Val: numStr, Int: v, Line: line})
			}
			continue
		}
		// Identifier
		if unicode.IsLetter(rune(ch)) || ch == '_' {
			start := i
			for i < n && (unicode.IsLetter(rune(src[i])) || unicode.IsDigit(rune(src[i])) || src[i] == '_') {
				i++
			}
			toks = append(toks, Token{Kind: TokIdent, Val: src[start:i], Line: line})
			continue
		}
		// Multi-char operators
		if i+2 < n {
			three := src[i : i+3]
			if three == "<<=" || three == ">>=" || three == ">>>" {
				toks = append(toks, Token{Kind: TokPunct, Val: three, Line: line})
				i += 3
				continue
			}
		}
		if i+1 < n {
			two := src[i : i+2]
			switch two {
			case ":=", "==", "!=", "<=", ">=", "&&", "||", "<<", ">>", "++", "--",
				"+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=":
				toks = append(toks, Token{Kind: TokPunct, Val: two, Line: line})
				i += 2
				continue
			}
		}
		toks = append(toks, Token{Kind: TokPunct, Val: string(ch), Line: line})
		i++
	}
	toks = append(toks, Token{Kind: TokEOF, Val: "", Line: line})
	return toks, nil
}

// ---------------------------------------------------------------------------
// Type & Symbol Definitions for LGolf Compiler
// ---------------------------------------------------------------------------
type TypeDesc struct {
	Kind       string // "scalar", "slice", "array", "struct", "void"
	ScalarType string // "int", "uint", "byte", "int8", "uint16", "int16", "bool", "word"
	ByteSize   int
	ElemSize   int
	Dim1       int      // for 1D/2D arrays
	Dim2       int      // for 2D arrays (>0 if 2D)
	ElemScalar string   // "byte", "uint16", "int", etc.
	StructName string
}

type StructField struct {
	Name   string
	Offset int
	Type   TypeDesc
}

type StructDef struct {
	Name   string
	Size   int
	Fields []StructField
}

type VarSym struct {
	Name     string
	IsGlobal bool
	Type     TypeDesc
}

type FuncDecl struct {
	Name       string
	Params     []VarSym
	ReturnType []TypeDesc
	BodyTokens []Token
	Used       bool
}

// Builtin constants preloaded into every LGolf compilation
var BuiltinConsts = map[string]int64{
	"true":              1,
	"false":             0,
	"nil":               0,
	"SCREEN_W":          320,
	"SCREEN_H":          240,
	"BLACK":             0x0000,
	"NAVY":              0x000F,
	"DARKGREEN":         0x03E0,
	"DARKCYAN":          0x03EF,
	"MAROON":            0x7800,
	"PURPLE":            0x780F,
	"OLIVE":             0x7BE0,
	"LIGHTGREY":         0xD69A,
	"DARKGREY":          0x7BEF,
	"BLUE":              0x001F,
	"GREEN":             0x07E0,
	"CYAN":              0x07FF,
	"RED":               0xF800,
	"MAGENTA":           0xF81F,
	"YELLOW":            0xFFE0,
	"WHITE":             0xFFFF,
	"ORANGE":            0xFDA0,
	"GREENYELLOW":       0xB7E0,
	"PINK":              0xFE19,
	"COLOR_BLACK":       0x0000,
	"COLOR_NAVY":        0x000F,
	"COLOR_DARKGREEN":   0x03E0,
	"COLOR_DARKCYAN":    0x03EF,
	"COLOR_MAROON":      0x7800,
	"COLOR_PURPLE":      0x780F,
	"COLOR_OLIVE":       0x7BE0,
	"COLOR_LIGHTGREY":   0xD69A,
	"COLOR_DARKGREY":    0x7BEF,
	"COLOR_BLUE":        0x001F,
	"COLOR_GREEN":       0x07E0,
	"COLOR_CYAN":        0x07FF,
	"COLOR_RED":         0xF800,
	"COLOR_MAGENTA":     0xF81F,
	"COLOR_YELLOW":      0xFFE0,
	"COLOR_WHITE":       0xFFFF,
	"COLOR_ORANGE":      0xFDA0,
	"COLOR_GREENYELLOW": 0xB7E0,
	"COLOR_PINK":        0xFE19,
	"TL_DATUM":          0,
	"TC_DATUM":          1,
	"TR_DATUM":          2,
	"ML_DATUM":          3,
	"MC_DATUM":          4,
	"MR_DATUM":          5,
	"BL_DATUM":          6,
	"BC_DATUM":          7,
	"BR_DATUM":          8,
}

var BuiltinFloatConsts = map[string]float64{
	"PI":         3.141592653589793,
	"Pi":         3.141592653589793,
	"TWO_PI":     6.283185307179586,
	"HALF_PI":    1.5707963267948966,
	"DEG_TO_RAD": 0.017453292519943295,
	"RAD_TO_DEG": 57.29577951308232,
	"E":          2.718281828459045,
}

func isFloatType(t TypeDesc) bool {
	return t.Kind == "scalar" && (t.ScalarType == "float32" || t.ScalarType == "float")
}

type Compiler struct {
	toks        []Token
	pos         int
	consts      map[string]int64
	floatConsts map[string]float64
	strConsts   map[string]string
	structs     map[string]*StructDef
	globals     []VarSym
	globalMap   map[string]VarSym
	funcs       map[string]*FuncDecl
	funcOrder   []string
	curFunc     *FuncDecl
	locals      []VarSym
	localMap    map[string]VarSym
	tempCount   int
	labelCount  int
	loopStarts  []string
	loopEnds    []string
	asmOut      strings.Builder
}

func NewCompiler() *Compiler {
	c := &Compiler{
		consts:      make(map[string]int64),
		floatConsts: make(map[string]float64),
		strConsts:   make(map[string]string),
		structs:     make(map[string]*StructDef),
		globalMap:   make(map[string]VarSym),
		funcs:       make(map[string]*FuncDecl),
		localMap:    make(map[string]VarSym),
	}
	for k, v := range BuiltinConsts {
		c.consts[k] = v
	}
	for k, v := range BuiltinFloatConsts {
		c.floatConsts[k] = v
	}
	return c
}

// CompileToAsm compiles one or more LGolf source strings into LCode assembly (.lasm).
func CompileToAsm(sources ...string) (string, error) {
	combined := strings.Join(sources, "\n")
	toks, err := LexLGolf(combined)
	if err != nil {
		return "", err
	}
	c := NewCompiler()
	return c.compileTokens(toks)
}

// CompileToLapp compiles LGolf source code directly into a signed 128-byte .lapp binary.
func CompileToLapp(title string, sources ...string) ([]byte, string, error) {
	asm, err := CompileToAsm(sources...)
	if err != nil {
		return nil, "", err
	}
	lapp, err := AssembleLapp(asm, title)
	if err != nil {
		return nil, asm, err
	}
	return lapp, asm, nil
}

func (c *Compiler) peek() Token {
	if c.pos >= len(c.toks) {
		return Token{Kind: TokEOF}
	}
	return c.toks[c.pos]
}

func (c *Compiler) next() Token {
	t := c.peek()
	if c.pos < len(c.toks) {
		c.pos++
	}
	return t
}

func (c *Compiler) matchPunct(val string) bool {
	if c.peek().Kind == TokPunct && c.peek().Val == val {
		c.pos++
		return true
	}
	return false
}

func (c *Compiler) expectPunct(val string) error {
	t := c.next()
	if t.Kind != TokPunct || t.Val != val {
		return fmt.Errorf("line %d: expected %q, got %q", t.Line, val, t.Val)
	}
	return nil
}

func (c *Compiler) expectIdent() (string, error) {
	t := c.next()
	if t.Kind != TokIdent {
		return "", fmt.Errorf("line %d: expected identifier, got %q", t.Line, t.Val)
	}
	return t.Val, nil
}

func (c *Compiler) skipSemicolons() {
	for c.matchPunct(";") {
	}
}

func (c *Compiler) newLabel(prefix string) string {
	c.labelCount++
	return fmt.Sprintf(".L_%s_%d", prefix, c.labelCount)
}

func (c *Compiler) evalConstExpr() (int64, error) {
	return c.evalConstOr()
}

func (c *Compiler) evalConstOr() (int64, error) {
	left, err := c.evalConstXor()
	if err != nil {
		return 0, err
	}
	for c.matchPunct("|") {
		right, err := c.evalConstXor()
		if err != nil {
			return 0, err
		}
		left |= right
	}
	return left, nil
}

func (c *Compiler) evalConstXor() (int64, error) {
	left, err := c.evalConstAnd()
	if err != nil {
		return 0, err
	}
	for c.matchPunct("^") {
		right, err := c.evalConstAnd()
		if err != nil {
			return 0, err
		}
		left ^= right
	}
	return left, nil
}

func (c *Compiler) evalConstAnd() (int64, error) {
	left, err := c.evalConstShift()
	if err != nil {
		return 0, err
	}
	for c.matchPunct("&") {
		right, err := c.evalConstShift()
		if err != nil {
			return 0, err
		}
		left &= right
	}
	return left, nil
}

func (c *Compiler) evalConstShift() (int64, error) {
	left, err := c.evalConstAdd()
	if err != nil {
		return 0, err
	}
	for {
		if c.matchPunct("<<") {
			r, err := c.evalConstAdd()
			if err != nil {
				return 0, err
			}
			left <<= uint(r)
		} else if c.matchPunct(">>") {
			r, err := c.evalConstAdd()
			if err != nil {
				return 0, err
			}
			left >>= uint(r)
		} else {
			break
		}
	}
	return left, nil
}

func (c *Compiler) evalConstAdd() (int64, error) {
	left, err := c.evalConstMul()
	if err != nil {
		return 0, err
	}
	for {
		if c.matchPunct("+") {
			r, err := c.evalConstMul()
			if err != nil {
				return 0, err
			}
			left += r
		} else if c.matchPunct("-") {
			r, err := c.evalConstMul()
			if err != nil {
				return 0, err
			}
			left -= r
		} else {
			break
		}
	}
	return left, nil
}

func (c *Compiler) evalConstMul() (int64, error) {
	left, err := c.evalConstUnary()
	if err != nil {
		return 0, err
	}
	for {
		if c.matchPunct("*") {
			r, err := c.evalConstUnary()
			if err != nil {
				return 0, err
			}
			left *= r
		} else if c.matchPunct("/") {
			r, err := c.evalConstUnary()
			if err != nil || r == 0 {
				return 0, fmt.Errorf("constant division error")
			}
			left /= r
		} else if c.matchPunct("%") {
			r, err := c.evalConstUnary()
			if err != nil || r == 0 {
				return 0, fmt.Errorf("constant modulo error")
			}
			left %= r
		} else {
			break
		}
	}
	return left, nil
}

func (c *Compiler) evalConstUnary() (int64, error) {
	if c.matchPunct("-") {
		v, err := c.evalConstUnary()
		return -v, err
	}
	if c.matchPunct("+") {
		return c.evalConstUnary()
	}
	if c.matchPunct("^") || c.matchPunct("~") {
		v, err := c.evalConstUnary()
		return ^v, err
	}
	if c.matchPunct("(") {
		v, err := c.evalConstExpr()
		if err != nil {
			return 0, err
		}
		if err := c.expectPunct(")"); err != nil {
			return 0, err
		}
		return v, nil
	}
	t := c.next()
	if t.Kind == TokInt {
		return t.Int, nil
	}
	if t.Kind == TokIdent {
		if v, ok := c.consts[t.Val]; ok {
			return v, nil
		}
		return 0, fmt.Errorf("line %d: unknown constant %q", t.Line, t.Val)
	}
	return 0, fmt.Errorf("line %d: unexpected token %q in constant expression", t.Line, t.Val)
}

func scalarByteSizeInAggregate(name string) int {
	switch name {
	case "byte", "uint8", "int8", "bool":
		return 1
	case "uint16", "int16", "half":
		return 2
	default:
		return 4
	}
}

func (c *Compiler) parseType() (TypeDesc, error) {
	// Pointer type: *T
	if c.matchPunct("*") {
		target, err := c.parseType()
		if err != nil {
			return TypeDesc{}, err
		}
		return TypeDesc{Kind: "scalar", ScalarType: "word", ByteSize: 4, ElemSize: 4, StructName: target.StructName}, nil
	}
	// Array or Slice type: [N]T, [N][M]T, or []T
	if c.matchPunct("[") {
		if c.matchPunct("]") {
			elem, err := c.parseType()
			if err != nil {
				return TypeDesc{}, err
			}
			eSz := scalarByteSizeInAggregate(elem.ScalarType)
			if elem.Kind == "struct" {
				eSz = elem.ByteSize
			}
			return TypeDesc{
				Kind:       "slice",
				ByteSize:   12,
				ElemSize:   eSz,
				ElemScalar: elem.ScalarType,
			}, nil
		}
		dim1, err := c.evalConstExpr()
		if err != nil {
			return TypeDesc{}, err
		}
		if err := c.expectPunct("]"); err != nil {
			return TypeDesc{}, err
		}
		if c.matchPunct("[") {
			dim2, err := c.evalConstExpr()
			if err != nil {
				return TypeDesc{}, err
			}
			if err := c.expectPunct("]"); err != nil {
				return TypeDesc{}, err
			}
			elem, err := c.parseType()
			if err != nil {
				return TypeDesc{}, err
			}
			eSz := scalarByteSizeInAggregate(elem.ScalarType)
			return TypeDesc{
				Kind:       "array",
				ByteSize:   int(dim1) * int(dim2) * eSz,
				ElemSize:   eSz,
				Dim1:       int(dim1),
				Dim2:       int(dim2),
				ElemScalar: elem.ScalarType,
			}, nil
		}
		elem, err := c.parseType()
		if err != nil {
			return TypeDesc{}, err
		}
		eSz := scalarByteSizeInAggregate(elem.ScalarType)
		if elem.Kind == "struct" {
			eSz = elem.ByteSize
		}
		return TypeDesc{
			Kind:       "array",
			ByteSize:   int(dim1) * eSz,
			ElemSize:   eSz,
			Dim1:       int(dim1),
			Dim2:       0,
			ElemScalar: elem.ScalarType,
			StructName: elem.StructName,
		}, nil
	}

	name, err := c.expectIdent()
	if err != nil {
		return TypeDesc{}, err
	}
	switch name {
	case "int", "int32", "uint", "uint32", "word", "byte", "uint8", "int8", "uint16", "int16", "half", "bool":
		return TypeDesc{Kind: "scalar", ScalarType: name, ByteSize: 4, ElemSize: scalarByteSizeInAggregate(name)}, nil
	case "float32", "float":
		return TypeDesc{Kind: "scalar", ScalarType: "float32", ByteSize: 4, ElemSize: 4}, nil
	case "string":
		return TypeDesc{Kind: "slice", ScalarType: "string", ByteSize: 12, ElemSize: 1, ElemScalar: "byte"}, nil
	default:
		if sd, ok := c.structs[name]; ok {
			return TypeDesc{Kind: "struct", StructName: name, ByteSize: sd.Size, ElemSize: sd.Size}, nil
		}
		return TypeDesc{}, fmt.Errorf("unknown type %q", name)
	}
}

func (c *Compiler) evalFloatConstExpr() (float64, error) {
	left, err := c.evalFloatConstMul()
	if err != nil {
		return 0, err
	}
	for {
		if c.matchPunct("+") {
			r, err := c.evalFloatConstMul()
			if err != nil {
				return 0, err
			}
			left += r
		} else if c.matchPunct("-") {
			r, err := c.evalFloatConstMul()
			if err != nil {
				return 0, err
			}
			left -= r
		} else {
			break
		}
	}
	return left, nil
}

func (c *Compiler) evalFloatConstMul() (float64, error) {
	left, err := c.evalFloatConstUnary()
	if err != nil {
		return 0, err
	}
	for {
		if c.matchPunct("*") {
			r, err := c.evalFloatConstUnary()
			if err != nil {
				return 0, err
			}
			left *= r
		} else if c.matchPunct("/") {
			r, err := c.evalFloatConstUnary()
			if err != nil || r == 0 {
				return 0, fmt.Errorf("constant float division error")
			}
			left /= r
		} else {
			break
		}
	}
	return left, nil
}

func (c *Compiler) evalFloatConstUnary() (float64, error) {
	if c.matchPunct("-") {
		v, err := c.evalFloatConstUnary()
		return -v, err
	}
	if c.matchPunct("+") {
		return c.evalFloatConstUnary()
	}
	if c.matchPunct("(") {
		v, err := c.evalFloatConstExpr()
		if err != nil {
			return 0, err
		}
		if err := c.expectPunct(")"); err != nil {
			return 0, err
		}
		return v, nil
	}
	t := c.next()
	if t.Kind == TokFloat {
		return t.Float, nil
	}
	if t.Kind == TokInt {
		return float64(t.Int), nil
	}
	if t.Kind == TokIdent {
		if fv, ok := c.floatConsts[t.Val]; ok {
			return fv, nil
		}
		if iv, ok := c.consts[t.Val]; ok {
			return float64(iv), nil
		}
		return 0, fmt.Errorf("line %d: unknown constant %q", t.Line, t.Val)
	}
	return 0, fmt.Errorf("line %d: unexpected token %q in float constant expression", t.Line, t.Val)
}

func (c *Compiler) parseSingleConstDecl() error {
	name, err := c.expectIdent()
	if err != nil {
		return err
	}
	explicitFloat := false
	// Optional type annotation
	if c.peek().Kind == TokIdent {
		typeTok := c.next()
		if typeTok.Val == "float32" || typeTok.Val == "float" {
			explicitFloat = true
		}
	}
	if err := c.expectPunct("="); err != nil {
		return err
	}
	if c.peek().Kind == TokString {
		st := c.next()
		c.strConsts[name] = st.Val
		return nil
	}
	savePos := c.pos
	if !explicitFloat {
		val, err := c.evalConstExpr()
		if err == nil && (c.peek().Kind == TokEOF || (c.peek().Kind == TokPunct && (c.peek().Val == ";" || c.peek().Val == ")"))) {
			c.consts[name] = val
			return nil
		}
	}
	c.pos = savePos
	fval, ferr := c.evalFloatConstExpr()
	if ferr != nil {
		return ferr
	}
	c.floatConsts[name] = fval
	return nil
}

func (c *Compiler) parseSingleGlobalVarDecl() error {
	name, err := c.expectIdent()
	if err != nil {
		return err
	}
	var td TypeDesc
	if !c.matchPunct("=") {
		td, err = c.parseType()
		if err != nil {
			return err
		}
		if c.matchPunct("=") {
			savePos := c.pos
			if _, err := c.evalConstExpr(); err != nil {
				c.pos = savePos
				_, _ = c.evalFloatConstExpr()
			}
		}
	} else {
		savePos := c.pos
		if _, err := c.evalConstExpr(); err == nil {
			td = TypeDesc{Kind: "scalar", ScalarType: "int", ByteSize: 4, ElemSize: 4}
		} else {
			c.pos = savePos
			if _, err := c.evalFloatConstExpr(); err != nil {
				return err
			}
			td = TypeDesc{Kind: "scalar", ScalarType: "float32", ByteSize: 4, ElemSize: 4}
		}
	}
	vs := VarSym{Name: name, IsGlobal: true, Type: td}
	c.globals = append(c.globals, vs)
	c.globalMap[name] = vs
	return nil
}

func (c *Compiler) compileTokens(toks []Token) (string, error) {
	c.toks = toks
	c.pos = 0

	// Top-level pass: collect consts, structs, globals, and function declarations
	for c.peek().Kind != TokEOF {
		c.skipSemicolons()
		t := c.peek()
		if t.Kind == TokEOF {
			break
		}
		if t.Kind != TokIdent {
			return "", fmt.Errorf("line %d: unexpected top-level token %q", t.Line, t.Val)
		}
		switch t.Val {
		case "package":
			c.next()
			_, err := c.expectIdent()
			if err != nil {
				return "", err
			}
		case "import":
			c.next()
			if c.matchPunct("(") {
				for !c.matchPunct(")") && c.peek().Kind != TokEOF {
					c.next()
				}
			} else if c.peek().Kind == TokString {
				c.next()
			}
		case "const":
			c.next()
			if c.matchPunct("(") {
				for !c.matchPunct(")") && c.peek().Kind != TokEOF {
					c.skipSemicolons()
					if c.matchPunct(")") {
						break
					}
					if err := c.parseSingleConstDecl(); err != nil {
						return "", err
					}
					c.skipSemicolons()
				}
			} else {
				if err := c.parseSingleConstDecl(); err != nil {
					return "", err
				}
			}
		case "type":
			c.next()
			typeName, err := c.expectIdent()
			if err != nil {
				return "", err
			}
			kw, err := c.expectIdent()
			if err != nil || kw != "struct" {
				return "", fmt.Errorf("line %d: expected 'struct' after type %s", t.Line, typeName)
			}
			if err := c.expectPunct("{"); err != nil {
				return "", err
			}
			sd := &StructDef{Name: typeName}
			c.structs[typeName] = sd
			off := 0
			for !c.matchPunct("}") && c.peek().Kind != TokEOF {
				c.skipSemicolons()
				if c.matchPunct("}") {
					break
				}
				fName, err := c.expectIdent()
				if err != nil {
					return "", err
				}
				fType, err := c.parseType()
				if err != nil {
					return "", err
				}
				fSz := fType.ByteSize
				if fType.Kind == "scalar" {
					fSz = 4
				}
				sd.Fields = append(sd.Fields, StructField{Name: fName, Offset: off, Type: fType})
				off += align4(fSz)
				c.skipSemicolons()
			}
			sd.Size = align4(off)
			c.structs[typeName] = sd

		case "var":
			c.next()
			if c.matchPunct("(") {
				for !c.matchPunct(")") && c.peek().Kind != TokEOF {
					c.skipSemicolons()
					if c.matchPunct(")") {
						break
					}
					if err := c.parseSingleGlobalVarDecl(); err != nil {
						return "", err
					}
					c.skipSemicolons()
				}
			} else {
				if err := c.parseSingleGlobalVarDecl(); err != nil {
					return "", err
				}
			}

		case "func":
			c.next()
			fnName, err := c.expectIdent()
			if err != nil {
				return "", err
			}
			if err := c.expectPunct("("); err != nil {
				return "", err
			}
			var params []VarSym
			for !c.matchPunct(")") && c.peek().Kind != TokEOF {
				pName, err := c.expectIdent()
				if err != nil {
					return "", err
				}
				pType, err := c.parseType()
				if err != nil {
					return "", err
				}
				params = append(params, VarSym{Name: pName, IsGlobal: false, Type: pType})
				c.matchPunct(",")
			}
			var retTypes []TypeDesc
			if c.matchPunct("(") {
				for !c.matchPunct(")") && c.peek().Kind != TokEOF {
					rt, err := c.parseType()
					if err != nil {
						return "", err
					}
					retTypes = append(retTypes, rt)
					c.matchPunct(",")
				}
			} else if !(c.peek().Kind == TokPunct && c.peek().Val == "{") {
				rt, err := c.parseType()
				if err != nil {
					return "", err
				}
				retTypes = append(retTypes, rt)
			}
			if err := c.expectPunct("{"); err != nil {
				return "", err
			}
			depth := 1
			bodyStart := c.pos
			for depth > 0 && c.peek().Kind != TokEOF {
				tk := c.next()
				if tk.Kind == TokPunct {
					if tk.Val == "{" {
						depth++
					} else if tk.Val == "}" {
						depth--
					}
				}
			}
			bodyToks := append([]Token{}, c.toks[bodyStart:c.pos-1]...)
			bodyToks = append(bodyToks, Token{Kind: TokEOF, Line: t.Line})
			fd := &FuncDecl{
				Name:       fnName,
				Params:     params,
				ReturnType: retTypes,
				BodyTokens: bodyToks,
			}
			c.funcs[fnName] = fd
			c.funcOrder = append(c.funcOrder, fnName)

		default:
			return "", fmt.Errorf("line %d: unexpected top-level identifier %q", t.Line, t.Val)
		}
	}

	// Dead-Function Elimination starting from main
	mainFn, ok := c.funcs["main"]
	if !ok {
		return "", fmt.Errorf("missing 'func main()'")
	}
	c.markUsedFunctions(mainFn)

	// Emit globals
	for _, g := range c.globals {
		fmt.Fprintf(&c.asmOut, ".global %s: %d\n", g.Name, align4(g.Type.ByteSize))
	}
	if len(c.globals) > 0 {
		c.asmOut.WriteString("\n")
	}

	// Emit each live function
	for _, fnName := range c.funcOrder {
		fn := c.funcs[fnName]
		if !fn.Used {
			continue
		}
		if err := c.emitFunction(fn); err != nil {
			return "", err
		}
	}

	return c.asmOut.String(), nil
}

func (c *Compiler) markUsedFunctions(fn *FuncDecl) {
	if fn == nil || fn.Used {
		return
	}
	fn.Used = true
	for _, tk := range fn.BodyTokens {
		if tk.Kind == TokIdent {
			if callee, ok := c.funcs[tk.Val]; ok && !callee.Used {
				c.markUsedFunctions(callee)
			}
		}
	}
}

func (c *Compiler) emitOp(op string, args ...any) {
	if len(args) == 0 {
		fmt.Fprintf(&c.asmOut, "    %s\n", op)
	} else {
		fmt.Fprintf(&c.asmOut, "    %s %s\n", op, fmt.Sprint(args...))
	}
}

func (c *Compiler) emitPushInt(v int64) {
	switch v {
	case 0:
		c.emitOp("PUSH_0")
	case 1:
		c.emitOp("PUSH_1")
	case -1:
		c.emitOp("PUSH_NEG1")
	default:
		if v >= -128 && v <= 127 {
			c.emitOp("PUSH_I8", v)
		} else if v >= 0 && v <= 255 {
			c.emitOp("PUSH_U8", v)
		} else if v >= -32768 && v <= 32767 {
			c.emitOp("PUSH_I16", v)
		} else if v >= 0 && v <= 65535 {
			c.emitOp("PUSH_U16", v)
		} else {
			c.emitOp("PUSH_I32", int32(v))
		}
	}
}

func (c *Compiler) emitPushFloat32(f float32) {
	bits := math.Float32bits(f)
	if bits == 0 {
		c.emitOp("PUSH_0")
	} else {
		c.emitOp("PUSH_I32", int32(bits))
	}
}

func (c *Compiler) emitBinaryFloatArgs(lt, rt TypeDesc) bool {
	if !isFloatType(lt) && !isFloatType(rt) {
		return false
	}
	if isFloatType(lt) && !isFloatType(rt) {
		c.emitOp("I32_TO_F32")
	} else if !isFloatType(lt) && isFloatType(rt) {
		c.emitOp("SWAP")
		c.emitOp("I32_TO_F32")
		c.emitOp("SWAP")
	}
	return true
}

func (c *Compiler) emitCoerceScalar(dstType, srcType TypeDesc) {
	if isFloatType(dstType) && !isFloatType(srcType) {
		c.emitOp("I32_TO_F32")
	} else if !isFloatType(dstType) && isFloatType(srcType) && dstType.Kind == "scalar" {
		c.emitOp("F32_TO_I32")
	}
}

func (c *Compiler) lookupVar(name string) (VarSym, bool) {
	if v, ok := c.localMap[name]; ok {
		return v, true
	}
	if v, ok := c.globalMap[name]; ok {
		return v, true
	}
	return VarSym{}, false
}

func (c *Compiler) ensureLocal(name string, td TypeDesc) VarSym {
	if v, ok := c.localMap[name]; ok {
		return v
	}
	vs := VarSym{Name: name, IsGlobal: false, Type: td}
	c.locals = append(c.locals, vs)
	c.localMap[name] = vs
	return vs
}

func (c *Compiler) allocTemp(td TypeDesc) VarSym {
	c.tempCount++
	name := fmt.Sprintf("_tmp_%d", c.tempCount)
	return c.ensureLocal(name, td)
}

func (c *Compiler) emitLoadVar(v VarSym) {
	if v.IsGlobal {
		if v.Type.Kind == "array" || v.Type.Kind == "struct" {
			c.emitOp("ADDR_OF_GLOBAL", v.Name)
		} else {
			c.emitOp("LOAD_GLOBAL", v.Name)
		}
	} else {
		if v.Type.Kind == "array" || v.Type.Kind == "struct" {
			c.emitOp("ADDR_OF_LOCAL", v.Name)
		} else {
			c.emitOp("LOAD_LOCAL", v.Name)
		}
	}
}

func (c *Compiler) emitStoreVar(v VarSym) {
	if v.IsGlobal {
		c.emitOp("STORE_GLOBAL", v.Name)
	} else {
		c.emitOp("STORE_LOCAL", v.Name)
	}
}

func (c *Compiler) emitAddrOfVar(v VarSym) {
	if v.IsGlobal {
		c.emitOp("ADDR_OF_GLOBAL", v.Name)
	} else {
		c.emitOp("ADDR_OF_LOCAL", v.Name)
	}
}

func (c *Compiler) emitFunction(fn *FuncDecl) error {
	c.curFunc = fn
	c.locals = nil
	c.localMap = make(map[string]VarSym)
	c.tempCount = 0

	for _, p := range fn.Params {
		c.localMap[p.Name] = p
	}

	// Buffer body instructions first so all discovered locals can be emitted in header
	savedOut := c.asmOut
	c.asmOut = strings.Builder{}

	c.toks = fn.BodyTokens
	c.pos = 0
	for c.peek().Kind != TokEOF {
		c.skipSemicolons()
		if c.peek().Kind == TokEOF {
			break
		}
		if err := c.compileStmt(); err != nil {
			return fmt.Errorf("in func %s: %w", fn.Name, err)
		}
	}

	// Ensure trailing return
	if len(fn.ReturnType) == 0 {
		c.emitOp("RET_VOID")
	} else if len(fn.ReturnType) == 1 && fn.ReturnType[0].Kind == "slice" {
		c.emitOp("PUSH_NIL_SLICE")
		c.emitOp("RET_SLICE")
	} else {
		c.emitOp("PUSH_0")
		c.emitOp("RET")
	}

	bodyAsm := c.asmOut.String()
	c.asmOut = savedOut

	entryTag := ""
	if fn.Name == "main" {
		entryTag = " entry"
	}
	fmt.Fprintf(&c.asmOut, ".function %s%s\n", fn.Name, entryTag)
	for _, p := range fn.Params {
		fmt.Fprintf(&c.asmOut, "    .param %s: %d\n", p.Name, align4(p.Type.ByteSize))
	}
	for _, l := range c.locals {
		fmt.Fprintf(&c.asmOut, "    .local %s: %d\n", l.Name, align4(l.Type.ByteSize))
	}
	c.asmOut.WriteString(bodyAsm)
	c.asmOut.WriteString(".endfunction\n\n")
	return nil
}

func (c *Compiler) compileBlock() error {
	if err := c.expectPunct("{"); err != nil {
		return err
	}
	for !c.matchPunct("}") && c.peek().Kind != TokEOF {
		c.skipSemicolons()
		if c.matchPunct("}") {
			break
		}
		if err := c.compileStmt(); err != nil {
			return err
		}
	}
	return nil
}

func (c *Compiler) compileStmt() error {
	c.skipSemicolons()
	t := c.peek()
	if t.Kind == TokEOF {
		return nil
	}
	if t.Kind == TokPunct && t.Val == "{" {
		return c.compileBlock()
	}

	if t.Kind == TokIdent {
		switch t.Val {
		case "var":
			c.next()
			name, err := c.expectIdent()
			if err != nil {
				return err
			}
			var td TypeDesc
			if !c.matchPunct("=") {
				td, err = c.parseType()
				if err != nil {
					return err
				}
				v := c.ensureLocal(name, td)
				if c.matchPunct("=") {
					et, err := c.compileExpr()
					if err != nil {
						return err
					}
					c.emitCoerceScalar(td, et)
					c.emitStoreVar(v)
				}
			} else {
				et, err := c.compileExpr()
				if err != nil {
					return err
				}
				v := c.ensureLocal(name, et)
				c.emitStoreVar(v)
			}
			c.skipSemicolons()
			return nil

		case "if":
			c.next()
			elseLbl := c.newLabel("else")
			endLbl := c.newLabel("endif")
			if _, err := c.compileExpr(); err != nil {
				return err
			}
			c.emitOp("JUMP_IF_FALSE", elseLbl)
			if err := c.compileBlock(); err != nil {
				return err
			}
			c.skipSemicolons()
			if c.peek().Kind == TokIdent && c.peek().Val == "else" {
				c.next()
				c.emitOp("JUMP", endLbl)
				fmt.Fprintf(&c.asmOut, "%s:\n", elseLbl)
				if c.peek().Kind == TokIdent && c.peek().Val == "if" {
					if err := c.compileStmt(); err != nil {
						return err
					}
				} else {
					if err := c.compileBlock(); err != nil {
						return err
					}
				}
				fmt.Fprintf(&c.asmOut, "%s:\n", endLbl)
			} else {
				fmt.Fprintf(&c.asmOut, "%s:\n", elseLbl)
			}
			return nil

		case "for":
			c.next()
			loopLbl := c.newLabel("for_cond")
			postLbl := c.newLabel("for_post")
			endLbl := c.newLabel("for_end")

			// Check for infinite loop: for { ... }
			if c.peek().Kind == TokPunct && c.peek().Val == "{" {
				fmt.Fprintf(&c.asmOut, "%s:\n", loopLbl)
				c.loopStarts = append(c.loopStarts, loopLbl)
				c.loopEnds = append(c.loopEnds, endLbl)
				if err := c.compileBlock(); err != nil {
					return err
				}
				c.loopStarts = c.loopStarts[:len(c.loopStarts)-1]
				c.loopEnds = c.loopEnds[:len(c.loopEnds)-1]
				c.emitOp("JUMP", loopLbl)
				fmt.Fprintf(&c.asmOut, "%s:\n", endLbl)
				return nil
			}

			// Scan ahead up to '{' to see if there is a ';' (3-clause for loop)
			hasSemi := false
			scanPos := c.pos
			parenDepth := 0
			for scanPos < len(c.toks) {
				st := c.toks[scanPos]
				if st.Kind == TokPunct {
					if st.Val == "(" {
						parenDepth++
					} else if st.Val == ")" {
						parenDepth--
					} else if st.Val == "{" && parenDepth == 0 {
						break
					} else if st.Val == ";" && parenDepth == 0 {
						hasSemi = true
						break
					}
				}
				scanPos++
			}

			if !hasSemi {
				// While-style: for cond { ... }
				fmt.Fprintf(&c.asmOut, "%s:\n", loopLbl)
				if _, err := c.compileExpr(); err != nil {
					return err
				}
				c.emitOp("JUMP_IF_FALSE", endLbl)
				c.loopStarts = append(c.loopStarts, loopLbl)
				c.loopEnds = append(c.loopEnds, endLbl)
				if err := c.compileBlock(); err != nil {
					return err
				}
				c.loopStarts = c.loopStarts[:len(c.loopStarts)-1]
				c.loopEnds = c.loopEnds[:len(c.loopEnds)-1]
				c.emitOp("JUMP", loopLbl)
				fmt.Fprintf(&c.asmOut, "%s:\n", endLbl)
				return nil
			}

			// 3-clause: for init; cond; post { ... }
			if !c.matchPunct(";") {
				if err := c.compileSimpleStmt(); err != nil {
					return err
				}
				_ = c.matchPunct(";")
			}
			fmt.Fprintf(&c.asmOut, "%s:\n", loopLbl)
			if !c.matchPunct(";") {
				if _, err := c.compileExpr(); err != nil {
					return err
				}
				c.emitOp("JUMP_IF_FALSE", endLbl)
				_ = c.matchPunct(";")
			}

			// Capture post statement tokens until '{'
			postStart := c.pos
			pDepth := 0
			for c.pos < len(c.toks) {
				tk := c.peek()
				if tk.Kind == TokPunct {
					if tk.Val == "(" {
						pDepth++
					} else if tk.Val == ")" {
						pDepth--
					} else if tk.Val == "{" && pDepth == 0 {
						break
					}
				}
				c.pos++
			}
			postToks := append([]Token{}, c.toks[postStart:c.pos]...)

			c.loopStarts = append(c.loopStarts, postLbl)
			c.loopEnds = append(c.loopEnds, endLbl)
			if err := c.compileBlock(); err != nil {
				return err
			}
			c.loopStarts = c.loopStarts[:len(c.loopStarts)-1]
			c.loopEnds = c.loopEnds[:len(c.loopEnds)-1]

			fmt.Fprintf(&c.asmOut, "%s:\n", postLbl)
			if len(postToks) > 0 {
				savedToks, savedPos := c.toks, c.pos
				c.toks = append(postToks, Token{Kind: TokEOF})
				c.pos = 0
				if err := c.compileSimpleStmt(); err != nil {
					return err
				}
				c.toks, c.pos = savedToks, savedPos
			}
			c.emitOp("JUMP", loopLbl)
			fmt.Fprintf(&c.asmOut, "%s:\n", endLbl)
			return nil

		case "switch":
			c.next()
			exprType, err := c.compileExpr()
			if err != nil {
				return err
			}
			swVar := c.allocTemp(exprType)
			c.emitStoreVar(swVar)
			if err := c.expectPunct("{"); err != nil {
				return err
			}
			endSwitch := c.newLabel("sw_end")
			for !c.matchPunct("}") && c.peek().Kind != TokEOF {
				c.skipSemicolons()
				if c.matchPunct("}") {
					break
				}
				kw, err := c.expectIdent()
				if err != nil {
					return err
				}
				if kw == "case" {
					nextCase := c.newLabel("sw_case")
					bodyLbl := c.newLabel("sw_body")
					for {
						c.emitLoadVar(swVar)
						if _, err := c.compileExpr(); err != nil {
							return err
						}
						if exprType.Kind == "slice" {
							c.emitOp("STR_CMP")
							c.emitOp("PUSH_0")
						}
						c.emitOp("CMP_EQ")
						if c.matchPunct(",") {
							c.emitOp("JUMP_IF_TRUE", bodyLbl)
						} else {
							c.emitOp("JUMP_IF_FALSE", nextCase)
							break
						}
					}
					if err := c.expectPunct(":"); err != nil {
						return err
					}
					fmt.Fprintf(&c.asmOut, "%s:\n", bodyLbl)
					for c.peek().Kind != TokEOF &&
						!(c.peek().Kind == TokPunct && c.peek().Val == "}") &&
						!(c.peek().Kind == TokIdent && (c.peek().Val == "case" || c.peek().Val == "default")) {
						if err := c.compileStmt(); err != nil {
							return err
						}
						c.skipSemicolons()
					}
					c.emitOp("JUMP", endSwitch)
					fmt.Fprintf(&c.asmOut, "%s:\n", nextCase)
				} else if kw == "default" {
					if err := c.expectPunct(":"); err != nil {
						return err
					}
					for c.peek().Kind != TokEOF &&
						!(c.peek().Kind == TokPunct && c.peek().Val == "}") &&
						!(c.peek().Kind == TokIdent && (c.peek().Val == "case" || c.peek().Val == "default")) {
						if err := c.compileStmt(); err != nil {
							return err
						}
						c.skipSemicolons()
					}
				} else {
					return fmt.Errorf("unexpected %q in switch", kw)
				}
			}
			fmt.Fprintf(&c.asmOut, "%s:\n", endSwitch)
			return nil

		case "break":
			c.next()
			if len(c.loopEnds) == 0 {
				return fmt.Errorf("line %d: break outside loop", t.Line)
			}
			c.emitOp("JUMP", c.loopEnds[len(c.loopEnds)-1])
			c.skipSemicolons()
			return nil

		case "continue":
			c.next()
			if len(c.loopStarts) == 0 {
				return fmt.Errorf("line %d: continue outside loop", t.Line)
			}
			c.emitOp("JUMP", c.loopStarts[len(c.loopStarts)-1])
			c.skipSemicolons()
			return nil

		case "return":
			c.next()
			if c.peek().Kind == TokPunct && (c.peek().Val == ";" || c.peek().Val == "}") {
				c.emitOp("RET_VOID")
				c.skipSemicolons()
				return nil
			}
			var lastType TypeDesc
			count := 0
			for {
				et, err := c.compileExpr()
				if err != nil {
					return err
				}
				lastType = et
				count++
				if !c.matchPunct(",") {
					break
				}
			}
			if count == 1 && lastType.Kind == "slice" {
				c.emitOp("RET_SLICE")
			} else {
				c.emitOp("RET")
			}
			c.skipSemicolons()
			return nil
		}
	}

	err := c.compileSimpleStmt()
	c.skipSemicolons()
	return err
}

func (c *Compiler) compileSimpleStmt() error {
	// Check for multi-assignment or short declaration: a, b, c := call() or a := expr
	if c.peek().Kind == TokIdent {
		savePos := c.pos
		var lhsNames []string
		isMultiIdent := true
		for {
			if c.peek().Kind != TokIdent {
				isMultiIdent = false
				break
			}
			lhsNames = append(lhsNames, c.next().Val)
			if !c.matchPunct(",") {
				break
			}
		}
		if isMultiIdent && (c.peek().Kind == TokPunct && (c.peek().Val == ":=" || (len(lhsNames) > 1 && c.peek().Val == "="))) {
			opVal := c.next().Val
			rhsTypes, err := c.compileMultiReturnExpr()
			if err != nil {
				return err
			}
			if len(lhsNames) == 1 {
				t0 := TypeDesc{Kind: "scalar", ScalarType: "int", ByteSize: 4, ElemSize: 4}
				if len(rhsTypes) > 0 {
					t0 = rhsTypes[0]
				}
				if lhsNames[0] == "_" {
					if t0.Kind == "slice" {
						c.emitOp("POP_SLICE")
					} else {
						c.emitOp("POP")
					}
				} else {
					var v VarSym
					if opVal == ":=" {
						v = c.ensureLocal(lhsNames[0], t0)
					} else {
						var ok bool
						v, ok = c.lookupVar(lhsNames[0])
						if !ok {
							return fmt.Errorf("unknown variable %q", lhsNames[0])
						}
					}
					c.emitStoreVar(v)
				}
				return nil
			}
			// Pop multi-return values in reverse order (top of stack is last return value)
			for i := len(lhsNames) - 1; i >= 0; i-- {
				name := lhsNames[i]
				rt := TypeDesc{Kind: "scalar", ScalarType: "int", ByteSize: 4, ElemSize: 4}
				if i < len(rhsTypes) {
					rt = rhsTypes[i]
				}
				if name == "_" {
					if rt.Kind == "slice" {
						c.emitOp("POP_SLICE")
					} else {
						c.emitOp("POP")
					}
				} else {
					var v VarSym
					if opVal == ":=" {
						v = c.ensureLocal(name, rt)
					} else {
						var ok bool
						v, ok = c.lookupVar(name)
						if !ok {
							v = c.ensureLocal(name, rt)
						}
					}
					c.emitStoreVar(v)
				}
			}
			return nil
		}
		c.pos = savePos
	}

	// Check for LValue assignment, compound assignment, or ++ / --
	// LValue can be: ident, ident[e1], ident[e1][e2], ident.field, ident[e1].field, ident.f1.f2
	if c.peek().Kind == TokIdent {
		savePos := c.pos
		identTok := c.next()
		if v, ok := c.lookupVar(identTok.Val); ok {
			// 1. Simple variable assignment: x = expr, x += expr, x++, x--
			if c.peek().Kind == TokPunct {
				op := c.peek().Val
				switch op {
				case "=":
					c.next()
					et, err := c.compileExpr()
					if err != nil {
						return err
					}
					c.emitCoerceScalar(v.Type, et)
					c.emitStoreVar(v)
					return nil
				case "++", "--":
					c.next()
					c.emitLoadVar(v)
					if isFloatType(v.Type) {
						c.emitPushFloat32(1.0)
						if op == "++" {
							c.emitOp("FADD")
						} else {
							c.emitOp("FSUB")
						}
					} else {
						c.emitOp("PUSH_1")
						if op == "++" {
							c.emitOp("ADD")
						} else {
							c.emitOp("SUB")
						}
					}
					c.emitStoreVar(v)
					return nil
				case "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>=":
					c.next()
					c.emitLoadVar(v)
					et, err := c.compileExpr()
					if err != nil {
						return err
					}
					if isFloatType(v.Type) {
						c.emitCoerceScalar(v.Type, et)
						c.emitCompoundFloatAlu(op)
					} else {
						c.emitCompoundAlu(op)
					}
					c.emitStoreVar(v)
					return nil
				}
			}
			// 2. Chained struct field / array / slice element assignment
			if c.peek().Kind == TokPunct && (c.peek().Val == "." || c.peek().Val == "[") {
				elemType, hasChain, err := c.compileVarLValueChain(v)
				if err != nil {
					return err
				}
				if hasChain && c.peek().Kind == TokPunct {
					op := c.peek().Val
					if c.matchPunct("=") {
						et, err := c.compileExpr()
						if err != nil {
							return err
						}
						c.emitCoerceScalar(elemType, et)
						c.emitPokeBySize(elemType.ElemSize)
						return nil
					} else if c.matchPunct("++") || c.matchPunct("--") {
						c.emitOp("DUP")
						c.emitPeekByType(elemType.ScalarType, elemType.ElemSize)
						if isFloatType(elemType) {
							c.emitPushFloat32(1.0)
							if op == "++" {
								c.emitOp("FADD")
							} else {
								c.emitOp("FSUB")
							}
						} else {
							c.emitOp("PUSH_1")
							if op == "++" {
								c.emitOp("ADD")
							} else {
								c.emitOp("SUB")
							}
						}
						c.emitPokeBySize(elemType.ElemSize)
						return nil
					} else if op == "+=" || op == "-=" || op == "*=" || op == "/=" || op == "%=" ||
						op == "&=" || op == "|=" || op == "^=" || op == "<<=" || op == ">>=" {
						c.next()
						c.emitOp("DUP")
						c.emitPeekByType(elemType.ScalarType, elemType.ElemSize)
						et, err := c.compileExpr()
						if err != nil {
							return err
						}
						if isFloatType(elemType) {
							c.emitCoerceScalar(elemType, et)
							c.emitCompoundFloatAlu(op)
						} else {
							c.emitCompoundAlu(op)
						}
						c.emitPokeBySize(elemType.ElemSize)
						return nil
					}
				}
			}
		}
		c.pos = savePos
	}

	// Otherwise evaluate expression statement (e.g. function call) and pop any unused return value
	et, err := c.compileExpr()
	if err != nil {
		return err
	}
	if et.Kind == "slice" {
		c.emitOp("POP_SLICE")
	} else if et.Kind == "scalar" {
		c.emitOp("POP")
	}
	return nil
}

// compileVarLValueChain emits instructions that compute the flat memory address
// of a chained field/element expression starting at variable v (e.g. s.X, p.X,
// pts[i].X, a.Next.Val, grid[r][c]) and returns the resulting element TypeDesc.
func (c *Compiler) compileVarLValueChain(v VarSym) (TypeDesc, bool, error) {
	if c.peek().Kind != TokPunct || (c.peek().Val != "." && c.peek().Val != "[") {
		return TypeDesc{}, false, nil
	}

	var curType TypeDesc
	started := false

	if c.peek().Val == "[" {
		c.next()
		c.emitElementAddressPrefix(v)
		if _, err := c.compileExpr(); err != nil {
			return TypeDesc{}, false, err
		}
		if err := c.expectPunct("]"); err != nil {
			return TypeDesc{}, false, err
		}
		if v.Type.Dim2 > 0 && c.matchPunct("[") {
			c.emitPushInt(int64(v.Type.Dim2))
			c.emitOp("MUL")
			if _, err := c.compileExpr(); err != nil {
				return TypeDesc{}, false, err
			}
			c.emitOp("ADD")
			if err := c.expectPunct("]"); err != nil {
				return TypeDesc{}, false, err
			}
		}
		c.emitScaleAdd(v.Type.ElemSize)
		if v.Type.StructName != "" {
			curType = TypeDesc{Kind: "struct", StructName: v.Type.StructName, ByteSize: v.Type.ElemSize, ElemSize: v.Type.ElemSize}
		} else {
			curType = TypeDesc{Kind: "scalar", ScalarType: v.Type.ElemScalar, ByteSize: 4, ElemSize: v.Type.ElemSize}
		}
		started = true
	} else if c.peek().Val == "." {
		if v.Type.Kind == "struct" {
			c.emitAddrOfVar(v)
			curType = v.Type
			started = true
		} else if v.Type.Kind == "scalar" && v.Type.StructName != "" {
			// Pointer to struct: load the pointer address
			c.emitLoadVar(v)
			curType = TypeDesc{Kind: "struct", StructName: v.Type.StructName}
			started = true
		} else {
			return TypeDesc{}, false, nil
		}
	}

	for c.matchPunct(".") {
		// If curType is a pointer to a struct (from a previous field), dereference it first
		if curType.Kind == "scalar" && curType.StructName != "" {
			c.emitOp("PEEK4")
			curType = TypeDesc{Kind: "struct", StructName: curType.StructName}
		}
		if curType.Kind != "struct" || curType.StructName == "" {
			return TypeDesc{}, false, fmt.Errorf("cannot access field on non-struct type")
		}
		fName, err := c.expectIdent()
		if err != nil {
			return TypeDesc{}, false, err
		}
		sd, ok := c.structs[curType.StructName]
		if !ok {
			return TypeDesc{}, false, fmt.Errorf("unknown struct %q", curType.StructName)
		}
		found := false
		for _, f := range sd.Fields {
			if f.Name == fName {
				if f.Offset > 0 {
					c.emitPushInt(int64(f.Offset))
					c.emitOp("ADD")
				}
				curType = f.Type
				found = true
				break
			}
		}
		if !found {
			return TypeDesc{}, false, fmt.Errorf("struct %s has no field %s", curType.StructName, fName)
		}
	}

	return curType, started, nil
}

func (c *Compiler) emitElementAddressPrefix(v VarSym) {
	if v.Type.Kind == "slice" {
		c.emitLoadVar(v)
		c.emitOp("SLICE_DATA")
	} else {
		c.emitAddrOfVar(v)
	}
}

func (c *Compiler) emitScaleAdd(elemSize int) {
	switch elemSize {
	case 1:
		c.emitOp("ADD")
	case 2:
		c.emitOp("SHL1_ADD")
	case 4:
		c.emitOp("SHL2_ADD")
	default:
		c.emitOp("INDEX_ADD", elemSize)
	}
}

func (c *Compiler) emitPeekByType(scalarName string, elemSize int) {
	switch elemSize {
	case 1:
		if scalarName == "int8" {
			c.emitOp("PEEKS1")
		} else {
			c.emitOp("PEEK1")
		}
	case 2:
		if scalarName == "int16" {
			c.emitOp("PEEKS2")
		} else {
			c.emitOp("PEEK2")
		}
	default:
		c.emitOp("PEEK4")
	}
}

func (c *Compiler) emitPokeBySize(elemSize int) {
	switch elemSize {
	case 1:
		c.emitOp("POKE1")
	case 2:
		c.emitOp("POKE2")
	default:
		c.emitOp("POKE4")
	}
}

func (c *Compiler) emitCompoundAlu(op string) {
	switch op {
	case "+=":
		c.emitOp("ADD")
	case "-=":
		c.emitOp("SUB")
	case "*=":
		c.emitOp("MUL")
	case "/=":
		c.emitOp("DIV")
	case "%=":
		c.emitOp("MOD")
	case "&=":
		c.emitOp("BIT_AND")
	case "|=":
		c.emitOp("BIT_OR")
	case "^=":
		c.emitOp("BIT_XOR")
	case "<<=":
		c.emitOp("SHL")
	case ">>=":
		c.emitOp("SHR")
	}
}

func (c *Compiler) emitCompoundFloatAlu(op string) {
	switch op {
	case "+=":
		c.emitOp("FADD")
	case "-=":
		c.emitOp("FSUB")
	case "*=":
		c.emitOp("FMUL")
	case "/=":
		c.emitOp("FDIV")
	}
}

// ---------------------------------------------------------------------------
// Expression Compiler (Precedence Climbing)
// ---------------------------------------------------------------------------
func (c *Compiler) compileExpr() (TypeDesc, error) {
	return c.compileLogicalOr()
}

func (c *Compiler) compileLogicalOr() (TypeDesc, error) {
	lt, err := c.compileLogicalAnd()
	if err != nil {
		return TypeDesc{}, err
	}
	for c.matchPunct("||") {
		trueLbl := c.newLabel("or_true")
		endLbl := c.newLabel("or_end")
		c.emitOp("JUMP_IF_TRUE", trueLbl)
		if _, err := c.compileLogicalAnd(); err != nil {
			return TypeDesc{}, err
		}
		c.emitOp("JUMP", endLbl)
		fmt.Fprintf(&c.asmOut, "%s:\n", trueLbl)
		c.emitOp("PUSH_TRUE")
		fmt.Fprintf(&c.asmOut, "%s:\n", endLbl)
		lt = TypeDesc{Kind: "scalar", ScalarType: "bool", ByteSize: 4, ElemSize: 4}
	}
	return lt, nil
}

func (c *Compiler) compileLogicalAnd() (TypeDesc, error) {
	lt, err := c.compileComparison()
	if err != nil {
		return TypeDesc{}, err
	}
	for c.matchPunct("&&") {
		falseLbl := c.newLabel("and_false")
		endLbl := c.newLabel("and_end")
		c.emitOp("JUMP_IF_FALSE", falseLbl)
		if _, err := c.compileComparison(); err != nil {
			return TypeDesc{}, err
		}
		c.emitOp("JUMP", endLbl)
		fmt.Fprintf(&c.asmOut, "%s:\n", falseLbl)
		c.emitOp("PUSH_FALSE")
		fmt.Fprintf(&c.asmOut, "%s:\n", endLbl)
		lt = TypeDesc{Kind: "scalar", ScalarType: "bool", ByteSize: 4, ElemSize: 4}
	}
	return lt, nil
}

func (c *Compiler) compileComparison() (TypeDesc, error) {
	lt, err := c.compileBitOr()
	if err != nil {
		return TypeDesc{}, err
	}
	for c.peek().Kind == TokPunct {
		op := c.peek().Val
		if op != "==" && op != "!=" && op != "<" && op != "<=" && op != ">" && op != ">=" {
			break
		}
		c.next()
		rt, err := c.compileBitOr()
		if err != nil {
			return TypeDesc{}, err
		}
		if c.emitBinaryFloatArgs(lt, rt) {
			switch op {
			case "==":
				c.emitOp("FCMP_EQ")
			case "!=":
				c.emitOp("FCMP_NE")
			case "<":
				c.emitOp("FCMP_LT")
			case "<=":
				c.emitOp("FCMP_LE")
			case ">":
				c.emitOp("FCMP_GT")
			case ">=":
				c.emitOp("FCMP_GE")
			}
			lt = TypeDesc{Kind: "scalar", ScalarType: "bool", ByteSize: 4, ElemSize: 4}
			continue
		}
		if lt.Kind == "slice" || rt.Kind == "slice" {
			c.emitOp("STR_CMP")
			c.emitOp("PUSH_0")
		}
		unsignedCmp := (lt.Kind == "scalar" && (lt.ScalarType == "uint" || lt.ScalarType == "uint32" || lt.ScalarType == "word")) ||
			(rt.Kind == "scalar" && (rt.ScalarType == "uint" || rt.ScalarType == "uint32" || rt.ScalarType == "word"))
		switch op {
		case "==":
			c.emitOp("CMP_EQ")
		case "!=":
			c.emitOp("CMP_NE")
		case "<":
			if unsignedCmp {
				c.emitOp("UCMP_LT")
			} else {
				c.emitOp("CMP_LT")
			}
		case "<=":
			if unsignedCmp {
				c.emitOp("UCMP_LE")
			} else {
				c.emitOp("CMP_LE")
			}
		case ">":
			if unsignedCmp {
				c.emitOp("UCMP_GT")
			} else {
				c.emitOp("CMP_GT")
			}
		case ">=":
			if unsignedCmp {
				c.emitOp("UCMP_GE")
			} else {
				c.emitOp("CMP_GE")
			}
		}
		lt = TypeDesc{Kind: "scalar", ScalarType: "bool", ByteSize: 4, ElemSize: 4}
	}
	return lt, nil
}

func (c *Compiler) compileBitOr() (TypeDesc, error) {
	lt, err := c.compileBitXor()
	if err != nil {
		return TypeDesc{}, err
	}
	for c.matchPunct("|") {
		if _, err := c.compileBitXor(); err != nil {
			return TypeDesc{}, err
		}
		c.emitOp("BIT_OR")
	}
	return lt, nil
}

func (c *Compiler) compileBitXor() (TypeDesc, error) {
	lt, err := c.compileBitAnd()
	if err != nil {
		return TypeDesc{}, err
	}
	for c.matchPunct("^") {
		if _, err := c.compileBitAnd(); err != nil {
			return TypeDesc{}, err
		}
		c.emitOp("BIT_XOR")
	}
	return lt, nil
}

func (c *Compiler) compileBitAnd() (TypeDesc, error) {
	lt, err := c.compileShift()
	if err != nil {
		return TypeDesc{}, err
	}
	for c.matchPunct("&") {
		if _, err := c.compileShift(); err != nil {
			return TypeDesc{}, err
		}
		c.emitOp("BIT_AND")
	}
	return lt, nil
}

func (c *Compiler) compileShift() (TypeDesc, error) {
	lt, err := c.compileAdd()
	if err != nil {
		return TypeDesc{}, err
	}
	for {
		if c.matchPunct("<<") {
			if _, err := c.compileAdd(); err != nil {
				return TypeDesc{}, err
			}
			c.emitOp("SHL")
		} else if c.matchPunct(">>") {
			if _, err := c.compileAdd(); err != nil {
				return TypeDesc{}, err
			}
			if lt.ScalarType == "uint" || lt.ScalarType == "uint32" || lt.ScalarType == "word" ||
				lt.ScalarType == "byte" || lt.ScalarType == "uint8" || lt.ScalarType == "uint16" || lt.ScalarType == "half" {
				c.emitOp("USHR")
			} else {
				c.emitOp("SHR")
			}
		} else if c.matchPunct(">>>") {
			if _, err := c.compileAdd(); err != nil {
				return TypeDesc{}, err
			}
			c.emitOp("USHR")
		} else {
			break
		}
	}
	return lt, nil
}

func (c *Compiler) compileAdd() (TypeDesc, error) {
	ScalarFloat := TypeDesc{Kind: "scalar", ScalarType: "float32", ByteSize: 4, ElemSize: 4}
	lt, err := c.compileMul()
	if err != nil {
		return TypeDesc{}, err
	}
	for {
		if c.matchPunct("+") {
			rt, err := c.compileMul()
			if err != nil {
				return TypeDesc{}, err
			}
			if c.emitBinaryFloatArgs(lt, rt) {
				c.emitOp("FADD")
				lt = ScalarFloat
			} else {
				c.emitOp("ADD")
			}
		} else if c.matchPunct("-") {
			rt, err := c.compileMul()
			if err != nil {
				return TypeDesc{}, err
			}
			if c.emitBinaryFloatArgs(lt, rt) {
				c.emitOp("FSUB")
				lt = ScalarFloat
			} else {
				c.emitOp("SUB")
			}
		} else {
			break
		}
	}
	return lt, nil
}

func (c *Compiler) compileMul() (TypeDesc, error) {
	ScalarFloat := TypeDesc{Kind: "scalar", ScalarType: "float32", ByteSize: 4, ElemSize: 4}
	lt, err := c.compileUnary()
	if err != nil {
		return TypeDesc{}, err
	}
	for {
		if c.matchPunct("*") {
			rt, err := c.compileUnary()
			if err != nil {
				return TypeDesc{}, err
			}
			if c.emitBinaryFloatArgs(lt, rt) {
				c.emitOp("FMUL")
				lt = ScalarFloat
			} else {
				c.emitOp("MUL")
			}
		} else if c.matchPunct("/") {
			rt, err := c.compileUnary()
			if err != nil {
				return TypeDesc{}, err
			}
			if c.emitBinaryFloatArgs(lt, rt) {
				c.emitOp("FDIV")
				lt = ScalarFloat
			} else if lt.ScalarType == "uint" || lt.ScalarType == "uint32" || lt.ScalarType == "word" ||
				rt.ScalarType == "uint" || rt.ScalarType == "uint32" || rt.ScalarType == "word" {
				c.emitOp("UDIV")
				lt = TypeDesc{Kind: "scalar", ScalarType: "uint", ByteSize: 4, ElemSize: 4}
			} else {
				c.emitOp("DIV")
			}
		} else if c.matchPunct("%") {
			rt, err := c.compileUnary()
			if err != nil {
				return TypeDesc{}, err
			}
			if lt.ScalarType == "uint" || lt.ScalarType == "uint32" || lt.ScalarType == "word" ||
				rt.ScalarType == "uint" || rt.ScalarType == "uint32" || rt.ScalarType == "word" {
				c.emitOp("UMOD")
				lt = TypeDesc{Kind: "scalar", ScalarType: "uint", ByteSize: 4, ElemSize: 4}
			} else {
				c.emitOp("MOD")
			}
		} else {
			break
		}
	}
	return lt, nil
}

func (c *Compiler) compileUnary() (TypeDesc, error) {
	if c.matchPunct("-") {
		t, err := c.compileUnary()
		if err != nil {
			return TypeDesc{}, err
		}
		if isFloatType(t) {
			c.emitOp("FNEG")
		} else {
			c.emitOp("NEG")
		}
		return t, nil
	}
	if c.matchPunct("!") {
		t, err := c.compileUnary()
		if err != nil {
			return TypeDesc{}, err
		}
		c.emitOp("NOT")
		return t, nil
	}
	if c.matchPunct("^") || c.matchPunct("~") {
		t, err := c.compileUnary()
		if err != nil {
			return TypeDesc{}, err
		}
		c.emitOp("BIT_NOT")
		return t, nil
	}
	if c.matchPunct("&") {
		name, err := c.expectIdent()
		if err != nil {
			return TypeDesc{}, err
		}
		v, ok := c.lookupVar(name)
		if !ok {
			return TypeDesc{}, fmt.Errorf("unknown variable %q after '&'", name)
		}
		if c.peek().Kind == TokPunct && (c.peek().Val == "." || c.peek().Val == "[") {
			elemType, hasChain, err := c.compileVarLValueChain(v)
			if err != nil {
				return TypeDesc{}, err
			}
			if hasChain {
				return TypeDesc{Kind: "scalar", ScalarType: "word", ByteSize: 4, ElemSize: 4, StructName: elemType.StructName}, nil
			}
		}
		c.emitAddrOfVar(v)
		return TypeDesc{Kind: "scalar", ScalarType: "word", ByteSize: 4, ElemSize: 4, StructName: v.Type.StructName}, nil
	}
	return c.compilePrimary()
}

func (c *Compiler) compileMultiReturnExpr() ([]TypeDesc, error) {
	t, err := c.compileExpr()
	if err != nil {
		return nil, err
	}
	if t.StructName == "__multi_2" {
		return []TypeDesc{
			{Kind: "scalar", ScalarType: "int", ByteSize: 4, ElemSize: 4},
			{Kind: "scalar", ScalarType: "int", ByteSize: 4, ElemSize: 4},
		}, nil
	}
	if t.StructName == "__multi_3" {
		return []TypeDesc{
			{Kind: "scalar", ScalarType: "bool", ByteSize: 4, ElemSize: 4},
			{Kind: "scalar", ScalarType: "int", ByteSize: 4, ElemSize: 4},
			{Kind: "scalar", ScalarType: "int", ByteSize: 4, ElemSize: 4},
		}, nil
	}
	if t.StructName == "__multi_4" {
		return []TypeDesc{
			{Kind: "scalar", ScalarType: "bool", ByteSize: 4, ElemSize: 4},
			{Kind: "scalar", ScalarType: "int", ByteSize: 4, ElemSize: 4},
			{Kind: "scalar", ScalarType: "int", ByteSize: 4, ElemSize: 4},
			{Kind: "scalar", ScalarType: "int", ByteSize: 4, ElemSize: 4},
		}, nil
	}
	if strings.HasPrefix(t.StructName, "__func_multi:") {
		fnName := strings.TrimPrefix(t.StructName, "__func_multi:")
		if fn, ok := c.funcs[fnName]; ok {
			return fn.ReturnType, nil
		}
	}
	return []TypeDesc{t}, nil
}

func (c *Compiler) compilePrimary() (TypeDesc, error) {
	ScalarInt := TypeDesc{Kind: "scalar", ScalarType: "int", ByteSize: 4, ElemSize: 4}
	ScalarFloat := TypeDesc{Kind: "scalar", ScalarType: "float32", ByteSize: 4, ElemSize: 4}
	SliceStr := TypeDesc{Kind: "slice", ScalarType: "string", ByteSize: 12, ElemSize: 1, ElemScalar: "byte"}

	if c.matchPunct("(") {
		et, err := c.compileExpr()
		if err != nil {
			return TypeDesc{}, err
		}
		if err := c.expectPunct(")"); err != nil {
			return TypeDesc{}, err
		}
		return et, nil
	}

	tk := c.next()
	switch tk.Kind {
	case TokInt:
		c.emitPushInt(tk.Int)
		return ScalarInt, nil

	case TokFloat:
		c.emitPushFloat32(float32(tk.Float))
		return ScalarFloat, nil

	case TokString:
		c.emitOp("PUSH_STR", fmt.Sprintf("%q", tk.Val))
		return SliceStr, nil

	case TokIdent:
		name := tk.Val

		// Check if 'name' is a variable followed by '.' or '['
		if v, isVar := c.lookupVar(name); isVar {
			if c.peek().Kind == TokPunct && c.peek().Val == "." {
				if v.Type.Kind == "slice" {
					c.next()
					sub, err := c.expectIdent()
					if err != nil {
						return TypeDesc{}, err
					}
					c.emitLoadVar(v)
					switch sub {
					case "Len", "len":
						c.emitOp("SLICE_LEN")
						return ScalarInt, nil
					case "Cap", "cap":
						c.emitOp("SLICE_CAP")
						return ScalarInt, nil
					case "Base", "Data", "Ptr", "base":
						c.emitOp("SLICE_DATA")
						return ScalarInt, nil
					default:
						return TypeDesc{}, fmt.Errorf("line %d: unknown slice property %s", tk.Line, sub)
					}
				}
				elemType, hasChain, err := c.compileVarLValueChain(v)
				if err != nil {
					return TypeDesc{}, err
				}
				if hasChain {
					c.emitPeekByType(elemType.ScalarType, elemType.ElemSize)
					return elemType, nil
				}
			}
			if c.peek().Kind == TokPunct && c.peek().Val == "[" {
				// Peek ahead to check if this is a slice subrange s[a:b]
				savePos := c.pos
				c.next() // '['
				isSubSlice := false
				depth := 0
				for c.pos < len(c.toks) {
					pt := c.peek()
					if pt.Kind == TokPunct {
						if pt.Val == "[" || pt.Val == "(" {
							depth++
						} else if pt.Val == "]" {
							if depth == 0 {
								break
							}
							depth--
						} else if pt.Val == ")" {
							depth--
						} else if pt.Val == ":" && depth == 0 {
							isSubSlice = true
							break
						}
					}
					c.pos++
				}
				c.pos = savePos

				if isSubSlice {
					c.next() // '['
					c.emitLoadVar(v)
					if _, err := c.compileExpr(); err != nil {
						return TypeDesc{}, err
					}
					if err := c.expectPunct(":"); err != nil {
						return TypeDesc{}, err
					}
					if _, err := c.compileExpr(); err != nil {
						return TypeDesc{}, err
					}
					if err := c.expectPunct("]"); err != nil {
						return TypeDesc{}, err
					}
					c.emitPushInt(int64(v.Type.ElemSize))
					c.emitOp("SLICE_SUB")
					return v.Type, nil
				}

				elemType, hasChain, err := c.compileVarLValueChain(v)
				if err != nil {
					return TypeDesc{}, err
				}
				if hasChain {
					c.emitPeekByType(elemType.ScalarType, elemType.ElemSize)
					return elemType, nil
				}
			}
			c.emitLoadVar(v)
			return v.Type, nil
		}

		// Support package-qualified names like os.ReadTouch(), tft.FillRect(), os.BLACK, math.Pi
		if c.matchPunct(".") {
			sub, err := c.expectIdent()
			if err != nil {
				return TypeDesc{}, err
			}
			if cVal, ok := c.consts[sub]; ok {
				c.emitPushInt(cVal)
				return ScalarInt, nil
			}
			if fVal, ok := c.floatConsts[sub]; ok {
				c.emitPushFloat32(float32(fVal))
				return ScalarFloat, nil
			}
			name = normalizeQualifiedName(name, sub)
		}

		// 1. Constant lookup
		if cVal, ok := c.consts[name]; ok {
			c.emitPushInt(cVal)
			return ScalarInt, nil
		}
		if fVal, ok := c.floatConsts[name]; ok {
			c.emitPushFloat32(float32(fVal))
			return ScalarFloat, nil
		}
		if sVal, ok := c.strConsts[name]; ok {
			c.emitOp("PUSH_STR", fmt.Sprintf("%q", sVal))
			return SliceStr, nil
		}

		// 2. Function or Builtin Call: name(...)
		if c.matchPunct("(") {
			return c.compileCall(name, tk.Line)
		}

		return TypeDesc{}, fmt.Errorf("line %d: undefined identifier %q", tk.Line, name)
	}

	return TypeDesc{}, fmt.Errorf("line %d: unexpected token %q in expression", tk.Line, tk.Val)
}

func camelToSnake(s string) string {
	var out strings.Builder
	for i, r := range s {
		if unicode.IsUpper(r) {
			if i > 0 && !unicode.IsUpper(rune(s[i-1])) && s[i-1] != '_' {
				out.WriteByte('_')
			}
			out.WriteRune(unicode.ToLower(r))
		} else {
			out.WriteRune(r)
		}
	}
	return out.String()
}

func normalizeQualifiedName(pkg, member string) string {
	snake := camelToSnake(member)
	pkg = strings.ToLower(pkg)
	if strings.HasPrefix(snake, pkg+"_") {
		return snake
	}
	return pkg + "_" + snake
}

func (c *Compiler) parseCallArgs() ([]TypeDesc, error) {
	var argTypes []TypeDesc
	for !c.matchPunct(")") && c.peek().Kind != TokEOF {
		at, err := c.compileExpr()
		if err != nil {
			return nil, err
		}
		argTypes = append(argTypes, at)
		c.matchPunct(",")
	}
	return argTypes, nil
}

func (c *Compiler) compileCall(name string, line int) (TypeDesc, error) {
	ScalarInt := TypeDesc{Kind: "scalar", ScalarType: "int", ByteSize: 4, ElemSize: 4}
	ScalarFloat := TypeDesc{Kind: "scalar", ScalarType: "float32", ByteSize: 4, ElemSize: 4}
	VoidType := TypeDesc{Kind: "void"}
	SliceStr := TypeDesc{Kind: "slice", ScalarType: "string", ByteSize: 12, ElemSize: 1, ElemScalar: "byte"}

	// Type casts and special syntax-taking builtins: new(T), make([]T, len[, cap])
	switch name {
	case "new":
		tDesc, err := c.parseType()
		if err != nil {
			return TypeDesc{}, err
		}
		if err := c.expectPunct(")"); err != nil {
			return TypeDesc{}, err
		}
		c.emitPushInt(int64(align4(tDesc.ByteSize)))
		c.emitOp("ZALLOC")
		return TypeDesc{Kind: "scalar", ScalarType: "word", ByteSize: 4, ElemSize: 4, StructName: tDesc.StructName}, nil
	case "make":
		tDesc, err := c.parseType()
		if err != nil {
			return TypeDesc{}, err
		}
		if err := c.expectPunct(","); err != nil {
			return TypeDesc{}, err
		}
		// Parse len
		if _, err := c.compileExpr(); err != nil {
			return TypeDesc{}, err
		}
		lenTmp := c.allocTemp(ScalarInt)
		c.emitStoreVar(lenTmp)
		capTmp := lenTmp
		if c.matchPunct(",") {
			// Parse explicit cap
			if _, err := c.compileExpr(); err != nil {
				return TypeDesc{}, err
			}
			capTmp = c.allocTemp(ScalarInt)
			c.emitStoreVar(capTmp)
		}
		if err := c.expectPunct(")"); err != nil {
			return TypeDesc{}, err
		}
		c.emitLoadVar(capTmp)
		c.emitPushInt(int64(tDesc.ElemSize))
		c.emitOp("MUL")
		c.emitOp("ZALLOC")
		c.emitLoadVar(capTmp)
		c.emitLoadVar(lenTmp)
		c.emitOp("SLICE_NEW")
		return tDesc, nil
	case "rgb565", "tft_color565", "color565":
		_, err := c.parseCallArgs()
		if err != nil {
			return TypeDesc{}, err
		}
		bTmp := c.allocTemp(ScalarInt)
		gTmp := c.allocTemp(ScalarInt)
		rTmp := c.allocTemp(ScalarInt)
		c.emitStoreVar(bTmp)
		c.emitStoreVar(gTmp)
		c.emitStoreVar(rTmp)
		c.emitLoadVar(rTmp)
		c.emitPushInt(0xF8)
		c.emitOp("BIT_AND")
		c.emitPushInt(8)
		c.emitOp("SHL")
		c.emitLoadVar(gTmp)
		c.emitPushInt(0xFC)
		c.emitOp("BIT_AND")
		c.emitPushInt(3)
		c.emitOp("SHL")
		c.emitOp("BIT_OR")
		c.emitLoadVar(bTmp)
		c.emitPushInt(3)
		c.emitOp("SHR")
		c.emitPushInt(0x1F)
		c.emitOp("BIT_AND")
		c.emitOp("BIT_OR")
		return ScalarInt, nil
	case "float32", "float":
		argTypes, err := c.parseCallArgs()
		if err != nil {
			return TypeDesc{}, err
		}
		if len(argTypes) == 1 && !isFloatType(argTypes[0]) {
			c.emitOp("I32_TO_F32")
		}
		return ScalarFloat, nil
	case "int", "int32", "bool":
		argTypes, err := c.parseCallArgs()
		if err != nil {
			return TypeDesc{}, err
		}
		if len(argTypes) == 1 && isFloatType(argTypes[0]) {
			c.emitOp("F32_TO_I32")
		}
		return ScalarInt, nil
	case "uint", "uint32", "word":
		argTypes, err := c.parseCallArgs()
		if err != nil {
			return TypeDesc{}, err
		}
		if len(argTypes) == 1 && isFloatType(argTypes[0]) {
			c.emitOp("F32_TO_I32")
		}
		return TypeDesc{Kind: "scalar", ScalarType: "uint", ByteSize: 4, ElemSize: 4}, nil
	case "float32_bits", "math_float32_bits", "math_float32bits":
		_, err := c.parseCallArgs()
		return ScalarInt, err
	case "float32_from_bits", "math_float32_from_bits", "math_float32frombits":
		_, err := c.parseCallArgs()
		return ScalarFloat, err
	case "byte", "uint8":
		argTypes, err := c.parseCallArgs()
		if err != nil {
			return TypeDesc{}, err
		}
		if len(argTypes) == 1 && isFloatType(argTypes[0]) {
			c.emitOp("F32_TO_I32")
		}
		c.emitPushInt(0xFF)
		c.emitOp("BIT_AND")
		return ScalarInt, nil
	case "uint16", "half":
		argTypes, err := c.parseCallArgs()
		if err != nil {
			return TypeDesc{}, err
		}
		if len(argTypes) == 1 && isFloatType(argTypes[0]) {
			c.emitOp("F32_TO_I32")
		}
		c.emitPushInt(0xFFFF)
		c.emitOp("BIT_AND")
		return ScalarInt, nil
	case "abs":
		argTypes, err := c.parseCallArgs()
		if err != nil {
			return TypeDesc{}, err
		}
		if len(argTypes) == 1 && isFloatType(argTypes[0]) {
			c.emitOp("FABS")
			return ScalarFloat, nil
		}
		posLbl := c.newLabel("abs_pos")
		c.emitOp("DUP")
		c.emitOp("PUSH_0")
		c.emitOp("CMP_GE")
		c.emitOp("JUMP_IF_TRUE", posLbl)
		c.emitOp("NEG")
		fmt.Fprintf(&c.asmOut, "%s:\n", posLbl)
		return ScalarInt, nil
	case "min":
		argTypes, err := c.parseCallArgs()
		if err != nil {
			return TypeDesc{}, err
		}
		if len(argTypes) == 2 && c.emitBinaryFloatArgs(argTypes[0], argTypes[1]) {
			bTmp := c.allocTemp(ScalarFloat)
			aTmp := c.allocTemp(ScalarFloat)
			c.emitStoreVar(bTmp)
			c.emitStoreVar(aTmp)
			c.emitLoadVar(aTmp)
			c.emitLoadVar(bTmp)
			c.emitOp("FCMP_LE")
			elseLbl := c.newLabel("fmin_b")
			endLbl := c.newLabel("fmin_end")
			c.emitOp("JUMP_IF_FALSE", elseLbl)
			c.emitLoadVar(aTmp)
			c.emitOp("JUMP", endLbl)
			fmt.Fprintf(&c.asmOut, "%s:\n", elseLbl)
			c.emitLoadVar(bTmp)
			fmt.Fprintf(&c.asmOut, "%s:\n", endLbl)
			return ScalarFloat, nil
		}
		// (a b -- min)
		bTmp := c.allocTemp(ScalarInt)
		aTmp := c.allocTemp(ScalarInt)
		c.emitStoreVar(bTmp)
		c.emitStoreVar(aTmp)
		c.emitLoadVar(aTmp)
		c.emitLoadVar(bTmp)
		c.emitOp("CMP_LE")
		elseLbl := c.newLabel("min_b")
		endLbl := c.newLabel("min_end")
		c.emitOp("JUMP_IF_FALSE", elseLbl)
		c.emitLoadVar(aTmp)
		c.emitOp("JUMP", endLbl)
		fmt.Fprintf(&c.asmOut, "%s:\n", elseLbl)
		c.emitLoadVar(bTmp)
		fmt.Fprintf(&c.asmOut, "%s:\n", endLbl)
		return ScalarInt, nil
	case "max":
		argTypes, err := c.parseCallArgs()
		if err != nil {
			return TypeDesc{}, err
		}
		if len(argTypes) == 2 && c.emitBinaryFloatArgs(argTypes[0], argTypes[1]) {
			bTmp := c.allocTemp(ScalarFloat)
			aTmp := c.allocTemp(ScalarFloat)
			c.emitStoreVar(bTmp)
			c.emitStoreVar(aTmp)
			c.emitLoadVar(aTmp)
			c.emitLoadVar(bTmp)
			c.emitOp("FCMP_GE")
			elseLbl := c.newLabel("fmax_b")
			endLbl := c.newLabel("fmax_end")
			c.emitOp("JUMP_IF_FALSE", elseLbl)
			c.emitLoadVar(aTmp)
			c.emitOp("JUMP", endLbl)
			fmt.Fprintf(&c.asmOut, "%s:\n", elseLbl)
			c.emitLoadVar(bTmp)
			fmt.Fprintf(&c.asmOut, "%s:\n", endLbl)
			return ScalarFloat, nil
		}
		bTmp := c.allocTemp(ScalarInt)
		aTmp := c.allocTemp(ScalarInt)
		c.emitStoreVar(bTmp)
		c.emitStoreVar(aTmp)
		c.emitLoadVar(aTmp)
		c.emitLoadVar(bTmp)
		c.emitOp("CMP_GE")
		elseLbl := c.newLabel("max_b")
		endLbl := c.newLabel("max_end")
		c.emitOp("JUMP_IF_FALSE", elseLbl)
		c.emitLoadVar(aTmp)
		c.emitOp("JUMP", endLbl)
		fmt.Fprintf(&c.asmOut, "%s:\n", elseLbl)
		c.emitLoadVar(bTmp)
		fmt.Fprintf(&c.asmOut, "%s:\n", endLbl)
		return ScalarInt, nil
	case "clamp", "constrain":
		_, err := c.parseCallArgs()
		if err != nil {
			return TypeDesc{}, err
		}
		hiTmp := c.allocTemp(ScalarInt)
		loTmp := c.allocTemp(ScalarInt)
		vTmp := c.allocTemp(ScalarInt)
		c.emitStoreVar(hiTmp)
		c.emitStoreVar(loTmp)
		c.emitStoreVar(vTmp)
		checkHi := c.newLabel("clamp_hi")
		doneLbl := c.newLabel("clamp_done")
		c.emitLoadVar(vTmp)
		c.emitLoadVar(loTmp)
		c.emitOp("CMP_LT")
		c.emitOp("JUMP_IF_FALSE", checkHi)
		c.emitLoadVar(loTmp)
		c.emitOp("JUMP", doneLbl)
		fmt.Fprintf(&c.asmOut, "%s:\n", checkHi)
		c.emitLoadVar(vTmp)
		c.emitLoadVar(hiTmp)
		c.emitOp("CMP_GT")
		useVal := c.newLabel("clamp_val")
		c.emitOp("JUMP_IF_FALSE", useVal)
		c.emitLoadVar(hiTmp)
		c.emitOp("JUMP", doneLbl)
		fmt.Fprintf(&c.asmOut, "%s:\n", useVal)
		c.emitLoadVar(vTmp)
		fmt.Fprintf(&c.asmOut, "%s:\n", doneLbl)
		return ScalarInt, nil
	}

	// User-defined function call
	if fn, ok := c.funcs[name]; ok {
		idx := 0
		for !c.matchPunct(")") && c.peek().Kind != TokEOF {
			at, err := c.compileExpr()
			if err != nil {
				return TypeDesc{}, err
			}
			if idx < len(fn.Params) {
				c.emitCoerceScalar(fn.Params[idx].Type, at)
			}
			idx++
			c.matchPunct(",")
		}
		c.emitOp("CALL", fn.Name)
		if len(fn.ReturnType) == 0 {
			return VoidType, nil
		}
		if len(fn.ReturnType) == 1 {
			return fn.ReturnType[0], nil
		}
		return TypeDesc{Kind: "scalar", StructName: "__func_multi:" + fn.Name, ByteSize: 4}, nil
	}

	// Built-in VM & Lakatos OS Intrinsics
	argTypes, err := c.parseCallArgs()
	if err != nil {
		return TypeDesc{}, err
	}

	switch strings.ToLower(name) {
	case "sqrt", "sqrtf", "math_sqrt":
		if len(argTypes) == 1 && !isFloatType(argTypes[0]) {
			c.emitOp("I32_TO_F32")
		}
		c.emitOp("FSQRT")
		return ScalarFloat, nil
	case "sin", "sinf", "math_sin":
		if len(argTypes) == 1 && !isFloatType(argTypes[0]) {
			c.emitOp("I32_TO_F32")
		}
		c.emitOp("FSIN")
		return ScalarFloat, nil
	case "cos", "cosf", "math_cos":
		if len(argTypes) == 1 && !isFloatType(argTypes[0]) {
			c.emitOp("I32_TO_F32")
		}
		c.emitOp("FCOS")
		return ScalarFloat, nil
	case "atan2", "atan2f", "math_atan2":
		if len(argTypes) == 2 {
			if !isFloatType(argTypes[0]) && !isFloatType(argTypes[1]) {
				c.emitOp("SWAP")
				c.emitOp("I32_TO_F32")
				c.emitOp("SWAP")
				c.emitOp("I32_TO_F32")
			} else {
				c.emitBinaryFloatArgs(argTypes[0], argTypes[1])
			}
		}
		c.emitOp("FATAN2")
		return ScalarFloat, nil
	case "fabs", "fabsf", "math_abs":
		if len(argTypes) == 1 && !isFloatType(argTypes[0]) {
			c.emitOp("I32_TO_F32")
		}
		c.emitOp("FABS")
		return ScalarFloat, nil
	case "floor", "floorf", "math_floor":
		if len(argTypes) == 1 && !isFloatType(argTypes[0]) {
			c.emitOp("I32_TO_F32")
		}
		c.emitOp("FFLOOR")
		return ScalarFloat, nil
	case "ceil", "ceilf", "math_ceil":
		if len(argTypes) == 1 && !isFloatType(argTypes[0]) {
			c.emitOp("I32_TO_F32")
		}
		c.emitOp("FCEIL")
		return ScalarFloat, nil
	case "round", "roundf", "math_round":
		if len(argTypes) == 1 && !isFloatType(argTypes[0]) {
			c.emitOp("I32_TO_F32")
		}
		c.emitPushFloat32(0.5)
		c.emitOp("FADD")
		c.emitOp("FFLOOR")
		return ScalarFloat, nil
	case "float_to_str", "ftoa", "fmt_float":
		if len(argTypes) == 1 {
			if !isFloatType(argTypes[0]) {
				c.emitOp("I32_TO_F32")
			}
			c.emitOp("PUSH_NEG1")
		} else if len(argTypes) == 2 && !isFloatType(argTypes[0]) {
			c.emitOp("SWAP")
			c.emitOp("I32_TO_F32")
			c.emitOp("SWAP")
		}
		c.emitOp("SYSCALL", "SYS_FMT_FLOAT_TO_STR")
		return SliceStr, nil
	case "len":
		c.emitOp("SLICE_LEN")
		return ScalarInt, nil
	case "cap":
		c.emitOp("SLICE_CAP")
		return ScalarInt, nil
	case "cstr", "slice_data", "slice_base":
		c.emitOp("SLICE_DATA")
		return ScalarInt, nil
	case "make_slice", "slice_new":
		c.emitOp("SLICE_NEW")
		return SliceStr, nil
	case "malloc", "os_malloc":
		c.emitOp("MALLOC")
		return ScalarInt, nil
	case "zalloc", "os_zalloc", "calloc":
		c.emitOp("ZALLOC")
		return ScalarInt, nil
	case "realloc", "os_realloc":
		c.emitOp("REALLOC")
		return ScalarInt, nil
	case "free", "os_free", "delete":
		if len(argTypes) == 1 && argTypes[0].Kind == "slice" {
			c.emitOp("SLICE_DATA")
		}
		c.emitOp("FREE")
		return VoidType, nil
	case "heap_free", "bytes_free", "bytesfree", "os_free_heap", "free_heap_bytes":
		c.emitOp("HEAP_FREE")
		return ScalarInt, nil
	case "peekb", "peek1":
		c.emitOp("PEEK1")
		return ScalarInt, nil
	case "peeks1":
		c.emitOp("PEEKS1")
		return ScalarInt, nil
	case "pokeb", "poke1":
		c.emitOp("POKE1")
		return VoidType, nil
	case "peekw", "peek2":
		c.emitOp("PEEK2")
		return ScalarInt, nil
	case "peeks2":
		c.emitOp("PEEKS2")
		return ScalarInt, nil
	case "pokew", "poke2":
		c.emitOp("POKE2")
		return VoidType, nil
	case "peek", "peek4", "peekl":
		c.emitOp("PEEK4")
		return ScalarInt, nil
	case "poke", "poke4", "pokel":
		c.emitOp("POKE4")
		return VoidType, nil
	case "memcpy":
		c.emitOp("MEM_COPY")
		return VoidType, nil
	case "memset":
		c.emitOp("MEM_SET")
		return VoidType, nil
	case "memcmp":
		c.emitOp("MEM_CMP")
		return ScalarInt, nil
	case "println", "os_println":
		emitScalarToStr := func(t TypeDesc) {
			c.emitOp("PUSH_0")
			if t.ScalarType == "uint" || t.ScalarType == "uint32" || t.ScalarType == "word" {
				c.emitPushInt(-2147483648) // 0x80000000 unsigned flag
			} else {
				c.emitOp("PUSH_0")
			}
			c.emitOp("SYSCALL", "SYS_FMT_INT_TO_STR")
		}
		if len(argTypes) == 1 {
			if isFloatType(argTypes[0]) {
				c.emitOp("PUSH_NEG1")
				c.emitOp("SYSCALL", "SYS_FMT_FLOAT_TO_STR")
			} else if argTypes[0].Kind != "slice" {
				emitScalarToStr(argTypes[0])
			}
			c.emitOp("IO_PRINT")
			return VoidType, nil
		}
		// Multi-argument println: pop args into temps in reverse order, print each separated by space
		tmps := make([]VarSym, len(argTypes))
		for i := len(argTypes) - 1; i >= 0; i-- {
			tmps[i] = c.allocTemp(argTypes[i])
			c.emitStoreVar(tmps[i])
		}
		for i := 0; i < len(argTypes)-1; i++ {
			c.emitLoadVar(tmps[i])
			if isFloatType(argTypes[i]) {
				c.emitOp("PUSH_NEG1")
				c.emitOp("SYSCALL", "SYS_FMT_FLOAT_TO_STR")
			} else if argTypes[i].Kind != "slice" {
				emitScalarToStr(argTypes[i])
			}
			c.emitOp("SWAP")
			c.emitOp("POP")
			c.emitOp("SYSCALL", "SYS_SERIAL_WRITE")
			c.emitOp("POP")
			c.emitOp("PUSH_STR", `" "`)
			c.emitOp("SWAP")
			c.emitOp("POP")
			c.emitOp("SYSCALL", "SYS_SERIAL_WRITE")
			c.emitOp("POP")
		}
		last := len(argTypes) - 1
		c.emitLoadVar(tmps[last])
		if isFloatType(argTypes[last]) {
			c.emitOp("PUSH_NEG1")
			c.emitOp("SYSCALL", "SYS_FMT_FLOAT_TO_STR")
		} else if argTypes[last].Kind != "slice" {
			emitScalarToStr(argTypes[last])
		}
		c.emitOp("IO_PRINT")
		return VoidType, nil
	case "panic":
		c.emitOp("PANIC")
		return VoidType, nil
	case "int_to_str", "itoa", "uint_to_str", "utoa", "os_int_to_str", "os_uint_to_str":
		isUnsigned := name == "uint_to_str" || name == "utoa" || name == "os_uint_to_str" ||
			(len(argTypes) >= 1 && (argTypes[0].ScalarType == "uint" || argTypes[0].ScalarType == "uint32" || argTypes[0].ScalarType == "word"))
		if len(argTypes) == 1 {
			c.emitOp("PUSH_0")
			if isUnsigned {
				c.emitPushInt(-2147483648)
			} else {
				c.emitOp("PUSH_0")
			}
		} else if isUnsigned {
			c.emitPushInt(-2147483648)
			c.emitOp("BIT_OR")
		}
		c.emitOp("SYSCALL", "SYS_FMT_INT_TO_STR")
		return SliceStr, nil

	// Lakatos Lifecycle & Timing
	case "os_exit", "exit":
		if len(argTypes) == 0 {
			c.emitOp("PUSH_0")
		}
		c.emitOp("SYSCALL", "SYS_EXIT")
		return VoidType, nil
	case "os_yield", "yield":
		c.emitOp("SYSCALL", "SYS_YIELD")
		return VoidType, nil
	case "os_millis", "millis":
		c.emitOp("SYSCALL", "SYS_MILLIS")
		return ScalarInt, nil
	case "os_micros", "micros":
		c.emitOp("SYSCALL", "SYS_MICROS")
		return ScalarInt, nil
	case "os_delay_ms", "delay_ms", "delay":
		c.emitOp("SYSCALL", "SYS_DELAY_MS")
		return VoidType, nil
	case "os_rand", "rand":
		c.emitOp("SYSCALL", "SYS_RAND")
		return ScalarInt, nil

	// Identity, Airport & Time
	case "os_get_airport", "get_airport":
		c.emitOp("SYSCALL", "SYS_GET_AIRPORT")
		return SliceStr, nil
	case "os_get_airport_info", "get_airport_info":
		c.emitOp("SYSCALL", "SYS_GET_AIRPORT_INFO")
		return ScalarInt, nil
	case "os_get_device_name", "get_device_name":
		c.emitOp("SYSCALL", "SYS_GET_DEVICE_NAME")
		return SliceStr, nil
	case "os_get_serial_number", "get_serial_number":
		c.emitOp("SYSCALL", "SYS_GET_SERIAL_NUMBER")
		return SliceStr, nil
	case "os_get_sh_url", "get_superhighway_url":
		c.emitOp("SYSCALL", "SYS_GET_SH_URL")
		return SliceStr, nil
	case "os_get_unix_time", "get_unix_time":
		c.emitOp("SYSCALL", "SYS_GET_UNIX_TIME")
		return ScalarInt, nil
	case "os_format_local_time", "format_local_time":
		c.emitOp("SYSCALL", "SYS_FORMAT_LOCAL_TIME")
		return ScalarInt, nil
	case "os_city_hash32", "os_cityhash32", "city_hash32", "cityhash32":
		if len(argTypes) == 1 && argTypes[0].Kind == "slice" {
			sTmp := c.allocTemp(argTypes[0])
			c.emitStoreVar(sTmp)
			c.emitLoadVar(sTmp)
			c.emitOp("SLICE_DATA")
			c.emitLoadVar(sTmp)
			c.emitOp("SLICE_LEN")
		} else if len(argTypes) == 2 && argTypes[0].Kind == "slice" {
			szTmp := c.allocTemp(ScalarInt)
			sTmp := c.allocTemp(argTypes[0])
			c.emitStoreVar(szTmp)
			c.emitStoreVar(sTmp)
			c.emitLoadVar(sTmp)
			c.emitOp("SLICE_DATA")
			c.emitLoadVar(szTmp)
		}
		c.emitOp("SYSCALL", "SYS_CITY_HASH32")
		return TypeDesc{Kind: "scalar", ScalarType: "uint", ByteSize: 4, ElemSize: 4}, nil

	// TFT Display
	case "tft_fill_screen", "os_tft_fill_screen":
		c.emitOp("SYSCALL", "SYS_TFT_FILL_SCREEN")
		return VoidType, nil
	case "tft_fill_rect", "os_tft_fill_rect":
		c.emitOp("SYSCALL", "SYS_TFT_FILL_RECT")
		return VoidType, nil
	case "tft_draw_rect", "os_tft_draw_rect":
		c.emitOp("SYSCALL", "SYS_TFT_DRAW_RECT")
		return VoidType, nil
	case "tft_fill_round_rect", "os_tft_fill_round_rect":
		c.emitOp("SYSCALL", "SYS_TFT_FILL_ROUND_RECT")
		return VoidType, nil
	case "tft_draw_round_rect", "os_tft_draw_round_rect":
		c.emitOp("SYSCALL", "SYS_TFT_DRAW_ROUND_RECT")
		return VoidType, nil
	case "tft_draw_line", "os_tft_draw_line":
		c.emitOp("SYSCALL", "SYS_TFT_DRAW_LINE")
		return VoidType, nil
	case "tft_draw_fast_hline", "tft_draw_fast_h_line", "os_tft_draw_fast_hline":
		c.emitOp("SYSCALL", "SYS_TFT_DRAW_FAST_HLINE")
		return VoidType, nil
	case "tft_draw_fast_vline", "tft_draw_fast_v_line", "os_tft_draw_fast_vline":
		c.emitOp("SYSCALL", "SYS_TFT_DRAW_FAST_VLINE")
		return VoidType, nil
	case "tft_fill_circle", "os_tft_fill_circle":
		c.emitOp("SYSCALL", "SYS_TFT_FILL_CIRCLE")
		return VoidType, nil
	case "tft_draw_circle", "os_tft_draw_circle":
		c.emitOp("SYSCALL", "SYS_TFT_DRAW_CIRCLE")
		return VoidType, nil
	case "tft_fill_triangle", "os_tft_fill_triangle":
		c.emitOp("SYSCALL", "SYS_TFT_FILL_TRIANGLE")
		return VoidType, nil
	case "tft_draw_char", "os_tft_draw_char":
		c.emitOp("SYSCALL", "SYS_TFT_DRAW_CHAR")
		return VoidType, nil
	case "tft_set_text_color", "os_tft_set_text_color":
		c.emitOp("SYSCALL", "SYS_TFT_SET_TEXT_COLOR")
		return VoidType, nil
	case "tft_set_text_datum", "os_tft_set_text_datum":
		c.emitOp("SYSCALL", "SYS_TFT_SET_TEXT_DATUM")
		return VoidType, nil
	case "tft_draw_string", "os_tft_draw_string":
		if len(argTypes) == 6 && argTypes[2].Kind == "slice" {
			// 6-arg convenience form: tft_draw_string(x, y, str, fg, bg, font)
			fontTmp := c.allocTemp(ScalarInt)
			bgTmp := c.allocTemp(ScalarInt)
			fgTmp := c.allocTemp(ScalarInt)
			strTmp := c.allocTemp(SliceStr)
			yTmp := c.allocTemp(ScalarInt)
			xTmp := c.allocTemp(ScalarInt)
			c.emitStoreVar(fontTmp)
			c.emitStoreVar(bgTmp)
			c.emitStoreVar(fgTmp)
			c.emitStoreVar(strTmp)
			c.emitStoreVar(yTmp)
			c.emitStoreVar(xTmp)
			c.emitLoadVar(fgTmp)
			c.emitLoadVar(bgTmp)
			c.emitOp("SYSCALL", "SYS_TFT_SET_TEXT_COLOR")
			c.emitLoadVar(strTmp)
			c.emitLoadVar(xTmp)
			c.emitLoadVar(yTmp)
			c.emitLoadVar(fontTmp)
		}
		c.emitOp("SYSCALL", "SYS_TFT_DRAW_STRING")
		return ScalarInt, nil
	case "tft_draw_cstr", "os_tft_draw_cstr":
		c.emitOp("SYSCALL", "SYS_TFT_DRAW_CSTR")
		return ScalarInt, nil
	case "tft_draw_int", "os_tft_draw_int":
		c.emitOp("SYSCALL", "SYS_TFT_DRAW_INT")
		return ScalarInt, nil
	case "tft_draw_float", "os_tft_draw_float":
		c.emitOp("SYSCALL", "SYS_TFT_DRAW_FLOAT")
		return ScalarInt, nil
	case "tft_push_rgb565", "os_tft_push_rgb565":
		c.emitOp("SYSCALL", "SYS_TFT_PUSH_RGB565")
		return VoidType, nil
	case "tft_draw_png_file", "os_draw_png_file", "os_tft_draw_png_file":
		c.emitOp("SYSCALL", "SYS_TFT_DRAW_PNG_FILE")
		return ScalarInt, nil
	case "tft_draw_png", "os_draw_png", "tft_draw_png_mem", "os_draw_png_mem", "os_tft_draw_png":
		if len(argTypes) == 3 && argTypes[0].Kind == "slice" {
			if argTypes[0].ScalarType == "string" {
				c.emitOp("SYSCALL", "SYS_TFT_DRAW_PNG_FILE")
				return ScalarInt, nil
			}
			// 3-arg slice form: tft_draw_png(pngBytesSlice, x, y)
			yTmp := c.allocTemp(ScalarInt)
			xTmp := c.allocTemp(ScalarInt)
			sTmp := c.allocTemp(argTypes[0])
			c.emitStoreVar(yTmp)
			c.emitStoreVar(xTmp)
			c.emitStoreVar(sTmp)
			c.emitLoadVar(sTmp)
			c.emitOp("SLICE_DATA")
			c.emitLoadVar(sTmp)
			c.emitOp("SLICE_LEN")
			c.emitLoadVar(xTmp)
			c.emitLoadVar(yTmp)
		}
		c.emitOp("SYSCALL", "SYS_TFT_DRAW_PNG_MEM")
		return ScalarInt, nil
	case "os_png_info", "tft_png_info", "png_info":
		if len(argTypes) == 3 {
			// 3-arg pointer form: os_png_info(path, &w, &h) -> bool
			phTmp := c.allocTemp(ScalarInt)
			pwTmp := c.allocTemp(ScalarInt)
			c.emitStoreVar(phTmp)
			c.emitStoreVar(pwTmp)
			c.emitOp("SYSCALL", "SYS_PNG_INFO")
			hTmp := c.allocTemp(ScalarInt)
			wTmp := c.allocTemp(ScalarInt)
			c.emitStoreVar(hTmp)
			c.emitStoreVar(wTmp)
			c.emitLoadVar(pwTmp)
			c.emitLoadVar(wTmp)
			c.emitOp("POKE4")
			c.emitLoadVar(phTmp)
			c.emitLoadVar(hTmp)
			c.emitOp("POKE4")
			return ScalarInt, nil
		}
		c.emitOp("SYSCALL", "SYS_PNG_INFO")
		return TypeDesc{Kind: "scalar", StructName: "__multi_3", ByteSize: 4}, nil
	case "tft_draw_jpg_file", "os_draw_jpg_file", "os_tft_draw_jpg_file":
		if len(argTypes) == 3 {
			c.emitOp("PUSH_0") // default scale = 0 (1:1)
		}
		c.emitOp("SYSCALL", "SYS_TFT_DRAW_JPG_FILE")
		return ScalarInt, nil
	case "tft_draw_jpg", "os_draw_jpg", "tft_draw_jpg_mem", "os_draw_jpg_mem", "os_tft_draw_jpg":
		if len(argTypes) >= 3 && argTypes[0].Kind == "slice" {
			if argTypes[0].ScalarType == "string" {
				if len(argTypes) == 3 {
					c.emitOp("PUSH_0")
				}
				c.emitOp("SYSCALL", "SYS_TFT_DRAW_JPG_FILE")
				return ScalarInt, nil
			}
			// Slice form: tft_draw_jpg(jpgBytesSlice, x, y[, scale])
			scaleTmp := c.allocTemp(ScalarInt)
			if len(argTypes) == 4 {
				c.emitStoreVar(scaleTmp)
			} else {
				c.emitOp("PUSH_0")
				c.emitStoreVar(scaleTmp)
			}
			yTmp := c.allocTemp(ScalarInt)
			xTmp := c.allocTemp(ScalarInt)
			sTmp := c.allocTemp(argTypes[0])
			c.emitStoreVar(yTmp)
			c.emitStoreVar(xTmp)
			c.emitStoreVar(sTmp)
			c.emitLoadVar(sTmp)
			c.emitOp("SLICE_DATA")
			c.emitLoadVar(sTmp)
			c.emitOp("SLICE_LEN")
			c.emitLoadVar(xTmp)
			c.emitLoadVar(yTmp)
			c.emitLoadVar(scaleTmp)
		} else if len(argTypes) == 4 {
			// 4-arg raw buffer form: tft_draw_jpg(bufAddr, len, x, y) -> default scale = 0
			c.emitOp("PUSH_0")
		}
		c.emitOp("SYSCALL", "SYS_TFT_DRAW_JPG_MEM")
		return ScalarInt, nil
	case "os_jpg_info", "tft_jpg_info", "jpg_info":
		if len(argTypes) == 3 {
			// 3-arg pointer form: os_jpg_info(path, &w, &h) -> bool
			phTmp := c.allocTemp(ScalarInt)
			pwTmp := c.allocTemp(ScalarInt)
			c.emitStoreVar(phTmp)
			c.emitStoreVar(pwTmp)
			c.emitOp("SYSCALL", "SYS_JPG_INFO")
			hTmp := c.allocTemp(ScalarInt)
			wTmp := c.allocTemp(ScalarInt)
			c.emitStoreVar(hTmp)
			c.emitStoreVar(wTmp)
			c.emitLoadVar(pwTmp)
			c.emitLoadVar(wTmp)
			c.emitOp("POKE4")
			c.emitLoadVar(phTmp)
			c.emitLoadVar(hTmp)
			c.emitOp("POKE4")
			return ScalarInt, nil
		}
		c.emitOp("SYSCALL", "SYS_JPG_INFO")
		return TypeDesc{Kind: "scalar", StructName: "__multi_3", ByteSize: 4}, nil
	case "os_prefetch_start", "prefetch_start":
		c.emitOp("SYSCALL", "SYS_PREFETCH_START")
		return ScalarInt, nil
	case "os_prefetch_status", "prefetch_status":
		if len(argTypes) == 1 {
			// 1-arg pointer form: os_prefetch_status(&bytesGot) -> status
			pGotTmp := c.allocTemp(ScalarInt)
			c.emitStoreVar(pGotTmp)
			c.emitOp("SYSCALL", "SYS_PREFETCH_STATUS")
			gotTmp := c.allocTemp(ScalarInt)
			c.emitStoreVar(gotTmp)
			c.emitLoadVar(pGotTmp)
			c.emitLoadVar(gotTmp)
			c.emitOp("POKE4")
			return ScalarInt, nil
		}
		c.emitOp("SYSCALL", "SYS_PREFETCH_STATUS")
		return TypeDesc{Kind: "scalar", StructName: "__multi_2", ByteSize: 4}, nil
	case "os_prefetch_cancel", "prefetch_cancel":
		c.emitOp("SYSCALL", "SYS_PREFETCH_CANCEL")
		return VoidType, nil

	// Touch, Keyboard, LED & Sensors
	case "os_read_touch", "read_touch", "os_poll_touch", "poll_touch":
		if len(argTypes) == 2 {
			// 2-arg pointer form: os_poll_touch(&tx, &ty) -> bool
			pyTmp := c.allocTemp(ScalarInt)
			pxTmp := c.allocTemp(ScalarInt)
			c.emitStoreVar(pyTmp)
			c.emitStoreVar(pxTmp)
			c.emitOp("SYSCALL", "SYS_READ_TOUCH")
			tyTmp := c.allocTemp(ScalarInt)
			txTmp := c.allocTemp(ScalarInt)
			c.emitStoreVar(tyTmp)
			c.emitStoreVar(txTmp)
			c.emitLoadVar(pxTmp)
			c.emitLoadVar(txTmp)
			c.emitOp("POKE4")
			c.emitLoadVar(pyTmp)
			c.emitLoadVar(tyTmp)
			c.emitOp("POKE4")
			return ScalarInt, nil
		}
		c.emitOp("SYSCALL", "SYS_READ_TOUCH")
		return TypeDesc{Kind: "scalar", StructName: "__multi_3", ByteSize: 4}, nil
	case "os_read_click", "read_click", "os_poll_click", "poll_click":
		if len(argTypes) == 2 {
			// 2-arg pointer form: os_poll_click(&tx, &ty) -> bool
			pyTmp := c.allocTemp(ScalarInt)
			pxTmp := c.allocTemp(ScalarInt)
			c.emitStoreVar(pyTmp)
			c.emitStoreVar(pxTmp)
			c.emitOp("SYSCALL", "SYS_READ_CLICK")
			tyTmp := c.allocTemp(ScalarInt)
			txTmp := c.allocTemp(ScalarInt)
			c.emitStoreVar(tyTmp)
			c.emitStoreVar(txTmp)
			c.emitLoadVar(pxTmp)
			c.emitLoadVar(txTmp)
			c.emitOp("POKE4")
			c.emitLoadVar(pyTmp)
			c.emitLoadVar(tyTmp)
			c.emitOp("POKE4")
			return ScalarInt, nil
		}
		c.emitOp("SYSCALL", "SYS_READ_CLICK")
		return TypeDesc{Kind: "scalar", StructName: "__multi_3", ByteSize: 4}, nil
	case "os_read_touch_raw", "read_touch_raw":
		c.emitOp("SYSCALL", "SYS_READ_TOUCH_RAW")
		return TypeDesc{Kind: "scalar", StructName: "__multi_4", ByteSize: 4}, nil
	case "os_prompt_keyboard", "prompt_keyboard":
		c.emitOp("SYSCALL", "SYS_PROMPT_KEYBOARD")
		return ScalarInt, nil
	case "os_set_rgb_led", "set_rgb_led":
		c.emitOp("SYSCALL", "SYS_SET_RGB_LED")
		return VoidType, nil
	case "os_read_ldr", "read_ldr":
		c.emitOp("SYSCALL", "SYS_READ_LDR")
		return ScalarInt, nil
	case "os_read_cpu_temp", "read_cpu_temp":
		c.emitOp("SYSCALL", "SYS_READ_CPU_TEMP")
		return ScalarInt, nil

	// VFS (/flash, /sd, /net), Wi-Fi & Serial
	case "fs_ls", "os_fs_ls":
		c.emitOp("SYSCALL", "SYS_FS_LS")
		return ScalarInt, nil
	case "fs_stat", "os_fs_stat":
		c.emitOp("SYSCALL", "SYS_FS_STAT")
		return ScalarInt, nil
	case "fs_read", "os_fs_read":
		c.emitOp("SYSCALL", "SYS_FS_READ")
		return ScalarInt, nil
	case "fs_write", "os_fs_write":
		c.emitOp("SYSCALL", "SYS_FS_WRITE")
		return ScalarInt, nil
	case "fs_write_at", "os_fs_write_at":
		c.emitOp("SYSCALL", "SYS_FS_WRITE_AT")
		return ScalarInt, nil
	case "fs_mkdir", "os_fs_mkdir":
		c.emitOp("SYSCALL", "SYS_FS_MKDIR")
		return ScalarInt, nil
	case "fs_rmdir", "os_fs_rmdir":
		c.emitOp("SYSCALL", "SYS_FS_RMDIR")
		return ScalarInt, nil
	case "fs_rm", "os_fs_rm":
		c.emitOp("SYSCALL", "SYS_FS_RM")
		return ScalarInt, nil
	case "fs_du", "os_fs_du":
		c.emitOp("SYSCALL", "SYS_FS_DU")
		return ScalarInt, nil
	case "os_wifi_connected", "wifi_is_connected":
		c.emitOp("SYSCALL", "SYS_WIFI_CONNECTED")
		return ScalarInt, nil
	case "os_http_get", "http_get", "os_fetch", "fetch":
		if len(argTypes) == 2 && argTypes[1].Kind == "slice" {
			sTmp := c.allocTemp(argTypes[1])
			c.emitStoreVar(sTmp)
			c.emitLoadVar(sTmp)
			c.emitOp("SLICE_DATA")
			c.emitLoadVar(sTmp)
			c.emitOp("SLICE_LEN")
		}
		c.emitOp("SYSCALL", "SYS_HTTP_GET")
		return ScalarInt, nil
	case "os_fetch_with_cache", "fetch_with_cache", "os_http_get_cached", "http_get_cached":
		if len(argTypes) == 4 && argTypes[1].ScalarType != "word" && argTypes[2].ScalarType == "word" {
			// Reorder (url, maxAgeSec, bufPtr, maxLen) -> (url, bufPtr, maxLen, maxAgeSec)
			c.emitOp("ROT")
		} else if len(argTypes) == 3 && argTypes[1].Kind == "slice" {
			// 3-arg slice form: (url, bufSlice, maxAgeSec) -> (url, bufPtr, maxLen, maxAgeSec)
			ageTmp := c.allocTemp(ScalarInt)
			sTmp := c.allocTemp(argTypes[1])
			c.emitStoreVar(ageTmp)
			c.emitStoreVar(sTmp)
			c.emitLoadVar(sTmp)
			c.emitOp("SLICE_DATA")
			c.emitLoadVar(sTmp)
			c.emitOp("SLICE_LEN")
			c.emitLoadVar(ageTmp)
		} else if len(argTypes) == 3 && argTypes[2].Kind == "slice" {
			// 3-arg slice form: (url, maxAgeSec, bufSlice) -> (url, bufPtr, maxLen, maxAgeSec)
			sTmp := c.allocTemp(argTypes[2])
			ageTmp := c.allocTemp(ScalarInt)
			c.emitStoreVar(sTmp)
			c.emitStoreVar(ageTmp)
			c.emitLoadVar(sTmp)
			c.emitOp("SLICE_DATA")
			c.emitLoadVar(sTmp)
			c.emitOp("SLICE_LEN")
			c.emitLoadVar(ageTmp)
		}
		c.emitOp("SYSCALL", "SYS_HTTP_GET_CACHED")
		return ScalarInt, nil
	case "os_serial_available", "serial_available":
		c.emitOp("SYSCALL", "SYS_SERIAL_AVAIL")
		return ScalarInt, nil
	case "os_serial_read", "serial_read":
		c.emitOp("SYSCALL", "SYS_SERIAL_READ")
		return ScalarInt, nil
	case "os_serial_write", "serial_write":
		c.emitOp("SYSCALL", "SYS_SERIAL_WRITE")
		return ScalarInt, nil
	}

	return TypeDesc{}, fmt.Errorf("line %d: unknown function or syscall %q", line, name)
}
