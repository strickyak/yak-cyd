// cyd-resize converts and resizes all images in an input directory into a PNG
// and three JPEGs (at quality levels 92, 88, and 75) in an output directory,
// optimized for the 320x240 display of the CYD (ESP32-2432S028R). Every output
// image preserves aspect ratio and fits within 320x240 with at least one
// dimension at the maximum (width == 320 or height == 240).
package main

import (
	"bufio"
	"bytes"
	"encoding/binary"
	"errors"
	"flag"
	"fmt"
	"image"
	"image/color"
	_ "image/gif"
	"image/jpeg"
	"image/png"
	"io"
	"math"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

const (
	DefaultMaxWidth  = 320
	DefaultMaxHeight = 240
	lanczosRadius    = 3.0
)

// DefaultJPEGQualities defines the three JPEG quality levels:
// normal (92), less (88), and much less (75).
var DefaultJPEGQualities = []int{92, 88, 75}

func init() {
	// Register Netpbm PPM (P6 binary and P3 ASCII) in addition to standard
	// JPEG, PNG, and GIF decoders.
	image.RegisterFormat("ppm", "P6", decodePPM, decodePPMConfig)
	image.RegisterFormat("ppm-ascii", "P3", decodePPM, decodePPMConfig)
}

// ComputeTargetDimensions calculates the resized (dstW, dstH) for an image of
// size (srcW, srcH) so that:
//   - Aspect ratio is preserved as closely as integer pixel dimensions allow
//   - 1 <= dstW <= maxW and 1 <= dstH <= maxH
//   - Either dstW == maxW (horizontal max, e.g. 320) or dstH == maxH (vertical max, e.g. 240)
func ComputeTargetDimensions(srcW, srcH, maxW, maxH int) (dstW, dstH int) {
	if srcW <= 0 || srcH <= 0 || maxW <= 0 || maxH <= 0 {
		return maxW, maxH
	}

	// Compare srcW/srcH against maxW/maxH using cross-multiplication:
	// srcW * maxH >= srcH * maxW means the image is at least as wide as maxW:maxH.
	if int64(srcW)*int64(maxH) >= int64(srcH)*int64(maxW) {
		dstW = maxW
		dstH = int(math.Round(float64(srcH) * float64(maxW) / float64(srcW)))
		if dstH < 1 {
			dstH = 1
		}
		if dstH > maxH {
			dstH = maxH
		}
	} else {
		dstH = maxH
		dstW = int(math.Round(float64(srcW) * float64(maxH) / float64(srcH)))
		if dstW < 1 {
			dstW = 1
		}
		if dstW > maxW {
			dstW = maxW
		}
	}
	return dstW, dstH
}

// sinc computes sin(pi*x) / (pi*x).
func sinc(x float64) float64 {
	if x == 0 {
		return 1.0
	}
	px := math.Pi * x
	return math.Sin(px) / px
}

// lanczosKernel evaluates the Lanczos windowed sinc kernel with radius a.
func lanczosKernel(x, a float64) float64 {
	if x < 0 {
		x = -x
	}
	if x >= a {
		return 0.0
	}
	return sinc(x) * sinc(x/a)
}

type contribSpan struct {
	start   int
	weights []float64
}

// precomputeContribs builds normalized resampling filter weights for a 1D pass
// mapping srcLen pixels to dstLen pixels.
func precomputeContribs(srcLen, dstLen int) []contribSpan {
	contribs := make([]contribSpan, dstLen)
	scale := float64(srcLen) / float64(dstLen)

	// When downsampling (scale > 1), widen the filter support by scale to
	// anti-alias high frequencies cleanly.
	filterScale := scale
	if filterScale < 1.0 {
		filterScale = 1.0
	}
	support := lanczosRadius * filterScale

	for dstIdx := 0; dstIdx < dstLen; dstIdx++ {
		center := (float64(dstIdx)+0.5)*scale - 0.5
		left := int(math.Ceil(center - support))
		right := int(math.Floor(center + support))
		if left < 0 {
			left = 0
		}
		if right >= srcLen {
			right = srcLen - 1
		}
		if left > right {
			srcClamp := int(math.Floor(center + 0.5))
			if srcClamp < 0 {
				srcClamp = 0
			} else if srcClamp >= srcLen {
				srcClamp = srcLen - 1
			}
			contribs[dstIdx] = contribSpan{
				start:   srcClamp,
				weights: []float64{1.0},
			}
			continue
		}

		weights := make([]float64, right-left+1)
		var sum float64
		for i := left; i <= right; i++ {
			w := lanczosKernel((float64(i)-center)/filterScale, lanczosRadius)
			weights[i-left] = w
			sum += w
		}
		if sum != 0 {
			invSum := 1.0 / sum
			for i := range weights {
				weights[i] *= invSum
			}
		} else {
			weights[0] = 1.0
		}
		contribs[dstIdx] = contribSpan{
			start:   left,
			weights: weights,
		}
	}
	return contribs
}

func clampByte(v float64) uint8 {
	iv := int(math.Round(v))
	if iv < 0 {
		return 0
	}
	if iv > 255 {
		return 255
	}
	return uint8(iv)
}

// toNRGBA converts any image.Image into a 0-based *image.NRGBA.
func toNRGBA(src image.Image) *image.NRGBA {
	b := src.Bounds()
	w, h := b.Dx(), b.Dy()
	if nrgba, ok := src.(*image.NRGBA); ok && b.Min.X == 0 && b.Min.Y == 0 {
		return nrgba
	}
	out := image.NewNRGBA(image.Rect(0, 0, w, h))
	for y := 0; y < h; y++ {
		for x := 0; x < w; x++ {
			c := color.NRGBAModel.Convert(src.At(b.Min.X+x, b.Min.Y+y)).(color.NRGBA)
			off := y*out.Stride + x*4
			out.Pix[off+0] = c.R
			out.Pix[off+1] = c.G
			out.Pix[off+2] = c.B
			out.Pix[off+3] = c.A
		}
	}
	return out
}

// ResizeLanczos resizes src to (dstW, dstH) using a 2-pass separable Lanczos-3
// filter in premultiplied alpha space.
func ResizeLanczos(src image.Image, dstW, dstH int) *image.NRGBA {
	srcNRGBA := toNRGBA(src)
	srcW := srcNRGBA.Bounds().Dx()
	srcH := srcNRGBA.Bounds().Dy()

	if srcW == dstW && srcH == dstH {
		return srcNRGBA
	}

	hContribs := precomputeContribs(srcW, dstW)
	vContribs := precomputeContribs(srcH, dstH)

	// Intermediate buffer: dstW x srcH with 4 float64 channels (premultiplied R, G, B, A)
	temp := make([]float64, dstW*srcH*4)

	// Pass 1: Horizontal resample (srcW -> dstW for each row y in 0..srcH-1)
	for y := 0; y < srcH; y++ {
		rowOff := y * srcNRGBA.Stride
		tempRowOff := y * dstW * 4
		for dx := 0; dx < dstW; dx++ {
			span := hContribs[dx]
			var rSum, gSum, bSum, aSum float64
			pixIdx := rowOff + span.start*4
			for _, w := range span.weights {
				a := float64(srcNRGBA.Pix[pixIdx+3])
				aw := a * w
				rSum += float64(srcNRGBA.Pix[pixIdx+0]) * aw
				gSum += float64(srcNRGBA.Pix[pixIdx+1]) * aw
				bSum += float64(srcNRGBA.Pix[pixIdx+2]) * aw
				aSum += aw
				pixIdx += 4
			}
			tOff := tempRowOff + dx*4
			temp[tOff+0] = rSum
			temp[tOff+1] = gSum
			temp[tOff+2] = bSum
			temp[tOff+3] = aSum
		}
	}

	// Pass 2: Vertical resample (srcH -> dstH for each column dx in 0..dstW-1)
	dst := image.NewNRGBA(image.Rect(0, 0, dstW, dstH))
	tempStride := dstW * 4
	for dy := 0; dy < dstH; dy++ {
		span := vContribs[dy]
		dstRowOff := dy * dst.Stride
		for dx := 0; dx < dstW; dx++ {
			var rSum, gSum, bSum, aSum float64
			tIdx := span.start*tempStride + dx*4
			for _, w := range span.weights {
				rSum += temp[tIdx+0] * w
				gSum += temp[tIdx+1] * w
				bSum += temp[tIdx+2] * w
				aSum += temp[tIdx+3] * w
				tIdx += tempStride
			}
			dOff := dstRowOff + dx*4
			if aSum <= 0.5 {
				dst.Pix[dOff+0] = 0
				dst.Pix[dOff+1] = 0
				dst.Pix[dOff+2] = 0
				dst.Pix[dOff+3] = 0
			} else {
				invA := 1.0 / aSum
				dst.Pix[dOff+0] = clampByte(rSum * invA)
				dst.Pix[dOff+1] = clampByte(gSum * invA)
				dst.Pix[dOff+2] = clampByte(bSum * invA)
				dst.Pix[dOff+3] = clampByte(aSum)
			}
		}
	}

	return dst
}

// readJPEGExifOrientation inspects JPEG APP1 EXIF metadata (if present) and
// returns the EXIF Orientation tag value (1..8, or 1 if absent).
func readJPEGExifOrientation(data []byte) int {
	if len(data) < 4 || data[0] != 0xFF || data[1] != 0xD8 {
		return 1
	}
	pos := 2
	for pos+4 <= len(data) {
		if data[pos] != 0xFF {
			return 1
		}
		marker := data[pos+1]
		if marker == 0xFF {
			pos++
			continue
		}
		if marker == 0xDA || marker == 0xD9 { // SOS or EOI
			break
		}
		segLen := int(binary.BigEndian.Uint16(data[pos+2 : pos+4]))
		if segLen < 2 || pos+2+segLen > len(data) {
			break
		}
		if marker == 0xE1 && segLen >= 16 {
			exif := data[pos+4 : pos+2+segLen]
			if bytes.HasPrefix(exif, []byte("Exif\x00\x00")) {
				tiff := exif[6:]
				if len(tiff) >= 8 {
					var bo binary.ByteOrder
					if string(tiff[0:2]) == "II" {
						bo = binary.LittleEndian
					} else if string(tiff[0:2]) == "MM" {
						bo = binary.BigEndian
					}
					if bo != nil && bo.Uint16(tiff[2:4]) == 0x002A {
						ifdOff := int(bo.Uint32(tiff[4:8]))
						if ifdOff >= 8 && ifdOff+2 <= len(tiff) {
							numEntries := int(bo.Uint16(tiff[ifdOff : ifdOff+2]))
							for i := 0; i < numEntries; i++ {
								eOff := ifdOff + 2 + i*12
								if eOff+12 > len(tiff) {
									break
								}
								tag := bo.Uint16(tiff[eOff : eOff+2])
								if tag == 0x0112 { // Orientation tag
									val := int(bo.Uint16(tiff[eOff+8 : eOff+10]))
									if val >= 1 && val <= 8 {
										return val
									}
								}
							}
						}
					}
				}
			}
		}
		pos += 2 + segLen
	}
	return 1
}

// applyExifOrientation transforms an image according to its EXIF orientation tag (1..8).
func applyExifOrientation(img image.Image, orient int) image.Image {
	if orient <= 1 || orient > 8 {
		return img
	}
	src := toNRGBA(img)
	w := src.Bounds().Dx()
	h := src.Bounds().Dy()

	switch orient {
	case 2: // Flip horizontal
		dst := image.NewNRGBA(image.Rect(0, 0, w, h))
		for y := 0; y < h; y++ {
			for x := 0; x < w; x++ {
				dst.SetNRGBA(w-1-x, y, src.NRGBAAt(x, y))
			}
		}
		return dst
	case 3: // Rotate 180
		dst := image.NewNRGBA(image.Rect(0, 0, w, h))
		for y := 0; y < h; y++ {
			for x := 0; x < w; x++ {
				dst.SetNRGBA(w-1-x, h-1-y, src.NRGBAAt(x, y))
			}
		}
		return dst
	case 4: // Flip vertical
		dst := image.NewNRGBA(image.Rect(0, 0, w, h))
		for y := 0; y < h; y++ {
			for x := 0; x < w; x++ {
				dst.SetNRGBA(x, h-1-y, src.NRGBAAt(x, y))
			}
		}
		return dst
	case 5: // Transpose (flip horizontal + rotate 270 CW)
		dst := image.NewNRGBA(image.Rect(0, 0, h, w))
		for y := 0; y < h; y++ {
			for x := 0; x < w; x++ {
				dst.SetNRGBA(y, x, src.NRGBAAt(x, y))
			}
		}
		return dst
	case 6: // Rotate 90 CW
		dst := image.NewNRGBA(image.Rect(0, 0, h, w))
		for y := 0; y < h; y++ {
			for x := 0; x < w; x++ {
				dst.SetNRGBA(h-1-y, x, src.NRGBAAt(x, y))
			}
		}
		return dst
	case 7: // Transverse
		dst := image.NewNRGBA(image.Rect(0, 0, h, w))
		for y := 0; y < h; y++ {
			for x := 0; x < w; x++ {
				dst.SetNRGBA(h-1-y, w-1-x, src.NRGBAAt(x, y))
			}
		}
		return dst
	case 8: // Rotate 270 CW (90 CCW)
		dst := image.NewNRGBA(image.Rect(0, 0, h, w))
		for y := 0; y < h; y++ {
			for x := 0; x < w; x++ {
				dst.SetNRGBA(y, w-1-x, src.NRGBAAt(x, y))
			}
		}
		return dst
	}
	return img
}

// readPPMToken reads the next whitespace-delimited token from r, skipping '#' comments.
func readPPMToken(r *bufio.Reader) (string, error) {
	var b strings.Builder
	for {
		ch, err := r.ReadByte()
		if err != nil {
			if b.Len() > 0 {
				return b.String(), nil
			}
			return "", err
		}
		if ch == '#' {
			_, _ = r.ReadBytes('\n')
			continue
		}
		if ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' {
			if b.Len() > 0 {
				return b.String(), nil
			}
			continue
		}
		b.WriteByte(ch)
	}
}

func parsePPMHeader(r *bufio.Reader) (magic string, w, h, maxVal int, err error) {
	magic, err = readPPMToken(r)
	if err != nil {
		return "", 0, 0, 0, err
	}
	if magic != "P6" && magic != "P3" {
		return "", 0, 0, 0, errors.New("unsupported PPM magic")
	}
	wStr, err := readPPMToken(r)
	if err != nil {
		return "", 0, 0, 0, err
	}
	hStr, err := readPPMToken(r)
	if err != nil {
		return "", 0, 0, 0, err
	}
	mStr, err := readPPMToken(r)
	if err != nil {
		return "", 0, 0, 0, err
	}
	w, err = strconv.Atoi(wStr)
	if err != nil || w <= 0 {
		return "", 0, 0, 0, errors.New("invalid PPM width")
	}
	h, err = strconv.Atoi(hStr)
	if err != nil || h <= 0 {
		return "", 0, 0, 0, errors.New("invalid PPM height")
	}
	maxVal, err = strconv.Atoi(mStr)
	if err != nil || maxVal <= 0 || maxVal > 65535 {
		return "", 0, 0, 0, errors.New("invalid PPM maxval")
	}
	return magic, w, h, maxVal, nil
}

func decodePPMConfig(r io.Reader) (image.Config, error) {
	br := bufio.NewReader(r)
	_, w, h, _, err := parsePPMHeader(br)
	if err != nil {
		return image.Config{}, err
	}
	return image.Config{
		ColorModel: color.NRGBAModel,
		Width:      w,
		Height:     h,
	}, nil
}

func decodePPM(r io.Reader) (image.Image, error) {
	br := bufio.NewReader(r)
	magic, w, h, maxVal, err := parsePPMHeader(br)
	if err != nil {
		return nil, err
	}
	img := image.NewNRGBA(image.Rect(0, 0, w, h))
	if magic == "P6" {
		if maxVal <= 255 {
			buf := make([]byte, w*h*3)
			if _, err := io.ReadFull(br, buf); err != nil {
				return nil, err
			}
			for i := 0; i < w*h; i++ {
				img.Pix[i*4+0] = uint8(int(buf[i*3+0]) * 255 / maxVal)
				img.Pix[i*4+1] = uint8(int(buf[i*3+1]) * 255 / maxVal)
				img.Pix[i*4+2] = uint8(int(buf[i*3+2]) * 255 / maxVal)
				img.Pix[i*4+3] = 255
			}
		} else {
			buf := make([]byte, w*h*6)
			if _, err := io.ReadFull(br, buf); err != nil {
				return nil, err
			}
			for i := 0; i < w*h; i++ {
				rv := int(binary.BigEndian.Uint16(buf[i*6+0 : i*6+2]))
				gv := int(binary.BigEndian.Uint16(buf[i*6+2 : i*6+4]))
				bv := int(binary.BigEndian.Uint16(buf[i*6+4 : i*6+6]))
				img.Pix[i*4+0] = uint8(rv * 255 / maxVal)
				img.Pix[i*4+1] = uint8(gv * 255 / maxVal)
				img.Pix[i*4+2] = uint8(bv * 255 / maxVal)
				img.Pix[i*4+3] = 255
			}
		}
		return img, nil
	}

	// P3 ASCII PPM
	for i := 0; i < w*h; i++ {
		rs, err := readPPMToken(br)
		if err != nil {
			return nil, err
		}
		gs, err := readPPMToken(br)
		if err != nil {
			return nil, err
		}
		bs, err := readPPMToken(br)
		if err != nil {
			return nil, err
		}
		rv, _ := strconv.Atoi(rs)
		gv, _ := strconv.Atoi(gs)
		bv, _ := strconv.Atoi(bs)
		img.Pix[i*4+0] = uint8(rv * 255 / maxVal)
		img.Pix[i*4+1] = uint8(gv * 255 / maxVal)
		img.Pix[i*4+2] = uint8(bv * 255 / maxVal)
		img.Pix[i*4+3] = 255
	}
	return img, nil
}

// DecodeImageFile reads and decodes an image from path, applying JPEG EXIF
// orientation if present.
func DecodeImageFile(path string) (image.Image, string, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, "", err
	}
	img, format, err := image.Decode(bytes.NewReader(data))
	if err != nil {
		return nil, "", err
	}
	if format == "jpeg" {
		orient := readJPEGExifOrientation(data)
		if orient > 1 {
			img = applyExifOrientation(img, orient)
		}
	}
	return img, format, nil
}

// OutputBaseName returns the filename without its extension.
func OutputBaseName(inName string) string {
	ext := filepath.Ext(inName)
	base := strings.TrimSuffix(inName, ext)
	if base == "" {
		return inName
	}
	return base
}

// OutputPNGName returns the output filename with a ".png" extension for a
// given input filename.
func OutputPNGName(inName string) string {
	return OutputBaseName(inName) + ".png"
}

// OutputJPEGName returns the output JPEG filename "<base>.<level>.jpg" for a
// given input filename and quality level.
func OutputJPEGName(inName string, level int) string {
	return fmt.Sprintf("%s.%d.jpg", OutputBaseName(inName), level)
}

func writePNGFile(path string, img image.Image) error {
	f, err := os.Create(path)
	if err != nil {
		return err
	}
	enc := png.Encoder{CompressionLevel: png.BestCompression}
	if err := enc.Encode(f, img); err != nil {
		_ = f.Close()
		return err
	}
	return f.Close()
}

func writeJPEGFile(path string, img image.Image, quality int) error {
	f, err := os.Create(path)
	if err != nil {
		return err
	}
	if err := jpeg.Encode(f, img, &jpeg.Options{Quality: quality}); err != nil {
		_ = f.Close()
		return err
	}
	return f.Close()
}

// ProcessImageFile decodes a single image at inPath, resizes it to fit
// (maxW, maxH) with one dimension at maximum, and writes:
//   - PNG at outPath (e.g. <outdir>/<base>.png)
//   - 3 JPEGs at <outdir>/<base>.<level>.jpg for DefaultJPEGQualities (92, 88, 75)
func ProcessImageFile(inPath, outPath string, maxW, maxH int) (srcW, srcH, dstW, dstH int, err error) {
	img, _, err := DecodeImageFile(inPath)
	if err != nil {
		return 0, 0, 0, 0, err
	}
	b := img.Bounds()
	srcW, srcH = b.Dx(), b.Dy()
	if srcW <= 0 || srcH <= 0 {
		return 0, 0, 0, 0, fmt.Errorf("invalid image dimensions %dx%d", srcW, srcH)
	}

	dstW, dstH = ComputeTargetDimensions(srcW, srcH, maxW, maxH)
	resized := ResizeLanczos(img, dstW, dstH)

	if err := os.MkdirAll(filepath.Dir(outPath), 0o755); err != nil {
		return srcW, srcH, dstW, dstH, err
	}

	if err := writePNGFile(outPath, resized); err != nil {
		return srcW, srcH, dstW, dstH, err
	}

	outBase := strings.TrimSuffix(outPath, filepath.Ext(outPath))
	for _, level := range DefaultJPEGQualities {
		jpgPath := fmt.Sprintf("%s.%d.jpg", outBase, level)
		if err := writeJPEGFile(jpgPath, resized, level); err != nil {
			return srcW, srcH, dstW, dstH, err
		}
	}

	return srcW, srcH, dstW, dstH, nil
}

// ProcessDirectory converts all images in inDir to resized PNG and JPEG files in outDir.
// When recursive is true, it descends into subdirectories and mirrors the directory
// structure in outDir (creating subdirectories as needed).
func ProcessDirectory(inDir, outDir string, maxW, maxH int, recursive, verbose bool) (int, error) {
	converted, err := processDir(inDir, outDir, maxW, maxH, recursive, verbose)
	if err != nil {
		return converted, err
	}
	if err := os.MkdirAll(outDir, 0o755); err != nil {
		return converted, fmt.Errorf("creating output directory %q: %w", outDir, err)
	}
	return converted, nil
}

func processDir(inDir, outDir string, maxW, maxH int, recursive, verbose bool) (int, error) {
	entries, err := os.ReadDir(inDir)
	if err != nil {
		return 0, fmt.Errorf("reading input directory %q: %w", inDir, err)
	}

	converted := 0
	for _, entry := range entries {
		name := entry.Name()
		if strings.HasPrefix(name, ".") {
			continue
		}
		inPath := filepath.Join(inDir, name)
		if entry.IsDir() {
			if !recursive {
				continue
			}
			subOutDir := filepath.Join(outDir, name)
			subCount, err := processDir(inPath, subOutDir, maxW, maxH, recursive, verbose)
			converted += subCount
			if err != nil {
				return converted, err
			}
			continue
		}
		outName := OutputPNGName(name)
		outPath := filepath.Join(outDir, outName)

		srcW, srcH, dstW, dstH, err := ProcessImageFile(inPath, outPath, maxW, maxH)
		if err != nil {
			if errors.Is(err, image.ErrFormat) {
				if verbose {
					fmt.Fprintf(os.Stderr, "skipping non-image file: %s\n", inPath)
				}
				continue
			}
			return converted, fmt.Errorf("processing %q: %w", inPath, err)
		}

		converted++
		if verbose {
			base := filepath.Join(outDir, OutputBaseName(name))
			fmt.Printf("%s (%dx%d) -> %s.{png,92.jpg,88.jpg,75.jpg} (%dx%d)\n",
				inPath, srcW, srcH, base, dstW, dstH)
		}
	}
	return converted, nil
}

func usage() {
	fmt.Fprintf(os.Stderr, "Usage: %s [flags] <indir> <outdir>\n\n", filepath.Base(os.Args[0]))
	fmt.Fprintf(os.Stderr, "Resizes all images in <indir> and writes <base>.png plus\n")
	fmt.Fprintf(os.Stderr, "<base>.92.jpg, <base>.88.jpg, and <base>.75.jpg to <outdir>,\n")
	fmt.Fprintf(os.Stderr, "optimized for the 320x240 CYD screen (width=320 or height=240).\n\n")
	flag.PrintDefaults()
}

func main() {
	maxW := flag.Int("w", DefaultMaxWidth, "maximum horizontal dimension in pixels")
	maxH := flag.Int("h", DefaultMaxHeight, "maximum vertical dimension in pixels")
	recursive := flag.Bool("r", false, "recurse into subdirectories and mirror directory structure in outdir")
	quiet := flag.Bool("q", false, "suppress per-file progress output")
	flag.Usage = usage
	flag.Parse()

	if flag.NArg() != 2 {
		usage()
		os.Exit(2)
	}

	inDir := flag.Arg(0)
	outDir := flag.Arg(1)

	count, err := ProcessDirectory(inDir, outDir, *maxW, *maxH, *recursive, !*quiet)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error: %v\n", err)
		os.Exit(1)
	}
	if !*quiet {
		fmt.Printf("Converted %d image(s) to %s\n", count, outDir)
	}
}
