#!/usr/bin/env python3
"""
Эталон PSBT для теста Android (android/app/src/test/resources/psbt.json): PSBT строит embit
(как Sparrow), подписывает Python-хост через симулятор; Kotlin-кошелёк должен получить то же
самое байт в байт (подписи детерминированы, RFC 6979).

  /tmp/venv/bin/python tools/sim/gen_psbt_vectors.py mwsim > android/app/src/test/resources/psbt.json
"""
import json
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
from test_psbt import SimDev  # noqa: E402

from embit import bip32, bip39  # noqa: E402
from embit.psbt import PSBT, DerivationPath  # noqa: E402
from embit.script import Script  # noqa: E402
from embit.transaction import Transaction, TransactionInput, TransactionOutput  # noqa: E402

H = 0x80000000
DEST = bytes.fromhex("0014" + "77" * 20)


def main():
    root = bip32.HDKey.from_seed(bip39.mnemonic_to_seed("abandon " * 11 + "about"))
    fp = root.my_fingerprint

    def pk(path):
        return root.derive(path).key.get_public_key()

    def p2wpkh(path):
        return b"\x00\x14" + TU.hash160(pk(path).sec())

    spend = [[84 | H, 1 | H, H, 0, 0], [84 | H, 1 | H, H, 0, 3], [84 | H, 1 | H, H, 1, 2]]
    chg = [84 | H, 1 | H, H, 1, 4]
    prev_raw, prev_id = TU.prev_tx([(30000 + 1000 * k, p2wpkh(p)) for k, p in enumerate(spend)])
    prev = Transaction.parse(prev_raw)

    def build(nw):
        vin = [TransactionInput(bytes.fromhex(prev_id), k, sequence=0xFFFFFFFD) for k in range(len(spend))]
        vout = [TransactionOutput(60000, Script(DEST)), TransactionOutput(32000, Script(p2wpkh(chg)))]
        p = PSBT(Transaction(vin=vin, vout=vout))
        for k, path in enumerate(spend):
            if nw:
                p.inputs[k].non_witness_utxo = prev
            p.inputs[k].witness_utxo = prev.vout[k]
            p.inputs[k].bip32_derivations[pk(path)] = DerivationPath(fp, path)
        p.outputs[1].bip32_derivations[pk(chg)] = DerivationPath(fp, chg)
        return p.to_string()

    with tempfile.TemporaryDirectory() as d:
        sim = Sim(sys.argv[1], d)
        sim.call("restore " + "00" * 16)
        TG.install_all(sim)
        w = W.Wallet(SimDev(sim))
        full, wonly = build(True), build(False)
        signed, _ = w.sign_psbt(full)
        signed_f, raw = w.sign_psbt(full, finalize=True)
        W.http_text = lambda url: prev_raw.hex()
        signed_w, _ = w.sign_psbt(wonly)
        out = {"psbt": full, "signed": signed, "signed_final": signed_f, "raw": raw,
               "psbt_wonly": wonly, "signed_wonly": signed_w, "prev": {prev_id: prev_raw.hex()},
               "descriptor": w.descriptor()}
        sim.close()
    json.dump(out, sys.stdout, indent=1)
    print()


if __name__ == "__main__":
    main()
