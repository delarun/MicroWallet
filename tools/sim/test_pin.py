#!/usr/bin/env python3
"""
PIN: длительность нажатия -> цифра, блокировка после включения, PIN после «Accept» подписи,
паузы после 3 ошибок, стирание после 10-й. «Набирает» PIN симулятор: $MW_SIM_PIN или
команда «simpin type 1234»; PIN ставит команда «simpin set» (есть только в симуляторе).

  tools/sim/build.sh /tmp/mwsim && sh apps/build.sh && python3 tools/sim/test_pin.py /tmp/mwsim
"""
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import test_golden as TG  # noqa: E402
sys.path.insert(0, str(HERE.parents[1] / "host"))
import wallet as W  # noqa: E402
from test_apps import Sim  # noqa: E402

REQ = json.loads((HERE / "golden.json").read_text())


class PinSim(Sim):
    def __init__(self, binary, d, **env):
        self.p = subprocess.Popen([binary, "lines"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, text=True,
                                  env={**os.environ, "MW_SIM_DIR": d, **env})

    def info(self):
        return dict(kv.split("=", 1) for kv in self.call("info").split()[1:])


def main():
    binary = sys.argv[1]
    with tempfile.TemporaryDirectory() as d:
        sim = PinSim(binary, d)
        net = sim.info()["net"]
        trx = REQ[net]["requests"]["tron_trx"]

        # ---- цифра по длительности: клик и до 1 с — 1, дальше +1 за 0.5 с, максимум 9 ----
        want = {0: 1, 300: 1, 999: 1, 1000: 2, 1499: 2, 1500: 3, 2000: 4, 4499: 8, 4500: 9, 9000: 9}
        for ms, digit in want.items():
            assert sim.call(f"simpin digit {ms}") == f"ok {digit}", (ms, sim.call(f"simpin digit {ms}"))
        print("1. цифры: <1 с — 1, 1 с — 2, 1.5 с — 3 … 4.5 с и дольше — 9")

        assert sim.call("restore " + "00" * 16).startswith("ok")
        TG.install_all(sim)
        assert sim.info()["pin"] == "0"
        assert sim.call(f"app tron sign 0 {trx}").startswith("ok "), "без PIN подпись как раньше"
        assert sim.call("simpin set 1234") == "ok" and sim.info()["pin"] == "1"
        assert sim.info()["state"] == "ready"
        assert sim.call(f"app tron sign 0 {trx}").startswith("ok "), "верный PIN после Accept"
        sim.call("simpin type 5678")
        r = sim.call(f"app tron sign 0 {trx}")
        assert r == "err rejected", r
        sim.call("simpin type 1234")
        assert sim.call(f"app tron sign 0 {trx}").startswith("ok ")
        print("2. подпись: без PIN — как раньше; с PIN — верный подписывает, неверный — отказ")
        sim.close()

        # ---- после включения — заблокировано ----
        sim = PinSim(binary, d)
        i = sim.info()
        assert i["state"] == "locked" and i["pin"] == "1", i

        class Dev:  # хост видит заблокированное устройство
            def call(self, line):
                return W.parse_reply(sim.call(line))

            def info(self):
                return dict(kv.split("=", 1) for kv in self.call("info").split() if "=" in kv)
        w = W.Wallet(Dev())
        assert w.locked and not w.ready
        for f in (lambda: w.address("tron"), lambda: Dev().call("fingerprint")):
            try:
                f()
                raise AssertionError("заблокированное устройство ответило")
            except W.WalletError as e:
                assert str(e) == W.LOCKED, e
        assert sim.call("ping") == "ok pong"
        for c in ("app tron addr 0", "fingerprint", "wipe", "apps", "ota abort"):
            assert sim.call(c) == "err locked", c
        sim.call("simpin type 1234")
        assert sim.call("app tron addr 0").startswith("ok T"), "верный PIN разблокирует"
        assert sim.info()["state"] == "ready"
        assert not w.refresh_info().locked and w.ready and w.address("tron").startswith("T")
        sim.close()
        print("3. после включения: заблокировано (кроме ping/info), верный PIN разблокирует")

        # ---- 3 ошибки — пауза: даже верный PIN до её конца не проверяется ----
        sim = PinSim(binary, d, MW_SIM_PIN="9999")
        for _ in range(3):
            assert sim.call("fingerprint") == "err locked"
        sim.call("simpin type 1234")
        assert sim.call("fingerprint") == "err locked", "во время паузы PIN не принимается"
        sim.close()
        sim = PinSim(binary, d, MW_SIM_PIN="1234", MW_SIM_NODELAY="1")
        assert sim.call("fingerprint").startswith("ok ")
        sim.close()
        print("4. после 3 ошибок — пауза (верный PIN ждёт); верный PIN сбрасывает счётчик")

        # ---- 10 ошибок подряд — кошелёк стёрт ----
        sim = PinSim(binary, d, MW_SIM_PIN="9999", MW_SIM_NODELAY="1")
        for k in range(9):
            assert sim.call("fingerprint") == "err locked", k
            assert sim.info()["state"] == "locked"
        assert sim.call("fingerprint") == "err no wallet on device"  # 10-я ошибка стёрла кошелёк
        i = sim.info()
        assert i["state"] == "empty" and i["pin"] == "0", i
        assert sim.call("restore " + "00" * 16).startswith("ok"), "после стирания — снова восстановить"
        assert sim.info()["pin"] == "0"
        assert sim.call("simpin set 4321") == "ok" and sim.call("simpin off") == "ok"
        assert sim.info()["pin"] == "0"
        sim.close()
        print("5. 10 ошибок подряд — seed и PIN стёрты; восстановление работает, PIN снимается")
    print("всё прошло")


if __name__ == "__main__":
    main()
