#!/usr/bin/env python3
"""
Compiles the multi-headed native binary (build/apps/native.bin) containing all
4 native Lakatos applications (piano, modtrack, midiplay, radio) using
xtensa-esp32-elf-g++ and ld/lakatos_app.ld, producing:
  - build/apps/native.elf
  - build/apps/native.bin (128-byte LakatosAppHeader + .text + .rodata + .data)
"""
import hashlib
import os
import struct
import subprocess
import tempfile

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_DIR = os.path.dirname(SCRIPT_DIR)
INCLUDE_DIR = os.path.join(REPO_DIR, "include")
LD_SCRIPT = os.path.join(REPO_DIR, "ld", "lakatos_app.ld")
BUILD_DIR = os.path.join(REPO_DIR, "build", "apps")

TOOLCHAIN_BIN = os.path.expanduser(
    "~/.platformio/packages/toolchain-xtensa-esp-elf/bin"
)
CXX = os.path.join(TOOLCHAIN_BIN, "xtensa-esp32-elf-g++")
NM = os.path.join(TOOLCHAIN_BIN, "xtensa-esp32-elf-nm")
OBJCOPY = os.path.join(TOOLCHAIN_BIN, "xtensa-esp32-elf-objcopy")

LAKATOS_APP_MAGIC = 0x414B414C  # "LAKA"
LAKATOS_HEADER_VERSION = 1
LAKATOS_ABI_VERSION = 1

CRT0_SRC = r"""
#include "lakatos_api.h"
#include <stdarg.h>

extern "C" {
typedef void (*init_fn_t)(void);
extern init_fn_t __init_array_start[];
extern init_fn_t __init_array_end[];

void piano_main(const LakatosApi* os);
void modtrack_main(const LakatosApi* os);
void midiplay_main(const LakatosApi* os);
void radio_main(const LakatosApi* os);

int strcmp(const char* a, const char* b);

void app_main(const LakatosApi* os) {
  const char* arg = lakatos_app_arg();
  if (strcmp(arg, "piano") == 0) {
    piano_main(os);
  } else if (strcmp(arg, "modtrack") == 0) {
    modtrack_main(os);
  } else if (strcmp(arg, "midiplay") == 0) {
    midiplay_main(os);
  } else if (strcmp(arg, "radio") == 0) {
    radio_main(os);
  }
}

__attribute__((section(".entry.text"), used))
void _lakatos_app_entry(const LakatosApi* os) {
  *(const LakatosApi**)(uintptr_t)LAKATOS_APP_DRAM_BASE = os;
  for (init_fn_t* fn = __init_array_start; fn < __init_array_end; ++fn) {
    if (*fn) (*fn)();
  }
  app_main(os);
  os->exit(0);
}

void* memcpy(void* dst, const void* src, size_t n) {
  if ((int32_t)n <= 0 || n > 0x100000u) return dst;
  uint8_t* d = (uint8_t*)dst;
  const uint8_t* s = (const uint8_t*)src;
  for (size_t i = 0; i < n; i++) d[i] = s[i];
  return dst;
}

void* memmove(void* dst, const void* src, size_t n) {
  if ((int32_t)n <= 0 || n > 0x100000u) return dst;
  uint8_t* d = (uint8_t*)dst;
  const uint8_t* s = (const uint8_t*)src;
  if (d < s) {
    for (size_t i = 0; i < n; i++) d[i] = s[i];
  } else if (d > s) {
    for (size_t i = n; i > 0; i--) d[i - 1] = s[i - 1];
  }
  return dst;
}

void* memset(void* dst, int c, size_t n) {
  if ((int32_t)n <= 0 || n > 0x100000u) return dst;
  uint8_t* d = (uint8_t*)dst;
  for (size_t i = 0; i < n; i++) d[i] = (uint8_t)c;
  return dst;
}

int memcmp(const void* a, const void* b, size_t n) {
  const uint8_t* pa = (const uint8_t*)a;
  const uint8_t* pb = (const uint8_t*)b;
  for (size_t i = 0; i < n; i++) {
    if (pa[i] != pb[i]) return (int)pa[i] - (int)pb[i];
  }
  return 0;
}

size_t strlen(const char* s) {
  size_t n = 0;
  while (s[n]) n++;
  return n;
}

int strcmp(const char* a, const char* b) {
  while (*a && (*a == *b)) { a++; b++; }
  return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

int strncmp(const char* a, const char* b, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (a[i] != b[i] || a[i] == 0) return (int)(uint8_t)a[i] - (int)(uint8_t)b[i];
  }
  return 0;
}

char* strcpy(char* dst, const char* src) {
  char* d = dst;
  while ((*d++ = *src++) != 0) {}
  return dst;
}

char* strncpy(char* dst, const char* src, size_t n) {
  size_t i = 0;
  for (; i < n && src[i] != 0; i++) dst[i] = src[i];
  for (; i < n; i++) dst[i] = 0;
  return dst;
}

void* malloc(size_t sz) { return lakatos_os()->malloc(sz); }
void* calloc(size_t n, size_t sz) { return lakatos_os()->calloc(n, sz); }
void* realloc(void* p, size_t sz) { return lakatos_os()->realloc(p, sz); }
void  free(void* p) { lakatos_os()->free(p); }

int vsnprintf(char* buf, size_t size, const char* fmt, va_list ap) {
  return lakatos_os()->vsnprintf(buf, size, fmt, ap);
}

int snprintf(char* buf, size_t size, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = lakatos_os()->vsnprintf(buf, size, fmt, ap);
  va_end(ap);
  return r;
}
}
"""


def get_symbols(elf_path):
    out = subprocess.check_output([NM, "-n", elf_path], text=True)
    syms = {}
    for line in out.splitlines():
        parts = line.strip().split()
        if len(parts) >= 3:
            addr_str, _sym_type, name = parts[0], parts[1], parts[2]
            try:
                syms[name] = int(addr_str, 16)
            except ValueError:
                pass
    return syms


def dump_section(elf_path, sec_name):
    with tempfile.NamedTemporaryFile(delete=False) as tf:
        tmp_path = tf.name
    try:
        subprocess.run(
            [OBJCOPY, "--dump-section", f"{sec_name}={tmp_path}", elf_path],
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        if os.path.exists(tmp_path):
            with open(tmp_path, "rb") as f:
                data = f.read()
            return data
        return b""
    finally:
        if os.path.exists(tmp_path):
            os.remove(tmp_path)


def build_native_bin(heads, extra_libs=None):
    os.makedirs(BUILD_DIR, exist_ok=True)
    elf_path = os.path.join(BUILD_DIR, "native.elf")
    bin_path = os.path.join(BUILD_DIR, "native.bin")

    common_flags = [
        "-mlongcalls",
        "-mtext-section-literals",
        "-ffunction-sections",
        "-fdata-sections",
        "-fno-exceptions",
        "-fno-rtti",
        "-fno-threadsafe-statics",
        "-fno-common",
        "-fno-builtin",
        "-fno-tree-loop-distribute-patterns",
        "-Os",
        "-Wall",
        f"-I{INCLUDE_DIR}",
    ]

    with tempfile.TemporaryDirectory() as tmpdir:
        crt0_cpp = os.path.join(tmpdir, "crt0.cpp")
        crt0_obj = os.path.join(tmpdir, "crt0.o")
        with open(crt0_cpp, "w") as f:
            f.write(CRT0_SRC)
        subprocess.run([CXX] + common_flags + ["-c", crt0_cpp, "-o", crt0_obj], check=True)

        obj_files = [crt0_obj]
        for head_name, src_path, head_flags in heads:
            obj_path = os.path.join(tmpdir, f"{head_name}.o")
            cmd_compile = (
                [CXX]
                + common_flags
                + [f"-Dapp_main={head_name}_main"]
                + (head_flags or [])
                + ["-c", src_path, "-o", obj_path]
            )
            subprocess.run(cmd_compile, check=True)
            obj_files.append(obj_path)

        cmd_link = (
            [CXX]
            + common_flags
            + obj_files
            + (extra_libs or [])
            + [
                "-nostdlib",
                "-Wl,-T," + LD_SCRIPT,
                "-Wl,--gc-sections",
                "-lm",
                "-lgcc",
                "-o",
                elf_path,
            ]
        )
        subprocess.run(cmd_link, check=True)

    syms = get_symbols(elf_path)
    entry_addr = syms["_lakatos_app_entry"]
    text_start = syms["_text_start"]
    text_end = syms["_text_end"]
    rodata_start = syms["_rodata_start"]
    rodata_end = syms["_rodata_end"]
    data_start = syms["_data_start"]
    data_end = syms["_data_end"]
    bss_start = syms["_bss_start"]
    bss_end = syms["_bss_end"]

    text_bytes = dump_section(elf_path, ".text")
    rodata_bytes = dump_section(elf_path, ".rodata")
    data_bytes = dump_section(elf_path, ".data")

    # Pad sections to match exact linker-aligned sizes
    text_size = text_end - text_start
    rodata_size = rodata_end - rodata_start
    data_size = data_end - data_start
    bss_size = bss_end - bss_start

    assert len(text_bytes) <= text_size
    text_bytes = text_bytes.ljust(text_size, b"\x00")
    assert len(rodata_bytes) <= rodata_size
    rodata_bytes = rodata_bytes.ljust(rodata_size, b"\x00")
    assert len(data_bytes) <= data_size
    data_bytes = data_bytes.ljust(data_size, b"\x00")

    text_offset = 0x80
    rodata_offset = text_offset + text_size
    data_offset = rodata_offset + rodata_size

    assert text_start == 0x40300000 + text_offset, (
        f"text_start {hex(text_start)} != {hex(0x40300000 + text_offset)}"
    )
    assert rodata_start == 0x3F700000 + rodata_offset, (
        f"rodata_start {hex(rodata_start)} != {hex(0x3F700000 + rodata_offset)}"
    )

    payload = text_bytes + rodata_bytes + data_bytes
    total_file_size = 0x80 + len(payload)
    sha256_digest = hashlib.sha256(payload).digest()

    title = "native.bin"
    name_bytes = title.encode("utf-8")[:31].ljust(32, b"\x00")

    header = struct.pack(
        "<14I32s32s2I",
        LAKATOS_APP_MAGIC,
        LAKATOS_HEADER_VERSION,
        LAKATOS_ABI_VERSION,
        entry_addr,
        text_offset,
        text_size,
        rodata_offset,
        rodata_size,
        data_offset,
        data_start,
        data_size,
        bss_start,
        bss_size,
        total_file_size,
        sha256_digest,
        name_bytes,
        0,
        0,
    )
    assert len(header) == 128

    bin_bytes = header + payload
    with open(bin_path, "wb") as f:
        f.write(bin_bytes)

    head_list = ", ".join(h[0] for h in heads)
    print(
        f"[Lakatos Native Pack] Built {bin_path} ({total_file_size} bytes) [{head_list}]\n"
        f"  entry  = 0x{entry_addr:08X}\n"
        f"  .text  = 0x{text_start:08X} ({text_size} B, file+0x{text_offset:X})\n"
        f"  .rodata= 0x{rodata_start:08X} ({rodata_size} B, file+0x{rodata_offset:X})\n"
        f"  .data  = 0x{data_start:08X} ({data_size} B, file+0x{data_offset:X})\n"
        f"  .bss   = 0x{bss_start:08X} ({bss_size} B, overlaid across {len(heads)} heads)"
    )
    return bin_path, bin_bytes


def main():
    esp32_libs_dir = os.path.expanduser(
        "~/.platformio/packages/framework-arduinoespressif32-esp32-libs"
    )
    helix_inc = os.path.join(
        esp32_libs_dir, "include", "chmorgan__esp-libhelix-mp3", "libhelix-mp3", "pub"
    )
    helix_lib = os.path.join(esp32_libs_dir, "lib", "libchmorgan__esp-libhelix-mp3.a")

    heads = [
        ("piano", os.path.join(REPO_DIR, "apps", "piano", "piano.cpp"), []),
        ("modtrack", os.path.join(REPO_DIR, "apps", "modtrack", "modtrack.cpp"), []),
        ("midiplay", os.path.join(REPO_DIR, "apps", "midiplay", "midiplay.cpp"), []),
        ("radio", os.path.join(REPO_DIR, "apps", "radio", "radio.cpp"), [f"-I{helix_inc}"]),
    ]
    build_native_bin(heads, [helix_lib])


if __name__ == "__main__":
    main()
