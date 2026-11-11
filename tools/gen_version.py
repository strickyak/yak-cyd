#!/usr/bin/env python3
"""Generates src/lakatos/lakatos_version.h with the current 64-bit Unix seconds timestamp."""

import pathlib
import time

def main():
    sec = int(time.time())
    out_path = pathlib.Path(__file__).resolve().parent.parent / "src" / "lakatos" / "lakatos_version.h"
    content = (
        "#pragma once\n"
        "#include <stdint.h>\n\n"
        f"#define LAKATOS_KERNEL_VERSION_SEC ((int64_t){sec}LL)\n"
    )
    out_path.write_text(content, encoding="utf-8")
    print(f"[Lakatos Version] LAKATOS_KERNEL_VERSION_SEC = {sec}")

if __name__ == "__main__":
    main()
