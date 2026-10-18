#!/usr/bin/env python3
"""
ed25519 (SLIP-10) и sr25519 (Substrate) в прошивке — через тестовое приложение в симуляторе.
  ed25519: ключ сверяется со своим SLIP-10 на Python, подпись — библиотекой cryptography;
  sr25519: известные векторы Substrate (dev-фраза: корень, //Alice, /foo), подпись —
           sr25519_verify из той же библиотеки (tools/sim/sr25519_verify.c).

  tools/sim/build.sh /tmp/mwsim && python3 tools/sim/test_curves.py /tmp/mwsim
"""
import base64
import hashlib
import hmac
import os
import subprocess
import sys
import tempfile
from pathlib import Path

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey, Ed25519PublicKey
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "host"))
import wallet as W  # noqa: E402

DEV = "bottom drive obey lake curtain smoke basket hold race lonely fit walk"  # Substrate dev
ABANDON = "abandon " * 11 + "about"


class Sim:
    def __init__(self, binary, d):
        self.p = subprocess.Popen([binary, "lines"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, text=True, env={**os.environ, "MW_SIM_DIR": d})

    def call(self, line):
        self.p.stdin.write(line + "\n")
        self.p.stdin.flush()
        return self.p.stdout.readline().strip()

    def install(self, wasm):
        assert self.call(f"app add {len(wasm)} {hashlib.sha256(wasm).hexdigest()}") == "ok"
        for off in range(0, len(wasm), 2048):
            assert self.call("app add b64 " + base64.b64encode(wasm[off:off + 2048]).decode()) == "ok"
        r = self.call("app add end")
        assert r == "ok", r


def slip10_ed25519(mnemonic, path):
    seed = hashlib.pbkdf2_hmac("sha512", mnemonic.encode(), b"mnemonic", 2048)
    i = hmac.new(b"ed25519 seed", seed, hashlib.sha512).digest()
    for idx in path:
        i = hmac.new(i[32:], b"\0" + i[:32] + (idx | 0x80000000).to_bytes(4, "big"), hashlib.sha512).digest()
    return Ed25519PrivateKey.from_private_bytes(i[:32]).public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)


def b58(b):
    a = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"
    n, s = int.from_bytes(b, "big"), ""
    while n:
        n, r = divmod(n, 58)
        s = a[r] + s
    return "1" * (len(b) - len(b.lstrip(b"\0"))) + s


def junction(hard, name):
    """Chain code Substrate: SCALE-строка (длина compact << 2), дополненная нулями до 32."""
    enc = bytes([len(name) << 2]) + name.encode()
    return ("01" if hard else "00") + (enc + b"\0" * (32 - len(enc))).hex()


def main():
    binary = sys.argv[1]
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "curves.wasm"
        subprocess.run(["sh", str(ROOT / "sdk/build.sh"), str(ROOT / "tools/sim/test_apps/curves/curves.c"), str(out)],
                       check=True, stdout=subprocess.DEVNULL)
        curves = out.read_bytes()
        verify = Path(tmp) / "srv"
        subprocess.run(["gcc", "-O2", str(ROOT / "tools/sim/sr25519_verify.c"), *map(str, sorted((ROOT / "build/sim-sr25519").glob("*.o"))),
                        "-o", str(verify)], check=True)

        # ---- ed25519 (abandon…about) ----
        d = Path(tmp) / "a"
        d.mkdir()
        sim = Sim(binary, str(d))
        assert sim.call("restore " + "00" * 16).startswith("ok")
        sim.install(curves)
        for path in ([44, 501, 0, 0], [44, 501, 1, 0], [44, 354, 0, 0, 0]):
            got = sim.call("app curves ed " + " ".join(map(str, path)))
            want = slip10_ed25519(ABANDON, path).hex()
            assert got == "ok " + want, (path, got, want)
        sol = b58(bytes.fromhex(sim.call("app curves ed 44 501 0 0")[3:]))
        print(f"1. ed25519 SLIP-10 совпал с Python; Solana m/44'/501'/0'/0' = {sol}")
        msg = b"MicroWallet ed25519"
        r = sim.call(f"app curves edsign 44 501 0 0 ; {msg.hex()}")
        assert r.startswith("ok "), r
        Ed25519PublicKey.from_public_bytes(slip10_ed25519(ABANDON, [44, 501, 0, 0])).verify(bytes.fromhex(r[3:]), msg)
        assert sim.call("app curves ed 44 0 0") == "err denied"
        assert sim.call("app curves edsoft") == "ok refused"
        print("2. ed25519: подпись верна (cryptography), чужой путь и не-hardened уровень — отказ")

        # ---- sr25519 (dev-фраза Substrate) ----
        d2 = Path(tmp) / "b"
        d2.mkdir()
        sim2 = Sim(binary, str(d2))
        assert sim2.call("restore " + W.mnemonic_to_entropy(DEV).hex()).startswith("ok")
        sim2.install(curves)
        vectors = {
            "": "46ebddef8cd9bb167dc30878d7113b7e168e6f0646beffd77d69d39bad76b47a",  # корень
            junction(True, "Alice"): "d43593c715fdd31c61141abd04a99fd6822c8558854ccde39a5684e7a56da27d",
            junction(False, "foo"): "40b9675df90efa6069ff623b0fdfcf706cd47ca7452a5056c7ad58194d23440a",
        }
        for j, want in vectors.items():
            got = sim2.call(f"app curves sr {j}")
            assert got == "ok " + want, (j[:6], got, want)
        print("3. sr25519: корень dev-фразы, //Alice, /foo — как в Substrate")
        msg = b"MicroWallet sr25519"
        alice = junction(True, "Alice")
        r = sim2.call(f"app curves srsign {alice} {msg.hex()}")
        assert r.startswith("ok "), r
        v = subprocess.run([str(verify), r[3:], msg.hex(), vectors[alice]], capture_output=True, text=True)
        assert v.stdout.strip() == "ok", v.stdout
        bad = subprocess.run([str(verify), r[3:], (msg + b"!").hex(), vectors[alice]], capture_output=True, text=True)
        assert bad.stdout.strip() == "bad"
        assert sim2.call("app curves blake ") == "ok 0e5751c026e543b2e8ab2eb06099daa1d1e5df47778f7787faab45cdf12fe3a8"
        print("4. sr25519: подпись //Alice верна, чужое сообщение — нет; blake2b-256 — по вектору")
    print("всё прошло")


if __name__ == "__main__":
    main()
