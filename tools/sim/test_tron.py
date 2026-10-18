#!/usr/bin/env python3
"""
TRON: memo, TRC10, оценка комиссии от хоста, отказы. Подпись проверяется восстановлением
адреса из (r, s, recid) над sha256(raw_data).

  tools/sim/build.sh /tmp/mwsim && sh apps/build.sh && python3 tools/sim/test_tron.py /tmp/mwsim
"""
import hashlib
import importlib.util
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(ROOT / "host"))
import wallet as W  # noqa: E402
import test_golden as TG  # noqa: E402
from test_apps import Sim, recover_address  # noqa: E402

spec = importlib.util.spec_from_file_location("gv", ROOT / "android/app/src/test/gen_vectors.py")
gv = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gv)
pb = gv.pb

TRANSFER = "type.googleapis.com/protocol.TransferContract"
ASSET = "type.googleapis.com/protocol.TransferAssetContract"
TRIGGER = "type.googleapis.com/protocol.TriggerSmartContract"


def raw(ctype, url, value, fee_limit=None, memo=None):
    r = gv.tron_raw(ctype, url, value, fee_limit)
    return r + pb(10, 2, memo) if memo is not None else r


def main():
    with tempfile.TemporaryDirectory() as d:
        sim = Sim(sys.argv[1], d)
        assert sim.call("restore " + "00" * 16).startswith("ok")
        TG.install_all(sim)
        a0 = sim.call("app tron addr 0")[3:]
        own, to = W.tron_decode(a0), W.tron_decode("TTojvqHHL6UQ2gcYSHSjLsYK8B6dFV7j7Z")
        me = W.evm_checksum(own[1:])

        def sign(tx, extra=""):
            r = sim.call(f"app tron sign 0 {tx.hex()}{extra}")
            assert r.startswith("ok "), r
            assert recover_address(hashlib.sha256(tx).digest(), bytes.fromhex(r[3:])) == me, "чужая подпись"

        def refuse(tx, want, extra=""):
            r = sim.call(f"app tron sign 0 {tx.hex() if isinstance(tx, bytes) else tx}{extra}")
            assert r.startswith("err ") and want in r, (want, r)

        trx = pb(1, 2, own) + pb(2, 2, to) + pb(3, 0, 5_000_000)
        sign(raw(1, TRANSFER, trx, memo=b"invoice 42"))
        sign(raw(1, TRANSFER, trx, memo=bytes(range(1, 40))))  # непечатный — hex
        sign(raw(1, TRANSFER, trx), " 345000")                   # оценка bandwidth
        print("1. TRX: memo текстом и hex, оценка комиссии — подписано, адрес совпал")

        asset = pb(1, 2, b"1002000") + pb(2, 2, own) + pb(3, 2, to) + pb(4, 0, 77)
        sign(raw(2, ASSET, asset, memo=b"hi"))
        refuse(raw(2, ASSET, asset, fee_limit=10_000_000), "fee limit")
        refuse(raw(2, ASSET, pb(1, 2, b"BTT") + asset[9:]), "bad token id")
        refuse(raw(2, ASSET, asset + pb(5, 0, 1)), "unsupported")
        refuse(raw(2, ASSET, asset[:-2]), "incomplete")
        refuse(raw(2, TRANSFER, asset), "")  # тип 2 с чужим type_url
        print("2. TRC10: подписан; отказ: fee_limit, ID не цифрами, лишнее поле, без суммы, чужой type_url")

        usdt = W.tron_decode("TXYZopYRdj2D9XRtbG411XZZ3kM5VkAeBf" if "testnet" in sim.call("info") else
                             "TR7NHqjeKQxGTCi8q8ZY4pL8otSzgjLj6t")
        data = bytes.fromhex("a9059cbb") + b"\0" * 12 + to[1:] + (12_500_000).to_bytes(32, "big")
        trc20 = raw(31, TRIGGER, pb(1, 2, own) + pb(2, 2, usdt) + pb(4, 2, data), fee_limit=30_000_000)
        sign(trc20, " 13000000")
        sign(trc20, " 30000000")
        refuse(trc20, "fee estimate above fee limit", " 30000001")
        refuse(trc20, "bad fee estimate", " 12x")
        refuse(trc20, "bad fee estimate", " 99999999999999999999")
        print("3. TRC20: оценка ≤ fee_limit — подписано; оценка выше потолка или мусор — отказ")

        refuse(raw(1, TRANSFER, trx, memo=b"x" * 97), "memo too long")
        refuse(raw(1, TRANSFER, trx, memo=b"\x01" * 49), "memo too long")
        sign(raw(1, TRANSFER, trx, memo=b"x" * 96))
        print("4. memo длиннее экрана — отказ (не обрезается)")
        sim.close()
    print("всё прошло")


if __name__ == "__main__":
    main()
