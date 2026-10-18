#!/usr/bin/env python3
"""Собирает кадры из oled_preview.c в одну картинку «как на OLED»."""
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

W, S = 128, 4                 # ширина экрана и масштаб; высота — в каждом кадре
PAD, LABEL, COLS = 16, 22, 3
ON, OFF, BG, BEZEL = (210, 235, 255), (10, 12, 16), (32, 34, 40), (0, 0, 0)


def frame(data, H):
    img = Image.new("RGB", (W * S, H * S), OFF)
    px = ImageDraw.Draw(img)
    for page in range(H // 8):
        for x in range(W):
            b = data[page * W + x]
            for bit in range(8):
                if b >> bit & 1:
                    y = page * 8 + bit
                    px.rectangle([x * S, y * S, x * S + S - 2, y * S + S - 2], fill=ON)
    return img


def main(src, dst):
    files = sorted(Path(src).glob("*.bin"))
    H = files[0].read_bytes()[0]
    rows = (len(files) + COLS - 1) // COLS
    cw, ch = W * S + 2 * PAD, H * S + 2 * PAD + LABEL
    sheet = Image.new("RGB", (cw * COLS, ch * rows), BG)
    draw = ImageDraw.Draw(sheet)
    try:
        font = ImageFont.truetype("DejaVuSans.ttf", 15)
    except OSError:
        font = ImageFont.load_default()
    for i, f in enumerate(files):
        raw = f.read_bytes()[1:]
        x, y = (i % COLS) * cw, (i // COLS) * ch
        draw.rectangle([x + PAD - 6, y + PAD - 6, x + PAD + W * S + 5, y + PAD + H * S + 5], fill=BEZEL)
        sheet.paste(frame(raw[:W * H // 8], H), (x + PAD, y + PAD))
        draw.text((x + PAD, y + PAD + H * S + 8), raw[W * H // 8:].decode(), fill=(170, 175, 185), font=font)
    sheet.save(dst)
    print(f"{len(files)} экранов -> {dst}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
