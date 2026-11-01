// THIS FILE WAS MODIFIED BY Henry Strickland (github: strickyak)
// IN THE FOLLOWING WAY:
//   The package name was chanaged from `serial` to `main`.
//   Rename type OpenOptions to OpenSerialOptions.
// See serial.LICENSE

package main

import (
	"errors"
	"io"
)

func openInternal(options OpenSerialOptions) (io.ReadWriteCloser, error) {
	return nil, errors.New("not implemented on this OS")
}
