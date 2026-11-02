package main

import (
	"bufio"
	"bytes"
	"encoding/base64"
	"fmt"
	"net"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"
)

func TestPushBytesOverTetherBridge(t *testing.T) {
	hostConn, devConn := net.Pipe()
	defer hostConn.Close()
	defer devConn.Close()

	var mu sync.Mutex
	files := make(map[string][]byte)
	dirs := map[string]bool{
		"/flash": true,
		"/sd":    true,
		"/net":   true,
	}
	committed := make(map[string]bool)

	// Fake ESP32 @FUSE serial responder
	go func() {
		scanner := bufio.NewScanner(devConn)
		for scanner.Scan() {
			line := strings.TrimSpace(scanner.Text())
			if !strings.HasPrefix(line, "@FUSE ") {
				continue
			}
			rest := strings.TrimSpace(line[len("@FUSE "):])
			sp1 := strings.IndexByte(rest, ' ')
			if sp1 <= 0 {
				continue
			}
			seq := rest[:sp1]
			rem := strings.TrimSpace(rest[sp1+1:])
			sp2 := strings.IndexByte(rem, ' ')
			op := rem
			args := ""
			if sp2 > 0 {
				op = rem[:sp2]
				args = strings.TrimSpace(rem[sp2+1:])
			}

			mu.Lock()
			switch op {
			case "STAT":
				if dirs[args] {
					_, _ = fmt.Fprintf(devConn, "@@FUSE %s OK 1 4096 0\r\n", seq)
				} else if data, ok := files[args]; ok {
					_, _ = fmt.Fprintf(devConn, "@@FUSE %s OK 0 %d 1700000000\r\n", seq, len(data))
				} else {
					_, _ = fmt.Fprintf(devConn, "@@FUSE %s ERR ENOENT\r\n", seq)
				}
			case "MKDIR":
				dirs[args] = true
				_, _ = fmt.Fprintf(devConn, "@@FUSE %s OK\r\n", seq)
			case "WRITE":
				// <path> <mode> <offset> <b64>
				fields := strings.Fields(args)
				if len(fields) != 4 {
					_, _ = fmt.Fprintf(devConn, "@@FUSE %s ERR EINVAL\r\n", seq)
					break
				}
				vfsPath := fields[0]
				mode := fields[1]
				offset, _ := strconv.Atoi(fields[2])
				b64 := fields[3]
				var raw []byte
				if b64 != "-" {
					var err error
					raw, err = base64.StdEncoding.DecodeString(b64)
					if err != nil {
						_, _ = fmt.Fprintf(devConn, "@@FUSE %s ERR EINVAL\r\n", seq)
						break
					}
				}
				if mode == "W" && offset == 0 {
					files[vfsPath] = append([]byte(nil), raw...)
				} else {
					cur := files[vfsPath]
					if offset+len(raw) > len(cur) {
						next := make([]byte, offset+len(raw))
						copy(next, cur)
						cur = next
					}
					copy(cur[offset:], raw)
					files[vfsPath] = cur
				}
				_, _ = fmt.Fprintf(devConn, "@@FUSE %s OK %d\r\n", seq, len(raw))
			case "COMMIT":
				committed[args] = true
				_, _ = fmt.Fprintf(devConn, "@@FUSE %s OK\r\n", seq)
			default:
				_, _ = fmt.Fprintf(devConn, "@@FUSE %s ERR ENOSYS\r\n", seq)
			}
			mu.Unlock()
		}
	}()

	bridge := NewTetherBridge()
	bridge.SetPort(hostConn)
	go bridge.ReadPortLoop(hostConn, nil)

	if err := bridge.WaitUntilReady("/flash", 2*time.Second); err != nil {
		t.Fatalf("WaitUntilReady failed: %v", err)
	}

	// 1. Push a multi-chunk payload (1500 bytes -> 3 chunks) to directory "/flash"
	payload := make([]byte, 1500)
	for i := range payload {
		payload[i] = byte((i * 37) & 0xFF)
	}
	gotPath, err := bridge.PushBytes("breakout.lapp", payload, "/flash")
	if err != nil {
		t.Fatalf("PushBytes to /flash failed: %v", err)
	}
	if gotPath != "/flash/breakout.lapp" {
		t.Fatalf("got target path %q, want /flash/breakout.lapp", gotPath)
	}

	mu.Lock()
	gotData := files["/flash/breakout.lapp"]
	wasCommitted := committed["/flash/breakout.lapp"]
	mu.Unlock()

	if !wasCommitted {
		t.Errorf("expected /flash/breakout.lapp to be committed")
	}
	if !bytes.Equal(gotData, payload) {
		t.Errorf("uploaded payload mismatch (got %d bytes, want %d bytes)", len(gotData), len(payload))
	}

	// 2. Push to a nested file path "/net/pix-2/slide.jpg" (auto-creates /net/pix-2)
	smallPayload := []byte("fake-jpeg-bytes")
	gotPath2, err := bridge.PushBytes("local.jpg", smallPayload, "/net/pix-2/slide.jpg")
	if err != nil {
		t.Fatalf("PushBytes to /net/pix-2/slide.jpg failed: %v", err)
	}
	if gotPath2 != "/net/pix-2/slide.jpg" {
		t.Fatalf("got target path %q, want /net/pix-2/slide.jpg", gotPath2)
	}

	mu.Lock()
	createdDir := dirs["/net/pix-2"]
	gotData2 := files["/net/pix-2/slide.jpg"]
	mu.Unlock()

	if !createdDir {
		t.Errorf("expected parent directory /net/pix-2 to be created via MKDIR")
	}
	if !bytes.Equal(gotData2, smallPayload) {
		t.Errorf("uploaded slide.jpg mismatch")
	}

	// 3. Invalid VFS prefix should return an error
	if _, err := bridge.PushBytes("foo.txt", smallPayload, "/invalid/foo.txt"); err == nil {
		t.Errorf("expected error for invalid VFS root /invalid/foo.txt")
	}
}
