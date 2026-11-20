#!/usr/bin/env python3
"""Pack the Krita-exported player sprite into a C header.

The design source is assets/player.svg; it is rasterised and exported with
Krita to assets/player.png. This script only turns that PNG into a byte array
so the editor does not need an image decoder at runtime.
"""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent.parent
PNG = ROOT / "assets" / "player.png"
OUT = ROOT / "src" / "player_sprite.h"

raw = subprocess.run(
    ["convert", str(PNG), "-depth", "8", "rgba:-"],
    check=True, capture_output=True,
).stdout
width, height = 96, 96
if len(raw) != width * height * 4:
    raise SystemExit(f"unexpected sprite size {len(raw)}")

lines = [
    "/* Generated from assets/player.png, exported by Krita from player.svg. */",
    "#ifndef PLAYER_SPRITE_H_INCLUDED",
    "#define PLAYER_SPRITE_H_INCLUDED",
    "",
    f"#define PLAYER_SPRITE_W {width}",
    f"#define PLAYER_SPRITE_H {height}",
    "",
    f"static const unsigned char PLAYER_SPRITE_RGBA[{len(raw)}] = {{",
]
for i in range(0, len(raw), 16):
    chunk = raw[i:i + 16]
    lines.append("    " + ",".join(str(b) for b in chunk) + ",")
lines += ["};", "", "#endif", ""]
OUT.write_text("\n".join(lines))
print(f"wrote {OUT} ({len(raw)} bytes)")
