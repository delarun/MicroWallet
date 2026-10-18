#!/usr/bin/env python3
"""
MicroWallet — настольное приложение (PySide6) поверх wallet.py.

  pip install -r host/requirements.txt
  python host/app.py            # с устройством
  python host/app.py --demo     # без устройства, на тестовых данных
  python host/app.py --ble auto # сразу по Bluetooth

Приложение не знает приватных ключей: оно собирает транзакции и показывает
их, а подписывает только устройство после подтверждения кнопкой.
"""
import argparse
import hashlib
import math
import random
import sys
import time
from decimal import Decimal
from pathlib import Path

from PySide6.QtCore import (QObject, QPointF, QRectF, QRunnable, QSize, Qt, QThreadPool,
                            QTimer, QUrl, Signal)
from PySide6.QtGui import (QColor, QDesktopServices, QFont, QFontDatabase, QGuiApplication,
                           QIcon, QLinearGradient, QPainter, QPainterPath, QPen, QPixmap,
                           QRadialGradient)
from PySide6.QtWidgets import (QApplication, QButtonGroup, QCheckBox, QCompleter, QDialog,
                               QFileDialog, QFrame, QGridLayout, QHBoxLayout, QLabel, QLineEdit,
                               QListWidget, QListWidgetItem, QMainWindow, QMessageBox,
                               QProgressBar, QPushButton, QScrollArea, QSpinBox,
                               QStackedWidget, QVBoxLayout, QWidget)

sys.path.insert(0, str(Path(__file__).resolve().parent))
import wallet as W  # noqa: E402

try:
    import ble as B  # noqa: E402  (нужен пакет cryptography; bleak — только для радио)
except ImportError:
    B = None

try:
    import qrcode
except ImportError:  # QR-код необязателен
    qrcode = None

# ---------------- оформление ----------------

C = {
    "bg": "#0E0F13", "side": "#121318", "card": "#16171E", "card2": "#1B1C25",
    "line": "#242530", "text": "#ECEDF2", "muted": "#8B8D98", "faint": "#5D5F6B",
    "accent": "#7C6CFF", "accent2": "#5B8CFF", "ok": "#3DD68C", "warn": "#F5B544",
    "bad": "#FF5C6C",
}
CHAINS = {
    "btc": {"name": "Bitcoin", "color": "#F7931A", "net": {"testnet": "testnet4", "mainnet": "mainnet"}},
    "tron": {"name": "TRON", "color": "#EF0027", "net": {"testnet": "Nile", "mainnet": "mainnet"}},
    "bsc": {"name": "BNB Chain", "color": "#F3BA2F", "net": {"testnet": "testnet", "mainnet": "mainnet"}},
    "prl": {"name": "Pearl", "color": "#B9C7DA", "net": {"testnet": "testnet2", "mainnet": "mainnet"}},
}
ORDER = ["btc", "prl", "tron", "bsc"]
MONO = "JetBrains Mono, Cascadia Mono, Consolas, Menlo, DejaVu Sans Mono, monospace"

QSS = f"""
* {{ font-size: 14px; color: {C['text']}; }}
QMainWindow, #content, #page {{ background: {C['bg']}; }}
QScrollArea > QWidget#qt_scrollarea_viewport {{ background: {C['bg']}; }}
#sidebar {{ background: {C['side']}; border-right: 1px solid {C['line']}; }}
#brand {{ font-size: 17px; font-weight: 700; }}
#nav {{ text-align: left; padding: 10px 14px; border: none; border-radius: 10px;
        color: {C['muted']}; font-weight: 600; background: transparent; }}
#nav:hover {{ background: #191A22; color: {C['text']}; }}
#nav:checked {{ background: #1F2030; color: {C['text']}; }}
#h1 {{ font-size: 26px; font-weight: 700; }}
#h2 {{ font-size: 17px; font-weight: 700; }}
#muted {{ color: {C['muted']}; }}
#faint {{ color: {C['faint']}; font-size: 12px; }}
#big {{ font-size: 22px; font-weight: 700; }}
#mono {{ font-family: {MONO}; font-size: 15px; }}
#card {{ background: {C['card']}; border: 1px solid {C['line']}; border-radius: 16px; }}
#inner {{ background: {C['card2']}; border: none; border-radius: 12px; }}
#badge {{ border-radius: 9px; padding: 3px 10px; font-size: 12px; font-weight: 700; }}
QPushButton {{ border-radius: 10px; padding: 10px 18px; font-weight: 600;
               background: #1E1F29; border: 1px solid #2A2B36; }}
QPushButton:hover {{ background: #252634; }}
QPushButton:disabled {{ color: {C['faint']}; background: #17181F; border-color: #1F2029; }}
#primary {{ border: none; color: white;
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 {C['accent']}, stop:1 {C['accent2']}); }}
#primary:hover {{ background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #8D7FFF, stop:1 #6E9AFF); }}
#primary:disabled {{ background: #2A2B3A; color: {C['faint']}; }}
#danger {{ background: #26151A; color: {C['bad']}; border: 1px solid #45202A; }}
#danger:hover {{ background: #321A21; }}
#link {{ background: transparent; border: none; color: {C['accent2']}; padding: 4px; }}
#link:hover {{ color: #8FB0FF; }}
#pill {{ border-radius: 18px; padding: 8px 16px 8px 10px; background: {C['card']};
         border: 1px solid {C['line']}; color: {C['muted']}; }}
#pill:checked {{ background: #20213A; border: 1px solid {C['accent']}; color: {C['text']}; }}
#seg {{ border-radius: 8px; padding: 7px 14px; background: transparent; border: none;
        color: {C['muted']}; }}
#seg:checked {{ background: #2A2B3C; color: {C['text']}; }}
#segbox {{ background: {C['card2']}; border-radius: 10px; }}
QLineEdit, QSpinBox {{ background: #101117; border: 1px solid #272833; border-radius: 10px;
                       padding: 10px 12px; selection-background-color: {C['accent']}; }}
QLineEdit:focus, QSpinBox:focus {{ border: 1px solid {C['accent']}; }}
QSpinBox::up-button, QSpinBox::down-button {{ width: 0; border: none; }}
QCheckBox {{ color: {C['muted']}; spacing: 8px; }}
QCheckBox::indicator {{ width: 18px; height: 18px; border-radius: 5px; border: 1px solid #343545;
                        background: #101117; }}
QCheckBox::indicator:checked {{ background: {C['accent']}; border-color: {C['accent']}; }}
QListWidget {{ background: transparent; border: none; font-family: {MONO}; font-size: 12px;
               color: {C['muted']}; }}
QScrollArea {{ border: none; background: transparent; }}
QScrollBar:vertical {{ background: transparent; width: 8px; }}
QScrollBar::handle:vertical {{ background: #2A2B36; border-radius: 4px; min-height: 30px; }}
QScrollBar::add-line, QScrollBar::sub-line {{ height: 0; }}
QMessageBox, QDialog {{ background: {C['card']}; }}
QMessageBox QLabel {{ color: {C['text']}; }}
QProgressBar {{ background: #101117; border: 1px solid #272833; border-radius: 6px; height: 10px;
                 text-align: center; color: transparent; }}
QProgressBar::chunk {{ border-radius: 5px;
    background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 {C['accent']}, stop:1 {C['accent2']}); }}
#warnbox {{ background: rgba(245, 181, 68, 0.10); border: 1px solid rgba(245, 181, 68, 0.35);
            border-radius: 12px; }}
#codebox {{ background: {C['card2']}; border-radius: 14px; font-size: 40px; font-weight: 800;
            letter-spacing: 6px; padding: 18px 26px; }}
QToolTip {{ background: #22232D; color: {C['text']}; border: 1px solid #333; padding: 6px; }}
"""


def hidpi_pixmap(size):
    ratio = QGuiApplication.primaryScreen().devicePixelRatio() if QGuiApplication.primaryScreen() else 1
    pm = QPixmap(int(size * max(ratio, 2)), int(size * max(ratio, 2)))
    pm.setDevicePixelRatio(max(ratio, 2))
    pm.fill(Qt.transparent)
    return pm


def coin_icon(kind, size=40):
    """Значки монет рисуются кодом: круг цвета сети и простой знак."""
    pm = hidpi_pixmap(size)
    p = QPainter(pm)
    p.setRenderHint(QPainter.Antialiasing)
    r = QRectF(0, 0, size, size)
    s = size / 40
    if kind == "prl":  # жемчужина: радиальный градиент и блик
        g = QRadialGradient(QPointF(size * .38, size * .32), size * .7)
        g.setColorAt(0, QColor("#FFFFFF"))
        g.setColorAt(.45, QColor("#DCE4F0"))
        g.setColorAt(1, QColor("#7F8CA6"))
        p.setBrush(g)
        p.setPen(Qt.NoPen)
        p.drawEllipse(r)
        p.setBrush(QColor(255, 255, 255, 200))
        p.drawEllipse(QRectF(size * .24, size * .18, size * .22, size * .15))
    elif kind == "logo":
        g = QLinearGradient(0, 0, size, size)
        g.setColorAt(0, QColor(C["accent"]))
        g.setColorAt(1, QColor(C["accent2"]))
        p.setBrush(g)
        p.setPen(Qt.NoPen)
        p.drawRoundedRect(r, size * .28, size * .28)
        p.setBrush(QColor("white"))
        p.drawRoundedRect(QRectF(size * .25, size * .3, size * .5, size * .3), 3 * s, 3 * s)
        p.setBrush(QColor(C["accent"]))
        p.drawRect(QRectF(size * .31, size * .36, size * .38, size * .18))
        p.setBrush(QColor("white"))
        p.drawEllipse(QRectF(size * .44, size * .66, size * .12, size * .12))
    else:
        color = {"usdt": "#26A17B"}.get(kind) or CHAINS[kind]["color"]
        p.setBrush(QColor(color))
        p.setPen(Qt.NoPen)
        p.drawEllipse(r)
        white = QColor("white")
        if kind == "btc":
            p.translate(size / 2, size / 2)
            p.rotate(14)
            f = QFont()
            f.setPixelSize(int(24 * s))
            f.setBold(True)
            p.setFont(f)
            p.setPen(white)
            p.drawText(QRectF(-size / 2, -size / 2, size, size), Qt.AlignCenter, "B")
            pen = QPen(white, 2.2 * s)
            p.setPen(pen)
            for x in (-3 * s, 2 * s):
                p.drawLine(QPointF(x, -12 * s), QPointF(x, -8.5 * s))
                p.drawLine(QPointF(x, 8.5 * s), QPointF(x, 12 * s))
        elif kind == "tron":
            path = QPainterPath()
            path.moveTo(10 * s, 11 * s)
            path.lineTo(31 * s, 15 * s)
            path.lineTo(19 * s, 31 * s)
            path.closeSubpath()
            p.setPen(QPen(white, 2.2 * s, Qt.SolidLine, Qt.RoundCap, Qt.RoundJoin))
            p.setBrush(Qt.NoBrush)
            p.drawPath(path)
            p.drawLine(QPointF(10 * s, 11 * s), QPointF(22 * s, 21 * s))
            p.drawLine(QPointF(22 * s, 21 * s), QPointF(31 * s, 15 * s))
            p.drawLine(QPointF(22 * s, 21 * s), QPointF(19 * s, 31 * s))
        elif kind == "bsc":
            p.setBrush(QColor("#1E2026"))
            p.translate(size / 2, size / 2)
            p.rotate(45)
            for dx, dy, w in ((0, 0, 6), (-7.5, -7.5, 4), (7.5, 7.5, 4), (-7.5, 7.5, 4), (7.5, -7.5, 4)):
                p.drawRect(QRectF((dx - w / 2) * s, (dy - w / 2) * s, w * s, w * s))
        elif kind == "usdt":
            p.setPen(Qt.NoPen)
            p.setBrush(white)
            p.drawRect(QRectF(11 * s, 10 * s, 18 * s, 4 * s))
            p.drawRect(QRectF(18 * s, 10 * s, 4 * s, 20 * s))
            p.setPen(QPen(white, 2 * s))
            p.setBrush(Qt.NoBrush)
            p.drawEllipse(QRectF(9 * s, 16 * s, 22 * s, 6 * s))
    p.end()
    return pm


def line_icon(name, color=C["text"], size=18):
    """Линейные иконки интерфейса."""
    pm = hidpi_pixmap(size)
    p = QPainter(pm)
    p.setRenderHint(QPainter.Antialiasing)
    s = size / 18
    pen = QPen(QColor(color), 1.8 * s, Qt.SolidLine, Qt.RoundCap, Qt.RoundJoin)
    p.setPen(pen)
    p.setBrush(Qt.NoBrush)
    if name == "overview":
        for x, y in ((2, 2), (10, 2), (2, 10), (10, 10)):
            p.drawRoundedRect(QRectF(x * s, y * s, 6 * s, 6 * s), 1.5 * s, 1.5 * s)
    elif name in ("send", "receive"):
        if name == "send":
            a, b, h1, h2 = (4, 14), (14, 4), (7, 4), (14, 11)
        else:
            a, b, h1, h2 = (14, 4), (4, 14), (4, 7), (11, 14)
        p.drawLine(QPointF(*[v * s for v in a]), QPointF(*[v * s for v in b]))
        p.drawLine(QPointF(*[v * s for v in h1]), QPointF(*[v * s for v in b]))
        p.drawLine(QPointF(*[v * s for v in h2]), QPointF(*[v * s for v in b]))
    elif name == "device":
        p.drawRoundedRect(QRectF(2 * s, 4 * s, 14 * s, 10 * s), 2.5 * s, 2.5 * s)
        p.drawRect(QRectF(4.5 * s, 6.5 * s, 6 * s, 5 * s))
        p.drawEllipse(QPointF(13 * s, 9 * s), 1 * s, 1 * s)
    elif name == "refresh":
        p.drawArc(QRectF(3 * s, 3 * s, 12 * s, 12 * s), 40 * 16, 290 * 16)
        p.drawLine(QPointF(14 * s, 2.5 * s), QPointF(14 * s, 7 * s))
        p.drawLine(QPointF(14 * s, 7 * s), QPointF(9.5 * s, 7 * s))
    elif name == "copy":
        p.drawRoundedRect(QRectF(6 * s, 6 * s, 9 * s, 9 * s), 2 * s, 2 * s)
        p.drawPolyline([QPointF(3 * s, 11 * s), QPointF(3 * s, 3 * s), QPointF(11 * s, 3 * s)])
    elif name == "check":
        p.drawPolyline([QPointF(3 * s, 9.5 * s), QPointF(7.5 * s, 14 * s), QPointF(15 * s, 4.5 * s)])
    elif name == "cross":
        p.drawLine(QPointF(4 * s, 4 * s), QPointF(14 * s, 14 * s))
        p.drawLine(QPointF(14 * s, 4 * s), QPointF(4 * s, 14 * s))
    elif name == "external":
        p.drawPolyline([QPointF(10 * s, 3 * s), QPointF(15 * s, 3 * s), QPointF(15 * s, 8 * s)])
        p.drawLine(QPointF(15 * s, 3 * s), QPointF(8 * s, 10 * s))
        p.drawPolyline([QPointF(13 * s, 11 * s), QPointF(13 * s, 15 * s), QPointF(3 * s, 15 * s),
                        QPointF(3 * s, 5 * s), QPointF(7 * s, 5 * s)])
    p.end()
    return QIcon(pm)


def label(text="", obj=None, wrap=False, sel=False):
    w = QLabel(text)
    if obj:
        w.setObjectName(obj)
    w.setWordWrap(wrap)
    if sel:
        w.setTextInteractionFlags(Qt.TextSelectableByMouse)
    return w


def card(obj="card"):
    f = QFrame()
    f.setObjectName(obj)
    return f


def tint(color, alpha=0.14):
    c = QColor(color)
    return f"rgba({c.red()}, {c.green()}, {c.blue()}, {alpha})"


def badge(text, color):
    b = label(text, "badge")
    b.setStyleSheet(f"background: {tint(color)}; color: {color};")
    return b


def button(text, obj=None, icon=None):
    b = QPushButton(text)
    if obj:
        b.setObjectName(obj)
    if icon:
        b.setIcon(icon)
        b.setIconSize(QSize(18, 18))
    b.setCursor(Qt.PointingHandCursor)
    return b


def short(addr, n=10):
    return addr if len(addr) <= 2 * n + 1 else f"{addr[:n]}…{addr[-n + 2:]}"


def addr_block(addr, width=32):
    """Адрес ровными строками (как на устройстве): 64 символа -> 2 x 32."""
    lines = max(1, math.ceil(len(addr) / width))
    per = math.ceil(len(addr) / lines)
    return "\n".join(addr[i:i + per] for i in range(0, len(addr), per))


def battery_text(mv):
    pts = [(3500, 0), (3600, 8), (3700, 20), (3750, 30), (3800, 40), (3850, 50),
           (3900, 60), (4000, 75), (4100, 90), (4200, 100)]  # как ui_battery_pct() в прошивке
    pct = 0 if mv <= 3500 else 100
    for (v0, p0), (v1, p1) in zip(pts, pts[1:]):
        if v0 < mv <= v1:
            pct = p0 + (p1 - p0) * (mv - v0) // (v1 - v0)
    return f"{mv / 1000:.2f} В · {pct}%"


def human(e):
    if isinstance(e, W.DeviceError):
        if str(e) == "rejected":
            return "Отклонено на устройстве"
        return f"Устройство: {e}"
    if isinstance(e, W.WalletError):
        return str(e)
    return f"{type(e).__name__}: {e}"


class Spinner(QWidget):
    def __init__(self, size=56, color=C["accent"]):
        super().__init__()
        self.setFixedSize(size, size)
        self.angle, self.color = 0, QColor(color)
        self.t = QTimer(self, interval=16, timeout=self._tick)
        self.t.start()

    def _tick(self):
        if not self.isVisible():
            return
        self.angle = (self.angle + 6) % 360
        self.update()

    def paintEvent(self, _):
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        w = self.width()
        p.setPen(QPen(QColor(C["line"]), w / 12))
        p.drawEllipse(QRectF(w / 12, w / 12, w * 10 / 12, w * 10 / 12))
        p.setPen(QPen(self.color, w / 12, Qt.SolidLine, Qt.RoundCap))
        p.drawArc(QRectF(w / 12, w / 12, w * 10 / 12, w * 10 / 12), -self.angle * 16, 100 * 16)


class Dot(QWidget):
    def __init__(self, color=C["faint"]):
        super().__init__()
        self.setFixedSize(10, 10)
        self.color = color

    def set(self, color):
        self.color = color
        self.update()

    def paintEvent(self, _):
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        p.setPen(Qt.NoPen)
        p.setBrush(QColor(self.color))
        p.drawEllipse(QRectF(1, 1, 8, 8))


def qr_pixmap(text, size, center=None):
    if not qrcode:
        return None
    q = qrcode.QRCode(error_correction=qrcode.constants.ERROR_CORRECT_H, border=0)
    q.add_data(text)
    q.make(fit=True)
    m = q.get_matrix()
    n = len(m)
    pm = hidpi_pixmap(size)
    p = QPainter(pm)
    p.setRenderHint(QPainter.Antialiasing)
    p.setPen(Qt.NoPen)
    p.setBrush(QColor("white"))
    p.drawRoundedRect(QRectF(0, 0, size, size), 14, 14)
    pad = 14
    cell = (size - 2 * pad) / n
    p.setBrush(QColor("#0E0F13"))
    for y, row in enumerate(m):
        for x, v in enumerate(row):
            if v:
                p.drawRect(QRectF(pad + x * cell, pad + y * cell, cell + .3, cell + .3))
    if center is not None:  # значок монеты в центре, коррекция H это выдерживает
        c = size * .2
        p.setBrush(QColor("white"))
        p.drawRoundedRect(QRectF((size - c) / 2 - 4, (size - c) / 2 - 4, c + 8, c + 8), 8, 8)
        p.drawPixmap(QRectF((size - c) / 2, (size - c) / 2, c, c), center, QRectF(center.rect()))
    p.end()
    return pm


# ---------------- фоновые задачи ----------------

class _Signals(QObject):
    done = Signal(object)
    failed = Signal(object)


class Task(QRunnable):
    def __init__(self, fn, *args):
        super().__init__()
        self.fn, self.args, self.sig = fn, args, _Signals()

    def run(self):
        try:
            r = self.fn(*self.args)
        except Exception as e:  # всё уходит в интерфейс как текст
            sig, val = self.sig.failed, e
        else:
            sig, val = self.sig.done, r
        try:
            sig.emit(val)
        except RuntimeError:  # окно уже закрыто — результат никому не нужен
            pass


# ---------------- демо-режим ----------------

class DemoWallet:
    """Тот же интерфейс, что у wallet.Wallet, но без устройства и сети."""
    net, coins, ready, chain_id = "testnet", ["btc", "tron", "bsc", "prl"], True, 97
    info = {"fw": "0.5", "net": "testnet", "state": "ready", "coins": "btc,tron,bsc,prl", "bsc_chain": "97",
            "hw": "esp32s3", "batt": "3920"}
    cfg = W.NETS["testnet"]
    ADDR = {"btc": "tb1qcr8te4kr609gcawutmrza0j4xv80jy8z306fyu",
            "tron": "TUEZSdKsoDHQMeZwihtdoBiN46zxhGWYdH",
            "bsc": "0x9858EfFD232B4033E47d90003D41EC34EcaEda94",
            "prl": "tprl1pr6yuq8u2r95wjzzgpdy8cpnncpl7l8zgy6x5q0367pnc53s2famqg7pt74"}

    def __init__(self, empty=False):
        self.dev = type("D", (), {"port": "demo"})()
        self.ready = not empty
        self.info = dict(self.info, state="ready" if self.ready else "empty")

    def restore(self, phrase):
        W.mnemonic_to_entropy(phrase)
        time.sleep(2.5)
        self.ready = True
        self.info["state"] = "ready"
        return True

    def symbol(self, asset):
        return W.asset_symbol(self.cfg, asset)

    def address(self, coin, index=0, chain=0):
        time.sleep(.2)
        a = self.ADDR[coin]
        return a if index == 0 else a[:-4] + f"{index:04d}"

    def show_address(self, coin, index=0):
        time.sleep(2.5)
        return self.address(coin, index)

    def balance(self, coin, index=0):
        time.sleep(random.uniform(.4, 1.2))
        a = {"btc": [{"id": "btc", "symbol": "tBTC", "decimals": 8, "amount": 4_120_000}],
             "prl": [{"id": "prl", "symbol": "tPRL", "decimals": 8, "amount": 451_069_071_991}],
             "tron": [{"id": "usdt", "symbol": "USDT", "decimals": 6, "amount": 250_000_000},
                      {"id": "trx", "symbol": "TRX", "decimals": 6, "amount": 120_500_000}],
             "bsc": [{"id": "usdt", "symbol": "USDT", "decimals": 18, "amount": 75 * 10**18},
                     {"id": "bnb", "symbol": "tBNB", "decimals": 18, "amount": 3 * 10**16}]}[coin]
        pend = 225_390_153_581 if coin == "prl" else 0
        return {"address": self.ADDR[coin], "assets": a, "pending": pend, "notes": []}

    def fee_levels(self, coin):
        time.sleep(.3)
        return {"fast": Decimal(1164), "normal": Decimal("1.4"), "slow": Decimal(1)} if coin == "prl" \
            else {"fast": Decimal(6), "normal": Decimal(3), "slow": Decimal(1)}

    def prepare_send(self, coin, dest, amount, asset=None, index=0, feerate=None, fee_limit=None,
                     gas_price=None):
        time.sleep(.8)
        {"btc": lambda: W.address_to_script(dest, "tb"), "prl": lambda: W.address_to_script(dest, "tprl", True),
         "tron": lambda: W.tron_decode(dest), "bsc": lambda: W.evm_decode(dest)}[coin]()
        asset = asset or W.COINS[coin]["assets"][0][0]
        fee = {"btc": ("0.00000424", "tBTC"), "prl": ("0.00246824", "tPRL"),
               "tron": (str(fee_limit or 40), "TRX"), "bsc": ("0.00000612", "tBNB")}[coin]
        return W.Draft(coin, asset, self.symbol(asset), self.ADDR[coin], dest.strip(), amount,
                       fee[0], fee[1], fee_label="Макс. комиссия" if coin in ("tron", "bsc") else "Комиссия",
                       notes=["Демо-режим: устройство не подключено"],
                       sign=lambda: (time.sleep(3.5), ("00" * 120, "%064x" % random.getrandbits(256)))[1],
                       broadcast=lambda raw: time.sleep(.5), tx_url="https://example.invalid/tx/")

    def sign_and_send(self, draft, broadcast=True):
        raw, txid = draft.sign()
        if broadcast:
            draft.broadcast(raw)
        return {"raw": raw, "txid": txid, "url": draft.tx_url + txid if broadcast else "",
                "broadcast": broadcast}


# ---------------- страницы ----------------

class Page(QWidget):
    def __init__(self, app, title, subtitle=""):
        super().__init__()
        self.setObjectName("page")
        self.app = app
        outer = QVBoxLayout(self)
        outer.setContentsMargins(36, 28, 36, 28)
        outer.setSpacing(18)
        head = QHBoxLayout()
        tv = QVBoxLayout()
        tv.setSpacing(2)
        tv.addWidget(label(title, "h1"))
        if subtitle:
            tv.addWidget(label(subtitle, "muted"))
        head.addLayout(tv)
        head.addStretch()
        self.head = head
        outer.addLayout(head)
        self.body = QVBoxLayout()
        self.body.setSpacing(16)
        outer.addLayout(self.body, 1)

    def wallet_changed(self):
        pass


class ChainCard(QFrame):
    def __init__(self, app, coin):
        super().__init__()
        self.setObjectName("card")
        self.app, self.coin = app, coin
        v = QVBoxLayout(self)
        v.setContentsMargins(20, 18, 20, 18)
        v.setSpacing(14)
        top = QHBoxLayout()
        ic = QLabel()
        ic.setPixmap(coin_icon(coin, 40))
        top.addWidget(ic)
        tv = QVBoxLayout()
        tv.setSpacing(0)
        tv.addWidget(label(CHAINS[coin]["name"], "h2"))
        self.net = label("", "faint")
        tv.addWidget(self.net)
        top.addLayout(tv)
        top.addStretch()
        self.addr = button("", "link", line_icon("copy", C["accent2"], 16))
        self.addr.setToolTip("Скопировать адрес")
        self.addr.clicked.connect(lambda: self.app.copy(self.full_addr, "Адрес скопирован"))
        top.addWidget(self.addr)
        v.addLayout(top)
        self.rows = QVBoxLayout()
        self.rows.setSpacing(8)
        v.addLayout(self.rows)
        v.addStretch()
        acts = QHBoxLayout()
        acts.setSpacing(8)
        s = button("Отправить", None, line_icon("send"))
        s.clicked.connect(lambda: self.app.goto_send(coin))
        r = button("Получить", None, line_icon("receive"))
        r.clicked.connect(lambda: self.app.goto_receive(coin))
        acts.addWidget(s)
        acts.addWidget(r)
        acts.addStretch()
        v.addLayout(acts)
        self.full_addr = ""
        self.set_loading()

    def _clear(self):
        while self.rows.count():
            it = self.rows.takeAt(0)
            if it.widget():
                it.widget().deleteLater()
            elif it.layout():
                while it.layout().count():
                    w = it.layout().takeAt(0).widget()
                    if w:
                        w.deleteLater()

    def set_loading(self):
        self._clear()
        self.rows.addWidget(label("Загрузка…", "muted"))
        self.addr.setText("")

    def set_error(self, msg):
        self._clear()
        e = label(msg, wrap=True)
        e.setStyleSheet(f"color: {C['bad']};")
        self.rows.addWidget(e)

    def set_balance(self, b, net):
        self._clear()
        self.net.setText(f"{CHAINS[self.coin]['net'][net]}")
        self.full_addr = b["address"]
        self.addr.setText(short(b["address"], 7))
        for a in b["assets"]:
            row = QFrame()
            row.setObjectName("inner")
            h = QHBoxLayout(row)
            h.setContentsMargins(12, 10, 14, 10)
            ic = QLabel()
            ic.setPixmap(coin_icon("usdt" if a["id"] == "usdt" else self.coin, 26))
            h.addWidget(ic)
            name = a["symbol"] + ("  ·  " + ("TRC20" if self.coin == "tron" else "BEP20") if a["id"] == "usdt" else "")
            h.addWidget(label(name, "muted"))
            h.addStretch()
            h.addWidget(label(W.fmt_units(a["amount"], a["decimals"]), "big"))
            self.rows.addWidget(row)
        if b.get("pending"):
            p = label(f"Созревает: {W.fmt_units(b['pending'], 8)} {b['assets'][0]['symbol']}  "
                      "(награды за блок, 100 подтверждений)", "faint", wrap=True)
            p.setStyleSheet(f"color: {C['warn']}; font-size: 12px;")
            self.rows.addWidget(p)
        for n in b.get("notes", []):
            self.rows.addWidget(label(n, "faint", wrap=True))


class OverviewPage(Page):
    def __init__(self, app):
        super().__init__(app, "Обзор", "Балансы всех сетей на устройстве")
        self.refresh_btn = button("Обновить", None, line_icon("refresh"))
        self.refresh_btn.clicked.connect(self.refresh)
        self.head.addWidget(self.refresh_btn)
        self.grid = QGridLayout()
        self.grid.setSpacing(16)
        self.cards = {}
        for i, c in enumerate(ORDER):
            self.cards[c] = ChainCard(app, c)
            self.grid.addWidget(self.cards[c], i // 2, i % 2)
        self.empty = label("", "muted", wrap=True)
        self.body.addWidget(self.empty)
        # Пустое устройство: создать на нём или восстановить из фразы
        self.setup = card()
        sl = QVBoxLayout(self.setup)
        sl.setContentsMargins(28, 24, 28, 24)
        sl.setSpacing(10)
        sl.addWidget(label("Устройство пустое", "h2"))
        sl.addWidget(label("Новый кошелёк лучше создать на самом устройстве: удерживай кнопку на "
                           "«New wallet» и запиши 12 слов на бумагу. Если у тебя уже есть seed-фраза, "
                           "её можно ввести здесь.", "muted", wrap=True))
        restore = button("Восстановить из seed-фразы", "primary", line_icon("receive", "white"))
        restore.clicked.connect(lambda: RestoreDialog(self.app).exec())
        rl = QHBoxLayout()
        rl.addWidget(restore)
        rl.addStretch()
        sl.addLayout(rl)
        self.setup.hide()
        self.body.addWidget(self.setup)
        self.body.addLayout(self.grid)
        self.body.addStretch()
        self.balances = {}

    def wallet_changed(self):
        w = self.app.wallet
        for c in self.cards.values():
            c.setVisible(bool(w))
        self.setup.setVisible(bool(w) and not w.ready)
        if not w:
            self.empty.setText("Подключи MicroWallet по USB или по Bluetooth (страница «Устройство») — "
                               "балансы появятся здесь.")
            self.empty.show()
        elif not w.ready:
            self.empty.hide()
            for c in self.cards.values():
                c.hide()
        else:
            self.empty.hide()
            self.refresh()

    def refresh(self):
        w = self.app.wallet
        if not w or not w.ready:
            return
        for coin, cardw in self.cards.items():
            if coin not in w.coins:
                cardw.hide()
                continue
            cardw.set_loading()
            self.app.run(w.balance, coin,
                         done=lambda b, coin=coin: self._done(coin, b),
                         failed=lambda e, coin=coin: self.cards[coin].set_error(human(e)))

    def _done(self, coin, b):
        self.balances[coin] = b
        self.cards[coin].set_balance(b, self.app.wallet.net)
        self.app.balances_changed.emit()


class CoinPills(QWidget):
    changed = Signal(str)

    def __init__(self):
        super().__init__()
        h = QHBoxLayout(self)
        h.setContentsMargins(0, 0, 0, 0)
        h.setSpacing(8)
        self.group = QButtonGroup(self)
        self.btns = {}
        for c in ORDER:
            b = QPushButton(CHAINS[c]["name"])
            b.setObjectName("pill")
            b.setCheckable(True)
            b.setIcon(QIcon(coin_icon(c, 22)))
            b.setIconSize(QSize(22, 22))
            b.setCursor(Qt.PointingHandCursor)
            b.clicked.connect(lambda _=False, c=c: self.changed.emit(c))
            self.group.addButton(b)
            self.btns[c] = b
            h.addWidget(b)
        h.addStretch()
        self.btns["btc"].setChecked(True)

    def set(self, coin):
        self.btns[coin].setChecked(True)
        self.changed.emit(coin)

    def current(self):
        return next(c for c, b in self.btns.items() if b.isChecked())


class Segmented(QFrame):
    changed = Signal(str)

    def __init__(self, items):
        super().__init__()
        self.setObjectName("segbox")
        self.h = QHBoxLayout(self)
        self.h.setContentsMargins(4, 4, 4, 4)
        self.h.setSpacing(4)
        self.group = QButtonGroup(self)
        self.btns = {}
        self.set_items(items)

    def set_items(self, items):
        for b in self.btns.values():
            self.group.removeButton(b)
            self.h.removeWidget(b)
            b.setParent(None)  # сразу убрать с экрана
            b.deleteLater()
        self.btns = {}
        for key, text in items:
            b = QPushButton(text)
            b.setObjectName("seg")
            b.setCheckable(True)
            b.setCursor(Qt.PointingHandCursor)
            b.clicked.connect(lambda _=False, k=key: self.changed.emit(k))
            self.group.addButton(b)
            self.btns[key] = b
            self.h.addWidget(b)
        if items:
            self.btns[items[0][0]].setChecked(True)

    def set_text(self, key, text):
        if key in self.btns:
            self.btns[key].setText(text)

    def current(self):
        return next((k for k, b in self.btns.items() if b.isChecked()), None)


class ReceivePage(Page):
    def __init__(self, app):
        super().__init__(app, "Получить", "Адрес для приёма — сверь его с экраном устройства")
        self.pills = CoinPills()
        self.pills.changed.connect(lambda _: self.load())
        self.body.addWidget(self.pills)

        c = card()
        h = QHBoxLayout(c)
        h.setContentsMargins(28, 28, 28, 28)
        h.setSpacing(32)
        self.qr = QLabel()
        self.qr.setFixedSize(236, 236)
        self.qr.setAlignment(Qt.AlignCenter)
        h.addWidget(self.qr, 0, Qt.AlignTop)
        v = QVBoxLayout()
        v.setSpacing(12)
        row = QHBoxLayout()
        self.title = label("", "h2")
        row.addWidget(self.title)
        row.addStretch()
        row.addWidget(label("Номер адреса", "muted"))
        self.index = QSpinBox()
        self.index.setRange(0, 19)
        self.index.setFixedWidth(70)
        self.index.valueChanged.connect(lambda _: self.load())
        row.addWidget(self.index)
        v.addLayout(row)
        # Адрес не выделяется мышью: копировать — кнопкой, без переносов строк
        self.addr = label("", "mono")
        self.addr.setStyleSheet(f"font-family: {MONO}; font-size: 18px; background: {C['card2']};"
                                "border-radius: 12px; padding: 14px 16px;")
        self.addr.setMinimumHeight(60)
        self.full = ""
        v.addWidget(self.addr)
        self.status = label("", wrap=True)
        v.addWidget(self.status)
        acts = QHBoxLayout()
        self.verify = button("Проверить на устройстве", "primary", line_icon("device", "white"))
        self.verify.clicked.connect(self.do_verify)
        self.copy = button("Скопировать", None, line_icon("copy"))
        self.copy.clicked.connect(lambda: self.app.copy(self.full, "Адрес скопирован"))
        acts.addWidget(self.verify)
        acts.addWidget(self.copy)
        acts.addStretch()
        v.addLayout(acts)
        v.addStretch()
        tip = label("Устройство покажет адрес по шагам: сверь все символы и удерживай кнопку на "
                    "«Approve». Если адрес отличается — удерживай на «Reject».", "faint", wrap=True)
        v.addWidget(tip)
        h.addLayout(v, 1)
        self.body.addWidget(c)
        self.body.addStretch()

    def wallet_changed(self):
        w = self.app.wallet  # номера адресов — по настройке устройства «Addresses»
        self.index.setRange(0, (w.addr_count() if w and hasattr(w, "addr_count") else W.ADDRS_DEFAULT) - 1)
        self.load()

    def set_status(self, kind, text):
        color = {"ok": C["ok"], "bad": C["bad"], "wait": C["warn"], "info": C["muted"]}[kind]
        self.status.setText(text)
        self.status.setStyleSheet(f"color: {color}; font-weight: 600;")

    def load(self):
        w = self.app.wallet
        coin = self.pills.current()
        self.title.setText(f"{CHAINS[coin]['name']}  #{self.index.value()}")
        self.verify.setEnabled(False)
        self.copy.setEnabled(False)
        self.full = ""
        if not w or not w.ready:
            self.addr.setText("")
            self.qr.clear()
            self.set_status("info", "Устройство не подключено" if not w else "На устройстве нет кошелька")
            return
        self.addr.setText("…")
        self.set_status("info", "")
        idx = self.index.value()
        self.app.run(w.address, coin, idx, done=lambda a: self._loaded(coin, idx, a),
                     failed=lambda e: self.set_status("bad", human(e)))

    def _loaded(self, coin, idx, a):
        if coin != self.pills.current() or idx != self.index.value():
            return
        self.full = a
        self.addr.setText(addr_block(a))
        pm = qr_pixmap(a, 236, coin_icon(coin, 48))
        if pm:
            self.qr.setPixmap(pm)
        else:
            self.qr.setText("pip install qrcode\nчтобы видеть QR-код")
            self.qr.setStyleSheet(f"color: {C['muted']}; border: 1px dashed {C['line']}; border-radius: 14px;")
        self.verify.setEnabled(True)
        self.copy.setEnabled(True)
        self.set_status("info", "Адрес ещё не проверен на устройстве")

    def do_verify(self):
        coin, idx = self.pills.current(), self.index.value()
        self.verify.setEnabled(False)
        self.set_status("wait", "Подтверди адрес на устройстве…")
        self.app.log(f"{coin}: проверка адреса #{idx} на устройстве")

        def done(a):
            self.verify.setEnabled(True)
            if a == self.full:
                self.set_status("ok", "✓  Адрес подтверждён на устройстве")
            else:
                self.set_status("bad", "Устройство показало другой адрес — не используй его!")

        def failed(e):
            self.verify.setEnabled(True)
            self.set_status("bad", human(e))

        self.app.run(self.app.wallet.show_address, coin, idx, done=done, failed=failed)


class SendPage(Page):
    def __init__(self, app):
        super().__init__(app, "Отправить", "Транзакция подписывается только на устройстве")
        self.stack = QStackedWidget()
        self.body.addWidget(self.stack, 1)
        self.draft = None
        self._form()
        self._review()
        self._signing()
        self._result()

    # ---- шаг 1: форма ----
    def _form(self):
        page = QWidget()
        v = QVBoxLayout(page)
        v.setContentsMargins(0, 0, 0, 0)
        v.setSpacing(16)
        self.pills = CoinPills()
        self.pills.changed.connect(self.coin_changed)
        v.addWidget(self.pills)
        c = card()
        f = QVBoxLayout(c)
        f.setContentsMargins(28, 24, 28, 24)
        f.setSpacing(10)

        asset_row = QHBoxLayout()
        self.asset_label = label("Актив", "muted")
        asset_row.addWidget(self.asset_label)
        self.asset = Segmented([])
        self.asset.changed.connect(lambda _: self.update_form())
        asset_row.addWidget(self.asset)
        asset_row.addStretch()
        f.addLayout(asset_row)

        f.addWidget(label("Получатель", "muted"))
        self.dest = QLineEdit()
        self.dest.setStyleSheet(f"font-family: {MONO};")
        self.dest.textChanged.connect(self.validate)
        f.addWidget(self.dest)
        self.dest_hint = label("", "faint")
        f.addWidget(self.dest_hint)

        f.addWidget(label("Сумма", "muted"))
        ar = QHBoxLayout()
        self.amount = QLineEdit()
        self.amount.setPlaceholderText("0.00")
        self.amount.setStyleSheet("font-size: 20px; font-weight: 700;")
        self.amount.textChanged.connect(self.validate)
        ar.addWidget(self.amount, 1)
        self.amount_sym = label("", "h2")
        self.amount_sym.setMinimumWidth(60)
        ar.addWidget(self.amount_sym)
        f.addLayout(ar)
        self.avail = label("", "faint")
        f.addWidget(self.avail)

        f.addSpacing(6)
        self.fee_title = label("Комиссия", "muted")
        f.addWidget(self.fee_title)
        self.fee_seg = Segmented([("slow", "Медленно"), ("normal", "Обычно"), ("fast", "Быстро")])
        self.fee_seg.btns["normal"].setChecked(True)
        f.addWidget(self.fee_seg, 0, Qt.AlignLeft)
        self.fee_limit = QLineEdit("40")
        self.fee_limit.setFixedWidth(120)
        f.addWidget(self.fee_limit, 0, Qt.AlignLeft)
        self.fee_info = label("", "faint", wrap=True)
        f.addWidget(self.fee_info)

        f.addSpacing(8)
        bottom = QHBoxLayout()
        self.dry = QCheckBox("Только подписать, не отправлять в сеть")
        bottom.addWidget(self.dry)
        bottom.addStretch()
        self.next_btn = button("Продолжить", "primary")
        self.next_btn.clicked.connect(self.prepare)
        bottom.addWidget(self.next_btn)
        f.addLayout(bottom)
        self.form_err = label("", wrap=True)
        self.form_err.setStyleSheet(f"color: {C['bad']};")
        f.addWidget(self.form_err)
        v.addWidget(c)
        v.addStretch()
        self.stack.addWidget(page)
        self.fee_levels = {}
        self.coin_changed("btc")

    def coin_changed(self, coin):
        assets = [(a, self.app.symbol(a)) for a, _ in W.COINS[coin]["assets"]]
        self.asset.set_items(assets)
        self.asset.setVisible(len(assets) > 1)
        self.asset_label.setVisible(len(assets) > 1)
        hints = {"btc": "tb1… или bc1…", "prl": "prl1p… (Taproot)", "tron": "T…", "bsc": "0x…"}
        self.dest.setPlaceholderText(f"Адрес {CHAINS[coin]['name']}: {hints[coin]}")
        self.fee_levels = {}
        self.update_form()
        self.validate()
        w = self.app.wallet
        if coin in ("btc", "prl") and w and w.ready:
            self.app.run(w.fee_levels, coin, done=lambda lv, coin=coin: self._fees(coin, lv),
                         failed=lambda e: None)

    def _fees(self, coin, lv):
        if coin != self.pills.current():
            return
        self.fee_levels = lv
        unit = "grain/vB" if coin == "prl" else "sat/vB"
        for k, t in (("slow", "Медленно"), ("normal", "Обычно"), ("fast", "Быстро")):
            self.fee_seg.set_text(k, f"{t}  ·  {lv[k]} {unit}")

    def update_form(self):
        coin, asset = self.pills.current(), self.asset.current()
        sym = self.app.symbol(asset or coin)
        self.amount_sym.setText(sym)
        utxo = coin in ("btc", "prl")
        self.fee_seg.setVisible(utxo)
        self.fee_limit.setVisible(coin == "tron" and asset == "usdt")
        if utxo:
            self.fee_title.setText("Скорость подтверждения")
            self.fee_info.setText("Комиссия считается по размеру транзакции и выбранной ставке")
        elif coin == "tron":
            self.fee_title.setText("Лимит комиссии, TRX" if asset == "usdt" else "Комиссия")
            self.fee_info.setText("Энергия оплачивается сжиганием TRX; это верхняя граница"
                                  if asset == "usdt" else "Обычно ~0.3 TRX за bandwidth; новому адресу +1 TRX")
        else:
            self.fee_title.setText("Комиссия")
            self.fee_info.setText(f"Цена газа — по сети, оплачивается в {self.app.symbol('bnb')}")
        bal = self.app.overview.balances.get(coin)
        a = next((x for x in bal["assets"] if x["id"] == (asset or coin)), None) if bal else None
        self.avail.setText(f"Доступно: {W.fmt_units(a['amount'], a['decimals'])} {a['symbol']}" if a else "")

    def validate(self):
        coin, cfg = self.pills.current(), self.app.cfg()
        text = self.dest.text().strip()
        ok = False
        if not text:
            self.dest_hint.setText("")
        else:
            try:
                {"btc": lambda: W.address_to_script(text, cfg["hrp"]),
                 "prl": lambda: W.address_to_script(text, cfg["prl_hrp"], True),
                 "tron": lambda: W.tron_decode(text), "bsc": lambda: W.evm_decode(text)}[coin]()
                self.dest_hint.setText("✓  Адрес корректный")
                self.dest_hint.setStyleSheet(f"color: {C['ok']}; font-size: 12px;")
                ok = True
            except W.WalletError as e:
                self.dest_hint.setText(str(e))
                self.dest_hint.setStyleSheet(f"color: {C['bad']}; font-size: 12px;")
        try:
            amount_ok = Decimal(self.amount.text().replace(",", ".")) > 0
        except Exception:
            amount_ok = False
        w = self.app.wallet
        self.next_btn.setEnabled(ok and amount_ok and bool(w and w.ready))

    def prepare(self):
        coin, asset, w = self.pills.current(), self.asset.current(), self.app.wallet
        self.form_err.setText("")
        self.next_btn.setEnabled(False)
        self.next_btn.setText("Собираю транзакцию…")
        kw = {"asset": asset}
        if coin in ("btc", "prl"):
            kw["feerate"] = self.fee_levels.get(self.fee_seg.current())
        if coin == "tron" and asset == "usdt":
            kw["fee_limit"] = self.fee_limit.text().strip() or "40"
        amount = self.amount.text().strip().replace(",", ".")

        def done(d):
            self.next_btn.setText("Продолжить")
            self.validate()
            self.draft = d
            self.show_review()

        def failed(e):
            self.next_btn.setText("Продолжить")
            self.validate()
            self.form_err.setText(human(e))

        self.app.run(lambda: w.prepare_send(coin, self.dest.text(), amount, **kw), done=done, failed=failed)

    # ---- шаг 2: проверка ----
    def _review(self):
        page = QWidget()
        v = QVBoxLayout(page)
        v.setContentsMargins(0, 0, 0, 0)
        c = card()
        self.rv = QVBoxLayout(c)
        self.rv.setContentsMargins(28, 24, 28, 24)
        self.rv.setSpacing(0)
        v.addWidget(c)
        acts = QHBoxLayout()
        back = button("Назад")
        back.clicked.connect(lambda: self.stack.setCurrentIndex(0))
        acts.addWidget(back)
        acts.addStretch()
        self.sign_btn = button("Подписать на устройстве", "primary", line_icon("device", "white"))
        self.sign_btn.clicked.connect(self.sign)
        acts.addWidget(self.sign_btn)
        v.addSpacing(16)
        v.addLayout(acts)
        v.addStretch()
        self.stack.addWidget(page)

    def show_review(self):
        d, lay = self.draft, self.rv
        while lay.count():
            it = lay.takeAt(0)
            if it.widget():
                it.widget().deleteLater()
        top = QHBoxLayout()
        ic = QLabel()
        ic.setPixmap(coin_icon("usdt" if d.asset == "usdt" else d.coin, 44))
        top_w = QWidget()
        top_w.setLayout(top)
        top.setContentsMargins(0, 0, 0, 16)
        top.addWidget(ic)
        tv = QVBoxLayout()
        tv.setSpacing(0)
        tv.addWidget(label(f"{d.amount} {d.symbol}", "h1"))
        tv.addWidget(label(f"{CHAINS[d.coin]['name']} · {CHAINS[d.coin]['net'][self.app.wallet.net]}", "muted"))
        top.addLayout(tv)
        top.addStretch()
        if self.dry.isChecked():
            top.addWidget(badge("БЕЗ ОТПРАВКИ", C["warn"]))
        lay.addWidget(top_w)
        rows = [("Откуда", d.from_addr, True), ("Куда", d.to, True),
                ("Сумма", f"{d.amount} {d.symbol}", False),
                (d.fee_label, f"{d.fee} {d.fee_symbol}", False)]
        for k, val, mono in rows:
            r = QFrame()
            r.setStyleSheet(f"border-top: 1px solid {C['line']};")
            h = QHBoxLayout(r)
            h.setContentsMargins(0, 12, 0, 12)
            kl = label(k, "muted")
            kl.setStyleSheet("border: none;")
            kl.setFixedWidth(160)
            h.addWidget(kl, 0, Qt.AlignTop)
            vl = label(val, wrap=True, sel=True)
            vl.setStyleSheet("border: none;" + (f"font-family: {MONO};" if mono else "font-weight: 600;"))
            h.addWidget(vl, 1)
            lay.addWidget(r)
        for n in d.notes:
            nl = label(n, "faint", wrap=True)
            lay.addWidget(nl)
        self.sign_btn.setEnabled(True)
        self.stack.setCurrentIndex(1)

    # ---- шаг 3: подпись ----
    def _signing(self):
        page = QWidget()
        v = QVBoxLayout(page)
        c = card()
        cv = QVBoxLayout(c)
        cv.setContentsMargins(40, 48, 40, 48)
        cv.setSpacing(14)
        cv.setAlignment(Qt.AlignHCenter)
        sp = Spinner(64)
        cv.addWidget(sp, 0, Qt.AlignHCenter)
        cv.addWidget(label("Подтверди на устройстве", "h1"), 0, Qt.AlignHCenter)
        steps = label("Коротким нажатием пролистай сумму, адрес и комиссию.\n"
                      "Если всё верно — удерживай кнопку на «Accept».\n"
                      "Чтобы отказаться — удерживай на «Reject».", "muted")
        steps.setAlignment(Qt.AlignCenter)
        cv.addWidget(steps, 0, Qt.AlignHCenter)
        v.addWidget(c)
        v.addStretch()
        self.stack.addWidget(page)

    def sign(self):
        self.sign_btn.setEnabled(False)
        self.stack.setCurrentIndex(2)
        d = self.draft
        self.app.log(f"{d.coin}: подпись {d.amount} {d.symbol} → {short(d.to)}")
        self.app.run(self.app.wallet.sign_and_send, d, not self.dry.isChecked(),
                     done=self.signed, failed=self.sign_failed)

    # ---- шаг 4: результат ----
    def _result(self):
        page = QWidget()
        v = QVBoxLayout(page)
        c = card()
        cv = QVBoxLayout(c)
        cv.setContentsMargins(40, 40, 40, 40)
        cv.setSpacing(12)
        cv.setAlignment(Qt.AlignHCenter)
        self.res_icon = QLabel()
        cv.addWidget(self.res_icon, 0, Qt.AlignHCenter)
        self.res_title = label("", "h1")
        cv.addWidget(self.res_title, 0, Qt.AlignHCenter)
        self.res_text = label("", "muted", wrap=True, sel=True)
        self.res_text.setAlignment(Qt.AlignCenter)
        self.res_text.setFixedWidth(560)  # с переносом QLabel иначе сам выбирает узкую ширину
        cv.addWidget(self.res_text, 0, Qt.AlignHCenter)
        self.res_txid = label("", "mono")
        self.res_txid.setAlignment(Qt.AlignCenter)
        self.res_txid.setStyleSheet(f"font-family: {MONO}; font-size: 13px; color: {C['muted']};"
                                    f"background: {C['card2']}; border-radius: 10px; padding: 10px 14px;")
        cv.addWidget(self.res_txid, 0, Qt.AlignHCenter)
        acts = QHBoxLayout()
        acts.addStretch()
        self.open_btn = button("Открыть в обозревателе", None, line_icon("external"))
        self.open_btn.clicked.connect(lambda: QDesktopServices.openUrl(QUrl(self.res_url)))
        self.copy_btn = button("Скопировать", None, line_icon("copy"))
        self.copy_btn.clicked.connect(lambda: self.app.copy(self.res_copy, "Скопировано"))
        again = button("Новый перевод", "primary")
        again.clicked.connect(self.reset)
        for b in (self.open_btn, self.copy_btn, again):
            acts.addWidget(b)
        acts.addStretch()
        cv.addSpacing(8)
        cv.addLayout(acts)
        v.addWidget(c)
        v.addStretch()
        self.stack.addWidget(page)
        self.res_url = self.res_copy = ""

    def _result_icon(self, ok):
        pm = hidpi_pixmap(72)
        p = QPainter(pm)
        p.setRenderHint(QPainter.Antialiasing)
        col = QColor(C["ok"] if ok else C["bad"])
        p.setBrush(QColor(col.red(), col.green(), col.blue(), 40))
        p.setPen(Qt.NoPen)
        p.drawEllipse(QRectF(0, 0, 72, 72))
        p.drawPixmap(QRectF(18, 18, 36, 36), line_icon("check" if ok else "cross", col.name(), 36).pixmap(72, 72),
                     QRectF(0, 0, 72, 72))
        p.end()
        self.res_icon.setPixmap(pm)

    def signed(self, r):
        self._result_icon(True)
        d = self.draft
        if r["broadcast"]:
            self.res_title.setText("Отправлено")
            self.res_text.setText(f"{d.amount} {d.symbol} → {short(d.to, 12)}")
            self.res_txid.setText("TXID\n" + addr_block(r["txid"]))
            self.res_txid.show()
            self.res_url, self.res_copy = r["url"], r["txid"]
            self.app.log(f"{d.coin}: отправлено, txid {r['txid']}")
        else:
            self.res_title.setText("Подписано")
            self.res_text.setText("Транзакция подписана, но не отправлена.\nСырую транзакцию можно "
                                  "скопировать кнопкой «Скопировать».")
            self.res_txid.setText("TXID\n" + addr_block(r["txid"]))
            self.res_txid.show()
            self.res_url, self.res_copy = "", r["raw"]
            self.app.log(f"{d.coin}: подписано без отправки, txid {r['txid']}")
        self.open_btn.setVisible(bool(self.res_url))
        self.copy_btn.setVisible(True)
        self.stack.setCurrentIndex(3)
        QTimer.singleShot(4000, self.app.overview.refresh)

    def sign_failed(self, e):
        self._result_icon(False)
        self.res_title.setText("Не отправлено")
        self.res_text.setText(human(e))
        self.res_txid.hide()
        self.app.log(f"ошибка: {human(e)}")
        self.open_btn.hide()
        self.copy_btn.hide()
        self.stack.setCurrentIndex(3)

    def reset(self):
        self.dest.clear()
        self.amount.clear()
        self.stack.setCurrentIndex(0)

    def wallet_changed(self):
        self.coin_changed(self.pills.current())


class DevicePage(Page):
    def __init__(self, app):
        super().__init__(app, "Устройство", "Состояние MicroWallet и служебные действия")
        grid = QVBoxLayout()
        grid.setSpacing(16)
        info = card()
        iv = QVBoxLayout(info)
        iv.setContentsMargins(24, 22, 24, 22)
        iv.setSpacing(10)
        top = QHBoxLayout()
        ic = QLabel()
        ic.setPixmap(coin_icon("logo", 44))
        top.addWidget(ic)
        tv = QVBoxLayout()
        tv.setSpacing(0)
        tv.addWidget(label("MicroWallet", "h2"))
        self.sub = label("", "muted")
        tv.addWidget(self.sub)
        top.addLayout(tv)
        top.addStretch()
        self.net_badge = QLabel()
        top.addWidget(self.net_badge)
        iv.addLayout(top)
        iv.addSpacing(6)
        self.kv = QGridLayout()
        self.kv.setVerticalSpacing(10)
        self.kv.setHorizontalSpacing(24)
        self.kv.setColumnMinimumWidth(0, 110)
        self.kv.setColumnStretch(1, 1)
        iv.addLayout(self.kv)
        iv.addStretch()
        acts = QHBoxLayout()
        self.reconnect = button("USB", None, line_icon("refresh"))
        self.reconnect.setToolTip("Подключиться (заново) по USB")
        self.reconnect.clicked.connect(self.app.connect_device)
        self.bt = button("Bluetooth…")
        self.bt.clicked.connect(lambda: BleDialog(self.app).exec())
        self.update_btn = button("Обновить прошивку…")
        self.update_btn.setToolTip("По USB или Bluetooth: устройство покажет хеш и попросит подтверждение")
        self.update_btn.clicked.connect(self.do_update)
        self.boot = button("Режим прошивки")
        self.boot.setToolTip("Перезагрузить в загрузчик ROM — для прошивки через esptool")
        self.boot.clicked.connect(self.do_boot)
        self.wipe = button("Стереть", "danger")
        self.wipe.clicked.connect(self.do_wipe)
        for b in (self.reconnect, self.bt, self.update_btn, self.boot):
            acts.addWidget(b)
        acts.addStretch()
        acts.addWidget(self.wipe)
        iv.addLayout(acts)
        grid.addWidget(info)

        logc = card()
        lv = QVBoxLayout(logc)
        lv.setContentsMargins(20, 18, 20, 18)
        lv.addWidget(label("Журнал", "h2"))
        self.logw = QListWidget()
        self.logw.setWordWrap(True)
        self.logw.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff)
        self.logw.setMinimumHeight(140)
        lv.addWidget(self.logw)
        grid.addWidget(logc)
        self.body.addLayout(grid)

        tips = card()
        tl = QVBoxLayout(tips)
        tl.setContentsMargins(24, 18, 24, 18)
        tl.addWidget(label("Управление на устройстве", "h2"))
        tl.addWidget(label("Короткое нажатие — следующий экран. Удержание 0.75 с — выбрать "
                           "(внизу экрана растёт полоска). В главном меню: монеты → Receive / Info / "
                           "Back, в Settings — яркость, сон, аккумулятор, Bluetooth и сброс.", "muted", wrap=True))
        self.body.addWidget(tips)
        self.body.addStretch()

    def add_log(self, text):
        self.logw.insertItem(0, f"{time.strftime('%H:%M:%S')}  {text}")

    def wallet_changed(self):
        w = self.app.wallet
        while self.kv.count():
            self.kv.takeAt(0).widget().deleteLater()
        if not w:
            self.sub.setText(self.app.conn_error or "не подключено")
            self.net_badge.clear()
            for b in (self.boot, self.wipe, self.update_btn):
                b.setEnabled(False)
            return
        i = w.info
        self.sub.setText(f"прошивка {i.get('fw', '?')}  ·  {w.dev.port}")
        mainnet = w.net == "mainnet"
        self.net_badge.setText(w.net.upper())
        self.net_badge.setObjectName("badge")
        col = C["bad"] if mainnet else C["warn"]
        self.net_badge.setStyleSheet(f"background: {tint(col)}; color: {col}; border-radius: 9px; "
                                     "padding: 3px 10px; font-size: 12px; font-weight: 700;")
        hw = i.get("hw", "?")
        ble_link = B is not None and isinstance(w.dev, B.BleDevice)
        rows = [("Плата", {"esp32s3": "ESP32-S3 (аппаратный ГСЧ, Bluetooth, зарядка на плате)"}.get(hw, hw)),
                ("Связь", "Bluetooth, шифрованный канал" if ble_link else "USB"),
                *([("Аккумулятор", "заряжается от USB")] if i.get("chg") == "1" else
                  [("Аккумулятор", battery_text(int(i["batt"])))] if int(i.get("batt", -1)) > 0 else []),
                ("Кошелёк", "создан" if w.ready else "нет — создай на устройстве или восстанови"),
                ("Сети", ", ".join(CHAINS[c]["name"] for c in ORDER if c in w.coins)),
                ("Seed", "в NVS, без шифрования в обычной сборке" + (" — держи только мелкие суммы" if mainnet else ""))]
        for r, (k, v) in enumerate(rows):
            self.kv.addWidget(label(k, "muted"), r, 0)
            self.kv.addWidget(label(v, wrap=True), r, 1)
        real = not isinstance(w, DemoWallet)
        self.boot.setEnabled(real and not ble_link)
        self.wipe.setEnabled(real)
        self.update_btn.setEnabled(real and hw.startswith("esp32"))

    def do_boot(self):
        self.app.run(self.app.wallet.dev.call, "boot", done=lambda _: self.app.disconnected(
            "в режиме загрузчика — прошивай через esptool"), failed=lambda e: self.app.toast(human(e)))

    def do_update(self):
        path, _ = QFileDialog.getOpenFileName(self, "Прошивка ESP32", str(Path.home()),
                                              "Образ приложения (*.bin)")
        if path:
            try:
                UpdateDialog(self.app, path).exec()
            except OSError as e:
                self.app.toast(f"Не удалось прочитать файл: {e}")

    def do_wipe(self):
        m = QMessageBox(self)
        m.setWindowTitle("Стереть устройство?")
        m.setText("Seed будет удалён с устройства. Без записанных 12 слов средства потеряются "
                  "навсегда.\n\nПодтверждение потребуется и на самом устройстве.")
        m.setStandardButtons(QMessageBox.Cancel | QMessageBox.Yes)
        m.button(QMessageBox.Yes).setText("Стереть")
        m.button(QMessageBox.Cancel).setText("Отмена")
        if m.exec() != QMessageBox.Yes:
            return
        self.app.toast("Подтверди сброс на устройстве: удерживай кнопку на «Wipe»")
        self.app.run(self.app.wallet.dev.call, "wipe", done=lambda _: self.app.connect_device(),
                     failed=lambda e: self.app.toast(human(e)))


# ---------------- диалоги ----------------

class Dialog(QDialog):
    def __init__(self, parent, title, subtitle=""):
        super().__init__(parent)
        self.setWindowTitle(title)
        self.setModal(True)
        self.setStyleSheet(f"QDialog {{ background: {C['card']}; }}")
        self.v = QVBoxLayout(self)
        self.v.setContentsMargins(28, 24, 28, 22)
        self.v.setSpacing(12)
        self.v.addWidget(label(title, "h1"))
        if subtitle:
            self.v.addWidget(label(subtitle, "muted", wrap=True))


class RestoreDialog(Dialog):
    """Ввод seed-фразы для пустого устройства: подсказки по словарю BIP39 и проверка
    контрольной суммы. Слова уходят на устройство один раз и в окне не хранятся."""

    def __init__(self, app):
        super().__init__(app, "Восстановить из seed-фразы",
                         "Введи слова в правильном порядке. Устройство покажет первое и последнее "
                         "слово — сверь их и подтверди кнопкой.")
        self.app = app
        self.setMinimumWidth(720)
        warn = card("warnbox")
        wl = QHBoxLayout(warn)
        wl.setContentsMargins(16, 12, 16, 12)
        wl.addWidget(label("⚠  Компьютер увидит seed-фразу. Делай это только на доверенном ПК, "
                           "без лишних программ. Надёжнее — создать новый кошелёк на устройстве.",
                           wrap=True))
        self.v.addWidget(warn)

        row = QHBoxLayout()
        row.addWidget(label("Количество слов", "muted"))
        self.count = Segmented([("12", "12"), ("18", "18"), ("24", "24")])
        self.count.changed.connect(lambda k: self.build(int(k)))
        row.addWidget(self.count)
        row.addStretch()
        self.v.addLayout(row)

        self.grid_w = QWidget()
        self.grid = QGridLayout(self.grid_w)
        self.grid.setContentsMargins(0, 0, 0, 0)
        self.grid.setHorizontalSpacing(10)
        self.grid.setVerticalSpacing(8)
        self.v.addWidget(self.grid_w)
        self.completer_words = W.bip39_words()
        self.fields = []

        self.status = label("", wrap=True)
        self.v.addWidget(self.status)
        acts = QHBoxLayout()
        cancel = button("Отмена")
        cancel.clicked.connect(self.reject)
        acts.addWidget(cancel)
        acts.addStretch()
        self.go = button("Восстановить", "primary", line_icon("device", "white"))
        self.go.clicked.connect(self.submit)
        acts.addWidget(self.go)
        self.v.addLayout(acts)
        self.build(12)

    def build(self, n):
        for f in self.fields:
            self.grid.removeWidget(f)
            f.setParent(None)
            f.deleteLater()
        while self.grid.count():
            w = self.grid.takeAt(0).widget()
            if w:
                w.setParent(None)
        self.fields = []
        cols = 3
        rows = (n + cols - 1) // cols
        for i in range(n):
            r, c = i % rows, i // rows  # нумерация по столбцам, как на бумаге
            num = label(f"{i + 1:>2}", "faint")
            num.setFixedWidth(22)
            f = QLineEdit()
            f.setStyleSheet(f"font-family: {MONO};")
            comp = QCompleter(self.completer_words, f)
            comp.setCaseSensitivity(Qt.CaseInsensitive)
            comp.setCompletionMode(QCompleter.InlineCompletion)
            f.setCompleter(comp)
            f.textChanged.connect(lambda t, i=i: self.changed(i, t))
            self.grid.addWidget(num, r, c * 2)
            self.grid.addWidget(f, r, c * 2 + 1)
            self.fields.append(f)
        self.validate()

    def changed(self, i, text):
        words = text.split()
        if len(words) > 1:  # вставили фразу целиком — разложить по полям
            if len(words) in (12, 18, 24) and len(words) != len(self.fields):
                self.count.btns[str(len(words))].setChecked(True)
                self.build(len(words))
                i = 0
            for k, w in enumerate(words[: len(self.fields) - i]):
                self.fields[i + k].blockSignals(True)
                self.fields[i + k].setText(w.lower())
                self.fields[i + k].blockSignals(False)
        self.validate()

    def phrase(self):
        return " ".join(f.text().strip().lower() for f in self.fields)

    def validate(self):
        words = [f.text().strip().lower() for f in self.fields]
        known = set(self.completer_words)
        for f, w in zip(self.fields, words):
            bad = bool(w) and w not in known
            f.setStyleSheet(f"font-family: {MONO};" + (f"border: 1px solid {C['bad']};" if bad else ""))
        ok = False
        if not all(words):
            self.set_status("info", f"Заполнено {sum(map(bool, words))} из {len(words)}")
        else:
            try:
                W.mnemonic_to_entropy(" ".join(words))
                self.set_status("ok", "✓  Фраза корректна (контрольная сумма сошлась)")
                ok = True
            except W.WalletError as e:
                self.set_status("bad", str(e))
        self.go.setEnabled(ok)

    def set_status(self, kind, text):
        color = {"ok": C["ok"], "bad": C["bad"], "wait": C["warn"], "info": C["muted"]}[kind]
        self.status.setText(text)
        self.status.setStyleSheet(f"color: {color}; font-weight: 600;")

    def submit(self):
        phrase = self.phrase()
        for f in self.fields:
            f.setEnabled(False)
        self.go.setEnabled(False)
        self.set_status("wait", "Подтверди на устройстве: сверь первое и последнее слово и удерживай кнопку на «Restore»…")
        self.app.log("восстановление из seed-фразы: ждём подтверждения на устройстве")

        def done(_):
            self.app.log("кошелёк восстановлен из seed-фразы")
            self.wipe()
            self.accept()
            self.app.wallet_updated()

        def failed(e):
            for f in self.fields:
                f.setEnabled(True)
            self.validate()
            self.set_status("bad", human(e))

        self.app.run(self.app.wallet.restore, phrase, done=done, failed=failed)

    def wipe(self):
        for f in self.fields:
            f.clear()

    def reject(self):
        self.wipe()
        super().reject()


class BleDialog(Dialog):
    def __init__(self, app):
        super().__init__(app, "Подключение по Bluetooth",
                         "Связь шифруется; при первом "
                         "подключении сверь код на экране устройства и здесь.")
        self.app = app
        self.setMinimumWidth(520)
        self.list = QListWidget()
        self.list.setStyleSheet(f"QListWidget {{ font-family: {MONO}; font-size: 13px; color: {C['text']};"
                                f" background: {C['card2']}; border-radius: 12px; padding: 6px; }}"
                                f"QListWidget::item {{ padding: 10px; border-radius: 8px; }}"
                                f"QListWidget::item:selected {{ background: #2A2B3C; }}")
        self.list.setMinimumHeight(180)
        self.v.addWidget(self.list)
        self.status = label("", "muted", wrap=True)
        self.v.addWidget(self.status)
        acts = QHBoxLayout()
        self.scan_btn = button("Искать", None, line_icon("refresh"))
        self.scan_btn.clicked.connect(self.scan)
        acts.addWidget(self.scan_btn)
        acts.addStretch()
        cancel = button("Отмена")
        cancel.clicked.connect(self.reject)
        acts.addWidget(cancel)
        self.go = button("Подключить", "primary")
        self.go.clicked.connect(self.connect_selected)
        self.go.setEnabled(False)
        acts.addWidget(self.go)
        self.v.addLayout(acts)
        self.list.itemSelectionChanged.connect(lambda: self.go.setEnabled(bool(self.list.selectedItems())))
        self.list.itemDoubleClicked.connect(lambda _: self.connect_selected())
        self.scan()

    def scan(self):
        self.list.clear()
        self.scan_btn.setEnabled(False)
        self.status.setText("Ищу устройства… (на MicroWallet должен быть включён Bluetooth)")

        def done(found):
            self.scan_btn.setEnabled(True)
            for addr, name, rssi in sorted(found, key=lambda x: -(x[2] or -999)):
                it = QListWidgetItem(f"{name}    {addr}    {rssi} dBm")
                it.setData(Qt.UserRole, (addr, name))
                self.list.addItem(it)
            self.status.setText(f"Найдено: {len(found)}" if found else "Ничего не найдено. Проверь, что "
                                "Bluetooth включён (Settings → Bluetooth) и рядом нет другого подключения.")

        def failed(e):
            self.scan_btn.setEnabled(True)
            self.status.setText(human(e))

        if not B:
            failed(W.WalletError("Нужен пакет cryptography: pip install cryptography bleak"))
            return
        self.app.run(B.scan, done=done, failed=failed)

    def connect_selected(self):
        items = self.list.selectedItems()
        if items:
            addr, name = items[0].data(Qt.UserRole)
            self.accept()
            self.app.connect_ble(addr, name)


class PairDialog(Dialog):
    def __init__(self, app, code):
        super().__init__(app, "Сопряжение с устройством",
                         "Сверь код с экраном MicroWallet. Если совпадает — удерживай кнопку на "
                         "«Pair». Если нет — удерживай на «Reject»: кто-то вмешивается в связь.")
        self.setMinimumWidth(460)
        code_l = label(code, "codebox")
        code_l.setAlignment(Qt.AlignCenter)
        self.v.addWidget(code_l)
        row = QHBoxLayout()
        row.addWidget(Spinner(22))
        row.addWidget(label("Жду подтверждения на устройстве…", "muted"))
        row.addStretch()
        self.v.addLayout(row)


class UpdateDialog(Dialog):
    def __init__(self, app, path):
        self.image = Path(path).read_bytes()
        digest = hashlib.sha256(self.image).hexdigest()
        super().__init__(app, "Обновление прошивки",
                         "Устройство покажет размер и SHA-256 — сверь их со значениями ниже и "
                         "удерживай кнопку на «Install».")
        self.app = app
        self.setMinimumWidth(560)
        kv = QGridLayout()
        kv.addWidget(label("Файл", "muted"), 0, 0)
        kv.addWidget(label(Path(path).name), 0, 1)
        kv.addWidget(label("Размер", "muted"), 1, 0)
        kv.addWidget(label(f"{len(self.image)} bytes"), 1, 1)
        kv.addWidget(label("SHA-256", "muted"), 2, 0, Qt.AlignTop)
        h = label(addr_block(digest, 32), "mono")
        h.setStyleSheet(f"font-family: {MONO}; font-size: 13px;")
        kv.addWidget(h, 2, 1)
        self.v.addLayout(kv)
        self.bar = QProgressBar()
        self.bar.setRange(0, 100)
        self.bar.setValue(0)
        self.v.addWidget(self.bar)
        self.status = label("", "muted", wrap=True)
        self.v.addWidget(self.status)
        acts = QHBoxLayout()
        acts.addStretch()
        self.close_btn = button("Отмена")
        self.close_btn.clicked.connect(self.reject)
        acts.addWidget(self.close_btn)
        self.go = button("Обновить", "primary")
        self.go.clicked.connect(self.start)
        acts.addWidget(self.go)
        self.v.addLayout(acts)

    progress_sig = Signal(int)

    def start(self):
        self.go.setEnabled(False)
        self.close_btn.setEnabled(False)
        self.status.setText("Подтверди обновление на устройстве…")
        self.progress_sig.connect(self.bar.setValue, Qt.QueuedConnection)

        def run():
            return self.app.wallet.update_firmware(
                self.image, lambda d, t: self.progress_sig.emit(d * 100 // t))

        def done(_):
            self.status.setText("Готово: устройство перезагружается с новой прошивкой.")
            self.close_btn.setText("Закрыть")
            self.close_btn.setEnabled(True)
            self.app.log("прошивка обновлена")
            self.app.disconnected("перезагрузка после обновления")

        def failed(e):
            self.status.setText(human(e))
            self.close_btn.setEnabled(True)
            self.go.setEnabled(True)

        self.app.run(run, done=done, failed=failed)


# ---------------- главное окно ----------------

class App(QMainWindow):
    balances_changed = Signal()
    pair_code = Signal(str)

    def __init__(self, demo=False, port=None, demo_empty=False):
        super().__init__()
        self.demo, self.port, self.demo_empty = demo, port, demo_empty
        self.ble_link = False
        self.pair_dialog = None
        self.pair_code.connect(self._show_pair_code)
        self.wallet, self.conn_error = None, ""
        self.pool = QThreadPool.globalInstance()
        self.tasks = set()
        self.setWindowTitle("MicroWallet")
        self.setWindowIcon(QIcon(coin_icon("logo", 64)))
        self.resize(1180, 760)
        self.setMinimumSize(1000, 660)

        root = QWidget()
        h = QHBoxLayout(root)
        h.setContentsMargins(0, 0, 0, 0)
        h.setSpacing(0)
        side = QFrame()
        side.setObjectName("sidebar")
        side.setFixedWidth(232)
        sv = QVBoxLayout(side)
        sv.setContentsMargins(16, 22, 16, 18)
        sv.setSpacing(4)
        brand = QHBoxLayout()
        logo = QLabel()
        logo.setPixmap(coin_icon("logo", 32))
        brand.addWidget(logo)
        brand.addWidget(label("MicroWallet", "brand"))
        brand.addStretch()
        sv.addLayout(brand)
        sv.addSpacing(22)

        self.stack = QStackedWidget()
        self.stack.setObjectName("content")
        self.overview = OverviewPage(self)
        self.send = SendPage(self)
        self.receive = ReceivePage(self)
        self.device = DevicePage(self)
        self.pages = [self.overview, self.send, self.receive, self.device]
        self.nav = QButtonGroup(self)
        for i, (name, icon) in enumerate((("Обзор", "overview"), ("Отправить", "send"),
                                          ("Получить", "receive"), ("Устройство", "device"))):
            b = QPushButton("  " + name)
            b.setObjectName("nav")
            b.setCheckable(True)
            b.setIcon(line_icon(icon, C["muted"]))
            b.setIconSize(QSize(18, 18))
            b.setCursor(Qt.PointingHandCursor)
            b.clicked.connect(lambda _=False, i=i: self.stack.setCurrentIndex(i))
            self.nav.addButton(b, i)
            sv.addWidget(b)
            scroll = QScrollArea()
            scroll.setWidgetResizable(True)
            scroll.setWidget(self.pages[i])
            self.stack.addWidget(scroll)
        self.nav.button(0).setChecked(True)
        sv.addStretch()

        status = card("inner")
        st = QVBoxLayout(status)
        st.setContentsMargins(14, 12, 14, 12)
        st.setSpacing(4)
        r1 = QHBoxLayout()
        self.dot = Dot()
        r1.addWidget(self.dot)
        self.st_title = label("Нет устройства")
        self.st_title.setStyleSheet("font-weight: 700;")
        r1.addWidget(self.st_title)
        r1.addStretch()
        st.addLayout(r1)
        self.st_sub = label("", "faint", wrap=True)
        st.addWidget(self.st_sub)
        sv.addWidget(status)

        h.addWidget(side)
        h.addWidget(self.stack, 1)
        self.setCentralWidget(root)

        self.toast_lbl = QLabel(self)
        self.toast_lbl.setStyleSheet(f"background: #23243A; border: 1px solid {C['accent']}; border-radius: 10px;"
                                     " padding: 10px 16px; font-weight: 600;")
        self.toast_lbl.hide()
        self.toast_timer = QTimer(self, singleShot=True, timeout=self.toast_lbl.hide)

        self.retry = QTimer(self, interval=3000, timeout=self._autoconnect)
        self.connect_device()

    # ---- инфраструктура ----

    def run(self, fn, *args, done=None, failed=None):
        t = Task(fn, *args)
        self.tasks.add(t)

        def fin(cb, v):
            self.tasks.discard(t)
            if isinstance(v, W.DeviceError) and "потеряна" in str(v):
                self.disconnected(str(v))
            elif B and isinstance(v, B.ChannelError) and self.wallet and isinstance(self.wallet.dev, B.BleDevice):
                self.disconnected(f"Bluetooth: {v}")
            if cb:
                cb(v)

        # результат приходит из рабочего потока — обрабатываем в главном
        t.sig.done.connect(lambda v: fin(done, v), Qt.QueuedConnection)
        t.sig.failed.connect(lambda e: fin(failed, e), Qt.QueuedConnection)
        self.pool.start(t)

    def cfg(self):
        return self.wallet.cfg if self.wallet else W.NETS["testnet"]

    def symbol(self, asset):
        return W.asset_symbol(self.cfg(), asset)

    def copy(self, text, msg):
        if text:
            QGuiApplication.clipboard().setText(text)
            self.toast(msg)

    def toast(self, msg):
        self.toast_lbl.setText(msg)
        self.toast_lbl.adjustSize()
        self.toast_lbl.move(self.width() - self.toast_lbl.width() - 28, self.height() - self.toast_lbl.height() - 24)
        self.toast_lbl.show()
        self.toast_lbl.raise_()
        self.toast_timer.start(3500)

    def log(self, text):
        self.device.add_log(text)

    def goto_send(self, coin):
        self.send.stack.setCurrentIndex(0)
        self.send.pills.set(coin)
        self.nav.button(1).click()

    def goto_receive(self, coin):
        self.receive.pills.set(coin)
        self.nav.button(2).click()

    def closeEvent(self, e):
        self.retry.stop()
        if self.wallet and not self.demo:
            self.wallet.dev.close()
        super().closeEvent(e)

    # ---- подключение ----

    def connect_device(self):
        if self.demo:
            self.set_wallet(DemoWallet(self.demo_empty))
            return
        if self.wallet:
            self.wallet.dev.close()
            self.wallet = None
        self.ble_link = False
        self.st_title.setText("Подключение…")

        def connect():
            dev = W.Device(self.port)
            try:
                return W.Wallet(dev)
            except Exception:
                dev.close()
                raise

        self.run(connect, done=self.set_wallet, failed=lambda e: self.disconnected(human(e)))

    def connect_ble(self, address, name="MicroWallet"):
        if self.wallet and not self.demo:
            self.wallet.dev.close()
        self.wallet, self.ble_link = None, True
        self.retry.stop()
        self.st_title.setText("Bluetooth…")
        self.log(f"Bluetooth: подключаюсь к {name} ({address})")

        def connect():
            dev = B.BleDevice(address, name, on_code=self.pair_code.emit)
            try:
                return W.Wallet(dev)
            except Exception:
                dev.close()
                raise

        def done(w):
            self._close_pair()
            if w.dev.ch.paired_now:
                self.log("Bluetooth: ПК сопряжён с устройством")
            self.set_wallet(w)

        def failed(e):
            self._close_pair()
            self.ble_link = False
            self.disconnected(human(e))

        self.run(connect, done=done, failed=failed)

    def _show_pair_code(self, code):
        self._close_pair()
        self.pair_dialog = PairDialog(self, code)
        self.pair_dialog.show()

    def _close_pair(self):
        if self.pair_dialog:
            self.pair_dialog.close()
            self.pair_dialog = None

    def wallet_updated(self):
        """Состояние кошелька изменилось (например, после восстановления)."""
        if self.wallet:
            self.set_wallet(self.wallet)

    def _autoconnect(self):
        if self.ble_link:
            return
        if not self.wallet and (self.port or W.find_ports()):
            self.retry.stop()
            self.connect_device()

    def set_wallet(self, w):
        self.wallet, self.conn_error = w, ""
        mainnet = w.net == "mainnet"
        self.dot.set(C["ok"] if w.ready else C["warn"])
        self.st_title.setText("Подключено" if w.ready else "Нет кошелька")
        self.st_sub.setText(f"{'ДЕМО · ' if self.demo else ''}{w.net}  ·  v{w.info.get('fw', '?')}")
        if mainnet:
            self.st_sub.setStyleSheet(f"color: {C['bad']}; font-size: 12px;")
        self.log(f"подключено: {w.dev.port}, {w.net}, прошивка {w.info.get('fw')}, плата {w.info.get('hw', '?')}")
        for p in self.pages:
            p.wallet_changed()

    def disconnected(self, why):
        if self.wallet and not self.demo:
            self.wallet.dev.close()
        self.wallet, self.conn_error = None, why
        self.dot.set(C["faint"])
        self.st_title.setText("Нет устройства")
        self.st_sub.setText(why)
        self.log(f"отключено: {why}")
        for p in self.pages:
            p.wallet_changed()
        if not self.demo and not self.ble_link:
            self.retry.start()
        self.ble_link = False


def main():
    ap = argparse.ArgumentParser(description="MicroWallet — настольное приложение")
    ap.add_argument("--demo", action="store_true", help="без устройства, на тестовых данных")
    ap.add_argument("--demo-empty", action="store_true", help="демо с пустым устройством (восстановление)")
    ap.add_argument("--port", help="последовательный порт устройства")
    ap.add_argument("--ble", metavar="ADDR", help="подключиться по Bluetooth (адрес или auto)")
    a = ap.parse_args()
    QGuiApplication.setHighDpiScaleFactorRoundingPolicy(Qt.HighDpiScaleFactorRoundingPolicy.PassThrough)
    qapp = QApplication(sys.argv[:1])
    qapp.setApplicationName("MicroWallet")
    qapp.setStyle("Fusion")
    families = set(QFontDatabase.families())
    for fam in ("Inter", "Segoe UI Variable", "Segoe UI", "SF Pro Text", "Noto Sans", "Cantarell"):
        if fam in families:
            qapp.setFont(QFont(fam, 10))
            break
    qapp.setStyleSheet(QSS)
    win = App(demo=a.demo or a.demo_empty, port=a.port, demo_empty=a.demo_empty)
    win.show()
    if a.ble and B:
        if a.ble == "auto":
            BleDialog(win).show()
        else:
            win.connect_ble(a.ble)
    sys.exit(qapp.exec())


if __name__ == "__main__":
    main()
