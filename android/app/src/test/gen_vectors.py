#!/usr/bin/env python3
"""
Эталоны для VectorsTest.kt: host/wallet.py собирает и подписывает транзакции через
симулятор устройства (tools/sim, seed «abandon ×11 about») на подменённых ответах сети.
Kotlin-кошелёк с тем же симулятором и теми же ответами должен получить те же байты.

  tools/sim/build.sh /tmp/mwsim && sh apps/build.sh
  python3 android/app/src/test/gen_vectors.py /tmp/mwsim > android/app/src/test/resources/vectors.json
Монеты — приложения (apps/dist): генератор ставит их в симулятор, как телефон.
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

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "host"))
import wallet as W  # noqa: E402


class SimDev:
    """Построчный протокол симулятора — как USB."""

    def __init__(self, binary, sim_dir):
        self.p = subprocess.Popen([binary, "lines"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, text=True, env={**os.environ, "MW_SIM_DIR": sim_dir})

    def call(self, line):
        self.p.stdin.write(line + "\n")
        self.p.stdin.flush()
        return W.parse_reply(self.p.stdout.readline().strip())

    def info(self):
        return dict(kv.split("=", 1) for kv in self.call("info").split() if "=" in kv)


def pb_varint(n):
    return W._varint(n)


def pb(field, wt, data):
    key = pb_varint(field << 3 | wt)
    if wt == 0:
        return key + pb_varint(data)
    return key + pb_varint(len(data)) + data


def tron_raw(contract_type, type_url, value, fee_limit=None):
    any_ = pb(1, 2, type_url.encode()) + pb(2, 2, value)
    contract = pb(1, 0, contract_type) + pb(2, 2, any_)
    raw = pb(1, 2, bytes.fromhex("1a2b")) + pb(4, 2, bytes.fromhex("0102030405060708"))
    raw += pb(8, 0, 1760000000000) + pb(11, 2, contract) + pb(14, 0, 1759999940000)
    if fee_limit is not None:
        raw += pb(18, 0, fee_limit)
    return raw


def main():
    sim = sys.argv[1]
    responses = {}  # "GET url" / "POST url method" -> JSON; "RAW url" -> текст

    def http_json(url, body=None, headers=None):
        key = f"GET {url}" if body is None else f"POST {url} {body.get('method', '')}".rstrip()
        if key not in responses:
            raise W.WalletError(f"нет ответа для {key}")
        return json.loads(json.dumps(responses[key]))

    def post_raw(url, raw_hex):
        return responses[f"RAW {url}"]

    def http_text(url):
        return responses[f"TEXT {url}"]

    W.http_json, W.post_raw, W.http_text = http_json, post_raw, http_text

    with tempfile.TemporaryDirectory() as d:
        dev = SimDev(sim, d)
        dev.call("restore " + "00" * 16)  # abandon ×11 about
        for f in sorted((ROOT / "apps/dist").glob("microwallet-app-*.wasm")):
            wasm = f.read_bytes()
            dev.call(f"app add {len(wasm)} {hashlib.sha256(wasm).hexdigest()}")
            for off in range(0, len(wasm), 2048):
                dev.call("app add b64 " + base64.b64encode(wasm[off:off + 2048]).decode())
            dev.call("app add end")
        w = W.Wallet(dev)
        cfg = w.cfg
        cases = []

        # ---------- BTC ----------
        # Предыдущие транзакции (устройство проверяет по ним суммы входов): детерминированные,
        # платят на наши адреса; их настоящие txid — в ответах /utxo
        def prev_tx(tag, outs):
            body = (struct.pack("<I", 2) + b"\x01" + bytes([tag]) * 32 + struct.pack("<I", 0) + b"\x00"
                    + struct.pack("<I", 0xFFFFFFFF) + bytes([len(outs)])
                    + b"".join(struct.pack("<QB", v, len(s)) + s for v, s in outs) + struct.pack("<I", 0))
            txid = hashlib.sha256(hashlib.sha256(body).digest()).digest()[::-1].hex()
            responses[f"TEXT {cfg['btc_api']}/tx/{txid}/hex"] = body.hex()
            return txid
        s00 = W.address_to_script(w.address("btc", 0, 0), cfg["hrp"])
        s01 = W.address_to_script(w.address("btc", 1, 0), cfg["hrp"])
        junk = bytes.fromhex("0014" + "99" * 20)
        ta = prev_tx(0xA1, [(30000, s00)])
        tb = prev_tx(0xA2, [(1, junk), (2, junk), (3, junk), (80000, s00)])
        tc = prev_tx(0xA3, [(4, junk), (25000, s01)])
        for chain in (0, 1):
            for i in range(W.ADDRS_DEFAULT if chain == 0 else W.CHANGE_MAX):
                a = w.address("btc", i, chain)
                used = (chain, i) in ((0, 0), (0, 1), (1, 0))
                responses[f"GET {cfg['btc_api']}/address/{a}"] = {
                    "chain_stats": {"tx_count": 1 if used else 0}, "mempool_stats": {"tx_count": 0}}
                if used:
                    utxo = {(0, 0): [(ta, 0, 30000, True), (tb, 3, 80000, True)],
                            (0, 1): [(tc, 1, 25000, False)], (1, 0): []}[(chain, i)]
                    responses[f"GET {cfg['btc_api']}/address/{a}/utxo"] = [
                        {"txid": t, "vout": v, "value": val, "status": {"confirmed": c}} for t, v, val, c in utxo]
        dest = "tb1qrp33g0q5c5txsp9arysrx4k6zdkfs4nce4xj0gdcccefvpysxf3q0sl5k7"
        d1 = w.prepare_send("btc", dest, "0.001", feerate=3)
        r = w.sign_and_send(d1, broadcast=False)
        cases.append({"coin": "btc", "dest": dest, "amount": "0.001", "feerate": "3",
                      "raw": r["raw"], "txid": r["txid"], "fee": d1.fee, "exact": True})

        # ---------- Pearl ----------
        for chain in (0, 1):
            for i in range(W.ADDRS_DEFAULT if chain == 0 else W.CHANGE_MAX):
                a = w.address("prl", i, chain)
                used = (chain, i) == (0, 0)
                responses[f"GET {cfg['prl_api']}/api/v2/address/{a}?details=basic"] = {"txs": 2 if used else 0}
                if used:
                    responses[f"GET {cfg['prl_api']}/api/v2/utxo/{a}"] = [
                        {"txid": "b1" * 32, "vout": 0, "value": "500000000", "confirmations": 150},
                        {"txid": "b2" * 32, "vout": 1, "value": "250000000", "confirmations": 5},
                        {"txid": "b3" * 32, "vout": 2, "value": "120000000", "confirmations": 7}]
                    responses[f"GET {cfg['prl_api']}/api/v2/tx/{'b2' * 32}"] = {"vin": [{"coinbase": "0101"}]}
                    responses[f"GET {cfg['prl_api']}/api/v2/tx/{'b3' * 32}"] = {"vin": [{"txid": "c0" * 32}]}
        pdest = w.address("prl", 7)
        d2 = w.prepare_send("prl", pdest, "5.5", feerate=2)
        r = w.sign_and_send(d2, broadcast=False)
        bal = w.balance("prl")
        # Подпись Schnorr со случайным aux — сравниваем txid (он без witness) и размер
        cases.append({"coin": "prl", "dest": pdest, "amount": "5.5", "feerate": "2", "raw": r["raw"],
                      "txid": r["txid"], "fee": d2.fee, "exact": False,
                      "balance": str(bal["assets"][0]["amount"]), "pending": str(bal["pending"])})

        # ---------- TRON ----------
        owner = w.address("tron", 0)
        tdest = "TTojvqHHL6UQ2gcYSHSjLsYK8B6dFV7j7Z"
        o21, t21 = W.tron_decode(owner), W.tron_decode(tdest)
        raw = tron_raw(1, "type.googleapis.com/protocol.TransferContract",
                       pb(1, 2, o21) + pb(2, 2, t21) + pb(3, 0, 5_000_000))
        responses[f"POST {cfg['tron_api']}/wallet/createtransaction"] = {
            "raw_data_hex": raw.hex(), "txID": hashlib.sha256(raw).hexdigest()}
        d3 = w.prepare_send("tron", tdest, "5", asset="trx")
        r = w.sign_and_send(d3, broadcast=False)
        cases.append({"coin": "tron", "asset": "trx", "dest": tdest, "amount": "5",
                      "raw": r["raw"], "txid": r["txid"], "fee": d3.fee, "exact": True})

        usdt21 = W.tron_decode(cfg["tron_usdt"])
        data = bytes.fromhex("a9059cbb") + b"\0" * 12 + t21[1:] + (12_500_000).to_bytes(32, "big")
        raw = tron_raw(31, "type.googleapis.com/protocol.TriggerSmartContract",
                       pb(1, 2, o21) + pb(2, 2, usdt21) + pb(4, 2, data), fee_limit=30_000_000)
        responses[f"POST {cfg['tron_api']}/wallet/triggersmartcontract"] = {
            "result": {"result": True}, "transaction": {"raw_data_hex": raw.hex(), "txID": hashlib.sha256(raw).hexdigest()}}
        d4 = w.prepare_send("tron", tdest, "12.5", asset="usdt", fee_limit="30")
        r = w.sign_and_send(d4, broadcast=False)
        cases.append({"coin": "tron", "asset": "usdt", "dest": tdest, "amount": "12.5", "feeLimit": "30",
                      "raw": r["raw"], "txid": r["txid"], "fee": d4.fee, "exact": True})

        # ---------- BSC ----------
        rpc = cfg["bsc_rpc"]
        responses[f"POST {rpc} eth_getTransactionCount"] = {"jsonrpc": "2.0", "id": 1, "result": "0x7"}
        responses[f"POST {rpc} eth_gasPrice"] = {"jsonrpc": "2.0", "id": 1, "result": hex(3 * 10**9)}
        responses[f"POST {rpc} eth_getBalance"] = {"jsonrpc": "2.0", "id": 1, "result": hex(10**18)}
        responses[f"POST {rpc} eth_call"] = {"jsonrpc": "2.0", "id": 1, "result": "0x" + (100 * 10**18).to_bytes(32, "big").hex()}
        responses[f"POST {rpc} eth_estimateGas"] = {"jsonrpc": "2.0", "id": 1, "result": hex(51000)}
        bdest = "0x3535353535353535353535353535353535353535"
        for asset, amt in (("bnb", "0.01"), ("usdt", "12.5")):
            d5 = w.prepare_send("bsc", bdest, amt, asset=asset)
            r = w.sign_and_send(d5, broadcast=False)
            cases.append({"coin": "bsc", "asset": asset, "dest": bdest, "amount": amt,
                          "raw": r["raw"], "txid": r["txid"], "fee": d5.fee, "exact": True})

        addrs = {c: w.address(c, 0) for c in ("btc", "prl", "tron", "bsc")}
        dev.p.stdin.close()

    json.dump({"addresses": addrs, "responses": responses, "cases": cases}, sys.stdout, indent=1)
    print()


if __name__ == "__main__":
    main()
