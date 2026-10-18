#!/usr/bin/env python3
"""
BTC и Pearl: подпись блоком (sign @), проверка сумм входов по предыдущим транзакциям,
много входов. Подписи проверяются здесь же, независимо от устройства (BIP143 + ECDSA,
BIP341 + BIP340).

  tools/sim/build.sh mwsim && sh apps/build.sh && python3 tools/sim/test_utxo.py mwsim
"""
import hashlib
import os
import struct
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parents[1] / "host"))
import wallet as W  # noqa: E402
import test_golden as TG  # noqa: E402  (кривая, BIP340/BIP341, установка приложений)
from test_apps import Sim  # noqa: E402

DEST = bytes.fromhex("0014" + "77" * 20)  # чужой P2WPKH


def dsha(b):
    return hashlib.sha256(hashlib.sha256(b).digest()).digest()


def hash160(b):
    return hashlib.new("ripemd160", hashlib.sha256(b).digest()).digest()


def varint(n):
    return bytes([n]) if n < 0xfd else b"\xfd" + struct.pack("<H", n)


def prev_tx(outs, segwit=True):
    """Предыдущая транзакция: один чужой вход, выходы (сумма, скрипт). -> (raw, txid)."""
    ins = varint(1) + os.urandom(32) + struct.pack("<I", 0) + b"\x00" + struct.pack("<I", 0xFFFFFFFF)
    o = varint(len(outs)) + b"".join(struct.pack("<Q", v) + varint(len(s)) + s for v, s in outs)
    lock = struct.pack("<I", 0)
    plain = struct.pack("<I", 2) + ins + o + lock
    txid = dsha(plain)[::-1].hex()
    if not segwit:
        return plain, txid
    wit = b"\x02" + b"\x47" + os.urandom(71) + b"\x21" + os.urandom(33)  # свидетель — в txid не входит
    return struct.pack("<I", 2) + b"\x00\x01" + ins + o + wit + lock, txid


def bip143(tx, i, pub):
    ins, outs = tx["ins"], tx["outs"]
    hp = dsha(b"".join(bytes.fromhex(x["txid"])[::-1] + struct.pack("<I", x["vout"]) for x in ins))
    hs = dsha(b"".join(struct.pack("<I", x["sequence"]) for x in ins))
    ho = dsha(b"".join(struct.pack("<Q", o["value"]) + bytes([len(o["script"])]) + o["script"] for o in outs))
    x = ins[i]
    code = bytes.fromhex("1976a914") + hash160(pub) + bytes.fromhex("88ac")
    pre = (struct.pack("<I", tx["version"]) + hp + hs + bytes.fromhex(x["txid"])[::-1] + struct.pack("<I", x["vout"])
           + code + struct.pack("<Q", x["value"]) + struct.pack("<I", x["sequence"]) + ho
           + struct.pack("<I", tx["locktime"]) + struct.pack("<I", 1))
    return int.from_bytes(dsha(pre), "big")


def ecdsa_verify(pub, z, der):
    assert der[0] == 0x30 and der[2] == 0x02
    rl = der[3]
    r = int.from_bytes(der[4:4 + rl], "big")
    assert der[4 + rl] == 0x02
    s = int.from_bytes(der[6 + rl:6 + rl + der[5 + rl]], "big")
    x = int.from_bytes(pub[1:], "big")
    y = pow((x ** 3 + 7) % TG.P, (TG.P + 1) // 4, TG.P)
    if y % 2 != pub[0] - 2:
        y = TG.P - y
    w = pow(s, -1, TG.N)
    pt = TG.ec_add(TG.ec_mul(z * w % TG.N), TG.ec_mul(r * w % TG.N, (x, y)))
    return pt is not None and pt[0] % TG.N == r and s <= TG.N // 2  # low-s


def main():
    binary = sys.argv[1]
    with tempfile.TemporaryDirectory() as d:
        sim = Sim(binary, d)
        assert sim.call("restore " + "00" * 16).startswith("ok")
        TG.install_all(sim)

        class Dev:
            call = staticmethod(lambda line: W.parse_reply(sim.call(line)))

        def own(coin, chain, idx):
            hrp = "tprl" if coin == "prl" else "tb"
            return W.address_to_script(sim.call(f"app {coin} addr {chain} {idx}")[3:], hrp, coin == "prl")

        def sign(tx, prev):
            try:
                return W.app_call(Dev, "app btc sign", W.sign_blob(tx, prev))
            except W.DeviceError as e:
                return "err " + str(e)

        def check_btc(tx, resp):
            parts = resp.split()
            assert len(parts) == len(tx["ins"]), resp[:80]
            for i, p in enumerate(parts):
                sig, pub = (bytes.fromhex(x) for x in p.split(":"))
                assert sig[-1] == 1 and ecdsa_verify(pub, bip143(tx, i, pub), sig[:-1]), f"вход {i}"
                assert b"\x00\x14" + hash160(pub) == own("btc", tx["ins"][i]["chain"], tx["ins"][i]["index"])

        # ---- BTC: три входа из двух предыдущих транзакций (segwit и legacy) ----
        p1, t1 = prev_tx([(30000, own("btc", 0, 0)), (999, DEST), (80000, own("btc", 0, 1))])
        p2, t2 = prev_tx([(5, DEST), (12000, own("btc", 1, 0))], segwit=False)
        ins = [{"txid": t1, "vout": 0, "value": 30000, "chain": 0, "index": 0, "sequence": 0xFFFFFFFD},
               {"txid": t1, "vout": 2, "value": 80000, "chain": 0, "index": 1, "sequence": 0xFFFFFFFD},
               {"txid": t2, "vout": 1, "value": 12000, "chain": 1, "index": 0, "sequence": 0xFFFFFFFD}]
        tx = {"version": 2, "locktime": 0, "ins": ins,
              "outs": [{"value": 70000, "script": DEST, "change": False},
                       {"value": 51000, "script": own("btc", 1, 1), "change": True, "change_index": 1}]}
        check_btc(tx, sign(tx, [p1, p1, p2]))
        print("1. BTC: 3 входа из segwit- и legacy-транзакций — подписи BIP143 верны")

        # ---- атаки и ошибки хоста ----
        lie = {**tx, "ins": [ins[0], {**ins[1], "value": 60000}, ins[2]],
               "outs": [{**tx["outs"][0], "value": 50000}, tx["outs"][1]]}
        assert sign(lie, [p1, p1, p2]) == "err input amount mismatch"
        assert sign(tx, [p2, p1, p1]) == "err prev tx does not match input"
        bad = bytearray(p1)
        bad[60] ^= 1  # испорчен байт выходов
        assert sign(tx, [bytes(bad), p1, p2]).startswith("err ")
        foreign = {**tx, "ins": [{**ins[0], "vout": 1, "value": 999}, ins[1], ins[2]],
                   "outs": [{**tx["outs"][0], "value": 999 + 80000 + 12000 - 1000}]}
        assert sign(foreign, [p1, p1, p2]) == "err prev output is not ours"
        assert sign(tx, [p1, p1]) == "err prev tx missing"
        try:
            W.app_call(Dev, "app btc sign", W.sign_blob(tx, [p1, p1, p2]) + b"\0")
            raise AssertionError("лишние байты приняты")
        except W.DeviceError as e:
            assert str(e) == "trailing bytes", e
        r = sim.call("app btc sign " + W.build_request(tx))
        assert r.startswith("err previous transactions required"), r
        chg = {**tx, "outs": [tx["outs"][0], {**tx["outs"][1], "change_index": 7}]}
        assert sign(chg, [p1, p1, p2]) == "err change mismatch"
        print("2. отказ: занижена сумма входа, чужая или испорченная prev tx, чужой выход, "
              "нет prev tx, лишние байты, запрос строкой, подмена сдачи")

        # ---- много входов: ответ длиннее строки, забирается кусками ----
        n = 40
        pb, tb = prev_tx([(10000 + i, own("btc", 0, i % 20)) for i in range(n)])
        big = {"version": 2, "locktime": 0,
               "ins": [{"txid": tb, "vout": i, "value": 10000 + i, "chain": 0, "index": i % 20,
                        "sequence": 0xFFFFFFFD} for i in range(n)],
               "outs": [{"value": 10000 * n, "script": DEST, "change": False}]}
        check_btc(big, sign(big, [pb] * n))
        pins = [{"txid": f"{i:02x}" * 32, "vout": i, "value": 10**8 + i, "chain": i % 2, "index": i,
                 "sequence": 0xFFFFFFFD} for i in range(n)]
        pdest = bytes.fromhex("5120" + "66" * 32)
        ptx = {"version": 2, "locktime": 0, "ins": pins,
               "outs": [{"value": 10**8 * n - 50000, "script": pdest, "change": False}]}
        resp = W.app_call(Dev, "app prl sign", W.sign_blob(ptx))
        preq = W.sign_blob(ptx)
        spks = [own("prl", x["chain"], x["index"]) for x in pins]
        sigs = [bytes.fromhex(x) for x in resp.split()]
        assert len(sigs) == n
        for k, s in enumerate(sigs):
            assert TG.bip340_verify(spks[k][2:], TG.bip341_sighash(preq, spks, k), s), f"prl {k}"
        print(f"3. {n} входов: BTC (prev tx, ответ кусками) и Pearl (BIP340) — подписи верны")
        sim.close()
    print("всё прошло")


if __name__ == "__main__":
    main()
