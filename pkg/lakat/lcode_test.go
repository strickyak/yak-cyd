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
		// Click line 2 (20 CLS 0) at y=40 to open ASCII keyboard line editor directly
		{Touched: true, X: 100, Y: 40},
		{Touched: false, X: 100, Y: 40},
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

func TestCompileAndRunLogoApp(t *testing.T) {
	srcBytes, err := os.ReadFile("../../apps/logo/logo.lgolf")
	if err != nil {
		t.Fatalf("ReadFile(logo.lgolf): %v", err)
	}
	lapp, asmText, err := CompileToLapp("logo", string(srcBytes))
	if err != nil {
		t.Fatalf("CompileToLapp: %v\nASM:\n%s", err, asmText)
	}

	tmpVfs := t.TempDir()

	vm, err := NewVM(lapp)
	if err != nil {
		t.Fatalf("NewVM: %v", err)
	}
	vm.VfsRoot = tmpVfs
	vm.MaxInstructions = 2500000
	vm.TouchQueue = []TouchEvent{
		{Touched: false},
		// Click line 2 (CS) at y=40 to open ASCII keyboard line editor directly
		{Touched: true, X: 100, Y: 40},
		{Touched: false, X: 100, Y: 40},
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
		"CS PU SETXY -120 0 PD PC 2 DOT PU HOME PD",
		"CUSTOM.LGO",
	}

	if _, err := vm.Run(); err != nil {
		t.Fatalf("VM Run: %v", err)
	}
	if vm.Running {
		t.Fatalf("logo app did not exit cleanly")
	}

	// Verify /sd/logo/CUSTOM.LGO was written with the edited line
	savedBytes, err := os.ReadFile(filepath.Join(tmpVfs, "sd/logo/CUSTOM.LGO"))
	if err != nil {
		t.Fatalf("expected /sd/logo/CUSTOM.LGO to exist: %v", err)
	}
	if !strings.Contains(string(savedBytes), "CS PU SETXY -120 0 PD PC 2 DOT") {
		t.Fatalf("expected edited line in /sd/logo/CUSTOM.LGO, got:\n%s", string(savedBytes))
	}

	// Verify DOT at (-120, 0) with PC 2 (Red = 0xF800) drew at Screen[24+108][160-120] = Screen[132][40]
	if got := vm.Screen[132][40]; got != 0xF800 {
		t.Fatalf("expected PC 2 DOT at (-120, 0) to draw red (0xF800) at Screen[132][40], got 0x%04X", got)
	}
}

func TestChatWebSocketMultiUser(t *testing.T) {
	receivedLines := make(chan string, 16)
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/ws/chat" {
			http.NotFound(w, r)
			return
		}
		key := strings.TrimSpace(r.Header.Get("Sec-WebSocket-Key"))
		if key == "" {
			http.Error(w, "missing key", http.StatusBadRequest)
			return
		}
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

		sendFrame := func(txt string) {
			msg := []byte(txt)
			frame := append([]byte{0x81, byte(len(msg))}, msg...)
			_, _ = conn.Write(frame)
		}

		// Send initial greeting, peer presence, and an existing message from bob
		sendFrame("YOU alice")
		sendFrame("STATE bob online")
		sendFrame("MSG bob Welcome to Lakatos Chat!")

		var hdr [2]byte
		for {
			if _, err := conn.Read(hdr[:]); err != nil {
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
				line := strings.TrimSpace(string(payload))
				receivedLines <- line
				if strings.HasPrefix(line, "MSG ") {
					// Broadcast back to the room as Superhighway does
					sendFrame("MSG alice " + strings.TrimSpace(strings.TrimPrefix(line, "MSG ")))
				}
			}
		}
	}))
	defer srv.Close()

	srcBytes, err := os.ReadFile("../../apps/chat/chat.lgolf")
	if err != nil {
		t.Fatalf("ReadFile(chat.lgolf): %v", err)
	}
	lapp, asmText, err := CompileToLapp("chat", string(srcBytes))
	if err != nil {
		t.Fatalf("CompileToLapp: %v\nASM:\n%s", err, asmText)
	}

	vm, err := NewVM(lapp)
	if err != nil {
		t.Fatalf("NewVM: %v", err)
	}
	vm.ShUrl = srv.URL
	vm.MaxInstructions = 1500000
	vm.TouchQueue = []TouchEvent{
		{Touched: false},
		// Tap bottom compose bar at (160, 224) to open keyboard and send message
		{Touched: true, X: 160, Y: 224},
		{Touched: false, X: 160, Y: 224},
		// Let one loop iteration process the echoed MSG frame from server
		{Touched: false},
		// Exit via NW corner tap
		{Touched: true, X: 5, Y: 5},
	}
	vm.KeyboardQueue = []string{
		"Hello Bob from CYD!",
	}

	if _, err := vm.Run(); err != nil {
		t.Fatalf("VM Run: %v", err)
	}
	if vm.Running {
		t.Fatalf("chat app did not exit cleanly")
	}

	var sawOnline, sawMsg bool
	for len(receivedLines) > 0 {
		l := <-receivedLines
		if l == "SET online" {
			sawOnline = true
		}
		if l == "MSG Hello Bob from CYD!" {
			sawMsg = true
		}
	}
	if !sawOnline {
		t.Fatalf("expected 'SET online' from chat app")
	}
	if !sawMsg {
		t.Fatalf("expected 'MSG Hello Bob from CYD!' from chat app")
	}
}

func TestWedgeMenuAndCornerSizeConfig(t *testing.T) {
	src := `package main
import "wedge"

func main() {
	// Default corner size = 12
	if CornerSize() != 12 {
		panic("expected default CornerSize 12")
	}
	DrawExitWedge()
	DrawMenuWedge()
	if !IsMenuWedge(308, 0) || !IsMenuWedge(319, 11) || IsMenuWedge(307, 0) || IsMenuWedge(319, 12) {
		panic("IsMenuWedge failed for size 12")
	}
	if !IsExitWedge(0, 0) || !IsExitWedge(11, 11) || IsExitWedge(12, 0) {
		panic("IsExitWedge failed for size 12")
	}

	// Switch corner size to 24
	os_set_corner_size(24)
	if os_get_corner_size() != 24 || CornerSize() != 24 {
		panic("expected CornerSize 24")
	}
	tft_fill_screen(BLACK)
	DrawExitWedge()
	DrawMenuWedge()
	if !IsMenuWedge(296, 0) || !IsMenuWedge(319, 23) || IsMenuWedge(295, 0) || IsMenuWedge(319, 24) {
		panic("IsMenuWedge failed for size 24")
	}
}
`
	lapp, asmText, err := CompileToLapp("wedge-test", src)
	if err != nil {
		t.Fatalf("CompileToLapp: %v\nASM:\n%s", err, asmText)
	}
	vm, err := NewVM(lapp)
	if err != nil {
		t.Fatalf("NewVM: %v", err)
	}
	if _, err := vm.Run(); err != nil {
		t.Fatalf("VM Run: %v", err)
	}
	// Verify 24x24 Exit X in NW corner: (0,0), (23,23), (23,0), (0,23) are WHITE
	if vm.Screen[0][0] != 0xFFFF || vm.Screen[23][23] != 0xFFFF || vm.Screen[0][23] != 0xFFFF || vm.Screen[23][0] != 0xFFFF {
		t.Fatalf("24x24 Exit X not drawn properly")
	}
	// Verify 24x24 downward-pointing Menu Wedge in NE corner (x = 296..319):
	// Hypotenuse across top row y=0 (x=296..319 all WHITE), right angle pointing down at y=11 (x=307..308 WHITE, 306 BLACK)
	if vm.Screen[0][296] != 0xFFFF || vm.Screen[0][319] != 0xFFFF || vm.Screen[0][295] != 0x0000 {
		t.Fatalf("24x24 Menu Wedge top hypotenuse not drawn properly: (296,0)=0x%04X (319,0)=0x%04X", vm.Screen[0][296], vm.Screen[0][319])
	}
	if vm.Screen[11][307] != 0xFFFF || vm.Screen[11][308] != 0xFFFF || vm.Screen[11][306] != 0x0000 || vm.Screen[12][307] != 0x0000 {
		t.Fatalf("24x24 Menu Wedge downward tip not drawn properly at y=11")
	}
}

func TestGalleryAmbientModeHidesAndRestoresWedges(t *testing.T) {
	srcBytes, err := os.ReadFile("../../apps/gallery/gallery.lgolf")
	if err != nil {
		t.Fatalf("ReadFile(gallery.lgolf): %v", err)
	}
	lapp, asmText, err := CompileToLapp("Gallery Viewer", string(srcBytes))
	if err != nil {
		t.Fatalf("CompileToLapp: %v\nASM:\n%s", err, asmText)
	}

	tmpDir := t.TempDir()
	pixDir := filepath.Join(tmpDir, "net", "pix")
	if err := os.MkdirAll(pixDir, 0o755); err != nil {
		t.Fatalf("MkdirAll: %v", err)
	}

	// Create a valid 16x16 dark blue JPEG image in /net/pix/pic1.jpg
	img := image.NewRGBA(image.Rect(0, 0, 16, 16))
	var jpgBuf bytes.Buffer
	if err := jpeg.Encode(&jpgBuf, img, nil); err != nil {
		t.Fatalf("jpeg.Encode: %v", err)
	}
	if err := os.WriteFile(filepath.Join(pixDir, "pic1.jpg"), jpgBuf.Bytes(), 0o644); err != nil {
		t.Fatalf("WriteFile(pic1.jpg): %v", err)
	}

	vm, err := NewVM(lapp)
	if err != nil {
		t.Fatalf("NewVM: %v", err)
	}
	vm.VfsRoot = tmpDir
	vm.MaxInstructions = 4000000

	sawInitialWedges := false
	sawHiddenInAmbient := false
	sawRestoredAfterClick := false
	injectedWakeClick := false
	injectedExit := false

	vm.StepHook = func(v *VM, pc int, op byte) {
		// 1. Before 60s: both NW Exit X (0,0) and NE Menu Wedge (319,0) are visible
		if v.VirtualMs >= 2000 && v.VirtualMs < 50000 && !sawInitialWedges {
			if v.Screen[0][0] == 0xFFFF && v.Screen[0][319] == 0xFFFF {
				sawInitialWedges = true
			}
		}
		// 2. After 60s with no touch clicks: both NW Exit X and NE Menu Wedge are hidden
		if v.VirtualMs >= 62000 && !sawHiddenInAmbient {
			if v.Screen[0][0] == 0x0000 && v.Screen[0][319] == 0x0000 {
				sawHiddenInAmbient = true
				if !injectedWakeClick {
					injectedWakeClick = true
					// Inject a touch click at (200, 120) to wake wedges from ambient mode
					v.TouchQueue = append(v.TouchQueue, TouchEvent{Touched: true, X: 200, Y: 120})
				}
			}
		}
		// 3. After the touch click: both NW Exit X and NE Menu Wedge reappear
		if sawHiddenInAmbient && injectedWakeClick && !sawRestoredAfterClick {
			if v.Screen[0][0] == 0xFFFF && v.Screen[0][319] == 0xFFFF {
				sawRestoredAfterClick = true
				if !injectedExit {
					injectedExit = true
					v.SerialIn = append(v.SerialIn, 'q')
				}
			}
		}
	}

	if _, err := vm.Run(); err != nil {
		t.Fatalf("VM Run: %v", err)
	}
	if !sawInitialWedges {
		t.Fatalf("expected gallery to show both Exit X and Menu Wedge during first 60s")
	}
	if !sawHiddenInAmbient {
		t.Fatalf("expected gallery to hide both Exit X and Menu Wedge after 60s with no touch clicks")
	}
	if !sawRestoredAfterClick {
		t.Fatalf("expected gallery to re-show both Exit X and Menu Wedge after a touch click")
	}
}

func TestGalleryConfigPersistence(t *testing.T) {
	srcBytes, err := os.ReadFile("../../apps/gallery/gallery.lgolf")
	if err != nil {
		t.Fatalf("ReadFile(gallery.lgolf): %v", err)
	}
	lapp, asmText, err := CompileToLapp("Gallery Viewer", string(srcBytes))
	if err != nil {
		t.Fatalf("CompileToLapp: %v\nASM:\n%s", err, asmText)
	}

	tmpDir := t.TempDir()
	pixDir := filepath.Join(tmpDir, "net", "pix")
	if err := os.MkdirAll(pixDir, 0o755); err != nil {
		t.Fatalf("MkdirAll: %v", err)
	}

	img := image.NewRGBA(image.Rect(0, 0, 16, 16))
	var jpgBuf bytes.Buffer
	if err := jpeg.Encode(&jpgBuf, img, nil); err != nil {
		t.Fatalf("jpeg.Encode: %v", err)
	}
	if err := os.WriteFile(filepath.Join(pixDir, "pic1.jpg"), jpgBuf.Bytes(), 0o644); err != nil {
		t.Fatalf("WriteFile(pic1.jpg): %v", err)
	}

	vm, err := NewVM(lapp)
	if err != nil {
		t.Fatalf("NewVM: %v", err)
	}
	vm.VfsRoot = tmpDir
	vm.MaxInstructions = 4000000

	injectedFlow := false
	vm.StepHook = func(v *VM, pc int, op byte) {
		if v.VirtualMs >= 1000 && !injectedFlow {
			injectedFlow = true
			// 1. Click NE Menu Wedge (315, 4) -> opens 2-item menu (Shuffle, Config)
			// 2. Click "Config" item (itemIdx == 1 at y = 24 + 3 + 29 + 12 = 68)
			// 3. Touch slider stop 4 (300s at x = 235, y = 116) + release
			// 4. Touch [DONE] button (160, 178) + release
			v.TouchQueue = append(v.TouchQueue,
				TouchEvent{Touched: true, X: 315, Y: 4},
				TouchEvent{Touched: true, X: 260, Y: 68},
				TouchEvent{Touched: true, X: 235, Y: 116},
				TouchEvent{Touched: false},
				TouchEvent{Touched: true, X: 160, Y: 178},
				TouchEvent{Touched: false},
			)
		}
		if v.VirtualMs >= 3000 {
			v.SerialIn = append(v.SerialIn, 'q')
		}
	}

	if _, err := vm.Run(); err != nil {
		t.Fatalf("VM Run: %v", err)
	}

	cfgBytes, err := os.ReadFile(filepath.Join(tmpDir, "flash", "etc", "gallery.cfg"))
	if err != nil {
		t.Fatalf("expected /flash/etc/gallery.cfg to be written: %v", err)
	}
	if string(cfgBytes) != "300\n" {
		t.Fatalf("expected gallery.cfg to contain %q, got %q", "300\n", string(cfgBytes))
	}
}

func TestAppHeaderRequiredKernelVersion(t *testing.T) {
	src := `package main
func main() {
	os_exit(42)
}
`
	// 1. Default REQ = 0
	lapp0, _, err := CompileToLapp("test-app", src)
	if err != nil {
		t.Fatalf("CompileToLapp: %v", err)
	}
	if got := LappReqKernelSec(lapp0); got != 0 {
		t.Fatalf("expected req=0, got %d", got)
	}

	// 2. Explicit REQ = 1234567890
	const reqVer int64 = 1234567890
	lappReq, _, err := CompileToLappWithReq("test-app", reqVer, src)
	if err != nil {
		t.Fatalf("CompileToLappWithReq: %v", err)
	}
	if got := LappReqKernelSec(lappReq); got != reqVer {
		t.Fatalf("expected req=%d, got %d", reqVer, got)
	}

	// 3. Running with older kernel fails with NEEDS_KERNEL
	vmOld, err := NewVM(lappReq)
	if err != nil {
		t.Fatalf("NewVM: %v", err)
	}
	vmOld.KernelVersionSec = reqVer - 1
	if _, err := vmOld.Run(); err == nil || !strings.Contains(err.Error(), "NEEDS_KERNEL:1234567890") {
		t.Fatalf("expected NEEDS_KERNEL:1234567890 error, got %v", err)
	}

	// 4. Running with equal or newer kernel succeeds
	vmNew, err := NewVM(lappReq)
	if err != nil {
		t.Fatalf("NewVM: %v", err)
	}
	vmNew.KernelVersionSec = reqVer
	code, err := vmNew.Run()
	if err != nil || code != 42 {
		t.Fatalf("expected exit 42 with nil err, got code=%d err=%v", code, err)
	}

	metaTxt, err := FormatMeta("test-app.lapp", lappReq)
	if err != nil {
		t.Fatalf("FormatMeta: %v", err)
	}
	if !strings.Contains(metaTxt, `"test-app"`) || !strings.Contains(metaTxt, "1234567890") || !strings.Contains(metaTxt, "[OK]") {
		t.Fatalf("unexpected FormatMeta output:\n%s", metaTxt)
	}
}

func TestCalcAppEqualsAndRpnModes(t *testing.T) {
	srcBytes, err := os.ReadFile("../../apps/calc/calc.lgolf")
	if err != nil {
		t.Fatalf("ReadFile(calc.lgolf): %v", err)
	}
	lapp, asmText, err := CompileToLapp("calc", string(srcBytes))
	if err != nil {
		t.Fatalf("CompileToLapp: %v\nASM:\n%s", err, asmText)
	}

	tmpDir := t.TempDir()
	vm, err := NewVM(lapp)
	if err != nil {
		t.Fatalf("NewVM: %v", err)
	}
	vm.VfsRoot = tmpDir
	vm.MaxInstructions = 4000000

	btnTap := func(row, col int) TouchEvent {
		return TouchEvent{
			Touched: true,
			X:       4 + col*63 + 29,
			Y:       88 + row*30 + 13,
		}
	}

	// Default mode is EQUALS:
	// 1. Tap FN (0,0) -> SIN (0,1) -> 3 (3,3) -> 0 (4,1) -> = (4,3) => sin(30 deg) = 0.5
	// 2. Open NE Menu Wedge (315, 4) -> select "Style: RPN" (item 0 at x=250, y=38)
	// 3. In RPN mode: tap 3 (3,3) -> ENTER (4,3) -> 4 (2,1) -> + (4,4) => 7
	// 4. Tap FN (0,0) -> EXP (2,0) -> FN (0,0) -> LN (1,0) => 7
	vm.TouchQueue = append(vm.TouchQueue,
		btnTap(0, 0), // FN prefix
		btnTap(0, 1), // SIN
		btnTap(3, 3), // 3
		btnTap(4, 1), // 0
		btnTap(4, 3), // =
		TouchEvent{Touched: true, X: 315, Y: 4},  // NE Menu Wedge
		TouchEvent{Touched: true, X: 250, Y: 38}, // Style: RPN
		btnTap(3, 3), // 3
		btnTap(4, 3), // ENTER
		btnTap(2, 1), // 4
		btnTap(4, 4), // +
		btnTap(0, 0), // FN prefix
		btnTap(2, 0), // EXP (e^7)
		btnTap(0, 0), // FN prefix
		btnTap(1, 0), // LN (ln(e^7) = 7)
		TouchEvent{Touched: true, X: 2, Y: 2}, // NW Exit Wedge
	)

	if _, err := vm.Run(); err != nil {
		t.Fatalf("VM Run: %v", err)
	}

	cfgBytes, err := os.ReadFile(filepath.Join(tmpDir, "flash", "etc", "calc.cfg"))
	if err != nil {
		t.Fatalf("expected /flash/etc/calc.cfg to be persisted: %v", err)
	}
	if string(cfgBytes) != "RD\n" {
		t.Fatalf("expected calc.cfg to be %q (RPN + DEG), got %q", "RD\n", string(cfgBytes))
	}
}

func TestGraphCalculatorApp(t *testing.T) {
	srcBytes, err := os.ReadFile("../../apps/graph/graph.lgolf")
	if err != nil {
		t.Fatalf("ReadFile(graph.lgolf): %v", err)
	}
	lapp, asmText, err := CompileToLapp("graph", string(srcBytes))
	if err != nil {
		t.Fatalf("CompileToLapp: %v\nASM:\n%s", err, asmText)
	}

	vm, err := NewVM(lapp)
	if err != nil {
		t.Fatalf("NewVM: %v", err)
	}
	vm.MaxInstructions = 8000000

	vm.KeyboardQueue = []string{
		"2x^2 - 4*cos(x) + ln(e)",
		"-4",
		"4",
		"-4",
		"12",
	}
	vm.TouchQueue = []TouchEvent{
		{Touched: true, X: 24, Y: 226},  // [f(x)] button -> enters "2x^2 - 4*cos(x) + ln(e)"
		{Touched: true, X: 65, Y: 226},  // [+] Zoom In
		{Touched: true, X: 102, Y: 226}, // [-] Zoom Out
		{Touched: true, X: 138, Y: 226}, // [<] Pan Left
		{Touched: true, X: 173, Y: 226}, // [>] Pan Right
		{Touched: true, X: 208, Y: 226}, // [^] Pan Up
		{Touched: true, X: 243, Y: 226}, // [v] Pan Down
		{Touched: true, X: 290, Y: 226}, // [BOUNDS] open Corner Coordinates modal
		{Touched: true, X: 80, Y: 78},   // X Min -> "-4"
		{Touched: true, X: 220, Y: 78},  // X Max -> "4"
		{Touched: true, X: 80, Y: 128},  // Y Min -> "-4"
		{Touched: true, X: 220, Y: 128}, // Y Max -> "12"
		{Touched: true, X: 254, Y: 181}, // [DONE] close modal & redraw graph
		{Touched: true, X: 2, Y: 2},     // NW Exit Wedge
	}

	if _, err := vm.Run(); err != nil {
		t.Fatalf("VM Run: %v", err)
	}
	if vm.Running {
		t.Fatalf("graph app did not exit cleanly")
	}

	// Verify that yellow curve pixels (0xFFE0) were plotted across the graph viewport
	yellowPixels := 0
	for y := 26; y < 208; y++ {
		for x := 0; x < 320; x++ {
			if vm.Screen[y][x] == 0xFFE0 {
				yellowPixels++
			}
		}
	}
	if yellowPixels < 150 {
		t.Fatalf("expected plotted curve to have >= 150 yellow pixels in viewport, got %d", yellowPixels)
	}
}

func TestColorbarsSubApp(t *testing.T) {
	srcBytes, err := os.ReadFile("../../subapps/colorbars/colorbars.lgolf")
	if err != nil {
		t.Fatalf("ReadFile colorbars.lgolf: %v", err)
	}
	sapp, asmText, err := CompileToLapp("colorbars", string(srcBytes))
	if err != nil {
		t.Fatalf("CompileToLapp colorbars.lgolf: %v\nASM:\n%s", err, asmText)
	}
	vm, err := NewVM(sapp)
	if err != nil {
		t.Fatalf("NewVM: %v", err)
	}

	if _, err := vm.Run(); err != nil {
		t.Fatalf("VM Run: %v", err)
	}
	if vm.Running {
		t.Fatalf("colorbars sub-app did not exit cleanly after 2s")
	}

	// Verify top SMPTE bars (y = 80), middle castellations (y = 170), and bottom row (y = 210)
	if vm.Screen[80][20] != 0xBDF7 || vm.Screen[80][65] != 0xFFE0 || vm.Screen[80][295] != 0x001F {
		t.Fatalf("unexpected top bar colors: col0=0x%04X col1=0x%04X col6=0x%04X",
			vm.Screen[80][20], vm.Screen[80][65], vm.Screen[80][295])
	}
	if vm.Screen[170][20] != 0x001F || vm.Screen[170][110] != 0xF81F || vm.Screen[170][295] != 0xBDF7 {
		t.Fatalf("unexpected mid castellation colors: col0=0x%04X col2=0x%04X col6=0x%04X",
			vm.Screen[170][20], vm.Screen[170][110], vm.Screen[170][295])
	}
	if vm.Screen[210][80] != 0xFFFF || vm.Screen[210][265] != 0x18C3 {
		t.Fatalf("unexpected bottom row colors: white=0x%04X pluge+4=0x%04X",
			vm.Screen[210][80], vm.Screen[210][265])
	}
}

func TestGreenRainSubApp(t *testing.T) {
	srcBytes, err := os.ReadFile("../../subapps/green-rain/green-rain.lgolf")
	if err != nil {
		t.Fatalf("ReadFile green-rain.lgolf: %v", err)
	}
	sapp, asmText, err := CompileToLapp("green-rain", string(srcBytes))
	if err != nil {
		t.Fatalf("CompileToLapp green-rain.lgolf: %v\nASM:\n%s", err, asmText)
	}
	vm, err := NewVM(sapp)
	if err != nil {
		t.Fatalf("NewVM: %v", err)
	}
	vm.MaxInstructions = 8000000

	if _, err := vm.Run(); err != nil {
		t.Fatalf("VM Run: %v", err)
	}
	if vm.Running {
		t.Fatalf("green-rain sub-app did not exit cleanly after 3s")
	}

	// Verify that green-rain rendered glowing green glyph pixels across the screen
	greenPixels := 0
	for y := 0; y < 240; y++ {
		for x := 0; x < 320; x++ {
			if vm.Screen[y][x] != 0x0000 {
				greenPixels++
			}
		}
	}
	if greenPixels < 500 {
		t.Fatalf("expected >= 500 lit Matrix rain glyph pixels on screen, got %d", greenPixels)
	}
}



