"""Разметка адресов для AML-проверки BSC в приложении на телефоне (android/.../assets/aml_labels.json).

То же, что update_labels.py в aml-check: публичные списки + своя разметка labels.manual.yaml
(ручные записи главнее). Файл попадает в APK — обновляется с релизом приложения.

  1. OFAC SDN — EVM-адреса (ETH, BSC, USDT/USDC ERC-20, ARB) из
     github.com/0xB10C/ofac-sanctioned-digital-currency-addresses (ветка lists, собирается
     из sdn_advanced.xml каждую ночь): Garantex, Grinex, Suex, Chatex и другие.
  2. HTX (Huobi) — публичные метки Etherscan/BscScan (github.com/brianleect/etherscan-labels,
     снимок 2023 года), только кошельки.

Запуск: python3 tools/aml/update_labels.py
"""
import json
import re
import sys
import urllib.request
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "android/app/src/main/assets/aml_labels.json"
MANUAL = Path(__file__).with_name("labels.manual.yaml")
OFAC_BASE = "https://raw.githubusercontent.com/0xB10C/ofac-sanctioned-digital-currency-addresses/lists/"
OFAC_FILES = ["ETH", "BSC", "USDT", "USDC", "ARB"]
LABELS_URL = "https://raw.githubusercontent.com/brianleect/etherscan-labels/main/data/{chain}/combined/combinedAllLabels.json"
EVM = re.compile(r"^0x[0-9a-fA-F]{40}$")
HTX_NAME = re.compile(r"^Huobi( \d+|: Deposit Funder( \d+)?)$")
RISKS = {"sanctions", "high", "medium", "low", "trusted"}


def get(url: str) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": "microwallet-labels"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.read()


def ofac() -> dict:
    out = {}
    for name in OFAC_FILES:
        for line in get(f"{OFAC_BASE}sanctioned_addresses_{name}.txt").decode().split():
            if EVM.match(line):
                out[line.lower()] = {"label": "OFAC SDN", "risk": "high",
                                     "source": f"OFAC SDN list ({name}), via 0xB10C mirror"}
    return out


def htx() -> dict:
    out = {}
    for chain in ("bscscan", "etherscan"):
        for addr, meta in json.loads(get(LABELS_URL.format(chain=chain))).items():
            name = (meta or {}).get("name", "")
            if EVM.match(addr) and HTX_NAME.match(name):
                out[addr.lower()] = {"label": f"HTX (ex-Huobi): {name}", "risk": "high",
                                     "source": f"{chain} public label (snapshot 2023)"}
    return out


def main():
    merged = {}
    for source in (htx(), ofac()):  # OFAC главнее HTX
        merged.update(source)
    manual = (yaml.safe_load(MANUAL.read_text(encoding="utf-8")) or {}).get("addresses") or [] if MANUAL.exists() else []
    for item in manual:
        addr = str(item["address"]).lower()
        if not EVM.match(addr) or item.get("risk", "medium") not in RISKS:
            sys.exit(f"labels.manual.yaml: плохая запись {item}")
        merged[addr] = {"label": item.get("label", ""), "risk": item.get("risk", "medium"),
                        "source": item.get("source", "labels.manual.yaml")}
    rows = [{"address": a, **v} for a, v in sorted(merged.items())]
    OUT.write_text(json.dumps({"addresses": rows}, ensure_ascii=False, indent=0) + "\n", encoding="utf-8")
    by = {}
    for r in rows:
        k = r["label"].split(":")[0]
        by[k] = by.get(k, 0) + 1
    print(f"{OUT.relative_to(ROOT)}: {len(rows)} адресов", by)


if __name__ == "__main__":
    main()
