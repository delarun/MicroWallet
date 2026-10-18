#!/usr/bin/env python3
"""
TON (GRAM, USDT; кошелёк W5): адреса, ключи и подписи против эталонов официальной библиотеки
@ton/ton (tools/sim/ton_vectors.json, генератор — gen_ton_vectors.mjs). Подпись проверяется
ed25519 над хешем ячейки, который посчитал эталон: устройство должно было посчитать тот же.

  tools/sim/build.sh /tmp/mwsim && sh apps/build.sh && python3 tools/sim/test_ton.py /tmp/mwsim
"""
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import test_golden as TG  # noqa: E402
from test_apps import Sim  # noqa: E402

V = json.loads((HERE / "ton_vectors.json").read_text())


class ScreenSim(Sim):
    """Симулятор, который пишет экраны ([screen] …) в файл: проверяем, что видит человек."""

    def __init__(self, binary, d):
        self.err = Path(d) / "stderr.txt"
        self.p = subprocess.Popen([binary, "lines"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=self.err.open("w"), text=True, env={**os.environ, "MW_SIM_DIR": d})

    def screens(self):
        return [l[9:].strip() for l in self.err.read_text().splitlines() if l.startswith("[screen] ")]


def main():
    with tempfile.TemporaryDirectory() as d:
        sim = ScreenSim(sys.argv[1], d)
        assert sim.call("restore " + "00" * 16).startswith("ok")
        TG.install_all(sim)
        net = "mainnet" if " net=mainnet " in sim.call("info") else "testnet"
        v = V["nets"][net]

        for i, (a, pk) in enumerate(zip(v["addrs"], v["pubkeys"])):
            assert sim.call(f"app ton addr {i}") == "ok " + a, (i, sim.call(f"app ton addr {i}"), a)
            assert sim.call(f"app ton pubkey {i}") == "ok " + pk
        assert sim.call("app ton show 0") == "ok " + v["addrs"][0]
        print(f"1. {net}: адреса W5 #0–2 и ключи как у @ton/ton (путь Ledger): {v['addrs'][0]}")

        pub = Ed25519PublicKey.from_public_bytes(bytes.fromhex(v["pubkeys"][0]))
        good, bad = [], []
        for c in v["cases"]:
            r = sim.call(f"app ton sign 0 {c['boc']}")
            if c.get("bad"):
                assert r.startswith("err "), (c["name"], r)
                bad.append(f"{c['name']} ({r[4:]})")
            else:
                assert r.startswith("ok "), (c["name"], r)
                pub.verify(bytes.fromhex(r[3:]), bytes.fromhex(c["hash"]))  # бросит, если не так
                sc = sim.screens()
                review = sc[len(sc) - 1 - sc[::-1].index("Review: TON transfer"):]
                want = [f"Amount: {c['amount']}"] + ([f"To: {c['to']}"] if "to" in c else []) + \
                    ([f"Comment: {c['comment']}"] if "comment" in c else [])
                for w in want:
                    assert w in review, (c["name"], w, review)
                good.append(c["name"])
        print(f"2. подписано, хеш совпал с эталоном, сумма/получатель/комментарий на экране верны: {', '.join(good)}")
        print(f"3. отказ: {'; '.join(bad)}")

        # чужая сеть (wallet_id другой), порча BoC, мусор
        other = V["nets"]["mainnet" if net == "testnet" else "testnet"]["cases"][0]["boc"]
        boc = v["cases"][0]["boc"]
        flipped = boc[:-10] + ("0" if boc[-10] != "0" else "1") + boc[-9:]
        for what, h in (("чужая сеть", other), ("испорчена контрольная сумма", flipped),
                        ("обрезан", boc[:-8]), ("не hex", "zz"), ("пусто", "")):
            r = sim.call(f"app ton sign 0 {h}".rstrip())
            assert r.startswith("err "), (what, r)
        assert sim.call("app ton sign 0 " + boc).startswith("ok ")
        print("4. отказ: чужая сеть, испорченный, обрезанный BoC, мусор")
        sim.close()
    print("всё прошло")


if __name__ == "__main__":
    main()
