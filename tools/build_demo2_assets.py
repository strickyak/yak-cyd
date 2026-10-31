#!/usr/bin/env python3
"""
Builds the 1 MB OS-9 formatted disk image (4096 x 256-byte sectors) with Basic09
and a compact set of essential CMDS, then generates src/demo2/os9_assets.h
containing:
  1. kDeep65280Img[65536] (64 KB boot image for deep65280v2)
  2. Sparse 4 KB blocks of the formatted 1 MB OS-9 disk image for fast
     on-device initialization of the 1 MB flash partition.
"""
import hashlib
import os
import subprocess
import tempfile

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_DIR = os.path.dirname(SCRIPT_DIR)
OUT_HEADER = os.path.join(REPO_DIR, "src", "demo2", "os9_assets.h")

OS9_BIN = "/home/strick/modoc/coco-shelf/bin/os9"
RECIPE_DIR = "/home/strick/modoc/coco-shelf/nitros9/recipes/deep65280"
MOD_DIR = os.path.join(RECIPE_DIR, ".mods")
BOOT_IMG_PATH = os.path.join(RECIPE_DIR, "deep65280.img")

BASIC09_DIR = "/home/strick/modoc/coco-shelf/nitros9-languages/basic09"
BASIC09_BIN = os.path.join(BASIC09_DIR, "basic09_6809")
RUNB_BIN = os.path.join(BASIC09_DIR, "runb_6809")

# Compact set of essential Level 2 commands
CMDS = [
    "attr", "cmp", "copy", "date", "del", "deldir", "devs", "dir",
    "dmem", "dump", "echo", "edit", "error", "free", "help", "ident",
    "link", "list", "load", "makdir", "mdir", "mfree", "mmap", "more",
    "pmap", "proc", "procs", "pwd", "pxd", "rename", "save", "setime",
    "shell", "sleep", "smap", "stop", "tee", "tmode", "touch", "unlink",
    "xmode",
]

# Additional Basic09 helper modules
B09_EXTRAS = [
    ("gfx", os.path.join(BASIC09_DIR, "gfx")),
    ("gfx2", os.path.join(BASIC09_DIR, "gfx2")),
    ("inkey", os.path.join(BASIC09_DIR, "inkey")),
    ("syscall", os.path.join(BASIC09_DIR, "syscall")),
]

DISK_SECTORS = 4096  # 4096 * 256 = 1,048,576 bytes (1.00 MB)


def run_cmd(args):
    subprocess.run(args, check=True)


def build_disk(dsk_path):
    if os.path.exists(dsk_path):
        os.remove(dsk_path)

    print(f"Formatting 1 MB OS-9 disk ({DISK_SECTORS} sectors): {dsk_path}")
    run_cmd([OS9_BIN, "format", "-e", "-nCYDOS9", f"-l{DISK_SECTORS}", dsk_path])
    run_cmd([OS9_BIN, "makdir", f"{dsk_path},CMDS"])

    installed = []
    for cmd in CMDS:
        src = os.path.join(MOD_DIR, cmd)
        if os.path.isfile(src):
            run_cmd([OS9_BIN, "copy", "-o=0", src, f"{dsk_path},CMDS/{cmd}"])
            installed.append(cmd)
        else:
            print(f"Warning: missing command {src}")

    if os.path.isfile(BASIC09_BIN):
        run_cmd([OS9_BIN, "copy", "-o=0", BASIC09_BIN, f"{dsk_path},CMDS/basic09"])
        installed.append("basic09")

    if os.path.isfile(RUNB_BIN):
        run_cmd([OS9_BIN, "copy", "-o=0", RUNB_BIN, f"{dsk_path},CMDS/runb"])
        installed.append("runb")

    for name, src in B09_EXTRAS:
        if os.path.isfile(src):
            run_cmd([OS9_BIN, "copy", "-o=0", src, f"{dsk_path},CMDS/{name}"])
            installed.append(name)

    attr_targets = [f"{dsk_path},CMDS/{c}" for c in installed]
    run_cmd([OS9_BIN, "attr", "-q", "-pe", "-npw", "-pr", "-e", "-w", "-r"] + attr_targets)

    # Create startup script
    with tempfile.NamedTemporaryFile("w", delete=False) as tf:
        tf.write("tmode pau=0 </2\n")
        tf.write("setime 2026/10/31 12:00:00\n")
        tf.write("echo * NitrOS-9 Level 2 on CYD (6809 / deep65280v2) *\n")
        tf.write("echo * Type 'basic09' for Basic09, or tap screen for keyboard *\n")
        startup_tmp = tf.name
    run_cmd([OS9_BIN, "copy", "-o=0", "-l", startup_tmp, f"{dsk_path},startup"])
    run_cmd([OS9_BIN, "attr", "-q", "-npw", "-pr", "-ne", "-w", "-r", f"{dsk_path},startup"])
    os.remove(startup_tmp)

    # Create sample Basic09 procedure file
    with tempfile.NamedTemporaryFile("w", delete=False) as tf:
        tf.write("PROCEDURE hello\n")
        tf.write("  FOR i=1 TO 5\n")
        tf.write('    PRINT "Hello from Basic09 on ESP32 CYD! #"; i\n')
        tf.write("  NEXT i\n")
        tf.write("END\n")
        b09_tmp = tf.name
    run_cmd([OS9_BIN, "copy", "-o=0", "-l", b09_tmp, f"{dsk_path},hello.b09"])
    os.remove(b09_tmp)

    print(f"Installed {len(installed)} modules in /d0/CMDS.")


def format_c_array(data, indent="  ", per_line=16):
    lines = []
    for i in range(0, len(data), per_line):
        chunk = data[i:i + per_line]
        hex_bytes = ", ".join(f"0x{b:02X}" for b in chunk)
        lines.append(f"{indent}{hex_bytes},")
    return "\n".join(lines)


def main():
    with open(BOOT_IMG_PATH, "rb") as f:
        boot_img = f.read()
    assert len(boot_img) == 65536, f"Expected 65536 byte boot image, got {len(boot_img)}"

    with tempfile.TemporaryDirectory() as tmpdir:
        dsk_path = os.path.join(tmpdir, "cyd_os9.dsk")
        build_disk(dsk_path)
        with open(dsk_path, "rb") as f:
            dsk_data = bytearray(f.read())

    assert len(dsk_data) == DISK_SECTORS * 256, (
        f"Expected {DISK_SECTORS * 256} bytes, got {len(dsk_data)}"
    )

    # Stamp a 16-byte SHA-256 build signature into unused bytes 0x70..0x7F of LSN 0
    build_sig = hashlib.sha256(dsk_data).digest()[:16]
    dsk_data[0x70:0x80] = build_sig
    dsk_data = bytes(dsk_data)

    # Split 1 MB disk into 256 x 4096-byte flash erase blocks.
    # Unused sectors formatted by `os9 format` are filled with 0xE5.
    empty_block = bytes([0xE5] * 4096)
    non_empty_blocks = []
    for blk_idx in range(len(dsk_data) // 4096):
        blk = dsk_data[blk_idx * 4096:(blk_idx + 1) * 4096]
        if blk != empty_block:
            non_empty_blocks.append((blk_idx, blk))

    print(
        f"Disk size: {len(dsk_data)} bytes ({len(dsk_data) // 4096} x 4KB blocks); "
        f"Non-empty blocks: {len(non_empty_blocks)} ({len(non_empty_blocks) * 4} KB)"
    )

    with open(OUT_HEADER, "w") as out:
        out.write("// Auto-generated by tools/build_demo2_assets.py - DO NOT EDIT\n")
        out.write("#pragma once\n")
        out.write("#ifdef ARDUINO\n")
        out.write("#include <Arduino.h>\n")
        out.write("#else\n")
        out.write("#ifndef PROGMEM\n")
        out.write("#define PROGMEM\n")
        out.write("#endif\n")
        out.write("#endif\n")
        out.write("#include <stddef.h>\n")
        out.write("#include <stdint.h>\n\n")
        out.write(f"static constexpr uint32_t kOs9DiskTotalSectors = {DISK_SECTORS};\n")
        out.write(f"static constexpr uint32_t kOs9DiskTotalBytes = {DISK_SECTORS * 256};\n\n")

        # Write LSN0 first 16 bytes + 16-byte build signature at 0x70..0x7F
        lsn0_sig = ", ".join(f"0x{b:02X}" for b in dsk_data[:16])
        build_sig_c = ", ".join(f"0x{b:02X}" for b in build_sig)
        out.write(f"static const uint8_t kOs9DiskLsn0Sig[16] = {{ {lsn0_sig} }};\n")
        out.write(f"static const uint8_t kOs9DiskBuildSig[16] = {{ {build_sig_c} }};\n\n")

        out.write("static const uint8_t kDeep65280Img[65536] PROGMEM = {\n")
        out.write(format_c_array(boot_img))
        out.write("\n};\n\n")

        out.write("struct Os9FlashBlockSeed {\n")
        out.write("  uint16_t blockIndex;\n")
        out.write("  uint8_t data[4096];\n")
        out.write("};\n\n")

        out.write(
            f"static constexpr size_t kOs9InitialBlockCount = {len(non_empty_blocks)};\n"
        )
        out.write(
            "static const Os9FlashBlockSeed kOs9InitialBlocks[] PROGMEM = {\n"
        )
        for blk_idx, blk in non_empty_blocks:
            out.write(f"  {{ {blk_idx}, {{\n")
            out.write(format_c_array(blk, indent="    "))
            out.write("\n  } },\n")
        out.write("};\n")

    print(f"Wrote {OUT_HEADER} successfully.")


if __name__ == "__main__":
    main()
