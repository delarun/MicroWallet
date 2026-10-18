#!/usr/bin/env python3
"""
Эталоны встроенных монет (до переноса в приложения): адреса, xpub, подписи.
Приложения btc/prl/tron/bsc обязаны выдавать то же самое — на этих адресах деньги.

  python3 tools/sim/golden.py <mwsim testnet> <mwsim mainnet> > tools/sim/golden.json
Команды задаются по-новому (app <id> ...); для встроенных монет — старые.
"""
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "host"))
import wallet as W  # noqa: E402


def sim_calls(binary, lines, env=None):
    with tempfile.TemporaryDirectory() as d:
        p = subprocess.run([binary, "lines"], input="\n".join(["restore " + "00" * 16] + lines) + "\n",
                           capture_output=True, text=True, env={**os.environ, "MW_SIM_DIR": d, **(env or {})})
        out = p.stdout.strip().split("\n")[1:]
    assert len(out) == len(lines), (len(out), len(lines), out[-3:])
    return dict(zip(lines, out))


def requests(net, addr):
    """Запросы на подпись: одинаковые для старых команд и приложений."""
    hrp, phrp = ("bc", "prl") if net == "mainnet" else ("tb", "tprl")
    chain_id = 56 if net == "mainnet" else 97
    # BTC: два входа, получатель P2WSH + сдача на chain 1 / 0
    btc_dest = bytes.fromhex("00201863143c14c5166804bd19203356da136c985678cd4d27a1b8c6329604903262")
    btc_change = W.address_to_script(addr["btc_chg0"], hrp)
    btc = {"version": 2, "locktime": 0,
           "ins": [{"txid": "a1" * 32, "vout": 0, "value": 30000, "chain": 0, "index": 0, "sequence": 0xFFFFFFFD},
                   {"txid": "a2" * 32, "vout": 3, "value": 80000, "chain": 0, "index": 1, "sequence": 0xFFFFFFFD}],
           "outs": [{"value": 50000, "script": btc_dest, "change": False},
                    {"value": 59000, "script": btc_change, "change": True, "change_index": 0}]}
    prl = {"version": 2, "locktime": 0,
           "ins": [{"txid": "b1" * 32, "vout": 0, "value": 500000000, "chain": 0, "index": 0, "sequence": 0xFFFFFFFD},
                   {"txid": "b2" * 32, "vout": 1, "value": 120000000, "chain": 1, "index": 2, "sequence": 0xFFFFFFFD}],
           "outs": [{"value": 550000000, "script": W.address_to_script(addr["prl7"], phrp, True), "change": False},
                    {"value": 69990000, "script": W.address_to_script(addr["prl_chg0"], phrp, True), "change": True,
                     "change_index": 0}]}
    # TRON: TransferContract от адреса #0
    import importlib.util
    spec = importlib.util.spec_from_file_location("gv", ROOT / "android/app/src/test/gen_vectors.py")
    gv = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(gv)
    o21, t21 = W.tron_decode(addr["tron0"]), W.tron_decode("TTojvqHHL6UQ2gcYSHSjLsYK8B6dFV7j7Z")
    tron_trx = gv.tron_raw(1, "type.googleapis.com/protocol.TransferContract",
                           gv.pb(1, 2, o21) + gv.pb(2, 2, t21) + gv.pb(3, 0, 5_000_000))
    usdt = W.tron_decode("TR7NHqjeKQxGTCi8q8ZY4pL8otSzgjLj6t" if net == "mainnet" else "TXYZopYRdj2D9XRtbG411XZZ3kM5VkAeBf")
    data = bytes.fromhex("a9059cbb") + b"\0" * 12 + t21[1:] + (12_500_000).to_bytes(32, "big")
    tron_usdt = gv.tron_raw(31, "type.googleapis.com/protocol.TriggerSmartContract",
                            gv.pb(1, 2, o21) + gv.pb(2, 2, usdt) + gv.pb(4, 2, data), fee_limit=30_000_000)
    to = bytes.fromhex("35" * 20)
    bsc_usdt = bytes.fromhex("55d398326f99059fF775485246999027B3197955" if net == "mainnet"
                             else "337610d27c682E347C9cD60BD4b3b107C9d34dDd")
    bdata = bytes.fromhex("a9059cbb") + b"\0" * 12 + to + (12 * 10**18).to_bytes(32, "big")
    return {
        "btc": W.build_request(btc),
        "prl": W.build_request(prl),
        "tron_trx": tron_trx.hex(),
        "tron_usdt": tron_usdt.hex(),
        "bsc_bnb": W.rlp([7, 3 * 10**9, 21000, to, 10**16, b"", chain_id, 0, 0]).hex(),
        "bsc_usdt": W.rlp([8, 3 * 10**9, 60000, bsc_usdt, 0, bdata, chain_id, 0, 0]).hex(),
    }


def golden(binary, net):
    idx = [0, 1, 5]
    addr_lines = ([f"addr {c} {i}" for c in (0, 1) for i in idx] + [f"prl addr {c} {i}" for c in (0, 1) for i in idx + [2, 7]]
                  + [f"tron addr {i}" for i in idx] + [f"bsc addr {i}" for i in idx] + ["xpub"])
    r = sim_calls(binary, addr_lines)
    a = {k: v[3:] for k, v in r.items()}
    addr = {"btc_chg0": a["addr 1 0"], "prl7": a["prl addr 0 7"], "prl_chg0": a["prl addr 1 0"], "tron0": a["tron addr 0"]}
    req = requests(net, addr)
    sign_lines = [f"sign {req['btc']}", f"prl sign {req['prl']}", f"tron sign 0 {req['tron_trx']}",
                  f"tron sign 0 {req['tron_usdt']}", f"bsc sign 0 {req['bsc_bnb']}", f"bsc sign 1 {req['bsc_usdt']}"]
    s = sim_calls(binary, sign_lines)
    sig = dict(zip(["btc", "prl", "tron_trx", "tron_usdt", "bsc_bnb", "bsc_usdt"], [s[l] for l in sign_lines]))
    return {
        "addresses": {
            "btc": {f"{c}/{i}": a[f"addr {c} {i}"] for c in (0, 1) for i in idx},
            "prl": {f"{c}/{i}": a[f"prl addr {c} {i}"] for c in (0, 1) for i in idx + [2, 7]},
            "tron": {str(i): a[f"tron addr {i}"] for i in idx},
            "bsc": {str(i): a[f"bsc addr {i}"] for i in idx},
        },
        "xpub": a["xpub"],
        "requests": req,
        "signatures": sig,  # prl — случайные (BIP340 с aux), сверяются проверкой подписи
    }


def main():
    out = {"testnet": golden(sys.argv[1], "testnet"), "mainnet": golden(sys.argv[2], "mainnet")}
    json.dump(out, sys.stdout, indent=1)
    print()


if __name__ == "__main__":
    main()
