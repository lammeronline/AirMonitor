Import("env")
import gzip
from pathlib import Path

root = Path(env["PROJECT_DIR"])
src  = root / "src" / "WebUI.html"
dst  = root / "src" / "WebUI.h"

if not src.exists():
    print(f"[WebUI] WARNING: {src} not found, skipping generation")
else:
    html_bytes = src.read_bytes()
    gz = gzip.compress(html_bytes, compresslevel=9)

    rows = []
    for i in range(0, len(gz), 16):
        chunk = gz[i:i + 16]
        rows.append("  " + ", ".join(f"0x{b:02x}" for b in chunk))

    dst.write_text(
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
    print(f"[WebUI] WebUI.html -> WebUI.h  {len(html_bytes):,} -> {len(gz):,} bytes (gzip -9, "
          f"{100 - len(gz) * 100 // len(html_bytes)}% smaller)")
