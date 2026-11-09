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

func TestAllEightNewAppsCompileAndRun(t *testing.T) {
	apps := []struct {
		name  string
		path  string
		title string
		demo  bool
	}{
		{"tetris", "../../apps/tetris/tetris.lgolf", "Tetris", false},
		{"galaga", "../../apps/galaga/galaga.lgolf", "Galaga", false},
		{"defender", "../../apps/defender/defender.lgolf", "Defender", false},
		{"bounce", "../../apps/bounce/bounce.lgolf", "Bouncing Ball", true},
		{"rubik", "../../apps/rubik/rubik.lgolf", "Rubik's Cube", true},
		{"ising", "../../apps/ising/ising.lgolf", "Ising Spin", true},
		{"puzzle15", "../../apps/puzzle15/puzzle15.lgolf", "15-Puzzle", true},
		{"dendrite", "../../apps/dendrite/dendrite.lgolf", "Dendrite DLA", true},
		{"paint", "../../apps/paint/paint.lgolf", "Paint", false},
		{"dice-3d", "../../apps/dice-3d/dice-3d.lgolf", "3D Dice Roller", false},
		{"croquet", "../../apps/croquet/croquet.lgolf", "Croquet 3D World", false},
	}

	for _, tc := range apps {
		t.Run(tc.name, func(t *testing.T) {
			srcBytes, err := os.ReadFile(tc.path)
			if err != nil {
				t.Fatalf("ReadFile(%s): %v", tc.path, err)
			}
			lapp, asmText, err := CompileToLapp(tc.title, string(srcBytes))
			if err != nil {
				t.Fatalf("CompileToLapp(%s): %v\nASM:\n%s", tc.name, err, asmText)
			}
			vm, err := NewVM(lapp)
			if err != nil {
				t.Fatalf("NewVM(%s): %v", tc.name, err)
			}
			vm.MaxInstructions = 400000
			vm.TouchQueue = append(vm.TouchQueue, TouchEvent{Touched: true, X: 5, Y: 5})
			if _, err := vm.Run(); err != nil {
				t.Fatalf("VM Run(%s): %v", tc.name, err)
			}
			if vm.Running {
				t.Fatalf("%s did not exit on top-left corner wedge tap (5,5) within 400k steps", tc.name)
			}
			if vm.ExitCode != 0 {
				t.Fatalf("%s exited with non-zero code %d", tc.name, vm.ExitCode)
			}
			// Verify the 12x12 top-left exit X mark was drawn via DrawExitWedge() from imported "wedge" library
			if vm.Screen[0][0] != 0xFFFF || vm.Screen[0][11] != 0xFFFF || vm.Screen[11][0] != 0xFFFF || vm.Screen[11][11] != 0xFFFF || vm.Screen[0][5] != 0x0000 {
				t.Fatalf("%s did not draw expected 12x12 exit X mark: (0,0)=0x%04X (11,0)=0x%04X (0,11)=0x%04X (11,11)=0x%04X (5,0)=0x%04X",
					tc.name, vm.Screen[0][0], vm.Screen[0][11], vm.Screen[11][0], vm.Screen[11][11], vm.Screen[0][5])
			}
			litPixels := 0
			for y := 25; y < 240; y++ {
				for x := 0; x < 320; x++ {
					if vm.Screen[y][x] != 0 {
						litPixels++
					}
				}
			}
			if litPixels < 100 {
				t.Fatalf("%s rendered too few pixels below header: %d", tc.name, litPixels)
			}
		})
	}
}

func TestPaintKebabSavePngPal4(t *testing.T) {
	srcBytes, err := os.ReadFile("../../apps/paint/paint.lgolf")
	if err != nil {
		t.Fatalf("ReadFile(paint.lgolf): %v", err)
	}
	lapp, asmText, err := CompileToLapp("Paint", string(srcBytes))
	if err != nil {
		t.Fatalf("CompileToLapp: %v\nASM:\n%s", err, asmText)
	}
	vm, err := NewVM(lapp)
	if err != nil {
		t.Fatalf("NewVM: %v", err)
	}
	tmpDir := t.TempDir()
	vm.VfsRoot = tmpDir
	vm.MaxInstructions = 400000

	// 1. Draw a stroke on the canvas at (100, 80) -> (120, 80) with default Cyan (swatch 8)
	// 2. Tap the Kebab [...] button at (9, 228)
	// 3. Tap [PALETTE] at (160, 165) to open the 16-Color RGB Palette Editor
	// 4. Drag Red slider to max at (264, 106), turning swatch 8 from Cyan (0,63,31) to White (31,63,31)
	// 5. Tap [APPLY & BACK] at (220, 200)
	// 6. Tap [SAVE PNG TO /net/paintings] at (160, 120)
	// 7. Tap [RESUME] at (250, 165)
	// 8. Tap top-left corner exit wedge at (5, 5)
	vm.TouchQueue = []TouchEvent{
		{Touched: true, X: 100, Y: 80},
		{Touched: true, X: 110, Y: 80},
		{Touched: true, X: 120, Y: 80},
		{Touched: false},
		{Touched: true, X: 9, Y: 228},
		{Touched: false},
		{Touched: true, X: 160, Y: 165},
		{Touched: false},
		{Touched: true, X: 264, Y: 106},
		{Touched: false},
		{Touched: true, X: 220, Y: 200},
		{Touched: false},
		{Touched: true, X: 160, Y: 120},
		{Touched: false},
		{Touched: true, X: 250, Y: 165},
		{Touched: false},
		{Touched: true, X: 5, Y: 5},
	}

	if _, err := vm.Run(); err != nil {
		t.Fatalf("VM Run: %v", err)
	}
	if vm.Running {
		t.Fatalf("paint did not exit cleanly")
	}

	// Verify /net/paintings/painting.png was created and is a valid 300x240 PNG
	savedFile := filepath.Join(tmpDir, "net", "paintings", "painting.png")
	pngData, err := os.ReadFile(savedFile)
	if err != nil {
		t.Fatalf("expected saved PNG at %s: %v", savedFile, err)
	}
	img, err := png.Decode(bytes.NewReader(pngData))
	if err != nil {
		t.Fatalf("png.Decode(%s, %d bytes) failed: %v", savedFile, len(pngData), err)
	}
	if img.Bounds().Dx() != 300 || img.Bounds().Dy() != 240 {
		t.Fatalf("expected 300x240 PNG, got %dx%d", img.Bounds().Dx(), img.Bounds().Dy())
	}
	// Verify stroke at canvas (80, 80) was recolored to White (R=255, G=255, B=255) by the Palette Editor
	r, g, b, _ := img.At(80, 80).RGBA()
	if r>>8 != 255 || g>>8 != 255 || b>>8 != 255 {
		t.Fatalf("expected recolored White pixel at canvas (80,80), got RGBA=(%d,%d,%d)", r>>8, g>>8, b>>8)
	}
	// Verify canvas pixel on VM screen at (100, 80) was restored with the new palette color (0xFFFF)
	if vm.Screen[80][100] != 0xFFFF {
		t.Fatalf("expected restored screen pixel (100,80)=0xFFFF after modal resume, got 0x%04X", vm.Screen[80][100])
	}
}

func TestCroquetWebSocketMultiUser(t *testing.T) {
	receivedPose := make(chan string, 8)

	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		hj, ok := w.(http.Hijacker)
		if !ok {
			http.Error(w, "no hijack", http.StatusInternalServerError)
			return
		}
		conn, rw, err := hj.Hijack()
		if err != nil {
			return
		}
		defer conn.Close()

		_, _ = rw.WriteString("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n")
		_ = rw.Flush()

		writeWsText := func(msg string) {
			data := []byte(msg)
			_ = rw.WriteByte(0x81)
			_ = rw.WriteByte(byte(len(data)))
			_, _ = rw.Write(data)
			_ = rw.Flush()
		}

		// Send initial YOU + a remote peer STATE standing near the front door
		writeWsText("YOU cyd-local\n")
		writeWsText("STATE cyd-peer -20 -120 0 3141 15\n")

		// Read client frames
		for {
			hdr := make([]byte, 2)
			if _, err := conn.Read(hdr); err != nil {
				return
			}
			opcode := hdr[0] & 0x0F
			masked := (hdr[1] & 0x80) != 0
			pLen := int(hdr[1] & 0x7F)
			var mask [4]byte
			if masked {
				if _, err := conn.Read(mask[:]); err != nil {
					return
				}
			}
			payload := make([]byte, pLen)
			if pLen > 0 {
				if _, err := conn.Read(payload); err != nil {
					return
				}
				if masked {
					for i := 0; i < pLen; i++ {
						payload[i] ^= mask[i&3]
					}
				}
			}
			if opcode == 0x8 {
				return
			}
			if opcode == 0x1 {
				select {
				case receivedPose <- string(payload):
				default:
				}
			}
		}
	}))
	defer srv.Close()

	srcBytes, err := os.ReadFile("../../apps/croquet/croquet.lgolf")
	if err != nil {
		t.Fatalf("ReadFile(croquet.lgolf): %v", err)
	}
	lapp, asmText, err := CompileToLapp("Croquet 3D World", string(srcBytes))
	if err != nil {
		t.Fatalf("CompileToLapp: %v\nASM:\n%s", err, asmText)
	}
	vm, err := NewVM(lapp)
	if err != nil {
		t.Fatalf("NewVM: %v", err)
	}
	vm.ShUrl = srv.URL
	vm.MaxInstructions = 900000

	// Walk forward several steps ([ ^ WALK ] at x=125, y=220), turn right ([ RIGHT > ] at x=280, y=220), then exit
	vm.TouchQueue = []TouchEvent{
		{Touched: true, X: 125, Y: 220},
		{Touched: true, X: 125, Y: 220},
		{Touched: true, X: 125, Y: 220},
		{Touched: true, X: 280, Y: 220},
		{Touched: false},
		{Touched: true, X: 5, Y: 5},
	}

	if _, err := vm.Run(); err != nil {
		t.Fatalf("VM Run: %v", err)
	}
	if vm.Running {
		t.Fatalf("croquet did not exit cleanly")
	}
	if dumpPath := os.Getenv("CROQUET_DUMP_PPM"); dumpPath != "" {
		_ = vm.SavePPM(dumpPath)
	}

	select {
	case msg := <-receivedPose:
		if !strings.HasPrefix(msg, "POSE ") {
			t.Fatalf("expected POSE message from croquet app, got %q", msg)
		}
	case <-time.After(2 * time.Second):
		t.Fatalf("timed out waiting for POSE message over WebSocket")
	}
}

func TestCompileAndRunBasicApp(t *testing.T) {
	srcBytes, err := os.ReadFile("../../apps/basic/basic.lgolf")
	if err != nil {
		t.Fatalf("ReadFile(basic.lgolf): %v", err)
	}
	lapp, asmText, err := CompileToLapp("basic", string(srcBytes))
	if err != nil {
		t.Fatalf("CompileToLapp: %v\nASM:\n%s", err, asmText)
	}

	tmpVfs := t.TempDir()

	// 1. Click existing line, edit via keyboard, SAVE to /sd/basic/CUSTOM.BAS, then RUN
	vm, err := NewVM(lapp)
	if err != nil {
		t.Fatalf("NewVM: %v", err)
	}
	vm.VfsRoot = tmpVfs
	vm.MaxInstructions = 2500000
	vm.TouchQueue = []TouchEvent{
		{Touched: false},
		// Click line 2 (20 CLS 0) at y=40 to open line editor modal
		{Touched: true, X: 100, Y: 40},
		{Touched: false, X: 100, Y: 40},
		// Click [OPEN FULL KEYBOARD (ABC/123)] at (100, 218)
		{Touched: true, X: 100, Y: 218},
		{Touched: false, X: 100, Y: 218},
		// Click SAVE in top bar at (290, 10)
		{Touched: true, X: 290, Y: 10},
		{Touched: false, X: 290, Y: 10},
		// Click RUN in top bar at (65, 10)
		{Touched: true, X: 65, Y: 10},
		{Touched: false, X: 65, Y: 10},
		// Exit via corner tap
		{Touched: true, X: 5, Y: 5},
	}
	vm.KeyboardQueue = []string{
		"25 POINT 160, 108, 2",
		"CUSTOM.BAS",
	}

	if _, err := vm.Run(); err != nil {
		t.Fatalf("VM Run: %v", err)
	}
	if vm.Running {
		t.Fatalf("basic app did not exit cleanly")
	}

	// Verify /sd/basic/CUSTOM.BAS was written with the edited line
	savedBytes, err := os.ReadFile(filepath.Join(tmpVfs, "sd/basic/CUSTOM.BAS"))
	if err != nil {
		t.Fatalf("expected /sd/basic/CUSTOM.BAS to exist: %v", err)
	}
	if !strings.Contains(string(savedBytes), "25 POINT 160, 108, 2") {
		t.Fatalf("expected edited line in /sd/basic/CUSTOM.BAS, got:\n%s", string(savedBytes))
	}

	// Verify POINT 160, 108, 2 (Red = 0xF800) drew at screen y = 24 + 108 = 132, x = 160
	if got := vm.Screen[132][160]; got != 0xF800 {
		t.Fatalf("expected POINT 160, 108, 2 to draw red (0xF800) at Screen[132][160], got 0x%04X", got)
	}
}
