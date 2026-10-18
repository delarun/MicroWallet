"""
TON для хоста: ячейки, BoC, адреса и сообщения кошелька W5 (v5R1) — ровно столько, сколько
нужно для GRAM и USDT. Подписывает устройство (apps/ton): хост собирает подписываемую ячейку,
устройство её разбирает, показывает и подписывает её хеш.

Сверено с официальной библиотекой @ton/ton (tools/sim/ton_vectors.json).
"""
import base64
import hashlib

# Код W5R1 (@ton/ton WalletContractV5R1) — нужен в StateInit первой отправки
W5_CODE_BOC = bytes.fromhex(
    "b5ee9c7241021401000281000114ff00f4a413f4bcf2c80b01020120020d020148030402dcd020d749c120915b8f6320d70b1f20"
    "82106578746ebd21821073696e74bdb0925f03e082106578746eba8eb48020d72101d074d721fa4030fa44f828fa443058bd915b"
    "e0ed44d0810141d721f4058307f40e6fa1319130e18040d721707fdb3ce03120d749810280b99130e070e2100f02012005"
    "0c020120060902016e07080019adce76a2684020eb90eb85ffc00019af1df6a2684010eb90eb858fc00201480a0b0017b325"
    "fb51341c75c875c2c7e00011b262fb513435c280200019be5f0f6a2684080a0eb90fa02c0102f20e011e20d70b1f82107369"
    "676ebaf2e08a7f0f01e68ef0eda2edfb218308d722028308d723208020d721d31fd31fd31fed44d0d200d31f20d31fd3ffd7"
    "0a000af90140ccf9109a28945f0adb31e1f2c087df02b35007b0f2d0845125baf2e0855036baf2e086f823bbf2d0882292f8"
    "00de01a47fc8ca00cb1f01cf16c9ed542092f80fde70db3cd81003f6eda2edfb02f404216e926c218e4c0221d73930709421"
    "c700b38e2d01d72820761e436c20d749c008f2e09320d74ac002f2e09320d71d06c712c2005230b0f2d089d74cd7393001a4"
    "e86c128407bbf2e093d74ac000f2e093ed55e2d20001c000915be0ebd72c08142091709601d72c081c12e25210b1e30f20d7"
    "4a111213009601fa4001fa44f828fa443058baf2e091ed44d0810141d718f405049d7fc8ca0040048307f453f2e08b8e1403"
    "8307f45bf2e08c22d70a00216e01b3b0f2d090e2c85003cf1612f400c9ed54007230d72c08248e2d21f2e092d200ed44d0d2"
    "005113baf2d08f54503091319c01810140d721d70a00f2e08ee2c8ca0058cf16c9ed5493f2c08de20010935bdb31e1d74cd0"
    "b4d6c35e")
# USDT: код кошелька жетона — library-ячейка со ссылкой на этот хеш
USDT_WALLET_CODE = bytes.fromhex("8f452d7a4dfd74066b682365177259ed05734435be76b5fd4bd5d8af2b7c3d68")

OP_SIGNED_EXTERNAL = 0x7369676E
OP_SEND_MSG = 0x0EC3C86D
OP_JETTON_TRANSFER = 0x0F8A7EA5
MODE = 3  # комиссия отдельно + игнорировать ошибки — единственный режим, который подпишет устройство
COMMENT_MAX = 96  # столько устройство покажет целиком
USDT_ATTACH = 50_000_000  # 0.05 GRAM на пересылку жетона, лишнее вернётся


class TonError(ValueError):
    pass


# ---------------- ячейки ----------------

class Cell:
    def __init__(self, bits="", refs=(), exotic=False):
        if len(bits) > 1023 or len(refs) > 4:
            raise TonError("ячейка переполнена")
        self.bits, self.refs, self.exotic = bits, list(refs), exotic
        self._hash = None

    def _data(self):
        b = self.bits
        if len(b) % 8:
            b += "1" + "0" * (7 - len(b) % 8)
        return int(b, 2).to_bytes(len(b) // 8, "big") if b else b""

    def _d1d2(self):
        n = len(self.bits)
        return bytes([len(self.refs) + (8 if self.exotic else 0), n // 8 + (n + 7) // 8])

    def depth(self):
        return 1 + max(r.depth() for r in self.refs) if self.refs else 0

    def hash(self):
        if self._hash is None:
            h = hashlib.sha256(self._d1d2() + self._data())
            for r in self.refs:
                h.update(r.depth().to_bytes(2, "big"))
            for r in self.refs:
                h.update(r.hash())
            self._hash = h.digest()
        return self._hash


class Builder:
    def __init__(self):
        self.bits, self.refs = "", []

    def uint(self, v, n):
        if v < 0 or v >= 1 << n:
            raise TonError("число не влезает")
        self.bits += format(v, f"0{n}b") if n else ""
        return self

    def bytes(self, b):
        return self.uint(int.from_bytes(b, "big"), 8 * len(b)) if b else self

    def coins(self, v):
        n = (v.bit_length() + 7) // 8
        return self.uint(n, 4).uint(v, 8 * n)

    def addr(self, a):  # (workchain, hash) или None — addr_none
        if a is None:
            return self.uint(0, 2)
        return self.uint(2, 2).uint(0, 1).uint(a[0] & 0xFF, 8).bytes(a[1])

    def ref(self, c):
        self.refs.append(c)
        return self

    def maybe_ref(self, c):
        return self.uint(1, 1).ref(c) if c is not None else self.uint(0, 1)

    def end(self):
        return Cell(self.bits, self.refs)


def comment_cell(text):
    return Builder().uint(0, 32).bytes(text.encode()).end()


# ---------------- BoC ----------------

def _crc32c(data):
    c = 0xFFFFFFFF
    for x in data:
        c ^= x
        for _ in range(8):
            c = (c >> 1) ^ 0x82F63B78 if c & 1 else c >> 1
    return c ^ 0xFFFFFFFF


def to_boc(root):
    """Один корень, без индекса, с CRC32C (как @ton/core). Порядок — сверху вниз:
    у каждой ячейки ссылки только на ячейки дальше по списку."""
    order, seen = [], {}

    def visit(c):  # обратный порядок обхода: родитель раньше потомков
        h = c.hash()
        if h in seen:
            return
        seen[h] = c
        for r in c.refs:
            visit(r)
        order.append(c)
    visit(root)
    order.reverse()
    # родитель должен стоять раньше всех потомков: топологическая сортировка по глубине
    order.sort(key=lambda c: -c.depth())
    idx = {c.hash(): i for i, c in enumerate(order)}
    n = len(order)
    s = max(1, (n.bit_length() + 7) // 8)
    body = b"".join(c._d1d2() + c._data() + b"".join(idx[r.hash()].to_bytes(s, "big") for r in c.refs)
                    for c in order)
    off = max(1, (len(body).bit_length() + 7) // 8)
    head = (b"\xb5\xee\x9c\x72" + bytes([0x40 | s, off]) + n.to_bytes(s, "big") + (1).to_bytes(s, "big")
            + (0).to_bytes(s, "big") + len(body).to_bytes(off, "big") + (0).to_bytes(s, "big"))
    data = head + body
    return data + _crc32c(data).to_bytes(4, "little")


def from_boc(b):
    """BoC с одним корнем -> Cell (нужно для кода W5 в StateInit)."""
    if b[:4] != b"\xb5\xee\x9c\x72":
        raise TonError("не BoC")
    flags, off = b[4], b[5]
    has_idx, has_crc, s = flags & 0x80, flags & 0x40, flags & 7
    p = 6
    n, roots, _absent = (int.from_bytes(b[p + i * s:p + (i + 1) * s], "big") for i in range(3))
    p += 3 * s
    p += off  # tot_cells_size
    root = int.from_bytes(b[p:p + s], "big")
    p += s * roots
    if has_idx:
        p += n * off
    raw = []
    for _ in range(n):
        d1, d2 = b[p], b[p + 1]
        p += 2
        ln = (d2 + 1) // 2
        data = b[p:p + ln]
        p += ln
        refs = [int.from_bytes(b[p + i * s:p + (i + 1) * s], "big") for i in range(d1 & 7)]
        p += s * (d1 & 7)
        bits = "".join(format(x, "08b") for x in data)
        if d2 & 1:
            bits = bits.rstrip("0")[:-1]
        raw.append((bits, refs, bool(d1 & 8)))
    if has_crc and _crc32c(b[:p]) != int.from_bytes(b[p:p + 4], "little"):
        raise TonError("BoC: контрольная сумма")
    cells = [None] * n
    for i in range(n - 1, -1, -1):
        bits, refs, ex = raw[i]
        cells[i] = Cell(bits, [cells[r] for r in refs], ex)
    return cells[root]


# ---------------- адреса ----------------

def _crc16(data):
    c = 0
    for x in data:
        c ^= x << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021 if c & 0x8000 else c << 1) & 0xFFFF
    return c


def friendly(addr, bounceable, testnet):
    wc, h = addr
    b = bytes([(0x11 if bounceable else 0x51) | (0x80 if testnet else 0), wc & 0xFF]) + h
    return base64.urlsafe_b64encode(b + _crc16(b).to_bytes(2, "big")).decode()


def parse_address(s, testnet):
    """«EQ…/UQ…» (base64 или base64url) или «0:hex» -> ((workchain, hash), bounceable)."""
    s = s.strip()
    if ":" in s:
        wc, h = s.split(":", 1)
        try:
            wc, hb = int(wc), bytes.fromhex(h)
        except ValueError:
            raise TonError("неверный адрес TON") from None
        if wc != 0 or len(hb) != 32:
            raise TonError("поддерживаются только адреса basechain (0:…)")
        return (0, hb), False
    try:
        b = base64.urlsafe_b64decode(s.replace("+", "-").replace("/", "_") + "=" * (-len(s) % 4))
    except ValueError:
        raise TonError("неверный адрес TON") from None
    if len(s) != 48 or len(b) != 36 or _crc16(b[:34]) != int.from_bytes(b[34:], "big"):
        raise TonError("неверный адрес TON: контрольная сумма")
    tag, wc = b[0], b[1]
    if tag & 0x7F not in (0x11, 0x51):  # 0x40 — неотскакивающий, 0x80 — только для testnet
        raise TonError("неверный адрес TON")
    if bool(tag & 0x80) != testnet:
        raise TonError("адрес для другой сети TON (testnet/mainnet)")
    if wc != 0:
        raise TonError("поддерживаются только адреса basechain")
    return (0, b[2:34]), not (tag & 0x40)


def raw_str(addr):
    return f"{addr[0]}:{addr[1].hex()}"


# ---------------- кошелёк W5 ----------------

def wallet_id(testnet):
    return ((-3 if testnet else -239) ^ -0x80000000) & 0xFFFFFFFF


def state_init(code, data):
    return Builder().uint(0, 2).maybe_ref(code).maybe_ref(data).uint(0, 1).end()


def w5_init(pubkey, testnet):
    data = Builder().uint(1, 1).uint(0, 32).uint(wallet_id(testnet), 32).bytes(pubkey).uint(0, 1).end()
    return state_init(from_boc(W5_CODE_BOC), data)


def w5_address(pubkey, testnet):
    return (0, w5_init(pubkey, testnet).hash())


def usdt_wallet(owner, master):
    lib = Cell("00000010" + "".join(format(x, "08b") for x in USDT_WALLET_CODE), exotic=True)
    data = Builder().uint(0, 4).coins(0).addr(owner).addr(master).end()
    return (0, state_init(lib, data).hash())


def internal(dest, value, bounce, body=None):
    """MessageRelaxed: служебные поля нулевые; тело — в той же ячейке, если влезает."""
    b = (Builder().uint(0, 1).uint(1, 1).uint(1 if bounce else 0, 1).uint(0, 1).addr(None).addr(dest)
         .coins(value).uint(0, 1).coins(0).coins(0).uint(0, 64).uint(0, 32).uint(0, 1))
    if body is None:
        return b.uint(0, 1).end()
    if len(b.bits) + 1 + len(body.bits) <= 1023 and len(body.refs) <= 4:
        b.uint(0, 1).bits += body.bits
        b.refs += body.refs
        return b.end()
    return b.uint(1, 1).ref(body).end()


def jetton_transfer(amount, dest, response, comment=None):
    b = (Builder().uint(OP_JETTON_TRANSFER, 32).uint(0, 64).coins(amount).addr(dest).addr(response)
         .uint(0, 1).coins(1 if comment else 0))
    return (b.uint(1, 1).ref(comment_cell(comment)) if comment else b.uint(0, 1)).end()


def signing_message(testnet, seqno, valid_until, msg):
    """То, что подписывает устройство: op «sign», wallet_id, срок, seqno, одно действие send_msg."""
    actions = Builder().ref(Cell()).uint(OP_SEND_MSG, 32).uint(MODE, 8).ref(msg).end()
    return (Builder().uint(OP_SIGNED_EXTERNAL, 32).uint(wallet_id(testnet), 32).uint(valid_until, 32)
            .uint(seqno, 32).maybe_ref(actions).uint(0, 1).end())


def signed_body(signing, signature):
    return Cell(signing.bits + "".join(format(x, "08b") for x in signature), signing.refs)


def external(dest, body, init=None):
    """Внешнее сообщение в кошелёк; init — StateInit для первой отправки (кошелёк не развёрнут)."""
    b = Builder().uint(2, 2).addr(None).addr(dest).coins(0)
    b = b.uint(1, 1).uint(1, 1).ref(init) if init is not None else b.uint(0, 1)
    return b.uint(1, 1).ref(body).end()


def normalized_hash(dest, body):
    """Нормализованный хеш внешнего сообщения (TEP-467) — по нему транзакцию находят обозреватели."""
    return external(dest, body).hash()
