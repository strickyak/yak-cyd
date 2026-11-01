package lcode

import (
	"bytes"
	"strings"
	"testing"
)

func compileAndRun(t *testing.T, src string, maxSteps int) (*VM, string) {
	t.Helper()
	lapp, asmText, err := CompileToLapp("TestApp", src)
	if err != nil {
		t.Fatalf("CompileToLapp failed: %v\nASM:\n%s", err, asmText)
	}
	vm, err := NewVM(lapp)
	if err != nil {
		t.Fatalf("NewVM failed: %v", err)
	}
	vm.MaxInstructions = maxSteps
	var out bytes.Buffer
	vm.Stdout = &out
	if _, err := vm.Run(); err != nil {
		t.Fatalf("VM Run failed at PC=%d: %v\nASM:\n%s", vm.PC, err, asmText)
	}
	return vm, out.String()
}

func TestArithmeticAndRecursion(t *testing.T) {
	src := `
package main

const BASE = 100

func fib(n int) int {
	if n <= 1 {
		return n
	}
	return fib(n - 1) + fib(n - 2)
}

func main() {
	x := fib(10) + BASE
	println(x)
}
`
	_, out := compileAndRun(t, src, 100000)
	if strings.TrimSpace(out) != "155" {
		t.Fatalf("expected 155, got %q", out)
	}
}

func TestStructsArraysAndPointers(t *testing.T) {
	src := `
package main

type Vec2 struct {
	X int
	Y int
}

var grid [4][4]byte
var pts [3]Vec2

func bump(p *Vec2, dx int, dy int) {
	p.X += dx
	p.Y += dy
}

func main() {
	for r := 0; r < 4; r++ {
		for c := 0; c < 4; c++ {
			grid[r][c] = byte(r*10 + c)
		}
	}
	pts[1].X = 10
	pts[1].Y = 20
	bump(&pts[1], 5, -3)
	println(int(grid[2][3]), pts[1].X, pts[1].Y)
}
`
	_, out := compileAndRun(t, src, 100000)
	if strings.TrimSpace(out) != "23 15 17" {
		t.Fatalf("expected '23 15 17', got %q", out)
	}
}

func TestHeapMallocFreeCoalesce(t *testing.T) {
	src := `
package main

type Node struct {
	Val int
	Next *Node
}

func main() {
	free0 := heap_free()
	a := new(Node)
	b := new(Node)
	a.Val = 42
	b.Val = 99
	a.Next = b
	sum := a.Val + a.Next.Val

	s := make([]int, 16)
	for i := 0; i < len(s); i++ {
		s[i] = i * 2
	}
	sum += s[15] + len(s)

	delete(s)
	delete(b)
	delete(a)
	free1 := heap_free()
	println(sum, free1 - free0)
}
`
	_, out := compileAndRun(t, src, 100000)
	// 42 + 99 + 30 + 16 = 187, and all heap blocks coalesced back so free1 - free0 == 0
	if strings.TrimSpace(out) != "187 0" {
		t.Fatalf("expected '187 0', got %q", out)
	}
}

func TestAsmDisasmRoundTrip(t *testing.T) {
	src := `
package main
func main() {
	tft_fill_screen(rgb565(10, 20, 30))
	tft_draw_string(12, 16, "Hello Lakatos", 0xFFFF, 0x0000, 2)
}
`
	asm1, err := CompileToAsm(src)
	if err != nil {
		t.Fatalf("CompileToAsm: %v", err)
	}
	lcod1, err := Assemble(asm1)
	if err != nil {
		t.Fatalf("Assemble 1: %v", err)
	}
	lapp1 := WrapLapp(lcod1, "RoundTrip")
	dis, err := Disassemble(lapp1)
	if err != nil {
		t.Fatalf("Disassemble: %v", err)
	}
	lcod2, err := Assemble(dis)
	if err != nil {
		t.Fatalf("Assemble 2 from disassembled output: %v\nDIS:\n%s", err, dis)
	}
	if !bytes.Equal(lcod1, lcod2) {
		t.Fatalf("LCOD payload mismatch after disasm->asm round-trip: %d vs %d bytes", len(lcod1), len(lcod2))
	}
}

func TestSyscallsAndTouch(t *testing.T) {
	src := `
package main
func main() {
	var tx int
	var ty int
	tft_fill_screen(COLOR_BLACK)
	if os_poll_touch(&tx, &ty) {
		tft_fill_rect(tx, ty, 10, 10, COLOR_GREEN)
		println(os_get_airport(), tx, ty)
	}
}
`
	lapp, _, err := CompileToLapp("TouchTest", src)
	if err != nil {
		t.Fatalf("CompileToLapp: %v", err)
	}
	vm, err := NewVM(lapp)
	if err != nil {
		t.Fatalf("NewVM: %v", err)
	}
	vm.MaxInstructions = 50000
	var out bytes.Buffer
	vm.Stdout = &out
	vm.TouchQueue = append(vm.TouchQueue, TouchEvent{Touched: true, X: 120, Y: 80})
	if _, err := vm.Run(); err != nil {
		t.Fatalf("Run: %v", err)
	}
	if strings.TrimSpace(out.String()) != "PDX 120 80" {
		t.Fatalf("expected 'PDX 120 80', got %q", out.String())
	}
	if vm.Screen[85][125] != 0x07E0 {
		t.Fatalf("expected pixel (125,85) to be 0x07E0, got 0x%04X", vm.Screen[85][125])
	}
}

func TestFloat32ArithmeticAndTrig(t *testing.T) {
	src := `
package main

const GRAVITY = 9.81

type Body struct {
	X  float32
	Y  float32
	VX float32
	VY float32
}

func hypot(dx float32, dy float32) float32 {
	return sqrt(dx*dx + dy*dy)
}

func main() {
	var b Body
	b.X = 3.0
	b.Y = 4.0
	d := hypot(b.X, b.Y)

	// Trig identity: sin^2(x) + cos^2(x) == 1.0
	ang := PI / 3.0
	s := sin(ang)
	c := cos(ang)
	ident := round((s*s + c*c) * 1000.0)

	// Mixed int & float32 expressions and casts
	var step float32 = 0.25
	step += 2
	asInt := int(d*10.0 + step*4.0)

	// Format float with 2 decimals
	txt := ftoa(GRAVITY, 2)
	tft_draw_float(d, 1, 10, 10, 2)
	println(int(d), int(ident), asInt, txt, int(floor(3.9)), int(ceil(3.1)), int(abs(-7.5)*2.0))
}
`
	_, out := compileAndRun(t, src, 100000)
	// d = 5.0 -> 5
	// ident = 1000.0 -> 1000
	// step = 2.25 -> d*10 + step*4 = 50 + 9 = 59
	// txt = "9.81"
	// floor(3.9) = 3, ceil(3.1) = 4, abs(-7.5)*2.0 = 15
	if strings.TrimSpace(out) != "5 1000 59 9.81 3 4 15" {
		t.Fatalf("expected '5 1000 59 9.81 3 4 15', got %q", out)
	}
}
