#!/usr/bin/env python3
"""Rasterize IBM Plex Mono into src/builtin_font.h (grayscale R8 atlas).

Usage:
    python3 scripts/bake_font.py
"""
from __future__ import annotations

from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
TTF = ROOT / "third_party/ibm-plex-mono/IBMPlexMono-Regular.ttf"
OUT = ROOT / "src/builtin_font.h"

CELL_W = 10
CELL_H = 20
FONT_PX = 16
BASELINE = 15  # CELL_H - descent (size 16 descent is 5)
FIRST_CP = 32
LAST_CP = 126
GLYPH_COUNT = LAST_CP - FIRST_CP + 1
BYTES = CELL_W * CELL_H


def raster_char(font: ImageFont.FreeTypeFont, ch: str) -> list[int]:
    img = Image.new("L", (CELL_W, CELL_H), 0)
    draw = ImageDraw.Draw(img)
    draw.text((0, BASELINE), ch, font=font, fill=255, anchor="ls")
    return list(img.tobytes())


def raster_fallback() -> list[int]:
    img = Image.new("L", (CELL_W, CELL_H), 0)
    draw = ImageDraw.Draw(img)
    draw.rectangle([1, 2, CELL_W - 2, CELL_H - 3], outline=200)
    return list(img.tobytes())


def c_bytes(data: list[int], indent: str = "        ") -> str:
    lines = []
    for y in range(CELL_H):
        row = data[y * CELL_W : (y + 1) * CELL_W]
        lines.append(indent + ", ".join(f"0x{b:02x}" for b in row))
    return ",\n".join(lines)


def main() -> None:
    font = ImageFont.truetype(str(TTF), FONT_PX)
    glyphs = [raster_char(font, chr(cp)) for cp in range(FIRST_CP, LAST_CP + 1)]
    fallback = raster_fallback()

    parts = [
        "/*",
        " * builtin_font.h -- grayscale ASCII atlas for the GL/WebGL text pass.",
        " *",
        " * Rasterized from IBM Plex Mono Regular (SIL Open Font License 1.1).",
        " * Source TTF: third_party/ibm-plex-mono/IBMPlexMono-Regular.ttf",
        " * License:    third_party/ibm-plex-mono/OFL.txt",
        " *",
        f" * Layout: {GLYPH_COUNT} glyphs covering codepoints {FIRST_CP}..{LAST_CP}.",
        f" * Each glyph is {CELL_W}x{CELL_H} R8 (row-major, 0 = empty, 255 = cover).",
        f" * Cell metric: {CELL_W} px wide, {CELL_H} px tall (font size {FONT_PX}, baseline {BASELINE}).",
        " *",
        " * Codepoints outside the printable ASCII range map to glyph 0",
        " * (sp_builtin_font_fallback, a hollow box).",
        " *",
        " * Generated -- do not edit by hand. To regenerate:",
        " *   python3 scripts/bake_font.py",
        " */",
        "#ifndef SPIRITTY_BUILTIN_FONT_H",
        "#define SPIRITTY_BUILTIN_FONT_H",
        "",
        "#include <stdint.h>",
        "",
        f"#define SP_BUILTIN_FONT_CELL_W            {CELL_W}",
        f"#define SP_BUILTIN_FONT_CELL_H            {CELL_H}",
        f"#define SP_BUILTIN_FONT_FIRST_CP          {FIRST_CP}",
        f"#define SP_BUILTIN_FONT_LAST_CP           {LAST_CP}",
        f"#define SP_BUILTIN_FONT_GLYPH_COUNT       {GLYPH_COUNT}",
        f"#define SP_BUILTIN_FONT_BYTES_PER_GLYPH   {BYTES}",
        "",
        "/* Row-major R8 coverage. */",
        "static const uint8_t sp_builtin_font_px"
        "[SP_BUILTIN_FONT_GLYPH_COUNT][SP_BUILTIN_FONT_BYTES_PER_GLYPH] = {",
    ]

    for i, data in enumerate(glyphs):
        cp = FIRST_CP + i
        ch = chr(cp)
        shown = ch if ch != "\\" else "\\\\"
        if ch == "'":
            shown = "\\'"
        parts.append(f"    /* {cp:3d} 0x{cp:02x} '{shown}' */ {{")
        parts.append(c_bytes(data))
        parts.append("    },")

    parts.append("};")
    parts.append("")
    parts.append("/* Fallback glyph for unmapped codepoints: hollow box. */")
    parts.append("static const uint8_t sp_builtin_font_fallback[SP_BUILTIN_FONT_BYTES_PER_GLYPH] = {")
    parts.append(c_bytes(fallback, indent="    "))
    parts.append("};")
    parts.append("")
    parts.append("#endif /* SPIRITTY_BUILTIN_FONT_H */")
    parts.append("")

    OUT.write_text("\n".join(parts))
    print(f"wrote {OUT} ({OUT.stat().st_size} bytes, {GLYPH_COUNT} glyphs {CELL_W}x{CELL_H})")


if __name__ == "__main__":
    main()
