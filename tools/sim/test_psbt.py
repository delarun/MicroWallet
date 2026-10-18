#!/usr/bin/env python3
"""
PSBT: Sparrow/Electrum-совместимость. PSBT строит независимая библиотека embit (как это
сделал бы Sparrow), хост (host/psbt.py + Wallet.sign_psbt) подписывает через симулятор
устройства, подписи и итоговая транзакция проверяются независимо (BIP143 + ECDSA, embit).

  python3 -m venv /tmp/venv && /tmp/venv/bin/pip install embit==0.8.0 pyserial
  tools/sim/build.sh mwsim && sh apps/build.sh && /tmp/venv/bin/python tools/sim/test_psbt.py mwsim
"""
import struct
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parents[1] / "host"))
import psbt as P  # noqa: E402
import wallet as W  # noqa: E402
import test_golden as TG  # noqa: E402
import test_utxo as TU  # noqa: E402  (prev_tx, bip143, ecdsa_verify)
from test_apps import Sim  # noqa: E402

from embit import bip32, bip39  # noqa: E402
from embit.descriptor.checksum import add_checksum  # noqa: E402
from embit.psbt import PSBT, DerivationPath  # noqa: E402
from embit.script import Script  # noqa: E402
from embit.transaction import Transaction, TransactionInput, TransactionOutput  # noqa: E402

H = 0x80000000
DEST = bytes.fromhex("0014" + "77" * 20)


class SimDev:
    """Устройство для Wallet: симулятор по строкам."""

    def __init__(self, sim):
        self.sim = sim

    def call(self, line):
        return W.parse_reply(self.sim.call(line))

    def info(self):
        return dict(kv.split("=", 1) for kv in self.call("info").split() if "=" in kv)


def main():
    root = bip32.HDKey.from_seed(bip39.mnemonic_to_seed("abandon " * 11 + "about"))
    fp = root.my_fingerprint

    def key(chain, idx):
        path = [84 | H, 1 | H, H, chain, idx]
        return root.derive(path).key.sec(), path

    def p2wpkh(pub):
        return b"\x00\x14" + TU.hash160(pub)

    def make(spend, outs, change=None, nw=True, sighash=None, fp_in=fp):
        """spend: [(chain, idx, amount)] из одной prev tx; outs: [(amount, script)];
        change: (amount, chain1 idx для пути, idx для скрипта)."""
        prev_raw, prev_id = TU.prev_tx([(a, p2wpkh(key(c, i)[0])) for c, i, a in spend])
        prev = Transaction.parse(prev_raw)
        vin = [TransactionInput(bytes.fromhex(prev_id), k, sequence=0xFFFFFFFD) for k in range(len(spend))]
        vout = [TransactionOutput(a, Script(s)) for a, s in outs]
        if change:
            vout.append(TransactionOutput(change[0], Script(p2wpkh(key(1, change[2])[0]))))
        p = PSBT(Transaction(vin=vin, vout=vout))
        for k, (c, i, _) in enumerate(spend):
            pub, path = key(c, i)
            if nw:
                p.inputs[k].non_witness_utxo = prev
            p.inputs[k].witness_utxo = prev.vout[k]
            p.inputs[k].bip32_derivations[root.derive(path).key.get_public_key()] = DerivationPath(fp_in, path)
            if sighash is not None:
                p.inputs[k].sighash_type = sighash
        if change:
            pub, path = key(1, change[1])
            p.outputs[-1].bip32_derivations[root.derive(path).key.get_public_key()] = DerivationPath(fp, path)
        return p.serialize(), prev_raw, prev_id

    with tempfile.TemporaryDirectory() as d:
        sim = Sim(sys.argv[1], d)
        assert sim.call("restore " + "00" * 16).startswith("ok")
        TG.install_all(sim)
        w = W.Wallet(SimDev(sim))
        assert w.fingerprint() == fp.hex()

        # ---- полный проход ----
        data, prev_raw, prev_id = make([(0, 0, 30000), (0, 3, 50000), (1, 2, 20000)], [(60000, DEST)], (39000, 4, 4))
        assert P.Psbt.parse(data).serialize() == data, "разбор/сборка PSBT меняет байты"
        signed, raw = w.sign_psbt(data)
        assert raw is None
        sp = PSBT.from_string(signed)  # embit читает наш PSBT
        mine = P.Psbt.parse(signed)
        tx = {"version": mine.tx["version"], "locktime": mine.tx["locktime"],
              "ins": [{"txid": t["txid"], "vout": t["vout"], "value": [30000, 50000, 20000][k],
                       "sequence": t["sequence"]} for k, t in enumerate(mine.tx["ins"])],
              "outs": mine.tx["outs"]}
        for k, inp in enumerate(sp.inputs):
            assert len(inp.partial_sigs) == 1
            (pub, sig), = inp.partial_sigs.items()
            assert sig[-1] == 1 and TU.ecdsa_verify(pub.sec(), TU.bip143(tx, k, pub.sec()), sig[:-1]), f"вход {k}"
        print("1. PSBT из embit: 3 входа, сдача — подписи BIP143 верны, embit читает результат, байты не теряются")

        # ---- финализация ----
        signed, raw = w.sign_psbt(data, finalize=True)
        final = Transaction.parse(bytes.fromhex(raw))
        unsigned = PSBT.parse(data).tx
        assert final.txid() == unsigned.txid(), "txid финальной транзакции не тот"
        for k, vin in enumerate(final.vin):
            sig, pub = vin.witness.items
            assert TU.ecdsa_verify(pub, TU.bip143(tx, k, pub), sig[:-1])
        assert PSBT.from_string(signed).inputs[0].final_scriptwitness is not None
        print("2. финализация: готовая транзакция, txid совпадает, свидетели верны")

        # ---- только witness_utxo: предыдущую транзакцию хост скачивает сам ----
        data2, prev2, id2 = make([(0, 1, 70000)], [(65000, DEST)], nw=False)
        W.http_text = lambda url: prev2.hex() if url.endswith(f"/tx/{id2}/hex") else (_ for _ in ()).throw(AssertionError(url))
        signed, _ = w.sign_psbt(data2)
        assert len(PSBT.from_string(signed).inputs[0].partial_sigs) == 1
        print("3. в PSBT только witness_utxo — предыдущая транзакция скачана, подпись есть")

        # ---- отказы ----
        def refuse(d, want):
            try:
                w.sign_psbt(d)
            except W.WalletError as e:
                assert want in str(e), (want, str(e))
                return
            raise AssertionError(f"подписано, а не должно: {want}")
        refuse(make([(0, 0, 30000)], [(25000, DEST)], fp_in=b"\x01\x02\x03\x04")[0], "не из этого кошелька")
        refuse(make([(0, 0, 30000)], [(25000, DEST)], sighash=2)[0], "SIGHASH_ALL")
        # сдача: путь говорит idx 5, а скрипт — от idx 6 — устройство выводит адрес само
        refuse(make([(0, 0, 30000)], [(10000, DEST)], (19000, 5, 6))[0], "change mismatch")
        print("4. отказ: чужой отпечаток, SIGHASH не ALL, подменённая сдача (ловит устройство)")

        # ---- дескриптор для Sparrow/Electrum ----
        acct = root.derive([84 | H, 1 | H, H]).to_public()
        want = add_checksum(f"wpkh([{fp.hex()}/84h/1h/0h]{acct.to_string(version=bytes.fromhex('043587cf'))}/<0;1>/*)")
        assert w.descriptor() == want, (w.descriptor(), want)
        print(f"5. дескриптор как у embit: {want[:40]}…")
        sim.close()
    print("всё прошло")


if __name__ == "__main__":
    main()
