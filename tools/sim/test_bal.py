#!/usr/bin/env python3
"""
Кэш баланса на устройстве (команда bal): телефон или ПК присылают суммы после обновления,
устройство показывает их в меню монеты. Пишется только при кошельке и только для установленного
приложения, печатный ASCII до 120 символов; стирается вместе с seed (wipe, restore).

  tools/sim/build.sh /tmp/mwsim && sh apps/build.sh && python3 tools/sim/test_bal.py /tmp/mwsim
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parents[1] / "host"))
import test_golden as TG  # noqa: E402
import wallet as W  # noqa: E402
from test_apps import Sim  # noqa: E402


class BalSim(Sim):
    def __init__(self, binary, d):
        self.p = subprocess.Popen([binary, "lines"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, text=True,
                                  env={**os.environ, "MW_SIM_DIR": d, "MW_SIM_ANSWER": "yes"})


def main():
    binary = sys.argv[1]
    with tempfile.TemporaryDirectory() as d:
        sim = BalSim(binary, d)
        TG.install_all(sim)
        assert sim.call("bal btc 1761777600 0.5 BTC").startswith("err no wallet"), "без кошелька — нельзя"
        assert sim.call("restore " + "00" * 16).startswith("ok")
        assert sim.call("bal btc") == "err none"
        assert sim.call("bal btc 1761777600 0.0123 BTC") == "ok"
        assert sim.call("bal eth 1761781200 0.5 ETH;120 USDT;3 DAI") == "ok"
        assert sim.call("bal btc") == "ok 1761777600 0.0123 BTC"
        assert sim.call("bal eth") == "ok 1761781200 0.5 ETH;120 USDT;3 DAI"
        print("1. суммы монеты и токенов сохраняются и читаются обратно")

        assert sim.call("bal btc 1761777600 0.0124 BTC") == "ok"
        assert sim.call("bal btc") == "ok 1761777600 0.0124 BTC", "новый баланс заменяет старый"
        for bad in ("bal xyz 1 1 X", "bal btc abc 1 BTC", "bal btc 1", "bal btc 1 " + "9" * 121,
                    "bal btc 1 0.5 \x01BTC", "bal"):
            assert sim.call(bad).startswith("err"), bad
        assert sim.call("bal btc") == "ok 1761777600 0.0124 BTC", "ошибочная команда не портит кэш"
        print("2. отказ: чужое приложение, плохое время, пустой текст, длиннее 120, не ASCII")

        assert sim.call("wipe").startswith("ok")
        assert sim.call("restore " + "00" * 16).startswith("ok")
        assert sim.call("bal btc") == "err none" and sim.call("bal eth") == "err none"
        assert sim.call("bal btc 1761777600 1 BTC") == "ok"
        assert sim.call("restore " + "11" * 16).startswith("err"), "restore поверх кошелька — отказ"
        sim.call("wipe")
        assert sim.call("restore " + "11" * 16).startswith("ok")
        assert sim.call("bal btc") == "err none"
        print("3. после сброса и восстановления другого seed — балансов нет")

        # ПК (host/wallet.py) шлёт то же, что телефон: монета первой, нулевые токены не нужны
        b = {"assets": [{"id": "usdt", "symbol": "USDT", "decimals": 6, "amount": 12_500_000},
                        {"id": "trx", "symbol": "TRX", "decimals": 6, "amount": 5_000_000},
                        {"id": "usdc", "symbol": "USDC", "decimals": 6, "amount": 0}]}
        assert W.balance_text("tron", b) == "5 TRX;12.5 USDT"
        assert sim.call(f"bal tron 1761777600 {W.balance_text('tron', b)}") == "ok"
        print("4. текст ПК: монета первой, ненулевые токены — устройство его принимает")
        sim.close()
    print("всё прошло")


if __name__ == "__main__":
    main()
