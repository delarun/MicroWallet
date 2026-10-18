#!/usr/bin/env python3
"""
Совместимость с Pearl Wallet: btcd/btcutil DeriveNonStandard (btcd #172) хэширует приватный
ключ выведенного родителя без ведущих нулевых байтов. Команда устройства «btcd <путь>» должна
отвечать «differs» ровно тогда, когда независимая эмуляция Go-кода даёт другой ключ аккаунта.

  tools/sim/build.sh /tmp/mwsim && python3 tools/sim/test_btcd.py /tmp/mwsim
"""
import hashlib
import hmac
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parents[1] / "host"))
import wallet as W  # noqa: E402
from test_apps import Sim  # noqa: E402

N = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141
H = 0x80000000


def seed_of(entropy):
    bits = "".join(f"{b:08b}" for b in entropy)
    bits += f"{hashlib.sha256(entropy).digest()[0]:08b}"[:len(entropy) // 4]
    words = " ".join(W.bip39_words()[int(bits[i:i + 11], 2)] for i in range(0, len(bits), 11))
    return hashlib.pbkdf2_hmac("sha512", words.encode(), b"mnemonic", 2048)


def account_key(seed, path, nonstd):
    """Только hardened-уровни. nonstd — как hdkeychain.DeriveNonStandard в pearl/node/btcutil:
    childKey = ilNum.Bytes() (без ведущих нулей), copy(data[1:], k.key) — прижат влево."""
    I = hmac.new(b"Bitcoin seed", seed, hashlib.sha512).digest()
    key, chain = I[:32], I[32:]  # NewMaster: все 32 байта
    for i in path:
        data = b"\0" + key + b"\0" * (32 - len(key)) + (i | H).to_bytes(4, "big")
        I = hmac.new(chain, data, hashlib.sha512).digest()
        k = (int.from_bytes(I[:32], "big") + int.from_bytes(key, "big")) % N
        key = k.to_bytes((k.bit_length() + 7) // 8, "big") if nonstd else k.to_bytes(32, "big")
        chain = I[32:]
    return int.from_bytes(key, "big")


def main():
    tmp = tempfile.TemporaryDirectory()
    sim = Sim(sys.argv[1], tmp.name)
    coin = 808276 if " net=mainnet " in sim.call("info") else 1
    path = [86, coin, 0]
    # сиды: обычный и такие, где Pearl Wallet выводит иначе (на уровне coin и на уровне account)
    found, n = {}, 0
    while len(found) < 3:
        e = n.to_bytes(16, "big")
        n += 1
        s = seed_of(e)
        same = account_key(s, path, False) == account_key(s, path, True)
        lvl = "same" if same else ("coin" if account_key(s, path[:2], False) != account_key(s, path[:2], True) else "account")
        found.setdefault(lvl, e)
    with tmp:
        assert sim.call("btcd 86h") == "err no wallet on device"
        for lvl, e in sorted(found.items()):
            if sim.call("info").find("state=ready") > 0:
                assert sim.call("wipe").startswith("ok")
            r = sim.call("restore " + e.hex())
            assert r.startswith("ok"), r
            want = "ok same" if lvl == "same" else "ok differs"
            got = sim.call(f"btcd 86h {coin}h 0h")
            assert got == want, (lvl, e.hex(), got)
            print(f"  {e.hex()}: {lvl:8} -> {got}")
        for bad in ("btcd", "btcd x", "btcd 86q", "btcd 4294967295", "btcd " + " ".join(["1"] * 9)):
            assert sim.call(bad).startswith("err usage"), bad
        sim.close()
    print(f"путь m/86'/{coin}'/0': ответ устройства совпал с эмуляцией btcutil; мусор отклонён")
    print("всё прошло")


if __name__ == "__main__":
    main()
