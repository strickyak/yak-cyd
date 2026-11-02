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

func TestProcessDirectory(t *testing.T) {
	inDir := t.TempDir()
	outDir := filepath.Join(t.TempDir(), "out-pngs")

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

	// 6. Nested subdirectory with an image (should be skipped when recursive=false)
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

	count, err := ProcessDirectory(inDir, outDir, 320, 240, false, false)
	if err != nil {
		t.Fatalf("ProcessDirectory failed: %v", err)
	}
	if count != 4 {
		t.Fatalf("got count=%d, want 4", count)
	}
	if _, err := os.Stat(filepath.Join(outDir, "albums")); !os.IsNotExist(err) {
		t.Errorf("expected subdirectory 'albums' not to be created when recursive=false")
	}

	expected := map[string][2]int{
		"landscape": {320, 160},
		"portrait":  {120, 240},
		"icon":      {240, 240},
		"capture":   {320, 240},
	}

	verifyOutputs := func(dir string, exp map[string][2]int) {
		for baseName, wantDims := range exp {
			// Verify PNG output
			pngPath := filepath.Join(dir, baseName+".png")
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

			// Verify 3 JPEG outputs (.92.jpg, .88.jpg, .75.jpg)
			var prevSize int64
			for i, level := range DefaultJPEGQualities {
				jpgPath := filepath.Join(dir, fmt.Sprintf("%s.%d.jpg", baseName, level))
				st, err := os.Stat(jpgPath)
				if err != nil {
					t.Fatalf("expected output JPEG %s missing: %v", jpgPath, err)
				}
				if i > 0 && st.Size() > prevSize {
					t.Errorf("%s quality %d size (%d B) > higher quality size (%d B)",
						baseName, level, st.Size(), prevSize)
				}
				prevSize = st.Size()

				jf, err := os.Open(jpgPath)
				if err != nil {
					t.Fatalf("failed to open JPEG %s: %v", jpgPath, err)
				}
				jimg, err := jpeg.Decode(jf)
				_ = jf.Close()
				if err != nil {
					t.Fatalf("failed to decode output JPEG %s: %v", jpgPath, err)
				}
				jw := jimg.Bounds().Dx()
				jh := jimg.Bounds().Dy()
				if jw != wantDims[0] || jh != wantDims[1] {
					t.Errorf("%s.%d.jpg dimensions = %dx%d, want %dx%d",
						baseName, level, jw, jh, wantDims[0], wantDims[1])
				}
			}
		}
	}

	verifyOutputs(outDir, expected)

	// Now test with recursive=true
	outDirRec := filepath.Join(t.TempDir(), "out-recursive")
	countRec, err := ProcessDirectory(inDir, outDirRec, 320, 240, true, false)
	if err != nil {
		t.Fatalf("ProcessDirectory(recursive=true) failed: %v", err)
	}
	if countRec != 5 {
		t.Fatalf("got recursive count=%d, want 5", countRec)
	}
	expectedRec := map[string][2]int{
		"landscape":                        {320, 160},
		"portrait":                         {120, 240},
		"icon":                             {240, 240},
		"capture":                          {320, 240},
		filepath.Join("albums", "2026", "vacation"): {320, 160},
	}
	verifyOutputs(outDirRec, expectedRec)
}
