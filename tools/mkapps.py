#!/usr/bin/env python3
"""
Образ раздела "apps" с готовыми приложениями — для полной прошивки и первой установки по USB.
Раскладка — как в src/apps.c: приложения подряд, каждое с границы сектора 4 КБ:
заголовок 512 байт (magic, len, seq, sha256, манифест) + модуль; остальное — 0xFF.

  python3 tools/mkapps.py out.bin [app.wasm ...]     (по умолчанию — apps/dist, порядок ниже)
  esptool.py --chip esp32s3 write_flash 0x280000 out.bin
"""
import hashlib
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SIZE = 0x180000  # раздел apps (esp32/partitions.csv)
SECTOR = 0x1000
HDR = 512
ORDER = ["btc", "prl", "tron", "bsc", "eth"]  # как на главном экране и в приложениях


def manifest(w):
    i = 8
    while i < len(w):
        sid = w[i]
        i += 1
        size = shift = 0
        while True:
            b = w[i]
            i += 1
            size |= (b & 0x7F) << shift
            shift += 7
            if not b & 0x80:
                break
        end = i + size
        if sid == 0:
            nl = w[i]
            if w[i + 1:i + 1 + nl] == b"microwallet":
                return w[i + 1 + nl:end]
        i = end
    sys.exit("в модуле нет манифеста")


def main():
    out = sys.argv[1]
    files = [Path(f) for f in sys.argv[2:]]
    if not files:
        found = {f.name.split("-")[2]: f for f in (ROOT / "apps/dist").glob("microwallet-app-*.wasm")}
        files = [found[k] for k in ORDER if k in found] + [f for k, f in sorted(found.items()) if k not in ORDER]
    img = bytearray(b"\xff" * SIZE)
    off = 0
    for f in files:
        w = f.read_bytes()
        m = manifest(w)
        if len(w) > 64 * 1024 - HDR or len(m) >= HDR - 48:
            sys.exit(f"{f.name}: слишком большой")
        span = (HDR + len(w) + SECTOR - 1) // SECTOR * SECTOR
        if off + span > SIZE:
            sys.exit("раздел apps переполнен")
        hdr = b"MWAPP2\0\0" + struct.pack("<II", len(w), 1) + hashlib.sha256(w).digest() + m
        hdr += b"\0" * (HDR - len(hdr))
        img[off:off + HDR + len(w)] = hdr + w
        print(f"{off:#08x}: {f.name} ({len(w)} байт, {span // 1024} КБ)")
        off += span
    print(f"занято {off // 1024} из {SIZE // 1024} КБ")
    Path(out).write_bytes(bytes(img))


if __name__ == "__main__":
    main()
