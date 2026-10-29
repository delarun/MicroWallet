#!/usr/bin/env python3
"""
Шрифты для цветного интерфейса (src/ui_lcd.c): TTF -> 4-битные сглаженные глифы ASCII 32..126.

  python3 tools/mkfont.py > src/fonts_lcd.c

Шрифты — Noto Sans и Noto Sans Mono (SIL Open Font License 1.1, текст — tools/fonts/OFL.txt).
"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

DIRS = [Path("/usr/share/fonts/noto"), Path("/usr/share/fonts/truetype/noto")]
# имя в C, файл, размер в пикселях
FONTS = [
    ("font_small", "NotoSans-Regular.ttf", 15),
    ("font_body", "NotoSans-Medium.ttf", 18),
    ("font_title", "NotoSans-Bold.ttf", 23),
    ("font_big", "NotoSans-Bold.ttf", 36),
    ("font_mono", "NotoSansMono-Medium.ttf", 20),
    ("font_mono_s", "NotoSansMono-Medium.ttf", 15),
]


def find(name):
    for d in DIRS:
        if (d / name).exists():
            return str(d / name)
    sys.exit(f"нет шрифта {name}")


def glyphs(path, size):
    f = ImageFont.truetype(path, size)
    ascent, descent = f.getmetrics()
    out = []
    for code in range(32, 127):
        ch = chr(code)
        adv = round(f.getlength(ch))
        img = Image.new("L", (size * 3, ascent + descent + 4), 0)
        ImageDraw.Draw(img).text((size, 0), ch, font=f, fill=255)
        box = img.getbbox()
        if not box:
            out.append((adv, 0, 0, 0, 0, b""))
            continue
        l, t, r, b = box
        w, h = r - l, b - t
        nib = [(img.getpixel((l + x, t + y)) + 8) // 17 for y in range(h) for x in range(w)]
        nib += [0] * (len(nib) & 1)
        data = bytes(nib[i] << 4 | nib[i + 1] for i in range(0, len(nib), 2))
        out.append((adv, l - size, t, w, h, data))
    return ascent, descent, out


def main():
    print("// Сгенерировано tools/mkfont.py — не править руками.")
    print("// Noto Sans, Noto Sans Mono: SIL Open Font License 1.1 (tools/fonts/OFL.txt).")
    print('#include "draw.h"\n')
    for name, file, size in FONTS:
        ascent, descent, gl = glyphs(find(file), size)
        bits, table, off = [], [], 0
        for adv, x, y, w, h, data in gl:
            table.append(f"{{{off}, {w}, {h}, {x}, {y}, {adv}}}")
            bits.append(data)
            off += len(data)
        blob = b"".join(bits)
        print(f"static const uint8_t {name}_bits[{len(blob)}] = {{")
        for i in range(0, len(blob), 24):
            print("  " + ", ".join(f"0x{v:02x}" for v in blob[i:i + 24]) + ",")
        print("};")
        print(f"static const glyph_t {name}_glyphs[95] = {{")
        for i in range(0, 95, 4):
            print("  " + ", ".join(table[i:i + 4]) + ",")
        print("};")
        print(f"const font_t {name} = {{{ascent + descent}, {ascent}, {name}_glyphs, {name}_bits}};  "
              f"// {file} {size}px\n")


if __name__ == "__main__":
    main()
