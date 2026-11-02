package main

import (
	"fmt"
	"image"
	"image/color"
	"image/gif"
	"image/jpeg"
	"image/png"
	"os"
	"path/filepath"
	"testing"
)

func TestComputeTargetDimensions(t *testing.T) {
	tests := []struct {
		srcW, srcH int
		wantW      int
		wantH      int
	}{
		{srcW: 1920, srcH: 1080, wantW: 320, wantH: 180}, // 16:9 wide
		{srcW: 1600, srcH: 1200, wantW: 320, wantH: 240}, // 4:3 exact
		{srcW: 320, srcH: 240, wantW: 320, wantH: 240},   // already 320x240
		{srcW: 160, srcH: 120, wantW: 320, wantH: 240},   // small 4:3 upscaled
		{srcW: 1000, srcH: 1000, wantW: 240, wantH: 240}, // 1:1 square
		{srcW: 64, srcH: 64, wantW: 240, wantH: 240},     // small square upscaled
		{srcW: 1080, srcH: 1920, wantW: 135, wantH: 240}, // 9:16 portrait
		{srcW: 400, srcH: 800, wantW: 120, wantH: 240},   // 1:2 portrait
		{srcW: 800, srcH: 400, wantW: 320, wantH: 160},   // 2:1 landscape
		{srcW: 5000, srcH: 1, wantW: 320, wantH: 1},      // extreme wide clamp
		{srcW: 1, srcH: 5000, wantW: 1, wantH: 240},      // extreme tall clamp
	}

	for _, tc := range tests {
		gotW, gotH := ComputeTargetDimensions(tc.srcW, tc.srcH, 320, 240)
		if gotW != tc.wantW || gotH != tc.wantH {
			t.Errorf("ComputeTargetDimensions(%d, %d, 320, 240) = (%d, %d), want (%d, %d)",
				tc.srcW, tc.srcH, gotW, gotH, tc.wantW, tc.wantH)
		}
		if gotW < 1 || gotW > 320 || gotH < 1 || gotH > 240 {
			t.Errorf("dimensions (%d, %d) out of 320x240 bounds", gotW, gotH)
		}
		if gotW != 320 && gotH != 240 {
			t.Errorf("neither dimension is at maximum (got %dx%d)", gotW, gotH)
		}
	}
}

func makeTestImage(w, h int) *image.NRGBA {
	img := image.NewNRGBA(image.Rect(0, 0, w, h))
	for y := 0; y < h; y++ {
		for x := 0; x < w; x++ {
			img.SetNRGBA(x, y, color.NRGBA{
				R: uint8((x * 255) / max(1, w-1)),
				G: uint8((y * 255) / max(1, h-1)),
				B: 128,
				A: 255,
			})
		}
	}
	return img
}

func populateTestInputs(t *testing.T, inDir string) map[string][2]int {
	t.Helper()

	// 1. Wide JPEG (800x400 -> 320x160)
	fJpg, err := os.Create(filepath.Join(inDir, "landscape.jpg"))
	if err != nil {
		t.Fatal(err)
	}
	if err := jpeg.Encode(fJpg, makeTestImage(800, 400), &jpeg.Options{Quality: 90}); err != nil {
		t.Fatal(err)
	}
	_ = fJpg.Close()

	// 2. Tall PNG (300x600 -> 120x240)
	fPng, err := os.Create(filepath.Join(inDir, "portrait.png"))
	if err != nil {
		t.Fatal(err)
	}
	if err := png.Encode(fPng, makeTestImage(300, 600)); err != nil {
		t.Fatal(err)
	}
	_ = fPng.Close()

	// 3. Square GIF (200x200 -> 240x240)
	fGif, err := os.Create(filepath.Join(inDir, "icon.gif"))
	if err != nil {
		t.Fatal(err)
	}
	if err := gif.Encode(fGif, makeTestImage(200, 200), nil); err != nil {
		t.Fatal(err)
	}
	_ = fGif.Close()

	// 4. PPM P6 (640x480 -> 320x240)
	ppmPath := filepath.Join(inDir, "capture.ppm")
	ppmFile, err := os.Create(ppmPath)
	if err != nil {
		t.Fatal(err)
	}
	_, _ = fmt.Fprintf(ppmFile, "P6\n640 480\n255\n")
	ppmPixels := make([]byte, 640*480*3)
	for i := range ppmPixels {
		ppmPixels[i] = byte(i % 251)
	}
	_, _ = ppmFile.Write(ppmPixels)
	_ = ppmFile.Close()

	// 5. Non-image file (should be skipped without failing)
	if err := os.WriteFile(filepath.Join(inDir, "readme.txt"), []byte("hello world\n"), 0o644); err != nil {
		t.Fatal(err)
	}

	// 6. Nested subdirectory with an image (recursive by default)
	nestedInDir := filepath.Join(inDir, "albums", "2026")
	if err := os.MkdirAll(nestedInDir, 0o755); err != nil {
		t.Fatal(err)
	}
	fNested, err := os.Create(filepath.Join(nestedInDir, "vacation.png"))
	if err != nil {
		t.Fatal(err)
	}
	if err := png.Encode(fNested, makeTestImage(640, 320)); err != nil {
		t.Fatal(err)
	}
	_ = fNested.Close()

	return map[string][2]int{
		"landscape":                        {320, 160},
		"portrait":                         {120, 240},
		"icon":                             {240, 240},
		"capture":                          {320, 240},
		filepath.Join("albums", "2026", "vacation"): {320, 160},
	}
}

func TestProcessDirectoryPNG(t *testing.T) {
	inDir := t.TempDir()
	expected := populateTestInputs(t, inDir)
	outDir := filepath.Join(t.TempDir(), "out-pngs")

	count, err := ProcessDirectory(inDir, outDir, "png", 0, 320, 240, false)
	if err != nil {
		t.Fatalf("ProcessDirectory(png) failed: %v", err)
	}
	if count != len(expected) {
		t.Fatalf("got count=%d, want %d", count, len(expected))
	}

	for baseName, wantDims := range expected {
		pngPath := filepath.Join(outDir, baseName+".png")
		f, err := os.Open(pngPath)
		if err != nil {
			t.Fatalf("expected output PNG %s missing: %v", pngPath, err)
		}
		img, err := png.Decode(f)
		_ = f.Close()
		if err != nil {
			t.Fatalf("failed to decode output PNG %s: %v", pngPath, err)
		}
		gotW := img.Bounds().Dx()
		gotH := img.Bounds().Dy()
		if gotW != wantDims[0] || gotH != wantDims[1] {
			t.Errorf("%s.png dimensions = %dx%d, want %dx%d", baseName, gotW, gotH, wantDims[0], wantDims[1])
		}

		// Ensure no .jpg variants were created
		jpgPath := filepath.Join(outDir, baseName+".jpg")
		if _, err := os.Stat(jpgPath); !os.IsNotExist(err) {
			t.Errorf("unexpected JPEG output %s created in png mode", jpgPath)
		}
	}
}

func TestProcessDirectoryJPG(t *testing.T) {
	inDir := t.TempDir()
	expected := populateTestInputs(t, inDir)
	outDir90 := filepath.Join(t.TempDir(), "out-jpg90")
	outDir75 := filepath.Join(t.TempDir(), "out-jpg75")

	count90, err := ProcessDirectory(inDir, outDir90, "jpg", 90, 320, 240, false)
	if err != nil {
		t.Fatalf("ProcessDirectory(jpg, 90) failed: %v", err)
	}
	if count90 != len(expected) {
		t.Fatalf("got count90=%d, want %d", count90, len(expected))
	}

	count75, err := ProcessDirectory(inDir, outDir75, "jpg", 75, 320, 240, false)
	if err != nil {
		t.Fatalf("ProcessDirectory(jpg, 75) failed: %v", err)
	}
	if count75 != len(expected) {
		t.Fatalf("got count75=%d, want %d", count75, len(expected))
	}

	for baseName, wantDims := range expected {
		jpg90Path := filepath.Join(outDir90, baseName+".jpg")
		st90, err := os.Stat(jpg90Path)
		if err != nil {
			t.Fatalf("expected output JPEG %s missing: %v", jpg90Path, err)
		}
		f, err := os.Open(jpg90Path)
		if err != nil {
			t.Fatalf("failed to open JPEG %s: %v", jpg90Path, err)
		}
		img, err := jpeg.Decode(f)
		_ = f.Close()
		if err != nil {
			t.Fatalf("failed to decode output JPEG %s: %v", jpg90Path, err)
		}
		gotW := img.Bounds().Dx()
		gotH := img.Bounds().Dy()
		if gotW != wantDims[0] || gotH != wantDims[1] {
			t.Errorf("%s.jpg dimensions = %dx%d, want %dx%d", baseName, gotW, gotH, wantDims[0], wantDims[1])
		}

		// Ensure no .png or .90.jpg variants were created
		pngPath := filepath.Join(outDir90, baseName+".png")
		if _, err := os.Stat(pngPath); !os.IsNotExist(err) {
			t.Errorf("unexpected PNG output %s created in jpg mode", pngPath)
		}
		variantPath := filepath.Join(outDir90, baseName+".90.jpg")
		if _, err := os.Stat(variantPath); !os.IsNotExist(err) {
			t.Errorf("unexpected variant %s created in jpg mode", variantPath)
		}

		// Compare size with quality 75
		jpg75Path := filepath.Join(outDir75, baseName+".jpg")
		st75, err := os.Stat(jpg75Path)
		if err != nil {
			t.Fatalf("expected output JPEG %s missing: %v", jpg75Path, err)
		}
		if st75.Size() > st90.Size() {
			t.Errorf("%s quality 75 size (%d B) > quality 90 size (%d B)",
				baseName, st75.Size(), st90.Size())
		}
	}
}
