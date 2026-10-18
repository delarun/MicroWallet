"""
PSBT (BIP174, версия 0) для Bitcoin: разбор, сборка, финализация — без сторонних библиотек.

Устройство PSBT не разбирает: хост переводит его в обычный запрос на подпись (входы с путями,
выходы, предыдущие транзакции), а подписи вставляет обратно. Для денег это не опаснее разбора
на устройстве: устройство само показывает получателей, само выводит адрес сдачи, само проверяет
суммы входов по предыдущим транзакциям и подписывает только SIGHASH_ALL.

Поля, которые нам не нужны, сохраняются как есть и в том же порядке — PSBT уходит обратно в
Sparrow/Electrum без потерь.
"""
import base64
import hashlib
import struct

MAGIC = b"psbt\xff"

# ключи (первый байт ключа) — BIP174
G_UNSIGNED_TX = 0x00
I_NON_WITNESS_UTXO = 0x00
I_WITNESS_UTXO = 0x01
I_PARTIAL_SIG = 0x02
I_SIGHASH_TYPE = 0x03
I_BIP32_DERIVATION = 0x06
I_FINAL_SCRIPTSIG = 0x07
I_FINAL_SCRIPTWITNESS = 0x08
O_BIP32_DERIVATION = 0x02

HARD = 0x80000000


class PsbtError(Exception):
    pass


# ---------------- примитивы ----------------

def dsha(b):
    return hashlib.sha256(hashlib.sha256(b).digest()).digest()


def ser_compact(n):
    if n < 0xfd:
        return bytes([n])
    if n <= 0xffff:
        return b"\xfd" + struct.pack("<H", n)
    if n <= 0xffffffff:
        return b"\xfe" + struct.pack("<I", n)
    return b"\xff" + struct.pack("<Q", n)


class Reader:
    def __init__(self, b):
        self.b, self.i = b, 0

    def take(self, n):
        if self.i + n > len(self.b):
            raise PsbtError("PSBT обрывается")
        v = self.b[self.i:self.i + n]
        self.i += n
        return v

    def compact(self):
        c = self.take(1)[0]
        if c < 0xfd:
            return c
        return int.from_bytes(self.take({0xfd: 2, 0xfe: 4, 0xff: 8}[c]), "little")

    def done(self):
        return self.i == len(self.b)


# ---------------- транзакция ----------------

def parse_tx(raw):
    """Сырая транзакция (с witness или без) -> словарь; txid — по сериализации без witness."""
    r = Reader(raw)
    version = struct.unpack("<i", r.take(4))[0]
    segwit = raw[4:6] == b"\x00\x01"
    if segwit:
        r.take(2)
    ins = []
    for _ in range(r.compact()):
        prev = r.take(32)
        vout = struct.unpack("<I", r.take(4))[0]
        script = r.take(r.compact())
        seq = struct.unpack("<I", r.take(4))[0]
        ins.append({"txid": prev[::-1].hex(), "vout": vout, "script_sig": script, "sequence": seq})
    outs = []
    for _ in range(r.compact()):
        value = struct.unpack("<Q", r.take(8))[0]
        outs.append({"value": value, "script": r.take(r.compact())})
    wit = []
    if segwit:
        for _ in ins:
            wit.append([r.take(r.compact()) for _ in range(r.compact())])
    locktime = struct.unpack("<I", r.take(4))[0]
    if not r.done():
        raise PsbtError("лишние байты в транзакции")
    tx = {"version": version, "ins": ins, "outs": outs, "locktime": locktime, "witness": wit}
    tx["txid"] = dsha(ser_tx(tx, False))[::-1].hex()
    return tx


def ser_tx(tx, with_witness=True):
    wit = tx.get("witness") or []
    use_wit = with_witness and any(w for w in wit)
    b = struct.pack("<i", tx["version"]) + (b"\x00\x01" if use_wit else b"")
    b += ser_compact(len(tx["ins"]))
    for i in tx["ins"]:
        b += bytes.fromhex(i["txid"])[::-1] + struct.pack("<I", i["vout"])
        b += ser_compact(len(i["script_sig"])) + i["script_sig"] + struct.pack("<I", i["sequence"])
    b += ser_compact(len(tx["outs"]))
    for o in tx["outs"]:
        b += struct.pack("<Q", o["value"]) + ser_compact(len(o["script"])) + o["script"]
    if use_wit:
        for stack in wit:
            b += ser_compact(len(stack)) + b"".join(ser_compact(len(x)) + x for x in stack)
    return b + struct.pack("<I", tx["locktime"])


# ---------------- PSBT ----------------

class Psbt:
    """Карты — списки пар (ключ, значение) в исходном порядке."""

    def __init__(self, glob, inputs, outputs):
        self.glob, self.inputs, self.outputs = glob, inputs, outputs
        raw = get(glob, G_UNSIGNED_TX)
        if raw is None:
            raise PsbtError("в PSBT нет неподписанной транзакции (нужна версия 0)")
        self.tx = parse_tx(raw)
        if any(i["script_sig"] for i in self.tx["ins"]) or any(self.tx["witness"]):
            raise PsbtError("неподписанная транзакция PSBT содержит подписи")
        if len(inputs) != len(self.tx["ins"]) or len(outputs) != len(self.tx["outs"]):
            raise PsbtError("число карт PSBT не совпадает с транзакцией")

    @classmethod
    def parse(cls, data):
        if isinstance(data, str):
            data = data.strip().encode()
        if not data.startswith(MAGIC):
            try:
                data = base64.b64decode(data, validate=True)
            except ValueError:
                raise PsbtError("это не PSBT (ни двоичный, ни base64)")
        if not data.startswith(MAGIC):
            raise PsbtError("это не PSBT")
        r = Reader(data)
        r.take(len(MAGIC))

        def read_map():
            m = []
            while True:
                kl = r.compact()
                if kl == 0:
                    return m
                k = r.take(kl)
                m.append((k, r.take(r.compact())))

        glob = read_map()
        n_in = n_out = 0
        raw = get(glob, G_UNSIGNED_TX)
        if raw is not None:
            t = parse_tx(raw)
            n_in, n_out = len(t["ins"]), len(t["outs"])
        inputs = [read_map() for _ in range(n_in)]
        outputs = [read_map() for _ in range(n_out)]
        if not r.done():
            raise PsbtError("лишние байты в PSBT")
        return cls(glob, inputs, outputs)

    def serialize(self):
        def ser_map(m):
            return b"".join(ser_compact(len(k)) + k + ser_compact(len(v)) + v for k, v in m) + b"\x00"
        return MAGIC + ser_map(self.glob) + b"".join(ser_map(m) for m in self.inputs + self.outputs)

    def to_base64(self):
        return base64.b64encode(self.serialize()).decode()

    def finalize_p2wpkh(self):
        """Свидетель [подпись, ключ] из partial_sig на каждый вход; лишние поля входа убираются
        (как у финализатора BIP174). -> сырая подписанная транзакция."""
        wit = []
        for k, m in enumerate(self.inputs):
            sigs = [(key[1:], v) for key, v in m if key[0] == I_PARTIAL_SIG]
            if len(sigs) != 1:
                raise PsbtError(f"вход #{k}: нужна ровно одна подпись")
            pub, sig = sigs[0]
            stack = [sig, pub]
            keep = [(key, v) for key, v in m if key[0] in (I_NON_WITNESS_UTXO, I_WITNESS_UTXO)]
            keep.append((bytes([I_FINAL_SCRIPTWITNESS]),
                         ser_compact(2) + b"".join(ser_compact(len(x)) + x for x in stack)))
            self.inputs[k] = keep
            wit.append(stack)
        return ser_tx({**self.tx, "witness": wit}).hex()


def get(m, key_type, key_data=b""):
    for k, v in m:
        if k[0] == key_type and k[1:] == key_data:
            return v
    return None


def derivations(m, key_type):
    """BIP32-пути карты: [(ключ33, отпечаток, [индексы])]."""
    out = []
    for k, v in m:
        if k[0] == key_type and len(k) == 34 and len(v) >= 4 and (len(v) - 4) % 4 == 0:
            out.append((k[1:], v[:4].hex(), list(struct.unpack(f"<{(len(v) - 4) // 4}I", v[4:]))))
    return out


# ---------------- xpub и дескриптор ----------------

B58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"


def b58check_decode(s):
    n = 0
    for c in s:
        n = n * 58 + B58.index(c)
    raw = n.to_bytes((n.bit_length() + 7) // 8, "big")
    raw = b"\0" * (len(s) - len(s.lstrip("1"))) + raw
    if dsha(raw[:-4])[:4] != raw[-4:]:
        raise PsbtError("плохая контрольная сумма base58")
    return raw[:-4]


def b58check_encode(raw):
    raw += dsha(raw)[:4]
    n = int.from_bytes(raw, "big")
    s = ""
    while n:
        n, r = divmod(n, 58)
        s = B58[r] + s
    return "1" * (len(raw) - len(raw.lstrip(b"\0"))) + s


SLIP132 = {"04b24746": "0488b21e", "045f1cf6": "043587cf"}  # zpub -> xpub, vpub -> tpub


def plain_xpub(x):
    """zpub/vpub (SLIP-132) -> xpub/tpub: дескрипторы задают тип скрипта сами."""
    raw = b58check_decode(x)
    v = SLIP132.get(raw[:4].hex())
    return b58check_encode(bytes.fromhex(v) + raw[4:]) if v else x


INPUT_CHARSET = "0123456789()[],'/*abcdefgh@:$%{}IJKLMNOPQRSTUVWXYZ&+-.;<=>?!^_|~ijklmnopqrstuvwxyzABCDEFGH`#\"\\ "
CHECKSUM_CHARSET = "qpzry9x8gf2tvdw0s3jn54khce6mua7l"


def descsum(s):
    """Контрольная сумма дескриптора (BIP380): s + '#' + 8 символов."""
    def polymod(c, val):
        c0 = c >> 35
        c = ((c & 0x7ffffffff) << 5) ^ val
        for i, g in enumerate((0xf5dee51989, 0xa9fdca3312, 0x1bab10e32d, 0x3706b1677a, 0x644d626ffd)):
            if c0 >> i & 1:
                c ^= g
        return c
    c, cls, clscount = 1, 0, 0
    for ch in s:
        pos = INPUT_CHARSET.find(ch)
        if pos < 0:
            raise PsbtError("недопустимый символ в дескрипторе")
        c = polymod(c, pos & 31)
        cls = cls * 3 + (pos >> 5)
        clscount += 1
        if clscount == 3:
            c = polymod(c, cls)
            cls = clscount = 0
    if clscount:
        c = polymod(c, cls)
    for _ in range(8):
        c = polymod(c, 0)
    c ^= 1
    return s + "#" + "".join(CHECKSUM_CHARSET[(c >> (5 * (7 - j))) & 31] for j in range(8))
