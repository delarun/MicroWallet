#!/usr/bin/env python3
"""Дописать в модуль .wasm манифест — пользовательскую секцию "microwallet".

  sdk/pack.py app.wasm manifest out.wasm
Ключи манифеста — в src/apps.h. Неизвестные ключи и ошибки формата — отказ.
"""
import re
import sys


def leb(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        out.append(b | (0x80 if n else 0))
        if not n:
            return bytes(out)


def check(text):
    keys = {}
    for line in text.splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        k, sep, v = line.partition("=")
        if not sep or k not in ("id", "name", "version", "icon", "path", "sr25519"):
            sys.exit(f"манифест: непонятная строка {line!r}")
        keys.setdefault(k, []).append(v)
    for k in ("id", "name", "version"):
        if len(keys.get(k, [])) != 1:
            sys.exit(f"манифест: нужен ровно один {k}=")
    if not re.fullmatch(r"[a-z0-9]{2,8}", keys["id"][0]):
        sys.exit("манифест: id — 2..8 символов a-z, 0-9")
    if len(keys["name"][0]) > 16 or len(keys["version"][0]) > 8:
        sys.exit("манифест: name до 16, version до 8 символов")
    for icon in keys.get("icon", []):
        if not re.fullmatch(r"[0-9a-fA-F]{16}", icon):
            sys.exit("манифест: icon — 16 hex-символов (8 строк по 8 точек)")
    if keys.get("sr25519", ["yes"]) != ["yes"]:
        sys.exit("манифест: sr25519=yes или строки нет")
    paths = keys.get("path", [])
    if len(paths) > 4:
        sys.exit("манифест: не больше 4 path=")
    for p in paths:
        if not re.fullmatch(r"\d+'(/\d+'?){1,4}", p):
            sys.exit(f"манифест: путь {p!r} — например 44'/60' (первый уровень hardened)")
    # Секция в одну строку на ключ, без комментариев
    return "\n".join(f"{k}={v}" for k in ("id", "name", "version", "icon", "path", "sr25519")
                     for v in keys.get(k, [])) + "\n"


def main():
    src, man, out = sys.argv[1:4]
    w = open(src, "rb").read()
    if w[:8] != b"\0asm\1\0\0\0":
        sys.exit(f"{src}: не модуль WebAssembly")
    m = check(open(man, encoding="utf-8").read()).encode()
    if len(m) > 460:
        sys.exit("манифест длиннее 460 байт")
    name = b"microwallet"
    payload = leb(len(name)) + name + m
    open(out, "wb").write(w + b"\0" + leb(len(payload)) + payload)


if __name__ == "__main__":
    main()
