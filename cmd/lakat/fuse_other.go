//go:build !linux

package main

func startTetherFuse(mountPoint string, bridge *TetherBridge) (func(), error) {
	return func() {}, nil
}
