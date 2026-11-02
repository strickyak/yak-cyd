package lakat

import (
	"bytes"
	"fmt"
	"image"
	"image/color"
	"image/jpeg"
	"image/png"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
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

func TestPngDecodeAndDraw(t *testing.T) {
	tmpDir := t.TempDir()
	flashDir := filepath.Join(tmpDir, "flash")
	if err := os.MkdirAll(flashDir, 0o755); err != nil {
		t.Fatalf("MkdirAll: %v", err)
	}

	// Create a 4x3 NRGBA PNG image:
	// - (0,0) opaque Red (255,0,0,255) -> RGB565 0xF800
	// - (1,0) transparent (0,255,0,0) -> should NOT overwrite background
	// - (2,1) opaque Blue (0,0,255,255) -> RGB565 0x001F
	img := image.NewNRGBA(image.Rect(0, 0, 4, 3))
	img.SetNRGBA(0, 0, color.NRGBA{R: 255, G: 0, B: 0, A: 255})
	img.SetNRGBA(1, 0, color.NRGBA{R: 0, G: 255, B: 0, A: 0})
	img.SetNRGBA(2, 1, color.NRGBA{R: 0, G: 0, B: 255, A: 255})

	var pngBuf bytes.Buffer
	if err := png.Encode(&pngBuf, img); err != nil {
		t.Fatalf("png.Encode: %v", err)
	}
	if err := os.WriteFile(filepath.Join(flashDir, "sprite.png"), pngBuf.Bytes(), 0o644); err != nil {
		t.Fatalf("WriteFile: %v", err)
	}

	src := `
package main

var rawBuf [512]byte

func main() {
	tft_fill_screen(0x07E0) // Green background
	ok, w, h := os_png_info("/flash/sprite.png")
	drawn1 := tft_draw_png_file("/flash/sprite.png", 10, 20)

	n := fs_read("/flash/sprite.png", 0, &rawBuf, 512)
	drawn2 := tft_draw_png(&rawBuf, n, 50, 60)

	println(ok, w, h, drawn1, drawn2)
}
`
	lapp, _, err := CompileToLapp("PngTest", src)
	if err != nil {
		t.Fatalf("CompileToLapp: %v", err)
	}
	vm, err := NewVM(lapp)
	if err != nil {
		t.Fatalf("NewVM: %v", err)
	}
	vm.VfsRoot = tmpDir
	vm.MaxInstructions = 100000
	var out bytes.Buffer
	vm.Stdout = &out
	if _, err := vm.Run(); err != nil {
		t.Fatalf("Run: %v", err)
	}
	if strings.TrimSpace(out.String()) != "1 4 3 1 1" {
		t.Fatalf("expected '1 4 3 1 1', got %q", out.String())
	}
	// Check tft_draw_png_file at (10, 20)
	if vm.Screen[20][10] != 0xF800 {
		t.Fatalf("expected pixel (10,20) to be red 0xF800, got 0x%04X", vm.Screen[20][10])
	}
	// Transparent pixel (11, 20) must preserve Green background 0x07E0
	if vm.Screen[20][11] != 0x07E0 {
		t.Fatalf("expected transparent pixel (11,20) to preserve green 0x07E0, got 0x%04X", vm.Screen[20][11])
	}
	if vm.Screen[21][12] != 0x001F {
		t.Fatalf("expected pixel (12,21) to be blue 0x001F, got 0x%04X", vm.Screen[21][12])
	}
	// Check tft_draw_png (from memory buffer) at (50, 60)
	if vm.Screen[60][50] != 0xF800 || vm.Screen[61][52] != 0x001F {
		t.Fatalf("unexpected memory PNG pixels: (50,60)=0x%04X (52,61)=0x%04X", vm.Screen[60][50], vm.Screen[61][52])
	}
}

func TestJpgDecodeAndDraw(t *testing.T) {
	tmpDir := t.TempDir()
	flashDir := filepath.Join(tmpDir, "flash")
	if err := os.MkdirAll(flashDir, 0o755); err != nil {
		t.Fatalf("MkdirAll: %v", err)
	}

	// Create a solid 16x16 white (255,255,255) JPEG image
	img := image.NewRGBA(image.Rect(0, 0, 16, 16))
	for y := 0; y < 16; y++ {
		for x := 0; x < 16; x++ {
			img.SetRGBA(x, y, color.RGBA{R: 255, G: 255, B: 255, A: 255})
		}
	}

	var jpgBuf bytes.Buffer
	if err := jpeg.Encode(&jpgBuf, img, &jpeg.Options{Quality: 100}); err != nil {
		t.Fatalf("jpeg.Encode: %v", err)
	}
	if err := os.WriteFile(filepath.Join(flashDir, "tile.jpg"), jpgBuf.Bytes(), 0o644); err != nil {
		t.Fatalf("WriteFile: %v", err)
	}

	src := `
package main

var rawBuf [2048]byte

func main() {
	tft_fill_screen(0x0000)
	ok, w, h := os_jpg_info("/flash/tile.jpg")
	drawn1 := tft_draw_jpg_file("/flash/tile.jpg", 10, 20, 0)

	n := fs_read("/flash/tile.jpg", 0, &rawBuf, 2048)
	drawn2 := tft_draw_jpg(&rawBuf, n, 50, 60, 1) // 1/2 scale (8x8)

	println(ok, w, h, drawn1, drawn2)
}
`
	lapp, _, err := CompileToLapp("JpgTest", src)
	if err != nil {
		t.Fatalf("CompileToLapp: %v", err)
	}
	vm, err := NewVM(lapp)
	if err != nil {
		t.Fatalf("NewVM: %v", err)
	}
	vm.VfsRoot = tmpDir
	vm.MaxInstructions = 100000
	var out bytes.Buffer
	vm.Stdout = &out
	if _, err := vm.Run(); err != nil {
		t.Fatalf("Run: %v", err)
	}
	if strings.TrimSpace(out.String()) != "1 16 16 1 1" {
		t.Fatalf("expected '1 16 16 1 1', got %q", out.String())
	}
	// Check 1:1 draw at (10,20) .. (25,35)
	if vm.Screen[20][10] != 0xFFFF || vm.Screen[35][25] != 0xFFFF {
		t.Fatalf("expected white 0xFFFF at (10,20) and (25,35), got 0x%04X and 0x%04X", vm.Screen[20][10], vm.Screen[35][25])
	}
	// Check 1:2 scaled draw at (50,60) .. (57,67), and (58,68) should remain black (0x0000)
	if vm.Screen[60][50] != 0xFFFF || vm.Screen[67][57] != 0xFFFF || vm.Screen[68][58] != 0x0000 {
		t.Fatalf("unexpected 1/2 scale pixels: (50,60)=0x%04X (57,67)=0x%04X (58,68)=0x%04X",
			vm.Screen[60][50], vm.Screen[67][57], vm.Screen[68][58])
	}
}

func TestPrefetchAndCenteredDraw(t *testing.T) {
	tmpDir := t.TempDir()
	flashDir := filepath.Join(tmpDir, "flash")
	if err := os.MkdirAll(flashDir, 0o755); err != nil {
		t.Fatalf("MkdirAll: %v", err)
	}

	img := image.NewNRGBA(image.Rect(0, 0, 10, 10))
	for y := 0; y < 10; y++ {
		for x := 0; x < 10; x++ {
			img.SetNRGBA(x, y, color.NRGBA{R: 255, G: 0, B: 0, A: 255})
		}
	}
	var pngBuf bytes.Buffer
	if err := png.Encode(&pngBuf, img); err != nil {
		t.Fatalf("png.Encode: %v", err)
	}
	if err := os.WriteFile(filepath.Join(flashDir, "box.png"), pngBuf.Bytes(), 0o644); err != nil {
		t.Fatalf("WriteFile: %v", err)
	}

	src := `
package main

var imgBuf [102400]byte

func main() {
	tft_fill_screen(0x0000)
	started := os_prefetch_start("/flash/box.png", &imgBuf, 102400)
	st, got := os_prefetch_status()
	drawn := tft_draw_png(&imgBuf, got, -1, 20)
	os_prefetch_cancel()
	stAfter, _ := os_prefetch_status()
	println(started, st, got > 0, drawn, stAfter)
}
`
	lapp, _, err := CompileToLapp("PrefetchTest", src)
	if err != nil {
		t.Fatalf("CompileToLapp: %v", err)
	}
	vm, err := NewVM(lapp)
	if err != nil {
		t.Fatalf("NewVM: %v", err)
	}
	vm.VfsRoot = tmpDir
	vm.MaxInstructions = 100000
	var out bytes.Buffer
	vm.Stdout = &out
	if _, err := vm.Run(); err != nil {
		t.Fatalf("Run: %v", err)
	}
	if strings.TrimSpace(out.String()) != "1 2 1 1 0" {
		t.Fatalf("expected '1 2 1 1 0', got %q", out.String())
	}
	// Centered 10x10 inside (0, 20, 320, 220) -> dstX = (320-10)/2 = 155, dstY = 20 + (220-10)/2 = 125
	if vm.Screen[125][155] != 0xF800 {
		t.Fatalf("expected centered pixel (155,125) to be red 0xF800, got 0x%04X", vm.Screen[125][155])
	}
}

func TestCityHash32(t *testing.T) {
	src := `
package main

var urlBuf [128]byte

func hashSlice(ptr *byte, n uint) uint {
	return os.CityHash32(ptr, n)
}

func main() {
	u := "https://aviationweather.gov/api/data/metar?ids=KSFO,KJFK,KLAX,KATL,KORD,KDEN,KSEA&format=json"
	for i := 0; i < u.Len; i++ {
		urlBuf[i] = byte(peek1(u.Base + i))
	}
	h1 := hashSlice(&urlBuf[0], uint(u.Len))
	h2 := os.CityHash32(u.Base, uint(u.Len))
	h3 := os.CityHash32(u)
	hShort := os.CityHash32("abc")
	hMed := os.CityHash32("hello world!")
	h18 := os.CityHash32("0123456789abcdefgh")
	println(h1 == h2, h2 == h3, int_to_str(h1), hShort != 0, hMed != 0, h18 != 0)
}
`
	_, out := compileAndRun(t, src, 100000)
	expectedHash := CityHash32([]byte("https://aviationweather.gov/api/data/metar?ids=KSFO,KJFK,KLAX,KATL,KORD,KDEN,KSEA&format=json"))
	want := fmt.Sprintf("1 1 %d 1 1 1", expectedHash)
	if strings.TrimSpace(out) != want {
		t.Fatalf("expected %q, got %q", want, strings.TrimSpace(out))
	}
}

func TestFetchWithCache(t *testing.T) {
	hits := 0
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		hits++
		fmt.Fprintf(w, "METAR-V%d", hits)
	}))
	defer srv.Close()

	tmpDir := t.TempDir()
	urlStr := srv.URL + "/metar?ids=KSFO"

	src := fmt.Sprintf(`
package main

var buf [256]byte

func main() {
	u := %q
	n1 := os.FetchWithCache(u, &buf[0], 256, 3600)
	s1 := make_slice(&buf[0], n1, n1)
	n2 := os.FetchWithCache(u, &buf[0], 256, 3600)
	s2 := make_slice(&buf[0], n2, n2)
	println(n1, s1, n2, s2)
}
`, urlStr)

	lapp, asmText, err := CompileToLapp("CacheTest", src)
	if err != nil {
		t.Fatalf("CompileToLapp: %v\nASM:\n%s", err, asmText)
	}
	vm, err := NewVM(lapp)
	if err != nil {
		t.Fatalf("NewVM: %v", err)
	}
	vm.VfsRoot = tmpDir
	vm.MaxInstructions = 100000
	var out bytes.Buffer
	vm.Stdout = &out
	if _, err := vm.Run(); err != nil {
		t.Fatalf("VM Run: %v", err)
	}
	if strings.TrimSpace(out.String()) != "8 METAR-V1 8 METAR-V1" {
		t.Fatalf("expected '8 METAR-V1 8 METAR-V1', got %q", out.String())
	}
	if hits != 1 {
		t.Fatalf("expected exactly 1 HTTP hit due to cache hit, got %d", hits)
	}

	// Verify cache file at /net/cache/%u has timestamp on line 1 and payload after
	hash := CityHash32([]byte(urlStr))
	cacheFile := filepath.Join(tmpDir, "net", "cache", fmt.Sprintf("%d", hash))
	cacheBytes, err := os.ReadFile(cacheFile)
	if err != nil {
		t.Fatalf("expected cache file %s to exist: %v", cacheFile, err)
	}
	parts := strings.SplitN(string(cacheBytes), "\n", 2)
	if len(parts) != 2 || parts[1] != "METAR-V1" {
		t.Fatalf("unexpected cache file contents: %q", string(cacheBytes))
	}

	// Backdate the cache file timestamp by 7200 seconds so maxAgeSec=3600 forces a re-fetch
	staleContent := fmt.Sprintf("%d\nOLD-DATA", uint32(time.Now().Unix()-7200))
	if err := os.WriteFile(cacheFile, []byte(staleContent), 0o644); err != nil {
		t.Fatalf("WriteFile stale: %v", err)
	}

	vm2, err := NewVM(lapp)
	if err != nil {
		t.Fatalf("NewVM 2: %v", err)
	}
	vm2.VfsRoot = tmpDir
	vm2.MaxInstructions = 100000
	out.Reset()
	vm2.Stdout = &out
	if _, err := vm2.Run(); err != nil {
		t.Fatalf("VM2 Run: %v", err)
	}
	if strings.TrimSpace(out.String()) != "8 METAR-V2 8 METAR-V2" {
		t.Fatalf("expected '8 METAR-V2 8 METAR-V2' after stale refresh, got %q", out.String())
	}
	if hits != 2 {
		t.Fatalf("expected 2 total HTTP hits after stale refresh, got %d", hits)
	}
}
