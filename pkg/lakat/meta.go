package lakat

import (
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"fmt"
	"hash/crc32"
	"strings"
	"time"
)

const (
	LakatosKernelMagic      = 0x4E524B4C // "LKRN" in little-endian
	LakatosKernelHeaderSize = 64
)

// FormatMeta parses and formats the Lakatos header metadata of a .lapp, .bin, .krn, or raw LCOD file.
func FormatMeta(path string, data []byte) (string, error) {
	if len(data) < 4 {
		return "", fmt.Errorf("%s: file too small (%d bytes)", path, len(data))
	}

	magic := binary.LittleEndian.Uint32(data[0:4])
	switch {
	case magic == LakatosAppMagic:
		return formatAppMeta(path, data)
	case magic == LakatosKernelMagic:
		return formatKrnMeta(path, data)
	case bytes.Equal(data[0:4], LCodeMagic):
		var sb strings.Builder
		fmt.Fprintf(&sb, "=== %s (Raw LCOD Bytecode, %d bytes) ===\n", path, len(data))
		formatLcodPayloadMeta(&sb, data)
		return sb.String(), nil
	default:
		return "", fmt.Errorf("%s: unrecognized magic 0x%08X (%q)", path, magic, string(data[0:4]))
	}
}

func formatTimestampSec(sec int64) string {
	if sec <= 0 {
		return "0 (any kernel)"
	}
	t := time.Unix(sec, 0).UTC()
	return fmt.Sprintf("%d (%s)", sec, t.Format("2006-01-02 15:04:05 UTC"))
}

func formatAppMeta(path string, data []byte) (string, error) {
	if len(data) < LakatosAppHeaderSize {
		return "", fmt.Errorf("%s: truncated LakatosAppHeader (%d < %d bytes)", path, len(data), LakatosAppHeaderSize)
	}

	hdrVer := binary.LittleEndian.Uint32(data[4:8])
	abiVer := binary.LittleEndian.Uint32(data[8:12])
	entryAddr := binary.LittleEndian.Uint32(data[12:16])
	textOff := binary.LittleEndian.Uint32(data[16:20])
	textSize := binary.LittleEndian.Uint32(data[20:24])
	rodataOff := binary.LittleEndian.Uint32(data[24:28])
	rodataSize := binary.LittleEndian.Uint32(data[28:32])
	dataOff := binary.LittleEndian.Uint32(data[32:36])
	dataDst := binary.LittleEndian.Uint32(data[36:40])
	dataSize := binary.LittleEndian.Uint32(data[40:44])
	bssDst := binary.LittleEndian.Uint32(data[44:48])
	bssSize := binary.LittleEndian.Uint32(data[48:52])
	totalFileSize := binary.LittleEndian.Uint32(data[52:56])
	expectedSha := data[56:88]

	nameRaw := data[88:112]
	if nul := bytes.IndexByte(nameRaw, 0); nul >= 0 {
		nameRaw = nameRaw[:nul]
	}
	appName := string(nameRaw)

	reqKernelSec := int64(binary.LittleEndian.Uint64(data[112:120]))
	flags := binary.LittleEndian.Uint32(data[120:124])
	reserved := binary.LittleEndian.Uint32(data[124:128])

	payload := data[LakatosAppHeaderSize:]
	actualSha := sha256.Sum256(payload)
	shaStatus := "OK"
	if !bytes.Equal(expectedSha, actualSha[:]) {
		shaStatus = "MISMATCH"
	}

	isLCode := (flags & LakatosAppFlagLCode) != 0
	kind := "Native Xtensa App"
	if isLCode {
		kind = "LCode Bytecode App"
	}

	var sb strings.Builder
	fmt.Fprintf(&sb, "=== %s (%s, %d bytes) ===\n", path, kind, len(data))
	fmt.Fprintf(&sb, "  magic           : \"LAKA\" (0x%08X)\n", LakatosAppMagic)
	fmt.Fprintf(&sb, "  name            : %q\n", appName)
	fmt.Fprintf(&sb, "  header_version  : %d\n", hdrVer)
	fmt.Fprintf(&sb, "  abi_version     : %d\n", abiVer)
	fmt.Fprintf(&sb, "  req_kernel_sec  : %s\n", formatTimestampSec(reqKernelSec))
	if isLCode {
		fmt.Fprintf(&sb, "  flags           : 0x%08X (LAKATOS_APP_FLAG_LCODE)\n", flags)
	} else {
		fmt.Fprintf(&sb, "  flags           : 0x%08X (NATIVE)\n", flags)
	}
	if reserved != 0 {
		fmt.Fprintf(&sb, "  reserved        : 0x%08X\n", reserved)
	}
	fmt.Fprintf(&sb, "  total_file_size : %d bytes\n", totalFileSize)
	fmt.Fprintf(&sb, "  sha256          : %x [%s]\n", expectedSha, shaStatus)
	fmt.Fprintf(&sb, "  entry_addr      : 0x%08X\n", entryAddr)
	fmt.Fprintf(&sb, "  text            : offset=0x%X (%d), size=%d bytes\n", textOff, textOff, textSize)
	if !isLCode || rodataSize != 0 || dataSize != 0 || bssSize != 0 {
		fmt.Fprintf(&sb, "  rodata          : offset=0x%X, size=%d bytes\n", rodataOff, rodataSize)
		fmt.Fprintf(&sb, "  data            : offset=0x%X, dst=0x%08X, size=%d bytes\n", dataOff, dataDst, dataSize)
		fmt.Fprintf(&sb, "  bss             : dst=0x%08X, size=%d bytes\n", bssDst, bssSize)
	}
	if isLCode && len(payload) >= 24 && bytes.Equal(payload[0:4], LCodeMagic) {
		formatLcodPayloadMeta(&sb, payload)
	}
	return sb.String(), nil
}

func formatLcodPayloadMeta(sb *strings.Builder, payload []byte) {
	if len(payload) < 24 {
		return
	}
	ver := binary.LittleEndian.Uint16(payload[4:6])
	lcodFlags := binary.LittleEndian.Uint16(payload[6:8])
	strPoolSz := binary.LittleEndian.Uint32(payload[8:12])
	globalsCount := binary.LittleEndian.Uint16(payload[12:14])
	funcCount := binary.LittleEndian.Uint16(payload[14:16])
	entryIdx := binary.LittleEndian.Uint16(payload[16:18])
	codeSz := binary.LittleEndian.Uint32(payload[20:24])
	fmt.Fprintf(sb, "  lcod_payload    : magic=\"LCOD\" v%d flags=0x%04X strings=%dB globals=%d funcs=%d entry_fn=%d code=%dB\n",
		ver, lcodFlags, strPoolSz, globalsCount, funcCount, entryIdx, codeSz)
}

func formatKrnMeta(path string, data []byte) (string, error) {
	if len(data) < LakatosKernelHeaderSize {
		return "", fmt.Errorf("%s: truncated LakatosKernelHeader (%d < %d bytes)", path, len(data), LakatosKernelHeaderSize)
	}

	hdrVer := binary.LittleEndian.Uint32(data[4:8])
	versionSec := int64(binary.LittleEndian.Uint64(data[8:16]))
	kernelSize := binary.LittleEndian.Uint32(data[16:20])
	updaterSize := binary.LittleEndian.Uint32(data[20:24])
	expectedSha := data[24:56]
	updaterCrc := binary.LittleEndian.Uint32(data[56:60])
	flags := binary.LittleEndian.Uint32(data[60:64])

	shaStatus := "UNCHECKED"
	if int(LakatosKernelHeaderSize+kernelSize) <= len(data) {
		actualSha := sha256.Sum256(data[LakatosKernelHeaderSize : LakatosKernelHeaderSize+kernelSize])
		if bytes.Equal(expectedSha, actualSha[:]) {
			shaStatus = "OK"
		} else {
			shaStatus = "MISMATCH"
		}
	} else {
		shaStatus = "TRUNCATED"
	}

	crcStatus := "NONE"
	if updaterSize > 0 {
		start := int(LakatosKernelHeaderSize + kernelSize)
		end := start + int(updaterSize)
		if end <= len(data) {
			calcCrc := crc32.ChecksumIEEE(data[start:end])
			if calcCrc == updaterCrc {
				crcStatus = "OK"
			} else {
				crcStatus = fmt.Sprintf("MISMATCH (got 0x%08X)", calcCrc)
			}
		} else {
			crcStatus = "TRUNCATED"
		}
	}

	var sb strings.Builder
	fmt.Fprintf(&sb, "=== %s (Lakatos Kernel Package, %d bytes) ===\n", path, len(data))
	fmt.Fprintf(&sb, "  magic           : \"LKRN\" (0x%08X)\n", LakatosKernelMagic)
	fmt.Fprintf(&sb, "  header_version  : %d\n", hdrVer)
	fmt.Fprintf(&sb, "  version_sec     : %s\n", formatTimestampSec(versionSec))
	fmt.Fprintf(&sb, "  kernel_size     : %d bytes\n", kernelSize)
	fmt.Fprintf(&sb, "  updater_size    : %d bytes\n", updaterSize)
	fmt.Fprintf(&sb, "  kernel_sha256   : %x [%s]\n", expectedSha, shaStatus)
	fmt.Fprintf(&sb, "  updater_crc32   : 0x%08X [%s]\n", updaterCrc, crcStatus)
	fmt.Fprintf(&sb, "  flags           : 0x%08X\n", flags)
	return sb.String(), nil
}
