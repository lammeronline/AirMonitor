#!/usr/bin/env python3
"""Manually regenerate src/WebUI.h from src/WebUI.html (gzip-compressed PROGMEM).

Run from the project root:
    python tools/build_web_ui.py
"""
import gzip
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC  = ROOT / "src" / "WebUI.html"
DST  = ROOT / "src" / "WebUI.h"

html_bytes = SRC.read_bytes()
gz = gzip.compress(html_bytes, compresslevel=9)

rows = []
for i in range(0, len(gz), 16):
    chunk = gz[i:i + 16]
    rows.append("  " + ", ".join(f"0x{b:02x}" for b in chunk))

DST.write_text(
    '#pragma once\n'
    '#include <Arduino.h>\n\n'
    '// Auto-generated from src/WebUI.html — do not edit directly.\n'
    f'static const size_t  WEB_INDEX_GZ_LEN = {len(gz)};\n'
    f'static const uint8_t WEB_INDEX_GZ[] PROGMEM = {{\n'
    + ",\n".join(rows) + '\n'
    '};\n',
    encoding="utf-8",
    newline="\n",
)
print(f"{SRC.relative_to(ROOT)} -> {DST.relative_to(ROOT)}  "
      f"{len(html_bytes):,} -> {len(gz):,} bytes (gzip -9, "
      f"{100 - len(gz) * 100 // len(html_bytes)}% smaller)")
