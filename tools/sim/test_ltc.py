#!/usr/bin/env python3
"""
Litecoin (приложение ltc = apps/utxo с UTXO_LTC) и старые адреса получателей (P2PKH, P2SH)
у Bitcoin и Litecoin. Адреса сверяются с независимым выводом BIP32/BIP84 прямо здесь,
подписи — BIP143 + ECDSA (test_utxo), получатель — по экрану устройства.

  tools/sim/build.sh /tmp/mwsim && sh apps/build.sh && python3 tools/sim/test_ltc.py /tmp/mwsim
"""
import hashlib
import hmac
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parents[1] / "host"))
import wallet as W  # noqa: E402
import test_golden as TG  # noqa: E402
import test_utxo as TU  # noqa: E402
from test_apps import Sim  # noqa: E402

H = 0x80000000


class ScreenSim(Sim):
    def __init__(self, binary, d):
        self.err = Path(d) / "stderr.txt"
        self.p = subprocess.Popen([binary, "lines"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=self.err.open("w"), text=True, env={**os.environ, "MW_SIM_DIR": d})

    def screens(self):
        return [l[9:].strip() for l in self.err.read_text().splitlines() if l.startswith("[screen] ")]


# ---- независимый BIP32 (без кода прошивки) ----

def master(seed):
    i = hmac.new(b"Bitcoin seed", seed, hashlib.sha512).digest()
    return int.from_bytes(i[:32], "big"), i[32:]


def ser_pub(k):
    x, y = TG.ec_mul(k)
    return bytes([2 + (y & 1)]) + x.to_bytes(32, "big")


def ckd(k, c, i):
    data = (b"\0" + k.to_bytes(32, "big") if i & H else ser_pub(k)) + i.to_bytes(4, "big")
    j = hmac.new(c, data, hashlib.sha512).digest()
    return (int.from_bytes(j[:32], "big") + k) % TG.N, j[32:]


def pub_at(seed, path):
    k, c = master(seed)
    for i in path:
        k, c = ckd(k, c, i)
    return ser_pub(k)


def bech32(hrp, prog):
    data = [0] + W._convertbits(prog, 8, 5, True)
    pm = W._polymod(W._hrp_expand(hrp) + data + [0] * 6) ^ 1
    return hrp + "1" + "".join(W.CHARSET[d] for d in data + [(pm >> 5 * (5 - i)) & 31 for i in range(6)])


def b58check(b):
    b += hashlib.sha256(hashlib.sha256(b).digest()).digest()[:4]
    n, s = int.from_bytes(b, "big"), ""
    while n:
        n, r = divmod(n, 58)
        s = W.B58[r] + s
    return "1" * (len(b) - len(b.lstrip(b"\0"))) + s


def main():
    binary = sys.argv[1]
    seed = hashlib.pbkdf2_hmac("sha512", ("abandon " * 11 + "about").encode(), b"mnemonic", 2048)
    with tempfile.TemporaryDirectory() as d:
        sim = ScreenSim(binary, d)
        assert sim.call("restore " + "00" * 16).startswith("ok")
        TG.install_all(sim)
        main_net = " net=mainnet " in sim.call("info")
        hrp, coin_type = ("ltc", 2) if main_net else ("tltc", 1)

        # ---- адреса: BIP84, coin type 2 (testnet 1), ltc1q… / tltc1q… ----
        for chain in (0, 1):
            for i in range(3):
                want = bech32(hrp, TU.hash160(pub_at(seed, [84 | H, coin_type | H, H, chain, i])))
                assert sim.call(f"app ltc addr {chain} {i}") == "ok " + want, (chain, i)
        print(f"1. адреса Litecoin m/84'/{coin_type}'/0'/c/i как у независимого BIP32: {want[:16]}…")
        assert sim.call("app ltc xpub").startswith("err "), "xpub — только у Bitcoin"

        class Dev:
            call = staticmethod(lambda line: W.parse_reply(sim.call(line)))

        def own(coin, chain, idx):
            h = "ltc" if coin == "ltc" else "bc"
            h = h if main_net else "t" + h if coin == "ltc" else "tb"
            return W.address_to_script(sim.call(f"app {coin} addr {chain} {idx}")[3:], h)

        def sign(coin, tx, prev):
            try:
                return W.app_call(Dev, f"app {coin} sign", W.sign_blob(tx, prev))
            except W.DeviceError as e:
                return "err " + str(e)

        def check(coin, tx, resp):
            parts = resp.split()
            assert len(parts) == len(tx["ins"]), resp[:80]
            for i, p in enumerate(parts):
                sig, pub = (bytes.fromhex(x) for x in p.split(":"))
                assert sig[-1] == 1 and TU.ecdsa_verify(pub, TU.bip143(tx, i, pub), sig[:-1]), f"{coin} вход {i}"
                assert b"\x00\x14" + TU.hash160(pub) == own(coin, tx["ins"][i]["chain"], tx["ins"][i]["index"])

        h20 = bytes([0x42] * 20)
        pkh, sh = b"\x76\xa9\x14" + h20 + b"\x88\xac", b"\xa9\x14" + h20 + b"\x87"
        # версии base58: Litecoin 30/32 (testnet 6f/3a), Bitcoin 00/05 (testnet 6f/c4)
        ver = {"ltc": (0x30, 0x32) if main_net else (0x6f, 0x3a), "btc": (0x00, 0x05) if main_net else (0x6f, 0xc4)}
        for coin in ("ltc", "btc"):
            p1, t1 = TU.prev_tx([(500000, own(coin, 0, 0)), (70000, own(coin, 0, 2))])
            ins = [{"txid": t1, "vout": 0, "value": 500000, "chain": 0, "index": 0, "sequence": 0xFFFFFFFD},
                   {"txid": t1, "vout": 1, "value": 70000, "chain": 0, "index": 2, "sequence": 0xFFFFFFFD}]
            tx = {"version": 2, "locktime": 0, "ins": ins,
                  "outs": [{"value": 200000, "script": pkh, "change": False},
                           {"value": 150000, "script": sh, "change": False},
                           {"value": 210000, "script": own(coin, 1, 0), "change": True, "change_index": 0}]}
            check(coin, tx, sign(coin, tx, [p1, p1]))
            sc = sim.screens()
            want = [b58check(bytes([ver[coin][0]]) + h20), b58check(bytes([ver[coin][1]]) + h20)]
            for a in want:
                assert any(a in s for s in sc), (coin, a, sc[-12:])
            name = "Litecoin" if coin == "ltc" else "Bitcoin"
            assert f"Review: {name} transfer" in sc
            print(f"2. {name}: на P2PKH и P2SH — подписи BIP143 верны, на экране {want[0][:6]}… и {want[1][:6]}…")

        # ---- старый P2SH Litecoin «3…» (такой же, как у Bitcoin): хост берёт с предупреждением,
        # устройство показывает тот же адрес как M… ----
        net = "mainnet" if main_net else "testnet"
        old = b58check(bytes([0x05 if main_net else 0xC4]) + h20)
        m = W.ltc_old_p2sh(old, net)
        assert m == b58check(bytes([0x32 if main_net else 0x3A]) + h20)
        assert W.address_to_script(old, hrp, False, W.NETS[net]["ltc_b58"]) == sh
        assert any(m in s for s in sim.screens()), "M… на экране"
        print(f"3. старый {old[:6]}… принимается с предупреждением; устройство показывает {m[:6]}…")

        # ---- чужая монета: выход на Bitcoin-адрес — не наш для Litecoin (в testnet у обеих
        # coin type 1 — ключи и скрипты совпадают, проверять нечего) ----
        if not main_net:
            sim.close()
            print("всё прошло")
            return
        pb, tb = TU.prev_tx([(90000, own("btc", 0, 0))])
        tx = {"version": 2, "locktime": 0,
              "ins": [{"txid": tb, "vout": 0, "value": 90000, "chain": 0, "index": 0, "sequence": 0xFFFFFFFD}],
              "outs": [{"value": 80000, "script": pkh, "change": False}]}
        assert sign("ltc", tx, [pb]) == "err prev output is not ours"
        print("4. отказ: выход предыдущей транзакции на Bitcoin-адрес Litecoin не подписывает")
        sim.close()
    print("всё прошло")


if __name__ == "__main__":
    main()
