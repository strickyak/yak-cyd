//go:build linux

package main

import (
	"context"
	"encoding/base64"
	"fmt"
	"os"
	"os/exec"
	"path"
	"strings"
	"sync"
	"syscall"
	"time"

	"bazil.org/fuse"
	"bazil.org/fuse/fs"
)

type dirCacheRecord struct {
	entries []FuseRemoteEntry
	fetched time.Time
}

type lakatosFS struct {
	bridge *TetherBridge
	uid    uint32
	gid    uint32

	mu       sync.Mutex
	rootNode *lakatosRootNode
	nodes    map[string]*lakatosNode
	dirCache map[string]dirCacheRecord
	entCache map[string]FuseRemoteEntry
}

func startTetherFuse(mountPoint string, bridge *TetherBridge) (func(), error) {
	if mountPoint == "" {
		return func() {}, nil
	}

	// Clean up any stale mount at mountPoint first.
	_ = fuse.Unmount(mountPoint)
	_ = exec.Command("fusermount3", "-u", "-z", mountPoint).Run()
	_ = exec.Command("fusermount", "-u", "-z", mountPoint).Run()

	if err := os.MkdirAll(mountPoint, 0o755); err != nil {
		return nil, fmt.Errorf("mkdir %s: %w", mountPoint, err)
	}

	conn, err := fuse.Mount(
		mountPoint,
		fuse.FSName("lakatos"),
		fuse.Subtype("lakatos"),
	)
	if err != nil {
		return nil, fmt.Errorf("fuse.Mount(%s): %w", mountPoint, err)
	}

	lfs := &lakatosFS{
		bridge:   bridge,
		uid:      uint32(os.Getuid()),
		gid:      uint32(os.Getgid()),
		nodes:    make(map[string]*lakatosNode),
		dirCache: make(map[string]dirCacheRecord),
		entCache: make(map[string]FuseRemoteEntry),
	}
	lfs.rootNode = &lakatosRootNode{fs: lfs}

	go func() {
		_ = fs.Serve(conn, lfs)
	}()

	var once sync.Once
	cleanup := func() {
		once.Do(func() {
			// Note: Do NOT call conn.Close() here; bazil.org/fuse holds Conn.rio.RLock()
			// inside blocking syscall.Read(/dev/fuse) in ReadRequest(), so Conn.Close()
			// deadlocks on Conn.rio.Lock() whenever another process (e.g. a shell cwd)
			// keeps the mount busy and forces a lazy (-z) unmount. Letting os.Exit close
			// the /dev/fuse descriptor in the kernel avoids the mutex deadlock.
			done := make(chan struct{})
			go func() {
				_ = fuse.Unmount(mountPoint)
				_ = exec.Command("fusermount3", "-u", "-z", mountPoint).Run()
				_ = exec.Command("fusermount", "-u", "-z", mountPoint).Run()
				close(done)
			}()
			select {
			case <-done:
			case <-time.After(800 * time.Millisecond):
			}
		})
	}
	return cleanup, nil
}

func (lfs *lakatosFS) Root() (fs.Node, error) {
	return lfs.rootNode, nil
}

func (lfs *lakatosFS) getNode(vfsPath string, isDir bool) *lakatosNode {
	lfs.mu.Lock()
	defer lfs.mu.Unlock()
	if n, ok := lfs.nodes[vfsPath]; ok {
		n.isDir = isDir
		return n
	}
	n := &lakatosNode{
		fs:      lfs,
		vfsPath: vfsPath,
		isDir:   isDir,
	}
	lfs.nodes[vfsPath] = n
	return n
}

func (lfs *lakatosFS) invalidateDir(dirPath string) {
	lfs.mu.Lock()
	defer lfs.mu.Unlock()
	delete(lfs.dirCache, dirPath)
}

func (lfs *lakatosFS) putEntryCache(vfsPath string, ent FuseRemoteEntry) {
	lfs.mu.Lock()
	defer lfs.mu.Unlock()
	lfs.entCache[vfsPath] = ent
}

func (lfs *lakatosFS) getEntryCache(vfsPath string) (FuseRemoteEntry, bool) {
	lfs.mu.Lock()
	defer lfs.mu.Unlock()
	e, ok := lfs.entCache[vfsPath]
	return e, ok
}

func (lfs *lakatosFS) deleteEntryCache(vfsPath string) {
	lfs.mu.Lock()
	defer lfs.mu.Unlock()
	delete(lfs.entCache, vfsPath)
}

func (lfs *lakatosFS) listDir(dirPath string, forceRefresh bool) ([]FuseRemoteEntry, error) {
	if !forceRefresh {
		lfs.mu.Lock()
		rec, ok := lfs.dirCache[dirPath]
		lfs.mu.Unlock()
		if ok && time.Since(rec.fetched) < 3*time.Second {
			return rec.entries, nil
		}
	}

	entries, err := lfs.bridge.FuseLs(dirPath)
	if err != nil {
		return nil, fuseErrno(err)
	}

	lfs.mu.Lock()
	lfs.dirCache[dirPath] = dirCacheRecord{
		entries: entries,
		fetched: time.Now(),
	}
	for _, e := range entries {
		childPath := path.Join(dirPath, e.Name)
		lfs.entCache[childPath] = e
	}
	lfs.mu.Unlock()
	return entries, nil
}

func (lfs *lakatosFS) statPath(vfsPath string) (FuseRemoteEntry, error) {
	if vfsPath == "/flash" || vfsPath == "/sd" || vfsPath == "/net" {
		return FuseRemoteEntry{
			Name:  strings.TrimPrefix(vfsPath, "/"),
			IsDir: true,
			Size:  4096,
		}, nil
	}

	// Check parent directory cache first.
	parentDir := path.Dir(vfsPath)
	baseName := path.Base(vfsPath)
	lfs.mu.Lock()
	rec, ok := lfs.dirCache[parentDir]
	cachedEnt, hasEnt := lfs.entCache[vfsPath]
	lfs.mu.Unlock()

	if ok && time.Since(rec.fetched) < 3*time.Second {
		for _, e := range rec.entries {
			if e.Name == baseName {
				if hasEnt && cachedEnt.Size > e.Size {
					return cachedEnt, nil
				}
				return e, nil
			}
		}
		if hasEnt {
			return cachedEnt, nil
		}
		return FuseRemoteEntry{}, syscall.ENOENT
	}

	if hasEnt {
		return cachedEnt, nil
	}

	st, err := lfs.bridge.FuseStat(vfsPath)
	if err != nil {
		return FuseRemoteEntry{}, syscall.ENOENT
	}
	lfs.putEntryCache(vfsPath, st)
	return st, nil
}

// ---------------------------------------------------------------------------
// Root Node (/tmp/lakatos -> flash/, sd/, net/)
// ---------------------------------------------------------------------------

type lakatosRootNode struct {
	fs *lakatosFS
}

func (r *lakatosRootNode) Attr(ctx context.Context, a *fuse.Attr) error {
	a.Inode = 1
	a.Mode = os.ModeDir | 0o755
	a.Size = 4096
	a.Uid = r.fs.uid
	a.Gid = r.fs.gid
	a.Valid = 5 * time.Second
	return nil
}

func (r *lakatosRootNode) Lookup(ctx context.Context, name string) (fs.Node, error) {
	switch name {
	case "flash", "sd", "net":
		return r.fs.getNode("/"+name, true), nil
	default:
		return nil, syscall.ENOENT
	}
}

func (r *lakatosRootNode) ReadDirAll(ctx context.Context) ([]fuse.Dirent, error) {
	return []fuse.Dirent{
		{Inode: 2, Name: "flash", Type: fuse.DT_Dir},
		{Inode: 3, Name: "sd", Type: fuse.DT_Dir},
		{Inode: 4, Name: "net", Type: fuse.DT_Dir},
	}, nil
}

// ---------------------------------------------------------------------------
// VFS Node (/flash/..., /sd/..., /net/...)
// ---------------------------------------------------------------------------

type lakatosNode struct {
	fs      *lakatosFS
	vfsPath string
	isDir   bool
}

func (n *lakatosNode) Attr(ctx context.Context, a *fuse.Attr) error {
	a.Uid = n.fs.uid
	a.Gid = n.fs.gid
	a.Valid = 1 * time.Second

	if n.vfsPath == "/flash" || n.vfsPath == "/sd" || n.vfsPath == "/net" {
		a.Mode = os.ModeDir | 0o755
		a.Size = 4096
		return nil
	}

	ent, err := n.fs.statPath(n.vfsPath)
	if err != nil {
		return err
	}
	if ent.IsDir {
		a.Mode = os.ModeDir | 0o755
		a.Size = 4096
	} else {
		mode := os.FileMode(0o644)
		if strings.HasSuffix(strings.ToLower(n.vfsPath), ".lapp") {
			mode = 0o755
		}
		a.Mode = mode
		if ent.Size > 0 {
			a.Size = uint64(ent.Size)
		}
	}
	if ent.Mtime > 1000000000 {
		t := time.Unix(ent.Mtime, 0)
		a.Mtime = t
		a.Atime = t
		a.Ctime = t
	}
	return nil
}

func (n *lakatosNode) Lookup(ctx context.Context, name string) (fs.Node, error) {
	if !n.isDir {
		return nil, syscall.ENOTDIR
	}
	entries, err := n.fs.listDir(n.vfsPath, false)
	if err == nil {
		for _, e := range entries {
			if e.Name == name {
				childPath := path.Join(n.vfsPath, name)
				return n.fs.getNode(childPath, e.IsDir), nil
			}
		}
		childPath := path.Join(n.vfsPath, name)
		if ent, ok := n.fs.getEntryCache(childPath); ok {
			return n.fs.getNode(childPath, ent.IsDir), nil
		}
		return nil, syscall.ENOENT
	}

	childPath := path.Join(n.vfsPath, name)
	st, serr := n.fs.bridge.FuseStat(childPath)
	if serr != nil {
		return nil, syscall.ENOENT
	}
	n.fs.putEntryCache(childPath, st)
	return n.fs.getNode(childPath, st.IsDir), nil
}

func (n *lakatosNode) ReadDirAll(ctx context.Context) ([]fuse.Dirent, error) {
	if !n.isDir {
		return nil, syscall.ENOTDIR
	}
	entries, err := n.fs.listDir(n.vfsPath, true)
	if err != nil {
		return nil, err
	}
	out := make([]fuse.Dirent, 0, len(entries))
	for _, e := range entries {
		dt := fuse.DT_File
		if e.IsDir {
			dt = fuse.DT_Dir
		}
		out = append(out, fuse.Dirent{
			Name: e.Name,
			Type: dt,
		})
	}
	return out, nil
}

func fuseErrno(err error) error {
	if err == nil {
		return nil
	}
	msg := err.Error()
	switch {
	case strings.HasSuffix(msg, "EACCES"), strings.HasSuffix(msg, "EPERM"):
		return syscall.EACCES
	case strings.HasSuffix(msg, "ENOENT"):
		return syscall.ENOENT
	case strings.HasSuffix(msg, "EEXIST"):
		return syscall.EEXIST
	case strings.HasSuffix(msg, "EINVAL"):
		return syscall.EINVAL
	case strings.HasSuffix(msg, "EFBIG"):
		return syscall.EFBIG
	case strings.HasSuffix(msg, "ENOMEM"):
		return syscall.ENOMEM
	case strings.Contains(msg, "ENOMEDIUM"):
		return syscall.ENOMEDIUM
	case strings.Contains(msg, "ENODEV"):
		return syscall.ENODEV
	default:
		return syscall.EIO
	}
}

func (n *lakatosNode) Open(ctx context.Context, req *fuse.OpenRequest, resp *fuse.OpenResponse) (fs.Handle, error) {
	if n.isDir {
		return n, nil
	}
	writable := req.Flags.IsWriteOnly() || req.Flags.IsReadWrite()
	dirty := false
	if writable && (req.Flags&fuse.OpenTruncate != 0) {
		if err := n.fs.bridge.FuseWriteChunk(n.vfsPath, "W", 0, nil); err != nil {
			return nil, fuseErrno(err)
		}
		n.fs.putEntryCache(n.vfsPath, FuseRemoteEntry{
			Name:  path.Base(n.vfsPath),
			IsDir: false,
			Size:  0,
			Mtime: time.Now().Unix(),
		})
		dirty = true
	}
	return &lakatosFileHandle{
		node:     n,
		writable: writable,
		dirty:    dirty,
	}, nil
}

func (n *lakatosNode) Create(ctx context.Context, req *fuse.CreateRequest, resp *fuse.CreateResponse) (fs.Node, fs.Handle, error) {
	if !n.isDir {
		return nil, nil, syscall.ENOTDIR
	}
	childPath := path.Join(n.vfsPath, req.Name)
	if err := n.fs.bridge.FuseWriteChunk(childPath, "W", 0, nil); err != nil {
		return nil, nil, fuseErrno(err)
	}
	ent := FuseRemoteEntry{
		Name:  req.Name,
		IsDir: false,
		Size:  0,
		Mtime: time.Now().Unix(),
	}
	n.fs.putEntryCache(childPath, ent)
	n.fs.invalidateDir(n.vfsPath)

	childNode := n.fs.getNode(childPath, false)
	fh := &lakatosFileHandle{
		node:     childNode,
		writable: true,
		dirty:    true,
	}
	return childNode, fh, nil
}

func (n *lakatosNode) Setattr(ctx context.Context, req *fuse.SetattrRequest, resp *fuse.SetattrResponse) error {
	if req.Valid.Size() && req.Size == 0 && !n.isDir {
		if err := n.fs.bridge.FuseWriteChunk(n.vfsPath, "W", 0, nil); err != nil {
			return fuseErrno(err)
		}
		n.fs.putEntryCache(n.vfsPath, FuseRemoteEntry{
			Name:  path.Base(n.vfsPath),
			IsDir: false,
			Size:  0,
			Mtime: time.Now().Unix(),
		})
		n.fs.invalidateDir(path.Dir(n.vfsPath))
	}
	return n.Attr(ctx, &resp.Attr)
}

func (n *lakatosNode) Remove(ctx context.Context, req *fuse.RemoveRequest) error {
	childPath := path.Join(n.vfsPath, req.Name)
	var err error
	if req.Dir {
		err = n.fs.bridge.FuseRmdir(childPath)
	} else {
		err = n.fs.bridge.FuseRm(childPath)
	}
	if err != nil {
		return fuseErrno(err)
	}
	n.fs.deleteEntryCache(childPath)
	n.fs.invalidateDir(n.vfsPath)
	return nil
}

func (n *lakatosNode) Mkdir(ctx context.Context, req *fuse.MkdirRequest) (fs.Node, error) {
	childPath := path.Join(n.vfsPath, req.Name)
	if err := n.fs.bridge.FuseMkdir(childPath); err != nil {
		return nil, fuseErrno(err)
	}
	ent := FuseRemoteEntry{
		Name:  req.Name,
		IsDir: true,
		Size:  4096,
		Mtime: time.Now().Unix(),
	}
	n.fs.putEntryCache(childPath, ent)
	n.fs.invalidateDir(n.vfsPath)
	return n.fs.getNode(childPath, true), nil
}

// ---------------------------------------------------------------------------
// File Handle (Chunked Read/Write over USB Serial)
// ---------------------------------------------------------------------------

type lakatosFileHandle struct {
	mu       sync.Mutex
	node     *lakatosNode
	writable bool
	dirty    bool
	wroteAny bool
}

func (fh *lakatosFileHandle) Read(ctx context.Context, req *fuse.ReadRequest, resp *fuse.ReadResponse) error {
	fh.mu.Lock()
	defer fh.mu.Unlock()

	const maxChunk = 1024
	toRead := int(req.Size)
	offset := uint32(req.Offset)
	out := make([]byte, 0, toRead)

	for len(out) < toRead {
		chunk := toRead - len(out)
		if chunk > maxChunk {
			chunk = maxChunk
		}
		data, err := fh.node.fs.bridge.FuseReadChunk(fh.node.vfsPath, offset, uint32(chunk))
		if err != nil {
			return fuseErrno(err)
		}
		if len(data) == 0 {
			break
		}
		out = append(out, data...)
		offset += uint32(len(data))
		if len(data) < chunk {
			break
		}
	}

	resp.Data = out
	return nil
}

func (fh *lakatosFileHandle) Write(ctx context.Context, req *fuse.WriteRequest, resp *fuse.WriteResponse) error {
	fh.mu.Lock()
	defer fh.mu.Unlock()

	const maxChunk = 512
	data := req.Data
	offset := uint32(req.Offset)
	written := 0

	for written < len(data) {
		chunk := len(data) - written
		if chunk > maxChunk {
			chunk = maxChunk
		}
		mode := "A"
		if offset == 0 && !fh.wroteAny {
			mode = "W"
		}
		if err := fh.node.fs.bridge.FuseWriteChunk(fh.node.vfsPath, mode, offset, data[written:written+chunk]); err != nil {
			return fuseErrno(err)
		}
		fh.wroteAny = true
		fh.dirty = true
		written += chunk
		offset += uint32(chunk)
	}

	newSize := req.Offset + int64(len(data))
	if old, ok := fh.node.fs.getEntryCache(fh.node.vfsPath); !ok || newSize > old.Size {
		fh.node.fs.putEntryCache(fh.node.vfsPath, FuseRemoteEntry{
			Name:  path.Base(fh.node.vfsPath),
			IsDir: false,
			Size:  newSize,
			Mtime: time.Now().Unix(),
		})
	}

	resp.Size = len(data)
	return nil
}

func (fh *lakatosFileHandle) Flush(ctx context.Context, req *fuse.FlushRequest) error {
	fh.mu.Lock()
	defer fh.mu.Unlock()
	if !fh.dirty {
		return nil
	}
	if err := fh.node.fs.bridge.FuseCommit(fh.node.vfsPath); err != nil {
		fh.dirty = false
		fh.node.fs.deleteEntryCache(fh.node.vfsPath)
		fh.node.fs.invalidateDir(path.Dir(fh.node.vfsPath))
		return fuseErrno(err)
	}
	fh.dirty = false
	fh.node.fs.invalidateDir(path.Dir(fh.node.vfsPath))
	return nil
}

func (fh *lakatosFileHandle) Release(ctx context.Context, req *fuse.ReleaseRequest) error {
	fh.mu.Lock()
	defer fh.mu.Unlock()
	if fh.dirty {
		_ = fh.node.fs.bridge.FuseCommit(fh.node.vfsPath)
		fh.dirty = false
		fh.node.fs.invalidateDir(path.Dir(fh.node.vfsPath))
	}
	return nil
}

// Unused import guard for base64 if needed
var _ = base64.StdEncoding
