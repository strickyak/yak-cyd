#!/usr/bin/env python3
"""Packs .pio/build/lakatos/firmware.bin (+ updater.bin) into build/apps/lakatos.krn."""

import hashlib
import pathlib
import re
import struct
import zlib

LAKATOS_KERNEL_MAGIC = 0x4E524B4C  # "LKRN" in little-endian

def read_version_sec(ver_h_path: pathlib.Path) -> int:
    text = ver_h_path.read_text(encoding="utf-8")
    m = re.search(r"LAKATOS_KERNEL_VERSION_SEC\s+\(\(int64_t\)(\d+)LL\)", text)
    if not m:
        raise RuntimeError(f"Could not parse LAKATOS_KERNEL_VERSION_SEC in {ver_h_path}")
    return int(m.group(1))

def main():
    root = pathlib.Path(__file__).resolve().parent.parent
    ver_h = root / "src" / "lakatos" / "lakatos_version.h"
    kernel_bin = root / ".pio" / "build" / "lakatos" / "firmware.bin"
    updater_bin = root / ".pio" / "build" / "updater" / "firmware.bin"
    out_krn = root / "build" / "apps" / "lakatos.krn"

    ver_sec = read_version_sec(ver_h)
    kernel_bytes = kernel_bin.read_bytes()
    updater_bytes = updater_bin.read_bytes() if updater_bin.exists() else b""

    kernel_sha256 = hashlib.sha256(kernel_bytes).digest()
    updater_crc32 = (zlib.crc32(updater_bytes) & 0xFFFFFFFF) if updater_bytes else 0

    hdr = struct.pack(
        "<IIqII32sII",
        LAKATOS_KERNEL_MAGIC,
        1,
        ver_sec,
        len(kernel_bytes),
        len(updater_bytes),
        kernel_sha256,
        updater_crc32,
        0,
    )
    assert len(hdr) == 64

    out_krn.parent.mkdir(parents=True, exist_ok=True)
    out_krn.write_bytes(hdr + kernel_bytes + updater_bytes)
    print(
        f"[Lakatos Kernel Pack] Built {out_krn} "
        f"(version={ver_sec}, kernel={len(kernel_bytes)} B, updater={len(updater_bytes)} B, "
        f"total={len(hdr) + len(kernel_bytes) + len(updater_bytes)} B)"
    )

if __name__ == "__main__":
    main()
