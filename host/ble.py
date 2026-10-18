"""
BLE-транспорт MicroWallet: шифрованный канал (описание — src/channel.h) поверх GATT.

  pip install bleak cryptography

BleDevice ведёт себя как wallet.Device (call/info/close), поэтому Wallet и
приложение работают с ним так же, как с USB.
"""
import asyncio
import hashlib
import hmac
import json
import os
import queue
import struct
import subprocess
import threading
from pathlib import Path

from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

import wallet as W

SERVICE_UUID = "6e7a0001-9a3b-4c2d-8f00-6d6963726f77"
RX_UUID = "6e7a0002-9a3b-4c2d-8f00-6d6963726f77"  # ПК пишет
TX_UUID = "6e7a0003-9a3b-4c2d-8f00-6d6963726f77"  # устройство уведомляет

T_HELLO, T_HELLO_R, T_ERROR, T_DATA = 0x01, 0x02, 0x03, 0x10
BONDS_FILE = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "microwallet" / "ble.json"


class ChannelError(W.WalletError):
    pass


# ---------------- сопряжения на стороне ПК ----------------

class BondStore:
    """{ключ устройства (BLE-адрес): {"id": hex, "key": hex}} в ~/.config/microwallet/ble.json"""

    def __init__(self, path=BONDS_FILE):
        self.path = Path(path)

    def _load(self):
        try:
            return json.loads(self.path.read_text())
        except (OSError, ValueError):
            return {}

    def get(self, dev):
        b = self._load().get(dev)
        return (bytes.fromhex(b["id"]), bytes.fromhex(b["key"])) if b else None

    def put(self, dev, bond_id, key):
        d = self._load()
        d[dev] = {"id": bond_id.hex(), "key": key.hex()}
        self.path.parent.mkdir(parents=True, exist_ok=True)
        tmp = self.path.with_suffix(".tmp")
        tmp.write_text(json.dumps(d, indent=1))
        os.chmod(tmp, 0o600)
        tmp.replace(self.path)

    def drop(self, dev):
        d = self._load()
        if d.pop(dev, None) is not None:
            self.path.write_text(json.dumps(d, indent=1))


# ---------------- канал ----------------

def _expand(prk, label):
    return hmac.new(prk, label.encode() + b"\x01", hashlib.sha256).digest()


def _nonce(ctr):
    return b"\0" * 4 + struct.pack(">Q", ctr)


class Channel:
    """Рукопожатие и шифрование. transport: send(frame), recv(timeout) -> frame."""

    def __init__(self, transport, dev_key, bonds=None, on_code=None):
        self.t, self.dev_key = transport, dev_key
        self.bonds = bonds or BondStore()
        self.on_code = on_code  # (код) -> None: показать код сопряжения пользователю
        self.tx_next = self.rx_next = 0
        self.paired_now = False

    def handshake(self, pair_timeout=180):
        eph = X25519PrivateKey.generate()
        host_pub = eph.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)
        bond = self.bonds.get(self.dev_key)
        host_id = bond[0] if bond else b"\0" * 8
        self.t.send(bytes([T_HELLO]) + host_pub + host_id)
        r = self._recv_raw(15)
        if r[0] != T_HELLO_R or len(r) != 34:
            raise ChannelError("неожиданный ответ устройства на рукопожатие")
        dev_pub, mode = r[1:33], r[33]
        shared = eph.exchange(X25519PublicKey.from_public_bytes(dev_pub))
        if mode == 1 and not bond:
            raise ChannelError("устройство считает ПК знакомым, но ключа сопряжения нет")
        th = hashlib.sha256(b"MicroWallet BLE v1" + host_pub + dev_pub + host_id + bytes([mode])).digest()
        prk = hmac.new(th, shared + (bond[1] if mode == 1 else b""), hashlib.sha256).digest()
        self.k_tx, self.k_rx = _expand(prk, "h2d"), _expand(prk, "d2h")
        if mode == 1:
            if self._recv_data(15) != "ready":
                raise ChannelError("устройство не подтвердило сессию")
            return
        # Новый ПК (или устройство нас забыло): код на экране и здесь должен совпасть
        if bond:
            self.bonds.drop(self.dev_key)
        sas = _expand(prk, "sas")
        v = int.from_bytes(sas[:4], "big") % 1_000_000
        if self.on_code:
            self.on_code(f"{v // 1000:03d} {v % 1000:03d}")
        msg = self._recv_data(pair_timeout)
        if not msg.startswith("paired "):
            raise ChannelError(f"сопряжение не удалось: {msg}")
        self.bonds.put(self.dev_key, bytes.fromhex(msg[7:]), _expand(prk, "bond"))
        self.paired_now = True

    def _recv_raw(self, timeout):
        try:
            f = self.t.recv(timeout)
        except queue.Empty:
            raise ChannelError("устройство не отвечает")
        if not f:
            raise ChannelError("пустой кадр")
        if f[0] == T_ERROR:
            msg = f[1:].decode(errors="replace")
            raise W.DeviceError(msg) if msg == "rejected" else ChannelError(f"устройство: {msg}")
        return f

    def _recv_data(self, timeout):
        f = self._recv_raw(timeout)
        if f[0] != T_DATA or len(f) < 9 + 16:
            raise ChannelError("неожиданный кадр")
        ctr = struct.unpack(">Q", f[1:9])[0]
        if ctr < self.rx_next:
            raise ChannelError("повтор кадра")
        try:
            pt = ChaCha20Poly1305(self.k_rx).decrypt(_nonce(ctr), f[9:], f[:9])
        except Exception:
            raise ChannelError("подпись кадра не сошлась — канал небезопасен")
        self.rx_next = ctr + 1
        return pt.decode()

    def send_line(self, line):
        hdr = bytes([T_DATA]) + struct.pack(">Q", self.tx_next)
        ct = ChaCha20Poly1305(self.k_tx).encrypt(_nonce(self.tx_next), line.encode(), hdr)
        self.tx_next += 1
        self.t.send(hdr + ct)

    def call(self, line, timeout=None):
        self.send_line(line)
        return self._recv_data(timeout)


# ---------------- транспорт BLE (bleak) ----------------

class BleTransport:
    """GATT: пишем кадры в RX, получаем уведомления с TX. Кадр = [длина 2 байта][данные]."""

    def __init__(self, address):
        try:
            import bleak  # noqa: F401
        except ImportError:
            raise W.WalletError("Для Bluetooth нужен пакет bleak: pip install bleak")
        self.address = address
        self.frames = queue.Queue()
        self.buf = b""
        self.loop = asyncio.new_event_loop()
        self.thread = threading.Thread(target=self.loop.run_forever, daemon=True)
        self.thread.start()
        self._run(self._connect())

    def _run(self, coro, timeout=None):
        return asyncio.run_coroutine_threadsafe(coro, self.loop).result(timeout)

    async def _connect(self):
        from bleak import BleakClient
        self.client = BleakClient(self.address, disconnected_callback=lambda _: self.frames.put(b""))
        try:
            await self.client.connect(timeout=15)
        except Exception as e:
            raise W.WalletError(f"Не удалось подключиться по Bluetooth: {e}")
        await self.client.start_notify(TX_UUID, self._on_notify)

    def _on_notify(self, _, data):
        self.buf += bytes(data)
        while len(self.buf) >= 2:
            n = int.from_bytes(self.buf[:2], "big")
            if len(self.buf) < 2 + n:
                break
            self.frames.put(self.buf[2:2 + n])
            self.buf = self.buf[2 + n:]

    def send(self, frame):
        data = len(frame).to_bytes(2, "big") + frame
        chunk = max(20, (self.client.mtu_size or 23) - 3)

        async def write():
            for i in range(0, len(data), chunk):
                await self.client.write_gatt_char(RX_UUID, data[i:i + chunk], response=True)
        self._run(write(), 30)

    def recv(self, timeout=None):
        return self.frames.get(timeout=timeout)

    def close(self):
        try:
            self._run(self.client.disconnect(), 5)
        except Exception:
            pass
        self.loop.call_soon_threadsafe(self.loop.stop)


def scan(timeout=5.0):
    """Список устройств MicroWallet рядом: [(адрес, имя, rssi)]."""
    try:
        from bleak import BleakScanner
    except ImportError:
        raise W.WalletError("Для Bluetooth нужен пакет bleak: pip install bleak")

    async def run():
        found = await BleakScanner.discover(timeout=timeout, return_adv=True, service_uuids=[SERVICE_UUID])
        return [(d.address, d.name or adv.local_name or "MicroWallet", adv.rssi)
                for d, adv in found.values()]
    try:
        return asyncio.run(run())
    except Exception as e:
        raise W.WalletError(f"Bluetooth недоступен: {e}")


# ---------------- симулятор (тесты без радио) ----------------

class SimTransport:
    """Запускает tools/sim (mwsim frames) и говорит с ним кадрами через stdin/stdout."""

    def __init__(self, binary, env=None):
        self.p = subprocess.Popen([binary, "frames"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  env={**os.environ, **(env or {})})
        self.frames = queue.Queue()
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self):
        while True:
            h = self.p.stdout.read(2)
            if len(h) < 2:
                self.frames.put(b"")
                return
            self.frames.put(self.p.stdout.read(int.from_bytes(h, "big")))

    def send(self, frame):
        self.p.stdin.write(len(frame).to_bytes(2, "big") + frame)
        self.p.stdin.flush()

    def recv(self, timeout=None):
        return self.frames.get(timeout=timeout)

    def close(self):
        self.p.stdin.close()
        self.p.wait(5)


# ---------------- устройство по BLE ----------------

class BleDevice:
    """То же, что wallet.Device, но через шифрованный канал."""

    def __init__(self, address, name="MicroWallet", on_code=None, transport=None, bonds=None):
        self.port = f"Bluetooth · {name}"
        self.lock = threading.Lock()
        self.t = transport or BleTransport(address)
        self.ch = Channel(self.t, address, bonds, on_code)
        try:
            self.ch.handshake()
        except Exception:
            self.close()
            raise

    def call(self, line):
        with self.lock:
            resp = self.ch.call(line)
        return W.parse_reply(resp)

    def stream(self, lines, window=4, on_reply=None):
        """Строки подряд, до window в полёте (см. wallet.Device.stream)."""
        with self.lock:
            sent = got = 0
            while got < len(lines):
                while sent < len(lines) and sent - got < window:
                    self.ch.send_line(lines[sent])
                    sent += 1
                resp = self.ch._recv_data(60)
                got += 1
                try:
                    W.parse_reply(resp)
                except W.DeviceError:
                    for _ in range(sent - got):
                        self.ch._recv_data(60)
                    raise
                if on_reply:
                    on_reply(got)

    def info(self):
        return dict(kv.split("=", 1) for kv in self.call("info").split() if "=" in kv)

    def close(self):
        try:
            self.t.close()
        except Exception:
            pass
