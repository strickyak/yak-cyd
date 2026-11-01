package main

import (
	"bufio"
	"flag"
	"fmt"
	"io"
	"os"
	"strings"
	"sync"
	"time"
)

func cmdTether(args []string) {
	fs := flag.NewFlagSet("tether", flag.ExitOnError)
	wire := fs.String("wire", "/dev/ttyUSB0", "Serial device connected to CYD (e.g. /dev/ttyUSB0 or /dev/ttyACM0)")
	baud := fs.Uint("baud", 115200, "Serial baud rate")
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

	var portMu sync.Mutex
	var activePort io.ReadWriteCloser

	writeToPort := func(data []byte) error {
		portMu.Lock()
		p := activePort
		portMu.Unlock()
		if p == nil {
			return fmt.Errorf("serial port not connected")
		}
		_, err := p.Write(data)
		return err
	}

	// Goroutine to read cooked lines from stdin and send them to the CYD
	go func() {
		// Wait until initial port connection is ready
		for {
			portMu.Lock()
			ready := activePort != nil
			portMu.Unlock()
			if ready {
				break
			}
			time.Sleep(20 * time.Millisecond)
		}

		// Send any positional arguments as initial commands
		for _, initCmd := range fs.Args() {
			cmd := strings.TrimRight(initCmd, "\r\n") + "\n"
			_ = writeToPort([]byte(cmd))
			time.Sleep(100 * time.Millisecond)
		}

		// If positional commands were supplied with -drain and stdin is a terminal,
		// drain and exit without waiting for interactive stdin EOF.
		if len(fs.Args()) > 0 && *drain > 0 {
			if fi, err := os.Stdin.Stat(); err == nil && (fi.Mode()&os.ModeCharDevice) != 0 {
				time.Sleep(*drain)
				os.Exit(0)
			}
		}

		reader := bufio.NewReader(os.Stdin)
		for {
			line, err := reader.ReadString('\n')
			if len(line) > 0 {
				clean := strings.TrimRight(line, "\r\n") + "\n"
				if werr := writeToPort([]byte(clean)); werr != nil {
					fmt.Fprintf(os.Stderr, "[tether write error: %v]\n", werr)
				}
			}
			if err != nil {
				if *drain > 0 {
					time.Sleep(*drain)
					// Exit directly without calling activePort.Close(), which would block
					// waiting for the blocking Read() syscall on the main goroutine.
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

		portMu.Lock()
		activePort = port
		portMu.Unlock()

		fmt.Fprintf(os.Stderr, "[tether connected to %s @ %d baud]\n", options.PortName, options.BaudRate)

		for {
			n, rerr := port.Read(buf)
			if n > 0 {
				_, _ = os.Stdout.Write(buf[:n])
			}
			if rerr != nil {
				break
			}
		}

		portMu.Lock()
		activePort = nil
		portMu.Unlock()
		_ = port.Close()

		if !*reconnect {
			fmt.Fprintln(os.Stderr, "\n[tether disconnected]")
			return
		}
		fmt.Fprintln(os.Stderr, "\n[tether disconnected; waiting to reconnect...]")
		time.Sleep(300 * time.Millisecond)
	}
}
