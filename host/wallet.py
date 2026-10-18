#!/usr/bin/env python3
"""
MicroWallet: библиотека для общения с устройством и консольная программа.
Монеты: BTC, TRON (TRX, USDT-TRC20), BSC (BNB, USDT-BEP20), Pearl (PRL).
Графическое приложение — app.py, оно использует этот же модуль.

  python wallet.py info | wipe | boot
  python wallet.py restore                 seed-фраза в пустое устройство
  python wallet.py update firmware.bin     обновить прошивку (по USB или --ble)
  python wallet.py ble-scan                найти устройства по Bluetooth
  python wallet.py apps                    установленные приложения (WebAssembly)
  python wallet.py app-install eth.wasm    установить или обновить (подтвердить на устройстве)
  python wallet.py app-rm eth              удалить (подтвердить на устройстве)
  python wallet.py app eth addr 0          команда приложению
  python wallet.py --ble ADDR ...          любая команда по Bluetooth (ADDR или auto)

  python wallet.py btc  receive [-i N] | balance | xpub
  python wallet.py btc  send ADDR SATS [--feerate SAT_PER_VB] [--dry-run]

  python wallet.py tron receive [-i N] | balance [-i N]
  python wallet.py tron send ADDR AMOUNT [--token usdt|trx] [--fee-limit 40] [--dry-run]

  python wallet.py bsc  receive [-i N] | balance [-i N]
  python wallet.py bsc  send ADDR AMOUNT [--token usdt|bnb] [--gas-price GWEI] [--dry-run]

  python wallet.py prl  receive [-i N] | balance
  python wallet.py prl  send ADDR AMOUNT [--feerate GRAIN_PER_VB] [--dry-run]

Старые команды (receive, balance, send, xpub без монеты) работают как btc.
Сеть (testnet/mainnet) берётся из прошивки (`info`).
По Bluetooth связь шифрованная: при первом подключении сверь 6-значный код
на экране устройства и здесь, затем подтверди кнопкой.

Pearl: адрес из `prl receive` можно указать как адрес выплат майнинга.
Награды за блок созревают 100 блоков, `prl balance` показывает их отдельно.

Хост НЕ знает приватных ключей. Он собирает транзакцию, а устройство само
показывает получателя/сумму/комиссию и подписывает только после
подтверждения кнопкой. Адрес для приёма (`receive`) тоже надо подтвердить.
"""
import argparse
import base64
import hashlib
import json
import math
import os
import struct
import sys
import threading
import time
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path
from decimal import Decimal, InvalidOperation
from typing import Callable

import serial
from serial.tools import list_ports

NETS = {
    "testnet": {
        "btc": "tBTC",
        "btc_api": "https://mempool.space/testnet4/api",
        "btc_tx_url": "https://mempool.space/testnet4/tx/",
        "hrp": "tb",
        "tron_api": "https://nile.trongrid.io",
        "tron_tx_url": "https://nile.tronscan.org/#/transaction/",
        "tron_usdt": "TXYZopYRdj2D9XRtbG411XZZ3kM5VkAeBf",
        "bsc_rpc": "https://bsc-testnet-rpc.publicnode.com",
        "bsc_tx_url": "https://testnet.bscscan.com/tx/",
        "bsc_usdt": "0x337610d27c682E347C9cD60BD4b3b107C9d34dDd",
        "bnb": "tBNB",
        "prl_api": "https://blockbook.testnet.pearlresearch.ai",
        "prl_hrp": "tprl",
        "prl": "tPRL",
    },
    "mainnet": {
        "btc": "BTC",
        "btc_api": "https://mempool.space/api",
        "btc_tx_url": "https://mempool.space/tx/",
        "hrp": "bc",
        "tron_api": "https://api.trongrid.io",
        "tron_tx_url": "https://tronscan.org/#/transaction/",
        "tron_usdt": "TR7NHqjeKQxGTCi8q8ZY4pL8otSzgjLj6t",
        "bsc_rpc": "https://bsc-dataseed.bnbchain.org",
        "bsc_tx_url": "https://bscscan.com/tx/",
        "bsc_usdt": "0x55d398326f99059fF775485246999027B3197955",
        "bnb": "BNB",
        "prl_api": "https://blockbook.pearlresearch.ai",
        "prl_hrp": "prl",
        "prl": "PRL",
    },
}
# Монеты и их активы: (id, decimals). Символ берётся из NETS там, где он зависит от сети.
COINS = {
    "btc": {"name": "Bitcoin", "assets": [("btc", 8)]},
    "tron": {"name": "TRON", "assets": [("usdt", 6), ("trx", 6)]},
    "bsc": {"name": "BNB Chain", "assets": [("usdt", 18), ("bnb", 18)]},
    "prl": {"name": "Pearl", "assets": [("prl", 8)]},
}
ADDRS_DEFAULT = 20  # адресов для получения, если прошивка не сообщает addrs= (до 0.6)
CHANGE_MAX = 20     # адреса сдачи идут подряд: проверяем до первого пустого, не дальше
TX_MAX_IN = 16  # лимит прошивки
PRL_COINBASE_MATURITY = 100
UA = {"User-Agent": "microwallet/0.5", "Content-Type": "application/json"}


class WalletError(Exception):
    """Понятная пользователю ошибка (сеть, ввод, нехватка средств)."""


class DeviceError(WalletError):
    """Устройство ответило "err ...". Например, "rejected" — отказ кнопкой."""


def asset_symbol(cfg, asset):
    return {"usdt": "USDT", "trx": "TRX"}.get(asset) or cfg[asset]


# ---------------- связь с устройством ----------------

USB_VIDS = (0x303A,)  # Espressif (ESP32-S3 USB Serial/JTAG)


def find_ports():
    return [p.device for p in list_ports.comports() if p.vid in USB_VIDS]


def parse_reply(resp):
    """"ok ..." -> текст после ok; "err ..." -> DeviceError."""
    if resp.startswith("ok"):
        return resp[3:]
    raise DeviceError(resp[4:] if resp.startswith("err ") else resp or "нет ответа")


CHUNK = 2048  # байт в одной строке app put b64 / app get


def app_call(dev, cmd, blob=None):
    """Команда приложению на устройстве (USB или BLE: у обоих один call()).
    blob — большой вход: уходит заранее блоком (app put), к команде добавляется « @».
    Ответ длиннее строки устройство отдаёт как «@<размер> <sha256>» — забираем кусками."""
    if blob is not None:
        dev.call(f"app put {len(blob)} {hashlib.sha256(blob).hexdigest()}")
        for off in range(0, len(blob), CHUNK):
            dev.call("app put b64 " + base64.b64encode(blob[off:off + CHUNK]).decode())
        dev.call("app put end")
        cmd += " @"
    r = dev.call(cmd)
    if not r.startswith("@"):
        return r
    size, sha = r[1:].split()
    data = b""
    while len(data) < int(size):
        part = base64.b64decode(dev.call(f"app get {len(data)}"))
        if not part:
            break
        data += part
    if len(data) != int(size) or hashlib.sha256(data).hexdigest() != sha:
        raise DeviceError("длинный ответ пришёл повреждённым")
    return data.decode()


class Device:
    def __init__(self, port=None):
        if port is None:
            ports = find_ports()
            if not ports:
                raise WalletError("MicroWallet не найден по USB. Подключи его (если он выключен — "
                                  "нажми кнопку) или укажи порт: --port /dev/ttyACM0 или COM5")
            port = ports[0]
        self.port = port
        try:
            self.s = serial.Serial(port, 115200, timeout=None)
        except serial.SerialException as e:
            raise WalletError(f"Не удалось открыть {port}: {e}")
        self.s.reset_input_buffer()
        self.lock = threading.Lock()

    def call(self, line):
        with self.lock:
            try:
                self.s.write((line + "\n").encode())
                resp = self.s.readline().decode().strip()
            except (serial.SerialException, OSError) as e:
                raise DeviceError(f"связь с устройством потеряна: {e}")
        return parse_reply(resp)

    def info(self):
        return dict(kv.split("=", 1) for kv in self.call("info").split() if "=" in kv)

    def close(self):
        try:
            self.s.close()
        except serial.SerialException:
            pass


def open_device(port=None, ble=None, on_code=None):
    """USB (по умолчанию) или Bluetooth: ble = адрес или "auto" (первое найденное)."""
    if not ble:
        return Device(port)
    import ble as B
    name = "MicroWallet"
    if ble == "auto":
        found = B.scan()
        if not found:
            raise WalletError("MicroWallet по Bluetooth не найден — включи Bluetooth на устройстве")
        ble, name, _ = max(found, key=lambda x: x[2] or -999)
    return B.BleDevice(ble, name, on_code=on_code)


# ---------------- seed-фраза (BIP39) ----------------

_WORDS = None


def bip39_words():
    global _WORDS
    if _WORDS is None:
        _WORDS = (Path(__file__).resolve().parent / "bip39_english.txt").read_text().split()
    return _WORDS


def mnemonic_to_entropy(phrase):
    """12/18/24 слова -> энтропия; проверяет словарь и контрольную сумму."""
    words = phrase.lower().split()
    if len(words) not in (12, 18, 24):
        raise WalletError(f"Нужно 12, 18 или 24 слова, а введено {len(words)}")
    index = {w: i for i, w in enumerate(bip39_words())}
    bad = [w for w in words if w not in index]
    if bad:
        raise WalletError(f"Слов нет в словаре BIP39: {', '.join(bad)}")
    bits = "".join(f"{index[w]:011b}" for w in words)
    ent_bits = len(bits) * 32 // 33
    ent = int(bits[:ent_bits], 2).to_bytes(ent_bits // 8, "big")
    chk = f"{hashlib.sha256(ent).digest()[0]:08b}"[:len(bits) - ent_bits]
    if bits[ent_bits:] != chk:
        raise WalletError("Неверная контрольная сумма фразы — проверь слова и их порядок")
    return ent


# ---------------- суммы ----------------

def to_units(s, decimals):
    try:
        d = Decimal(str(s))
    except InvalidOperation:
        raise WalletError(f"Неверная сумма: {s}")
    v = d.scaleb(decimals)
    if d <= 0 or v != v.to_integral_value():
        raise WalletError(f"Неверная сумма: {s} (больше 0, не более {decimals} знаков после точки)")
    return int(v)


def fmt_units(v, decimals):
    s = f"{Decimal(v).scaleb(-decimals):f}"
    return s.rstrip("0").rstrip(".") if "." in s else s


# ---------------- http ----------------

RETRIES = 3  # на 429/503: сервер просит подождать (mempool.space ограничивает частоту)


def _retry_wait(e, attempt):
    """Сколько ждать перед повтором: Retry-After (секунды), иначе 2, 4, 8 с."""
    try:
        return min(30.0, max(1.0, float(e.headers.get("Retry-After", ""))))
    except (TypeError, ValueError):
        return 2.0 ** (attempt + 1)


def http_json(url, body=None, headers=None):
    data = None if body is None else json.dumps(body).encode()
    host = url.split('/')[2]
    for attempt in range(RETRIES + 1):
        req = urllib.request.Request(url, data=data, headers={**UA, **(headers or {})})
        try:
            with urllib.request.urlopen(req, timeout=20) as r:
                return json.loads(r.read())
        except urllib.error.HTTPError as e:
            if e.code in (429, 503) and attempt < RETRIES:
                time.sleep(_retry_wait(e, attempt))
                continue
            if e.code == 429:
                raise WalletError(f"{host}: слишком много запросов, попробуй через минуту")
            raise WalletError(f"HTTP {e.code} от {url}: {e.read().decode(errors='replace')[:300]}")
        except (urllib.error.URLError, TimeoutError, OSError) as e:
            raise WalletError(f"Нет связи с {host}: {e}")


def post_raw(url, raw_hex):
    req = urllib.request.Request(url, data=raw_hex.encode(), method="POST",
                                 headers={"User-Agent": UA["User-Agent"]})
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            return r.read().decode()
    except urllib.error.HTTPError as e:
        raise WalletError(f"Сеть отклонила транзакцию: {e.read().decode(errors='replace')[:300]}")
    except (urllib.error.URLError, TimeoutError, OSError) as e:
        raise WalletError(f"Нет связи с {url.split('/')[2]}: {e}")


# ---------------- bech32 / bech32m (BIP173, BIP350) ----------------

CHARSET = "qpzry9x8gf2tvdw0s3jn54khce6mua7l"


def _polymod(values):
    gen = [0x3B6A57B2, 0x26508E6D, 0x1EA119FA, 0x3D4233DD, 0x2A1462B3]
    chk = 1
    for v in values:
        b = chk >> 25
        chk = (chk & 0x1FFFFFF) << 5 ^ v
        for i in range(5):
            chk ^= gen[i] if (b >> i) & 1 else 0
    return chk


def _hrp_expand(hrp):
    return [ord(x) >> 5 for x in hrp] + [0] + [ord(x) & 31 for x in hrp]


def _convertbits(data, frombits, tobits, pad):
    acc = bits = 0
    out = []
    maxv = (1 << tobits) - 1
    for v in data:
        acc = (acc << frombits) | v
        bits += frombits
        while bits >= tobits:
            bits -= tobits
            out.append((acc >> bits) & maxv)
    if pad and bits:
        out.append((acc << (tobits - bits)) & maxv)
    elif not pad and (bits >= frombits or (acc << (tobits - bits)) & maxv):
        return None
    return out


def address_to_script(addr, hrp, pearl=False):
    a = addr.lower()
    if addr != a and addr != addr.upper():
        raise WalletError("смешанный регистр в адресе")
    pos = a.rfind("1")
    if a[:pos] != hrp:
        raise WalletError(f"поддерживаются только bech32-адреса этой сети ({hrp}1...)")
    data = [CHARSET.find(c) for c in a[pos + 1:]]
    if -1 in data or len(data) < 7:
        raise WalletError("неверный адрес")
    const = _polymod(_hrp_expand(hrp) + data)
    ver = data[0]
    if (ver == 0 and const != 1) or (ver != 0 and const != 0x2BC830A3):
        raise WalletError("неверная контрольная сумма адреса")
    prog = _convertbits(data[1:-6], 5, 8, False)
    if prog is None or not (2 <= len(prog) <= 40):
        raise WalletError("неверный адрес")
    if ver == 0 and len(prog) not in (20, 32):
        raise WalletError("неверный адрес")
    if pearl:
        # Pearl принимает только Taproot (v1) и P2MR (v2) с 32-байтной программой
        if ver not in (1, 2) or len(prog) != 32:
            raise WalletError("Pearl поддерживает только адреса prl1p... (Taproot) и P2MR")
    elif not ((ver == 0) or (ver == 1 and len(prog) == 32)):
        raise WalletError("устройство пока понимает только v0 и taproot")
    return bytes([0x50 + ver if ver else 0x00, len(prog)]) + bytes(prog)


# ---------------- keccak256 (не путать с SHA3-256 из hashlib) ----------------

_RC = [
    0x0000000000000001, 0x0000000000008082, 0x800000000000808A, 0x8000000080008000,
    0x000000000000808B, 0x0000000080000001, 0x8000000080008081, 0x8000000000008009,
    0x000000000000008A, 0x0000000000000088, 0x0000000080008009, 0x000000008000000A,
    0x000000008000808B, 0x800000000000008B, 0x8000000000008089, 0x8000000000008003,
    0x8000000000008002, 0x8000000000000080, 0x000000000000800A, 0x800000008000000A,
    0x8000000080008081, 0x8000000000008080, 0x0000000080000001, 0x8000000080008008,
]
_ROT = [[0, 36, 3, 41, 18], [1, 44, 10, 45, 2], [62, 6, 43, 15, 61],
        [28, 55, 25, 21, 56], [27, 20, 39, 8, 14]]
_M64 = (1 << 64) - 1


def _rol(x, n):
    return ((x << n) | (x >> (64 - n))) & _M64 if n else x


def keccak256(msg):
    m = bytearray(msg) + b"\x01"
    while len(m) % 136:
        m += b"\x00"
    m[-1] |= 0x80
    A = [[0] * 5 for _ in range(5)]
    for off in range(0, len(m), 136):
        for i in range(17):
            A[i % 5][i // 5] ^= int.from_bytes(m[off + 8 * i:off + 8 * i + 8], "little")
        for rc in _RC:
            C = [A[x][0] ^ A[x][1] ^ A[x][2] ^ A[x][3] ^ A[x][4] for x in range(5)]
            D = [C[(x - 1) % 5] ^ _rol(C[(x + 1) % 5], 1) for x in range(5)]
            B = [[0] * 5 for _ in range(5)]
            for x in range(5):
                for y in range(5):
                    B[y][(2 * x + 3 * y) % 5] = _rol(A[x][y] ^ D[x], _ROT[x][y])
            A = [[B[x][y] ^ (~B[(x + 1) % 5][y] & B[(x + 2) % 5][y]) for y in range(5)]
                 for x in range(5)]
            A[0][0] ^= rc
    return b"".join(A[i % 5][i // 5].to_bytes(8, "little") for i in range(4))


# ---------------- адреса TRON и EVM ----------------

B58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"


def tron_decode(addr):
    """T... -> 21 байт (0x41 + 20)."""
    n = 0
    for c in addr:
        if c not in B58:
            raise WalletError("неверный TRON-адрес")
        n = n * 58 + B58.index(c)
    raw = n.to_bytes(25, "big") if n < 1 << 200 else b""
    if len(raw) != 25 or raw[0] != 0x41:
        raise WalletError("неверный TRON-адрес (ожидается T...)")
    body, chk = raw[:21], raw[21:]
    if hashlib.sha256(hashlib.sha256(body).digest()).digest()[:4] != chk:
        raise WalletError("неверная контрольная сумма TRON-адреса")
    return body


def evm_checksum(a20):
    h = a20.hex()
    kh = keccak256(h.encode()).hex()
    return "0x" + "".join(c.upper() if int(kh[i], 16) >= 8 else c for i, c in enumerate(h))


def evm_decode(addr):
    """0x... -> 20 байт. Адрес в смешанном регистре проверяется по EIP-55."""
    if not (addr.startswith("0x") and len(addr) == 42):
        raise WalletError("неверный BSC-адрес (ожидается 0x + 40 hex)")
    try:
        a20 = bytes.fromhex(addr[2:])
    except ValueError:
        raise WalletError("неверный BSC-адрес")
    body = addr[2:]
    if body != body.lower() and body != body.upper() and evm_checksum(a20) != addr:
        raise WalletError("неверная контрольная сумма EIP-55 — опечатка в адресе?")
    return a20


# ---------------- транзакции UTXO (BTC, Pearl) ----------------

def dsha(b):
    return hashlib.sha256(hashlib.sha256(b).digest()).digest()


def vsize_estimate(n_in, out_scripts, in_vbytes):
    # P2WPKH-вход ~68 vB, P2TR-вход ~57.5 vB, выход = 8 + 1 + len(script), заголовок ~11 vB
    return math.ceil(11 + in_vbytes * n_in + sum(9 + len(s) for s in out_scripts))


def build_request(tx):
    b = struct.pack("<II", tx["version"], tx["locktime"])
    b += bytes([len(tx["ins"])])
    for i in tx["ins"]:
        b += bytes.fromhex(i["txid"])[::-1]
        b += struct.pack("<IIQBI", i["vout"], i["sequence"], i["value"], i["chain"], i["index"])
    b += bytes([len(tx["outs"])])
    for o in tx["outs"]:
        b += struct.pack("<QB", o["value"], len(o["script"])) + o["script"]
        b += struct.pack("<BI", 1 if o["change"] else 0, o.get("change_index", 0))
    return b.hex()


def serialize(tx, witnesses):
    """witnesses: по стеку элементов на вход ([sig, pub] для P2WPKH, [sig] для P2TR)."""
    def body(with_witness):
        b = struct.pack("<I", tx["version"])
        if with_witness:
            b += b"\x00\x01"
        b += bytes([len(tx["ins"])])
        for i in tx["ins"]:
            b += bytes.fromhex(i["txid"])[::-1] + struct.pack("<I", i["vout"])
            b += b"\x00" + struct.pack("<I", i["sequence"])
        b += bytes([len(tx["outs"])])
        for o in tx["outs"]:
            b += struct.pack("<QB", o["value"], len(o["script"])) + o["script"]
        if with_witness:
            for stack in witnesses:
                b += bytes([len(stack)])
                for item in stack:
                    b += bytes([len(item)]) + item
        return b + struct.pack("<I", tx["locktime"])

    txid = dsha(body(False))[::-1].hex()
    return body(True).hex(), txid


# ---------------- TRON ----------------

def tron_err(r):
    msg = r.get("message") or r.get("Error") or r
    try:
        msg = bytes.fromhex(msg).decode()  # нода часто отдаёт текст ошибки в hex
    except (TypeError, ValueError):
        pass
    return msg


def _varint(n):
    out = b""
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out += bytes([b | 0x80])
        else:
            return out + bytes([b])


# ---------------- BSC ----------------

def rlp(x):
    def length(n, off):
        if n < 56:
            return bytes([off + n])
        b = n.to_bytes((n.bit_length() + 7) // 8, "big")
        return bytes([off + 55 + len(b)]) + b

    if isinstance(x, list):
        body = b"".join(rlp(i) for i in x)
        return length(len(body), 0xC0) + body
    if isinstance(x, int):
        x = x.to_bytes((x.bit_length() + 7) // 8, "big")
    if len(x) == 1 and x[0] < 0x80:
        return x
    return length(len(x), 0x80) + x


# ================= кошелёк =================

@dataclass
class Draft:
    """Собранная, но ещё не подписанная транзакция: что показать пользователю."""
    coin: str
    asset: str
    symbol: str
    from_addr: str
    to: str
    amount: str              # "12.5"
    fee: str                 # "0.00000424"
    fee_symbol: str
    fee_label: str = "Комиссия"
    notes: list = field(default_factory=list)
    sign: Callable = None    # () -> (raw_hex, txid); спрашивает устройство
    broadcast: Callable = None  # (raw_hex) -> None
    tx_url: str = ""


class Wallet:
    def __init__(self, dev):
        self.dev = dev
        self.info = dev.info()
        self.net = self.info.get("net", "testnet")
        self.cfg = NETS[self.net]
        # С v0.6 монеты — приложения на устройстве (coins= — установленные), команды "app <id> ..."
        self.legacy = tuple(int(x) for x in self.info.get("fw", "0.6").split(".")[:2]) < (0, 6)
        self.coins = [c for c in self.info.get("coins", "").split(",") if c in COINS]
        self.chain_id = int(self.info.get("bsc_chain", 56 if self.net == "mainnet" else 97))
        self.ready = self.info.get("state") == "ready"

    def cmd(self, coin, rest):
        """Команда монеты: "app btc addr 0 1"; у прошивок до v0.6 — встроенные "addr 0 1", "prl …"."""
        if self.legacy:
            return rest if coin == "btc" else f"{coin} {rest}"
        return f"app {coin} {rest}"

    # ---- общие запросы ----

    def need(self, coin):
        if coin not in self.coins:
            raise WalletError(f"На устройстве нет приложения {COINS[coin]['name']} — установи его")
        if not self.ready:
            raise WalletError("На устройстве нет кошелька — создай его на самом устройстве")

    def address(self, coin, index=0, chain=0):
        self.need(coin)
        return self.dev.call(self.cmd(coin, f"addr {chain} {index}" if coin in ("btc", "prl") else f"addr {index}"))

    def show_address(self, coin, index=0):
        """Адрес, подтверждённый на устройстве. Отказ кнопкой — DeviceError("rejected")."""
        self.need(coin)
        return self.dev.call(self.cmd(coin, f"show 0 {index}" if coin in ("btc", "prl") else f"show {index}"))

    def symbol(self, asset):
        return asset_symbol(self.cfg, asset)

    def update_firmware(self, image, progress=None):
        """Обновить прошивку по USB или Bluetooth. Устройство попросит подтверждение."""
        hw = self.info.get("hw", "?")
        if not hw.startswith("esp32"):
            raise WalletError(f"Обновление прошивки не поддерживается для {hw}")
        if len(image) < 24 or image[:1] != b"\xe9":
            raise WalletError("Это не образ приложения ESP32 (ожидается build/microwallet.bin)")
        # В заголовке образа — для какого чипа он собран (ESP-IDF: esp_chip_id_t)
        chip = {0x0005: "esp32c3", 0x0009: "esp32s3"}.get(int.from_bytes(image[12:14], "little"), "?")
        if chip != hw:
            raise WalletError(f"Образ собран для {chip}, а устройство — {hw}")
        digest = hashlib.sha256(image).hexdigest()
        self.dev.call(f"ota begin {len(image)} {digest}")  # ждёт кнопку
        try:
            # base64 по 2 КБ (прошивки с v0.5); старые понимают только hex по 1 КБ
            b64, off = True, 0
            while off < len(image):
                part = image[off:off + (2048 if b64 else 1024)]
                try:
                    self.dev.call("ota b64 " + base64.b64encode(part).decode() if b64 else "ota data " + part.hex())
                except DeviceError as e:
                    if not b64 or off or str(e) != "unknown ota command":
                        raise
                    b64 = False
                    continue
                off += len(part)
                if progress:
                    progress(off, len(image))
            return self.dev.call("ota end")
        except DeviceError:
            try:
                self.dev.call("ota abort")
            except WalletError:
                pass
            raise

    # ---- приложения WebAssembly ----

    def apps(self):
        """[(id, версия, имя)] установленных приложений."""
        r = self.dev.call("apps")
        return [tuple(e.split("|", 2)) for e in r.split(";") if e]

    def install_app(self, wasm, progress=None):
        """Передать модуль; устройство покажет имя, версию, пути и хеш и спросит кнопку."""
        if wasm[:8] != b"\0asm\1\0\0\0":
            raise WalletError("Это не модуль WebAssembly")
        self.dev.call(f"app add {len(wasm)} {hashlib.sha256(wasm).hexdigest()}")
        for off in range(0, len(wasm), 2048):
            self.dev.call("app add b64 " + base64.b64encode(wasm[off:off + 2048]).decode())
            if progress:
                progress(min(off + 2048, len(wasm)), len(wasm))
        return self.dev.call("app add end")

    def remove_app(self, app_id):
        return self.dev.call(f"app rm {app_id}")

    def restore(self, phrase):
        """Записать seed-фразу в ПУСТОЕ устройство (надо подтвердить на устройстве)."""
        if self.ready:
            raise WalletError("На устройстве уже есть кошелёк")
        ent = mnemonic_to_entropy(phrase)
        try:
            self.dev.call(f"restore {ent.hex()}")
        finally:
            ent = None
        self.info = self.dev.info()
        self.ready = self.info.get("state") == "ready"
        return self.ready

    # ---- баланс ----

    def balance(self, coin, index=0):
        """{"address", "assets": [{"id","symbol","amount","decimals"}], "pending", "utxos", "notes"}"""
        self.need(coin)
        if coin in ("btc", "prl"):
            utxos, immature, _ = self._scan(coin)
            sym = self.symbol(coin)
            return {"address": self.address(coin, 0), "notes": [],
                    "assets": [{"id": coin, "symbol": sym, "decimals": 8,
                                "amount": sum(u["value"] for u in utxos)}],
                    "pending": sum(u["value"] for u in immature),
                    "utxos": utxos, "immature": immature}
        addr = self.address(coin, index)
        if coin == "tron":
            acc = self._tron_post("/wallet/getaccount", {"address": addr, "visible": True})
            notes = [] if acc else ["Аккаунт не активирован: нужно получить хотя бы 1 TRX"]
            return {"address": addr, "notes": notes, "pending": 0, "assets": [
                {"id": "usdt", "symbol": "USDT", "decimals": 6, "amount": self._tron_usdt(addr)},
                {"id": "trx", "symbol": "TRX", "decimals": 6, "amount": acc.get("balance", 0)}]}
        bal = int(self._bsc_rpc("eth_getBalance", addr, "latest"), 16)
        return {"address": addr, "notes": [], "pending": 0, "assets": [
            {"id": "usdt", "symbol": "USDT", "decimals": 18, "amount": self._bsc_usdt(addr)},
            {"id": "bnb", "symbol": self.cfg["bnb"], "decimals": 18, "amount": bal}]}

    # ---- отправка ----

    def prepare_send(self, coin, dest, amount, asset=None, index=0,
                     feerate=None, fee_limit=None, gas_price=None):
        """Собирает транзакцию (без подписи). amount — строка в целых монетах."""
        self.need(coin)
        asset = asset or COINS[coin]["assets"][0][0]
        if coin in ("btc", "prl"):
            return self._prepare_utxo(coin, dest, amount, feerate)
        if coin == "tron":
            return self._prepare_tron(index, dest, amount, asset, fee_limit)
        return self._prepare_bsc(index, dest, amount, asset, gas_price)

    def sign_and_send(self, draft, broadcast=True):
        """Подписывает на устройстве (ждёт кнопку) и отправляет в сеть."""
        raw, txid = draft.sign()
        if broadcast:
            draft.broadcast(raw)
        return {"raw": raw, "txid": txid, "url": draft.tx_url + txid if broadcast else "",
                "broadcast": broadcast}

    # ================= BTC и Pearl =================

    def _btc_get(self, path):
        return http_json(self.cfg["btc_api"] + path)

    def _prl_get(self, path):
        return http_json(self.cfg["prl_api"] + "/api/v2" + path)

    def addr_count(self):
        """Сколько адресов для получения (настройка устройства «Addresses»)."""
        try:
            return max(1, int(self.info.get("addrs", ADDRS_DEFAULT)))
        except ValueError:
            return ADDRS_DEFAULT

    def _scan(self, coin):
        """UTXO на адресах получения 0..addrs-1 и на адресах сдачи (подряд до первого
        пустого), незрелые награды, свободный адрес сдачи."""
        prl = coin == "prl"
        utxos, immature, first_unused_change = [], [], None
        for chain in (0, 1):
            for i in range(self.addr_count() if chain == 0 else CHANGE_MAX):
                if chain == 1 and first_unused_change is not None:
                    break  # сдача кладётся на первый свободный адрес — дальше пусто
                addr = self.address(coin, i, chain)
                if prl:
                    info = self._prl_get(f"/address/{addr}?details=basic")
                    used = info.get("txs", 0) + info.get("unconfirmedTxs", 0) > 0
                else:
                    info = self._btc_get(f"/address/{addr}")
                    used = info["chain_stats"]["tx_count"] + info["mempool_stats"]["tx_count"] > 0
                if chain == 1 and not used and first_unused_change is None:
                    first_unused_change = i
                if not used:
                    continue
                for u in (self._prl_get(f"/utxo/{addr}") if prl else self._btc_get(f"/address/{addr}/utxo")):
                    if prl:
                        conf = u.get("confirmations", 0)
                        u.update(value=int(u["value"]), confirmed=conf > 0)
                    else:
                        u["confirmed"] = u["status"]["confirmed"]
                    u.update(chain=chain, index=i, address=addr)
                    # Blockbook не помечает coinbase в /utxo — смотрим саму транзакцию
                    if prl and conf < PRL_COINBASE_MATURITY and \
                            "coinbase" in self._prl_get(f"/tx/{u['txid']}")["vin"][0]:
                        immature.append(u)
                    else:
                        utxos.append(u)
        return utxos, immature, first_unused_change if first_unused_change is not None else 0

    def fee_levels(self, coin):
        """Ставки sat/vB (grain/vB): {"fast", "normal", "slow"}."""
        if coin == "prl":  # Blockbook отдаёт PRL за kB на подтверждение за N блоков
            def est(blocks):
                v = Decimal(self._prl_get(f"/estimatefee/{blocks}").get("result", "0"))
                return max(Decimal(1), (v * 10**8 / 1000).quantize(Decimal("0.1")))
            return {"fast": est(2), "normal": est(6), "slow": est(24)}
        try:
            r = self._btc_get("/v1/fees/recommended")
            return {k: Decimal(max(1, r.get(f, 2))) for k, f in
                    (("fast", "fastestFee"), ("normal", "halfHourFee"), ("slow", "hourFee"))}
        except WalletError:
            return {"fast": Decimal(5), "normal": Decimal(2), "slow": Decimal(1)}

    def default_feerate(self, coin):
        return self.fee_levels(coin)["fast" if coin == "prl" else "normal"]

    def _prepare_utxo(self, coin, dest, amount, feerate):
        prl = coin == "prl"
        cfg, hrp = self.cfg, self.cfg["prl_hrp"] if prl else self.cfg["hrp"]
        units = to_units(amount, 8)
        feerate = Decimal(str(feerate)) if feerate is not None else self.default_feerate(coin)
        dest_script = address_to_script(dest.strip(), hrp, prl)
        utxos, _, change_idx = self._scan(coin)
        utxos.sort(key=lambda u: -u["value"])
        change_script = address_to_script(self.address(coin, change_idx, 1), hrp, prl)

        chosen, total = [], 0
        for u in utxos:
            chosen.append(u)
            total += u["value"]
            vsize = vsize_estimate(len(chosen), [dest_script, change_script], 57.5 if prl else 68)
            fee = math.ceil(vsize * feerate)
            if total >= units + fee:
                break
        else:
            raise WalletError(f"Недостаточно средств: доступно {fmt_units(total, 8)} {self.symbol(coin)}")
        if len(chosen) > TX_MAX_IN:
            raise WalletError(f"Нужно {len(chosen)} входов, а устройство подписывает не больше "
                              f"{TX_MAX_IN}. Отправь сумму поменьше или сначала объедини мелкие "
                              "выходы, переведя их себе на адрес частями.")

        outs = [{"value": units, "script": dest_script, "change": False}]
        change = total - units - fee
        if change >= 546:  # иначе это "пыль", пусть уйдёт в комиссию
            outs.append({"value": change, "script": change_script,
                         "change": True, "change_index": change_idx})
        else:
            fee = total - units
        tx = {
            "version": 2, "locktime": 0,
            "ins": [{"txid": u["txid"], "vout": u["vout"], "value": u["value"],
                     "chain": u["chain"], "index": u["index"],
                     "sequence": 0xFFFFFFFD} for u in chosen],  # RBF включён
            "outs": outs,
        }

        def sign():
            resp = self.dev.call(self.cmd(coin, "sign " + build_request(tx)))
            if prl:
                wit = [[bytes.fromhex(sig)] for sig in resp.split()]
            else:
                wit = [[bytes.fromhex(x) for x in part.split(":")] for part in resp.split()]
            if len(wit) != len(chosen):
                raise WalletError("Устройство вернуло не столько подписей, сколько входов")
            return serialize(tx, wit)

        def broadcast(raw):
            if prl:
                r = json.loads(post_raw(cfg["prl_api"] + "/api/v2/sendtx/", raw))
                if "error" in r:
                    raise WalletError(f"Сеть отклонила транзакцию: {r['error']}")
            else:
                post_raw(cfg["btc_api"] + "/tx", raw)

        unit = "grain/vB" if prl else "sat/vB"
        return Draft(coin, coin, self.symbol(coin), self.address(coin, 0), dest.strip(),
                     fmt_units(units, 8), fmt_units(fee, 8), self.symbol(coin),
                     notes=[f"Входов: {len(chosen)}, ставка {feerate} {unit}"],
                     sign=sign, broadcast=broadcast,
                     tx_url=f"{cfg['prl_api']}/tx/" if prl else cfg["btc_tx_url"])

    # ================= TRON =================

    def _tron_post(self, path, body):
        key = os.environ.get("TRONGRID_API_KEY")
        return http_json(self.cfg["tron_api"] + path, body, {"TRON-PRO-API-KEY": key} if key else None)

    def _tron_usdt(self, owner):
        param = (b"\0" * 12 + tron_decode(owner)[1:]).hex()
        r = self._tron_post("/wallet/triggerconstantcontract", {
            "owner_address": owner, "contract_address": self.cfg["tron_usdt"],
            "function_selector": "balanceOf(address)", "parameter": param, "visible": True})
        res = r.get("constant_result")
        return int(res[0], 16) if res and res[0] else 0

    def _prepare_tron(self, index, dest, amount, asset, fee_limit):
        dest = dest.strip()
        owner = self.address("tron", index)
        to21 = tron_decode(dest)
        fee_limit = Decimal(str(fee_limit if fee_limit is not None else 40))
        if asset == "usdt":
            units = to_units(amount, 6)
            param = (b"\0" * 12 + to21[1:]).hex() + units.to_bytes(32, "big").hex()
            r = self._tron_post("/wallet/triggersmartcontract", {
                "owner_address": owner, "contract_address": self.cfg["tron_usdt"],
                "function_selector": "transfer(address,uint256)", "parameter": param,
                "fee_limit": to_units(fee_limit, 6), "call_value": 0, "visible": True})
            if not r.get("result", {}).get("result"):
                raise WalletError(f"Нода отказалась собрать транзакцию: {tron_err(r.get('result', r))}")
            tx = r["transaction"]
        else:
            units = to_units(amount, 6)
            tx = self._tron_post("/wallet/createtransaction", {
                "owner_address": owner, "to_address": dest, "amount": units, "visible": True})
            if "raw_data_hex" not in tx:
                raise WalletError(f"Нода отказалась собрать транзакцию: {tron_err(tx)}")

        raw = bytes.fromhex(tx["raw_data_hex"])
        if hashlib.sha256(raw).hexdigest() != tx["txID"]:
            raise WalletError("Нода вернула несогласованную транзакцию (txID != sha256(raw_data))")

        def sign():
            sig = bytes.fromhex(self.dev.call(self.cmd("tron", f"sign {index} {raw.hex()}")))
            sig = sig[:64] + bytes([sig[64] + 27])
            signed = b"\x0a" + _varint(len(raw)) + raw + b"\x12" + _varint(len(sig)) + sig
            return signed.hex(), tx["txID"]

        def broadcast(raw_hex):
            r = self._tron_post("/wallet/broadcasthex", {"transaction": raw_hex})
            if not r.get("result"):
                raise WalletError(f"Сеть отклонила транзакцию: {r.get('code')} {tron_err(r)}")

        usdt = asset == "usdt"
        return Draft("tron", asset, "USDT" if usdt else "TRX", owner, dest, fmt_units(units, 6),
                     f"{fee_limit}" if usdt else "~0.3", "TRX",
                     fee_label="Макс. комиссия" if usdt else "Комиссия",
                     notes=["Реально спишется меньше — по расходу энергии"] if usdt else
                     ["Платится сетью за bandwidth; новому адресу +1 TRX за активацию"],
                     sign=sign, broadcast=broadcast, tx_url=self.cfg["tron_tx_url"])

    # ================= BSC =================

    def _bsc_rpc(self, method, *params):
        r = http_json(self.cfg["bsc_rpc"], {"jsonrpc": "2.0", "id": 1, "method": method,
                                            "params": list(params)})
        if "error" in r:
            raise WalletError(f"BSC RPC {method}: {r['error'].get('message', r['error'])}")
        return r["result"]

    def _bsc_usdt(self, addr):
        data = "0x70a08231" + "00" * 12 + addr[2:].lower()
        r = self._bsc_rpc("eth_call", {"to": self.cfg["bsc_usdt"], "data": data}, "latest")
        return int(r, 16) if r not in ("0x", None) else 0

    def _prepare_bsc(self, index, dest, amount, asset, gas_price):
        cfg, chain_id = self.cfg, self.chain_id
        frm = self.address("bsc", index)
        dest20 = evm_decode(dest.strip())
        nonce = int(self._bsc_rpc("eth_getTransactionCount", frm, "pending"), 16)
        if gas_price is not None:
            gas_price = to_units(gas_price, 9)
        else:
            gas_price = int(self._bsc_rpc("eth_gasPrice"), 16)

        if asset == "usdt":
            units = to_units(amount, 18)
            if self._bsc_usdt(frm) < units:
                raise WalletError("Недостаточно USDT")
            to, value = evm_decode(cfg["bsc_usdt"]), 0
            data = bytes.fromhex("a9059cbb") + b"\0" * 12 + dest20 + units.to_bytes(32, "big")
            est = self._bsc_rpc("eth_estimateGas",
                                {"from": frm, "to": cfg["bsc_usdt"], "data": "0x" + data.hex()})
            gas = int(est, 16) * 12 // 10  # запас 20%
            sym = "USDT"
        else:
            units = to_units(amount, 18)
            to, value, data, gas, sym = dest20, units, b"", 21000, cfg["bnb"]

        fee = gas * gas_price
        bal = int(self._bsc_rpc("eth_getBalance", frm, "latest"), 16)
        if bal < fee + value:
            raise WalletError(f"Недостаточно {cfg['bnb']}: есть {fmt_units(bal, 18)}, "
                              f"нужно {fmt_units(fee + value, 18)} (с комиссией)")

        fields = [nonce, gas_price, gas, to, value, data]
        unsigned = rlp(fields + [chain_id, 0, 0])

        def sign():
            sig = bytes.fromhex(self.dev.call(self.cmd("bsc", f"sign {index} {unsigned.hex()}")))
            r, s, v = int.from_bytes(sig[:32], "big"), int.from_bytes(sig[32:64], "big"), sig[64]
            signed = rlp(fields + [chain_id * 2 + 35 + v, r, s])
            return "0x" + signed.hex(), "0x" + keccak256(signed).hex()

        def broadcast(raw_hex):
            self._bsc_rpc("eth_sendRawTransaction", raw_hex)

        return Draft("bsc", asset, sym, frm, evm_checksum(dest20), fmt_units(units, 18),
                     fmt_units(fee, 18), cfg["bnb"], fee_label="Макс. комиссия",
                     notes=[f"nonce {nonce}, gas {gas} × {fmt_units(gas_price, 9)} gwei"],
                     sign=sign, broadcast=broadcast, tx_url=cfg["bsc_tx_url"])


# ================= CLI =================

def print_balance(w, coin, b):
    print(b["address"])
    for note in b["notes"]:
        print("  " + note)
    if coin in ("btc", "prl"):
        show = lambda v: f"{fmt_units(v, 8)} {w.symbol(coin)}"
        for u in b["utxos"]:
            print(f'{show(u["value"]):>22}  {u["address"]}  [{"ok" if u["confirmed"] else "mempool"}]')
        for u in b["immature"]:
            print(f'{show(u["value"]):>22}  {u["address"]}  [награда, '
                  f'{u.get("confirmations", 0)}/{PRL_COINBASE_MATURITY} подтв.]')
        print(f'Доступно: {show(b["assets"][0]["amount"])}')
        if b["pending"]:
            print(f'Ещё созревает: {show(b["pending"])}')
        return
    for a in b["assets"]:
        print(f'  {a["symbol"]}: {fmt_units(a["amount"], a["decimals"])}')


def cli_send(w, coin, a):
    amount = a.amount
    if coin == "btc":  # исторически сумма для BTC — в сатоши
        amount = fmt_units(to_units(Decimal(a.amount).scaleb(-8), 8), 8)
    d = w.prepare_send(coin, a.address, amount, asset=getattr(a, "token", None), index=a.index,
                       feerate=getattr(a, "feerate", None), fee_limit=getattr(a, "fee_limit", None),
                       gas_price=getattr(a, "gas_price", None))
    print(f"{d.from_addr} -> {d.to}: {d.amount} {d.symbol}")
    print(f"{d.fee_label}: {d.fee} {d.fee_symbol}")
    for note in d.notes:
        print("  " + note)
    print("Проверь получателя и сумму на экране устройства и подтверди удержанием кнопки...")
    r = w.sign_and_send(d, broadcast=not a.dry_run)
    print("\nRAW:", r["raw"], "\nTXID:", r["txid"])
    print(r["url"] if r["broadcast"] else "--dry-run: не отправляю")


def main():
    p = argparse.ArgumentParser(description="MicroWallet host: BTC, TRON, BSC, Pearl")
    p.add_argument("--port")
    p.add_argument("--ble", metavar="ADDR", help="подключиться по Bluetooth (адрес или auto)")
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("info")
    sub.add_parser("wipe")
    sub.add_parser("boot")
    sub.add_parser("restore")
    up = sub.add_parser("update")
    up.add_argument("image", help="build/microwallet.bin")
    sub.add_parser("ble-scan")
    sub.add_parser("apps")
    ad = sub.add_parser("addrs", help="сколько адресов для получения: 1, 5, 10 или 20")
    ad.add_argument("n", nargs="?", type=int)
    ai = sub.add_parser("app-install")
    ai.add_argument("wasm")
    ar = sub.add_parser("app-rm")
    ar.add_argument("id")
    ac = sub.add_parser("app")
    ac.add_argument("id")
    ac.add_argument("args", nargs=argparse.REMAINDER)

    def coin(name, actions, amount_help, extra):
        cp = sub.add_parser(name)
        cs = cp.add_subparsers(dest="action", required=True)
        for act in actions:
            ap = cs.add_parser(act)
            if act != "xpub":
                ap.add_argument("-i", "--index", type=int, default=0, help="номер адреса")
            if act == "send":
                ap.add_argument("address")
                ap.add_argument("amount", help=amount_help)
                ap.add_argument("--dry-run", action="store_true")
                extra(ap)

    coin("btc", ["receive", "balance", "xpub", "send"], "сумма в сатоши",
         lambda ap: ap.add_argument("--feerate", type=Decimal, help="sat/vB (по умолчанию оценка сети)"))

    def tron_opts(ap):
        ap.add_argument("--token", choices=["usdt", "trx"], default="usdt")
        ap.add_argument("--fee-limit", type=Decimal, default=Decimal(40),
                        help="макс. TRX, которые можно сжечь на энергию (TRC20)")
    coin("tron", ["receive", "balance", "send"], "сумма, например 12.5", tron_opts)

    def bsc_opts(ap):
        ap.add_argument("--token", choices=["usdt", "bnb"], default="usdt")
        ap.add_argument("--gas-price", type=Decimal, help="gwei (по умолчанию eth_gasPrice)")
    coin("bsc", ["receive", "balance", "send"], "сумма, например 12.5", bsc_opts)

    coin("prl", ["receive", "balance", "send"], "сумма в PRL, например 12.5",
         lambda ap: ap.add_argument("--feerate", type=Decimal,
                                    help="grain/vB (по умолчанию оценка сети, минимум 1)"))

    # совместимость с v0.1: "wallet.py send ..." == "wallet.py btc send ..."
    argv = sys.argv[1:]
    j = 2 if argv[:1] in (["--port"], ["--ble"]) else 0
    if len(argv) > j and argv[j] in ("receive", "balance", "xpub", "send"):
        argv.insert(j, "btc")
    a = p.parse_args(argv)

    def show_code(code):
        print(f"\nКод сопряжения: {code}\nСверь его с экраном устройства и удерживай кнопку на «Pair».")

    try:
        if a.cmd == "ble-scan":
            import ble as B
            for addr, name, rssi in B.scan():
                print(f"{addr}  {name}  ({rssi} dBm)")
            return
        dev = open_device(a.port, a.ble, show_code)
        if a.cmd == "restore":
            import getpass
            w = Wallet(dev)
            print("Вводить seed на компьютере небезопасно: так делают только на доверенном ПК.")
            phrase = getpass.getpass("Seed-фраза (ввод скрыт): ")
            W_ent = mnemonic_to_entropy(phrase)  # проверить слова до похода на устройство
            del W_ent
            print("Сверь первое и последнее слово на экране и удерживай кнопку на «Restore»…")
            w.restore(phrase)
            print("Готово: кошелёк восстановлен.")
            return
        if a.cmd == "update":
            image = Path(a.image).read_bytes()
            w = Wallet(dev)
            print(f"{len(image)} байт, SHA-256 {hashlib.sha256(image).hexdigest()}")
            print("Сверь размер и хеш на экране и удерживай кнопку на «Install»…")
            w.update_firmware(image, lambda d, t: print(f"\r{d * 100 // t}%", end="", flush=True))
            print("\nГотово, устройство перезагружается.")
            return
        if a.cmd == "apps":
            for app_id, ver, name in Wallet(dev).apps():
                print(f"{app_id:8} {ver:8} {name}")
            return
        if a.cmd == "app-install":
            wasm = Path(a.wasm).read_bytes()
            print(f"{len(wasm)} байт, SHA-256 {hashlib.sha256(wasm).hexdigest()}")
            print("Сверь имя, пути ключей и хеш на экране и удерживай кнопку на «Install»…")
            Wallet(dev).install_app(wasm)
            print("Установлено.")
            return
        if a.cmd == "app-rm":
            print("Подтверди удаление на устройстве…")
            Wallet(dev).remove_app(a.id)
            print("Удалено.")
            return
        if a.cmd == "app":
            print(dev.call(" ".join(["app", a.id] + a.args)))
            return
        if a.cmd == "addrs":
            print(dev.call(f"addrs {a.n}") if a.n else Wallet(dev).addr_count())
            return
        if a.cmd == "info":
            print(dev.call("info"))
        elif a.cmd == "wipe":
            print("Подтверди удержанием кнопки на устройстве...")
            print(dev.call("wipe"))
        elif a.cmd == "boot":
            print(dev.call("boot"))
            print("Устройство в режиме загрузчика: прошивай через esptool")
        else:
            w = Wallet(dev)
            if w.net == "mainnet":
                print("!! MAINNET: настоящие деньги, seed на устройстве не зашифрован !!")
            if a.action == "xpub":
                print(dev.call(w.cmd("btc", "xpub")))
            elif a.action == "receive":
                print("Сверь адрес с экраном устройства и подтверди удержанием на «Approve».")
                print(w.show_address(a.cmd, a.index))
            elif a.action == "balance":
                print_balance(w, a.cmd, w.balance(a.cmd, a.index))
            elif a.action == "send":
                cli_send(w, a.cmd, a)
    except DeviceError as e:
        sys.exit(f"Устройство ответило: {e}")
    except WalletError as e:
        sys.exit(f"Ошибка: {e}")


if __name__ == "__main__":
    main()
