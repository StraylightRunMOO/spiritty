#!/usr/bin/env python3
"""Pack a TTF into a C header of unsigned bytes.

Usage:
    python3 scripts/embed_ttf.py IN.ttf OUT.h
"""
from __future__ import annotations

import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: embed_ttf.py IN.ttf OUT.h", file=sys.stderr)
        return 2
    src = Path(sys.argv[1])
    dst = Path(sys.argv[2])
    data = src.read_bytes()
    lines = [
        "/* Generated — do not edit. SIL OFL 1.1 IBM Plex Mono. */",
        "#ifndef SPIRITTY_BUILTIN_TTF_H",
        "#define SPIRITTY_BUILTIN_TTF_H",
        "",
        "#include <stddef.h>",
        "#include <stdint.h>",
        "",
        f"static const size_t sp_builtin_ttf_size = {len(data)};",
        "static const uint8_t sp_builtin_ttf[] = {",
    ]
    for i in range(0, len(data), 16):
        chunk = data[i : i + 16]
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in chunk) + ",")
    lines += ["};", "", "#endif /* SPIRITTY_BUILTIN_TTF_H */", ""]
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_text("\n".join(lines))
    print(f"wrote {dst} ({len(data)} ttf bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
