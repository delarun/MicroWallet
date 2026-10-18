"""
Solana для хоста: base58, сообщение перевода SOL (legacy), транзакция. Подписывает устройство
(apps/sol): оно само разбирает сообщение и подписывает его целиком.

Перевод SOL: [подписавший-плательщик, получатель, System Program, ComputeBudget],
инструкции: SetComputeUnitLimit, SetComputeUnitPrice, transfer.
Перевод токена (USDC, USDT): TransferChecked со своего ATA на ATA получателя, при нужде перед ним
CreateIdempotent. Устройству вместе с сообщением уходят владелец и bump ATA получателя.
"""
import hashlib
import struct

B58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"
SYSTEM = bytes(32)
COMPUTE_BUDGET = bytes.fromhex("0306466fe5211732ffecadba72c39be7bc8ce5bbc5f7126b2c439b3a40000000")
CU_LIMIT = 1000  # перевод и две инструкции ComputeBudget — около 450 единиц
TOKEN = bytes.fromhex("06ddf6e1d765a193d9cbe146ceeb79ac1cb485ed5f5b37913a8cf5857eff00a9")
ATA_PROGRAM = bytes.fromhex("8c97258f4e2489f1bb3d1029148e0d830b5a1399daff1084048e7bd8dbe9f859")
TOKEN_CU = 15000         # TransferChecked — около 6 тыс. единиц
TOKEN_CREATE_CU = 50000  # + создание счёта получателя — около 25 тыс.
TOKEN_ACCOUNT_RENT = 2039280  # lamports: рента токен-счёта (165 байт)

_P = 2**255 - 19
_D = -121665 * pow(121666, -1, _P) % _P


def on_curve(b):
    """Лежит ли 32 байта на кривой ed25519 (RFC 8032, распаковка точки). Адрес кошелька — да,
    PDA (в том числе токен-счёт ATA) — нет."""
    y = int.from_bytes(b, "little") & ((1 << 255) - 1)
    if y >= _P:
        return False
    u, v = (y * y - 1) % _P, (_D * y * y + 1) % _P
    x2 = u * pow(v, -1, _P) % _P
    x = pow(x2, (_P + 3) // 8, _P)
    if x * x % _P != x2:
        x = x * pow(2, (_P - 1) // 4, _P) % _P
    if x * x % _P != x2:
        return False
    return not (x == 0 and b[31] >> 7)


def find_pda(seeds, program):
    for bump in range(255, -1, -1):
        h = hashlib.sha256(b"".join(seeds) + bytes([bump]) + program + b"ProgramDerivedAddress").digest()
        if not on_curve(h):
            return h, bump
    raise SolError("нет адреса PDA")


def ata(owner, mint):
    """Счёт токена владельца (Associated Token Account) и его bump."""
    return find_pda([owner, TOKEN, mint], ATA_PROGRAM)


class SolError(ValueError):
    pass


def b58encode(b):
    n, s = int.from_bytes(b, "big"), ""
    while n:
        n, r = divmod(n, 58)
        s = B58[r] + s
    return "1" * (len(b) - len(b.lstrip(b"\0"))) + s


def b58decode(s, size=32):
    n = 0
    for c in s:
        if c not in B58:
            raise SolError("неверный адрес Solana")
        n = n * 58 + B58.index(c)
    zeros = len(s) - len(s.lstrip("1"))
    raw = b"\0" * zeros + (n.to_bytes((n.bit_length() + 7) // 8, "big") if n else b"")
    if len(raw) != size:
        raise SolError("неверный адрес Solana")
    return raw


def parse_address(s):
    s = s.strip()
    if not 32 <= len(s) <= 44:
        raise SolError("неверный адрес Solana")
    return b58decode(s)


def compact(n):
    out = b""
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out += bytes([b | 0x80])
        else:
            return out + bytes([b])


def transfer_message(payer, dest, lamports, blockhash, cu_price):
    """Сообщение (legacy) перевода lamports с payer на dest; cu_price — микро-lamports за единицу."""
    if payer == dest:
        raise SolError("перевод самому себе")
    keys = [payer, dest, SYSTEM, COMPUTE_BUDGET]
    ix = [
        (3, [], bytes([2]) + struct.pack("<I", CU_LIMIT)),
        (3, [], bytes([3]) + struct.pack("<Q", cu_price)),
        (2, [0, 1], struct.pack("<IQ", 2, lamports)),
    ]
    msg = bytes([1, 0, 2]) + compact(len(keys)) + b"".join(keys) + blockhash
    msg += compact(len(ix))
    for prog, acc, data in ix:
        msg += bytes([prog]) + compact(len(acc)) + bytes(acc) + compact(len(data)) + data
    return msg


def _message(keys, ro_unsigned, blockhash, ix):
    msg = bytes([1, 0, ro_unsigned]) + compact(len(keys)) + b"".join(keys) + blockhash + compact(len(ix))
    for prog, acc, data in ix:
        msg += bytes([prog]) + compact(len(acc)) + bytes(acc) + compact(len(data)) + data
    return msg


def token_message(payer, owner, mint, decimals, amount, blockhash, cu_price, create):
    """Перевод токена владельцу owner (кошелёк): -> (сообщение, bump ATA получателя, лимит вычислений)."""
    if payer == owner:
        raise SolError("перевод самому себе")
    src, _ = ata(payer, mint)
    dst, bump = ata(owner, mint)
    keys = [payer, dst, src] + ([owner] if create else []) + [mint] + ([SYSTEM] if create else []) + [TOKEN] \
        + ([ATA_PROGRAM] if create else []) + [COMPUTE_BUDGET]
    k = {key: i for i, key in enumerate(keys)}
    limit = TOKEN_CREATE_CU if create else TOKEN_CU
    ix = [(k[COMPUTE_BUDGET], [], bytes([2]) + struct.pack("<I", limit)),
          (k[COMPUTE_BUDGET], [], bytes([3]) + struct.pack("<Q", cu_price))]
    if create:  # CreateIdempotent: [плательщик, ATA, владелец, mint, System, Token]
        ix.append((k[ATA_PROGRAM], [0, k[dst], k[owner], k[mint], k[SYSTEM], k[TOKEN]], bytes([1])))
    ix.append((k[TOKEN], [k[src], k[mint], k[dst], 0], bytes([12]) + struct.pack("<QB", amount, decimals)))
    return _message(keys, len(keys) - 3, blockhash, ix), bump, limit


def max_fee(cu_price, limit=CU_LIMIT):
    """Потолок комиссии, как его считает устройство: 5000 + цена × лимит / 1e6 (вверх)."""
    return 5000 + -(-cu_price * limit // 1_000_000)


def transaction(msg, sig):
    return compact(1) + sig + msg
