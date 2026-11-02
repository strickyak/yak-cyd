package main

import (
	"bufio"
	"bytes"
	"encoding/base64"
	"flag"
	"fmt"
	"io"
	"os"
	"os/signal"
	"path"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
	"time"
)

type FuseRemoteEntry struct {
	Name  string
	IsDir bool
	Size  int64
	Mtime int64
}

type fuseRpcReply struct {
	ok      bool
	errCode string
	rest    string
	entries []FuseRemoteEntry
}

type TetherBridge struct {
	portMu     sync.Mutex
	activePort io.ReadWriteCloser

	rpcMu      sync.Mutex
	seqCounter uint64

	pendMu    sync.Mutex
	pendSeq   string
	pendEnts  []FuseRemoteEntry
	pendReply chan fuseRpcReply
}

func NewTetherBridge() *TetherBridge {
	return &TetherBridge{}
}

func (b *TetherBridge) SetPort(p io.ReadWriteCloser) {
	b.portMu.Lock()
	b.activePort = p
	b.portMu.Unlock()
}

func (b *TetherBridge) WriteLine(line string) error {
	clean := strings.TrimRight(line, "\r\n") + "\n"
	b.portMu.Lock()
	defer b.portMu.Unlock()
	if b.activePort == nil {
		return fmt.Errorf("serial port not connected")
	}
	_, err := b.activePort.Write([]byte(clean))
	return err
}

func (b *TetherBridge) handleFuseLine(line string) {
	line = strings.TrimRight(line, "\r\n")
	if strings.HasPrefix(line, "@@FUSE_ENT ") {
		// Format: @@FUSE_ENT <seq> <isDir:0|1> <size> <mtime> <name>
		rest := strings.TrimSpace(line[len("@@FUSE_ENT "):])
		parts := strings.SplitN(rest, " ", 5)
		if len(parts) < 5 {
			return
		}
		seq := parts[0]
		isDir := parts[1] == "1"
		sz, _ := strconv.ParseInt(parts[2], 10, 64)
		mt, _ := strconv.ParseInt(parts[3], 10, 64)
		name := parts[4]

		b.pendMu.Lock()
		if b.pendSeq == seq {
			b.pendEnts = append(b.pendEnts, FuseRemoteEntry{
				Name:  name,
				IsDir: isDir,
				Size:  sz,
				Mtime: mt,
			})
		}
		b.pendMu.Unlock()
		return
	}

	if strings.HasPrefix(line, "@@FUSE ") {
		// Format: @@FUSE <seq> OK [rest...]  OR  @@FUSE <seq> ERR <code...>
		rest := strings.TrimSpace(line[len("@@FUSE "):])
		parts := strings.SplitN(rest, " ", 3)
		if len(parts) < 2 {
			return
		}
		seq := parts[0]
		status := parts[1]
		tail := ""
		if len(parts) == 3 {
			tail = parts[2]
		}

		b.pendMu.Lock()
		if b.pendSeq == seq && b.pendReply != nil {
			rep := fuseRpcReply{
				ok:      status == "OK",
				errCode: tail,
				rest:    tail,
				entries: b.pendEnts,
			}
			ch := b.pendReply
			b.pendSeq = ""
			b.pendEnts = nil
			b.pendReply = nil
			b.pendMu.Unlock()
			ch <- rep
			return
		}
		b.pendMu.Unlock()
	}
}

func (b *TetherBridge) callFuse(cmd string, timeout time.Duration) (fuseRpcReply, error) {
	b.rpcMu.Lock()
	defer b.rpcMu.Unlock()

	seqNum := atomic.AddUint64(&b.seqCounter, 1)
	seq := strconv.FormatUint(seqNum, 10)
	ch := make(chan fuseRpcReply, 1)

	b.pendMu.Lock()
	b.pendSeq = seq
	b.pendEnts = nil
	b.pendReply = ch
	b.pendMu.Unlock()

	defer func() {
		b.pendMu.Lock()
		if b.pendSeq == seq {
			b.pendSeq = ""
			b.pendEnts = nil
			b.pendReply = nil
		}
		b.pendMu.Unlock()
	}()

	wireCmd := fmt.Sprintf("\n@FUSE %s %s", seq, cmd)
	if err := b.WriteLine(wireCmd); err != nil {
		return fuseRpcReply{}, err
	}

	select {
	case rep := <-ch:
		if !rep.ok {
			return rep, fmt.Errorf("fuse remote error: %s", rep.errCode)
		}
		return rep, nil
	case <-time.After(timeout):
		return fuseRpcReply{}, fmt.Errorf("fuse serial timeout (%s)", cmd)
	}
}

func (b *TetherBridge) FuseStat(vfsPath string) (FuseRemoteEntry, error) {
	rep, err := b.callFuse("STAT "+vfsPath, 6*time.Second)
	if err != nil {
		return FuseRemoteEntry{}, err
	}
	fields := strings.Fields(rep.rest)
	if len(fields) < 3 {
		return FuseRemoteEntry{}, fmt.Errorf("malformed STAT reply: %q", rep.rest)
	}
	isDir := fields[0] == "1"
	sz, _ := strconv.ParseInt(fields[1], 10, 64)
	mt, _ := strconv.ParseInt(fields[2], 10, 64)
	return FuseRemoteEntry{
		Name:  path.Base(vfsPath),
		IsDir: isDir,
		Size:  sz,
		Mtime: mt,
	}, nil
}

func (b *TetherBridge) FuseLs(vfsPath string) ([]FuseRemoteEntry, error) {
	rep, err := b.callFuse("LS "+vfsPath, 8*time.Second)
	if err != nil {
		return nil, err
	}
	return rep.entries, nil
}

func (b *TetherBridge) FuseReadChunk(vfsPath string, offset, length uint32) ([]byte, error) {
	cmd := fmt.Sprintf("READ %s %d %d", vfsPath, offset, length)
	rep, err := b.callFuse(cmd, 8*time.Second)
	if err != nil {
		return nil, err
	}
	fields := strings.Fields(rep.rest)
	if len(fields) < 2 {
		return nil, fmt.Errorf("malformed READ reply: %q", rep.rest)
	}
	got, _ := strconv.Atoi(fields[0])
	if got <= 0 || fields[1] == "-" {
		return nil, nil
	}
	raw, err := base64.StdEncoding.DecodeString(fields[1])
	if err != nil {
		return nil, err
	}
	if len(raw) > got {
		raw = raw[:got]
	}
	return raw, nil
}

func (b *TetherBridge) FuseWriteChunk(vfsPath, mode string, offset uint32, data []byte) error {
	b64 := "-"
	if len(data) > 0 {
		b64 = base64.StdEncoding.EncodeToString(data)
	}
	cmd := fmt.Sprintf("WRITE %s %s %d %s", vfsPath, mode, offset, b64)
	_, err := b.callFuse(cmd, 8*time.Second)
	return err
}

func (b *TetherBridge) FuseCommit(vfsPath string) error {
	_, err := b.callFuse("COMMIT "+vfsPath, 10*time.Second)
	return err
}

func (b *TetherBridge) FuseRm(vfsPath string) error {
	_, err := b.callFuse("RM "+vfsPath, 6*time.Second)
	return err
}

func (b *TetherBridge) FuseMkdir(vfsPath string) error {
	_, err := b.callFuse("MKDIR "+vfsPath, 6*time.Second)
	return err
}

func (b *TetherBridge) FuseRmdir(vfsPath string) error {
	_, err := b.callFuse("RMDIR "+vfsPath, 6*time.Second)
	return err
}

func cmdTether(args []string) {
	fs := flag.NewFlagSet("tether", flag.ExitOnError)
	wire := fs.String("wire", "/dev/ttyUSB0", "Serial device connected to CYD (e.g. /dev/ttyUSB0 or /dev/ttyACM0)")
	baud := fs.Uint("baud", 115200, "Serial baud rate")
	mount := fs.String("mount", "/tmp/lakatos", "Linux FUSE mountpoint for /flash, /sd, and /net (empty to disable)")
	reconnect := fs.Bool("reconnect", false, "Reconnect if the USB serial connection drops")
	drain := fs.Duration("drain", 0, "If > 0, exit this duration after stdin reaches EOF (0 = keep streaming logs)")
	fs.Parse(args)

	options := OpenSerialOptions{
		PortName:        *wire,
		BaudRate:        *baud,
		DataBits:        8,
		StopBits:        1,
		MinimumReadSize: 1,
	}

	bridge := NewTetherBridge()
	cleanupFuse := func() {}

	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, os.Interrupt, syscall.SIGTERM)
	go func() {
		<-sigCh
		cleanupFuse()
		fmt.Fprintln(os.Stderr, "\n[tether exiting]")
		os.Exit(0)
	}()

	var fuseStarted sync.Once
	maybeStartFuse := func() {
		fuseStarted.Do(func() {
			if *mount == "" {
				return
			}
			cf, err := startTetherFuse(*mount, bridge)
			if err != nil {
				fmt.Fprintf(os.Stderr, "[tether FUSE warning: %v]\n", err)
				return
			}
			cleanupFuse = cf
			fmt.Fprintf(os.Stderr, "[tether FUSE mounted at %s/{flash,sd,net}]\n", *mount)
		})
	}

	// Goroutine to read cooked lines from stdin and send them to the CYD
	go func() {
		for {
			bridge.portMu.Lock()
			ready := bridge.activePort != nil
			bridge.portMu.Unlock()
			if ready {
				break
			}
			time.Sleep(20 * time.Millisecond)
		}

		// Send any positional arguments as initial commands
		for _, initCmd := range fs.Args() {
			_ = bridge.WriteLine(initCmd)
			time.Sleep(100 * time.Millisecond)
		}

		// If positional commands were supplied with -drain and stdin is a terminal,
		// drain and exit without waiting for interactive stdin EOF.
		if len(fs.Args()) > 0 && *drain > 0 {
			if fi, err := os.Stdin.Stat(); err == nil && (fi.Mode()&os.ModeCharDevice) != 0 {
				time.Sleep(*drain)
				cleanupFuse()
				os.Exit(0)
			}
		}

		reader := bufio.NewReader(os.Stdin)
		for {
			line, err := reader.ReadString('\n')
			if len(line) > 0 {
				if werr := bridge.WriteLine(line); werr != nil {
					fmt.Fprintf(os.Stderr, "[tether write error: %v]\n", werr)
				}
			}
			if err != nil {
				if *drain > 0 {
					time.Sleep(*drain)
					cleanupFuse()
					os.Exit(0)
				}
				return
			}
		}
	}()

	buf := make([]byte, 1024)
	for {
		port, err := OpenSerial(options)
		if err != nil {
			if !*reconnect {
				fmt.Fprintf(os.Stderr, "lakat tether: cannot open %s: %v\n", options.PortName, err)
				os.Exit(1)
			}
			time.Sleep(300 * time.Millisecond)
			continue
		}

		bridge.SetPort(port)
		fmt.Fprintf(os.Stderr, "[tether connected to %s @ %d baud]\n", options.PortName, options.BaudRate)
		maybeStartFuse()

		var lineBuf bytes.Buffer
		capturingFuse := false
		const fusePrefix = "@@FUSE"

		for {
			n, rerr := port.Read(buf)
			if n > 0 {
				var outBytes []byte
				for _, b := range buf[:n] {
					if !capturingFuse {
						if b == '@' {
							capturingFuse = true
							lineBuf.Reset()
							lineBuf.WriteByte(b)
						} else {
							outBytes = append(outBytes, b)
						}
						continue
					}

					lineBuf.WriteByte(b)
					if lineBuf.Len() <= len(fusePrefix) {
						if fusePrefix[:lineBuf.Len()] != lineBuf.String() {
							// Not an @@FUSE packet; flush buffered bytes to stdout.
							// If the latest byte was '@', keep it as the start of a new candidate.
							raw := lineBuf.Bytes()
							if b == '@' && len(raw) > 1 {
								outBytes = append(outBytes, raw[:len(raw)-1]...)
								lineBuf.Reset()
								lineBuf.WriteByte('@')
							} else {
								outBytes = append(outBytes, raw...)
								lineBuf.Reset()
								capturingFuse = false
							}
						}
						continue
					}

					if b == '\n' {
						bridge.handleFuseLine(lineBuf.String())
						lineBuf.Reset()
						capturingFuse = false
					}
				}
				if len(outBytes) > 0 {
					_, _ = os.Stdout.Write(outBytes)
				}
			}
			if rerr != nil {
				break
			}
		}

		bridge.SetPort(nil)
		_ = port.Close()

		if !*reconnect {
			cleanupFuse()
			fmt.Fprintln(os.Stderr, "\n[tether disconnected]")
			return
		}
		fmt.Fprintln(os.Stderr, "\n[tether disconnected; waiting to reconnect...]")
		time.Sleep(300 * time.Millisecond)
	}
}
