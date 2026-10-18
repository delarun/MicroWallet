#!/usr/bin/env python3
"""
Приложения btc/prl/tron/bsc против эталонов встроенных монет (tools/sim/golden.json, сняты
с прошивки до переноса): адреса, xpub, подписи — те же. Pearl подписывает со случайным aux,
поэтому его подписи проверяются BIP340 по ключу из прежнего адреса и BIP341-sighash.

  sh apps/build.sh
  python3 tools/sim/test_golden.py <mwsim testnet> <mwsim mainnet>
"""
import base64
import hashlib
import json
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "host"))
import wallet as W  # noqa: E402

P = 2**256 - 2**32 - 977
N = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141
G = (0x79BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798,
     0x483ADA7726A3C4655DA4FBFC0E1108A8FD17B448A68554199C47D08FFB10D4B8)


def ec_add(a, b):
    if a is None:
        return b
    if b is None:
        return a
    if a[0] == b[0] and (a[1] + b[1]) % P == 0:
        return None
    m = (3 * a[0] * a[0] * pow(2 * a[1], -1, P)) if a == b else (b[1] - a[1]) * pow(b[0] - a[0], -1, P)
    x = (m * m - a[0] - b[0]) % P
    return x, (m * (a[0] - x) - a[1]) % P


def ec_mul(k, p=G):
    r = None
    while k:
        if k & 1:
            r = ec_add(r, p)
        p = ec_add(p, p)
        k >>= 1
    return r


def tagged(tag, msg):
    t = hashlib.sha256(tag.encode()).digest()
    return hashlib.sha256(t + t + msg).digest()


def bip340_verify(xonly, msg, sig):
    x = int.from_bytes(xonly, "big")
    y = pow((x**3 + 7) % P, (P + 1) // 4, P)
    if y & 1:
        y = P - y
    r, s = int.from_bytes(sig[:32], "big"), int.from_bytes(sig[32:], "big")
    e = int.from_bytes(tagged("BIP0340/challenge", sig[:32] + xonly + msg), "big") % N
    R = ec_add(ec_mul(s), ec_mul(N - e, (x, y)))
    return R is not None and R[1] % 2 == 0 and R[0] == r


def parse_req(b):
    """Формат запроса apps/utxo/utxo.c."""
    ver, lock = struct.unpack_from("<II", b, 0)
    o = 8
    ins = []
    n_in = b[o]
    o += 1
    for _ in range(n_in):
        vout, seq, amt, chain, idx = struct.unpack_from("<IIQBI", b, o + 32)
        ins.append((b[o:o + 32], vout, seq, amt, chain, idx))
        o += 32 + 21
    outs = []
    n_out = b[o]
    o += 1
    for _ in range(n_out):
        amt, sl = struct.unpack_from("<QB", b, o)
        outs.append((amt, b[o + 9:o + 9 + sl]))
        o += 9 + sl + 5
    return ver, lock, ins, outs


def bip341_sighash(req, spks, idx):
    ver, lock, ins, outs = parse_req(req)
    sha = lambda x: hashlib.sha256(x).digest()  # noqa: E731
    prevouts = sha(b"".join(t + struct.pack("<I", v) for t, v, *_ in ins))
    amounts = sha(b"".join(struct.pack("<Q", a) for _, _, _, a, _, _ in ins))
    scripts = sha(b"".join(bytes([len(s)]) + s for s in spks))
    seqs = sha(b"".join(struct.pack("<I", s) for _, _, s, *_ in ins))
    outputs = sha(b"".join(struct.pack("<Q", a) + bytes([len(s)]) + s for a, s in outs))
    m = b"\0\0" + struct.pack("<II", ver, lock) + prevouts + amounts + scripts + seqs + outputs + b"\0" + struct.pack("<I", idx)
    return tagged("TapSighash", m)


class Sim:
    def __init__(self, binary, d):
        self.p = subprocess.Popen([binary, "lines"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, text=True, env={**os.environ, "MW_SIM_DIR": d})

    def call(self, line):
        self.p.stdin.write(line + "\n")
        self.p.stdin.flush()
        return self.p.stdout.readline().strip()


def install_all(sim):
    for f in sorted((ROOT / "apps/dist").glob("microwallet-app-*.wasm")):
        w = f.read_bytes()
        assert sim.call(f"app add {len(w)} {hashlib.sha256(w).hexdigest()}") == "ok"
        for off in range(0, len(w), 2048):
            assert sim.call("app add b64 " + base64.b64encode(w[off:off + 2048]).decode()) == "ok"
        r = sim.call("app add end")
        assert r == "ok", (f.name, r)


def check(binary, net, g):
    with tempfile.TemporaryDirectory() as d:
        sim = Sim(binary, d)
        assert sim.call("restore " + "00" * 16).startswith("ok")
        install_all(sim)
        n = 0
        for coin in ("btc", "prl"):
            for k, want in g["addresses"][coin].items():
                c, i = k.split("/")
                got = sim.call(f"app {coin} addr {c} {i}")
                assert got == "ok " + want, (net, coin, k, got, want)
                n += 1
            assert sim.call(f"app {coin} addr 0") == "ok " + g["addresses"][coin]["0/0"]  # меню: addr <i>
        for coin in ("tron", "bsc"):
            for i, want in g["addresses"][coin].items():
                got = sim.call(f"app {coin} addr {i}")
                assert got == "ok " + want, (net, coin, i, got, want)
                n += 1
        assert sim.call("app btc xpub") == "ok " + g["xpub"], (sim.call("app btc xpub"), g["xpub"])
        req, sig = g["requests"], g["signatures"]
        # BTC теперь подписывает только с предыдущими транзакциями (sign @): эталон прежней
        # прошивки построен на выдуманных txid — подписи проверяет test_utxo.py независимо
        assert sim.call(f"app btc sign {req['btc']}").startswith("err previous transactions required")
        assert sim.call(f"app tron sign 0 {req['tron_trx']}") == sig["tron_trx"], "tron trx"
        assert sim.call(f"app tron sign 0 {req['tron_usdt']}") == sig["tron_usdt"], "tron usdt"
        assert sim.call(f"app bsc sign 0 {req['bsc_bnb']}") == sig["bsc_bnb"], "bsc bnb"
        assert sim.call(f"app bsc sign 1 {req['bsc_usdt']}") == sig["bsc_usdt"], "bsc usdt"
        # Pearl: BIP340 по ключам прежних адресов
        r = sim.call(f"app prl sign {req['prl']}")
        assert r.startswith("ok "), r
        preq = bytes.fromhex(req["prl"])
        _, _, ins, _ = parse_req(preq)
        hrp = "prl" if net == "mainnet" else "tprl"
        spks = [W.address_to_script(g["addresses"]["prl"][f"{c}/{i}"], hrp, True) for *_, c, i in ins]
        sigs = [bytes.fromhex(x) for x in r[3:].split()]
        assert len(sigs) == len(ins)
        for k, s in enumerate(sigs):
            assert bip340_verify(spks[k][2:], bip341_sighash(preq, spks, k), s), f"prl sig {k}"
        sim.p.stdin.close()
        sim.p.wait(5)
        print(f"{net}: {n} адресов, xpub, подписи TRON/BSC — как у встроенных; Pearl — {len(sigs)} BIP340 верны")


def main():
    g = json.load(open(ROOT / "tools/sim/golden.json"))
    check(sys.argv[1], "testnet", g["testnet"])
    check(sys.argv[2], "mainnet", g["mainnet"])
    print("всё совпало")


if __name__ == "__main__":
    main()
