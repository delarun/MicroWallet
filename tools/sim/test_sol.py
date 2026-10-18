#!/usr/bin/env python3
"""
Solana: адреса (как Phantom), подписи и экраны против эталонов @solana/web3.js
(tools/sim/sol_vectors.json, генератор — gen_sol_vectors.mjs). ed25519 детерминирован —
подпись устройства должна совпасть с эталонной байт в байт.

  tools/sim/build.sh /tmp/mwsim && sh apps/build.sh && python3 tools/sim/test_sol.py /tmp/mwsim
"""
import json
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import test_golden as TG  # noqa: E402
from test_ton import ScreenSim  # noqa: E402

V = json.loads((HERE / "sol_vectors.json").read_text())


def main():
    with tempfile.TemporaryDirectory() as d:
        sim = ScreenSim(sys.argv[1], d)
        assert sim.call("restore " + "00" * 16).startswith("ok")
        TG.install_all(sim)
        main_net = " net=mainnet " in sim.call("info")
        for i, a in enumerate(V["addrs"]):
            assert sim.call(f"app sol addr {i}") == "ok " + a, (i, sim.call(f"app sol addr {i}"))
        assert sim.call("app sol show 0") == "ok " + V["addrs"][0]
        print(f"1. адреса m/44'/501'/i'/0' как в Phantom: {V['addrs'][0]}")

        good, bad = [], []
        net = "mainnet" if main_net else "testnet"
        for c in V["cases"]:
            if c.get("net", net) != net:  # токены — свои для каждой сети
                continue
            r = sim.call(f"app sol sign 0 {c['msg']} {c.get('args', '')}".rstrip())
            if c.get("bad"):
                assert r.startswith("err "), (c["name"], r)
                bad.append(f"{c['name']} ({r[4:]})")
                continue
            assert r == "ok " + c["sig"], (c["name"], r)
            sc = sim.screens()
            title = "Review: Token transfer" if "args" in c else "Review: Solana transfer"
            review = sc[len(sc) - 1 - sc[::-1].index(title):]
            want = [f"Amount: {c['amount']}", f"To: {V['dest']}", f"Max fee: {c['fee']}"]
            if c.get("create"):
                want.append("Account rent: 0.00203928 SOL")
            if c.get("high"):
                want.append("High fee!: check it carefully")
            if not main_net:
                want.append("Network: Solana devnet")
            for w in want:
                assert w in review, (c["name"], w, review)
            good.append(c["name"])
        print(f"2. подпись совпала с @solana/web3.js байт в байт, экраны верны: {', '.join(good)}")
        print(f"3. отказ: {'; '.join(bad)}")
        for junk in ("zz", "", "00"):
            assert sim.call(f"app sol sign 0 {junk}".rstrip()).startswith("err ")
        sim.close()
    print("всё прошло")


if __name__ == "__main__":
    main()
