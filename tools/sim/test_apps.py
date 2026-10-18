#!/usr/bin/env python3
"""
Приложения WebAssembly в симуляторе: установка, Ethereum (адреса, подписи), песочница.

  tools/sim/build.sh /tmp/mwsim
  python3 tools/sim/test_apps.py /tmp/mwsim
"""
import base64
import hashlib
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "host"))
import wallet as W  # noqa: E402

P = 2**256 - 2**32 - 977
N = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141
G = (0x79BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798,
     0x483ADA7726A3C4655DA4FBFC0E1108A8FD17B448A68554199C47D08FFB10D4B8)


def ec_add(a, b):
    if a is None:
        return b
    if b is None:
        return a
    if a[0] == b[0] and (a[1] + b[1]) % P == 0:
        return None
    if a == b:
        m = 3 * a[0] * a[0] * pow(2 * a[1], -1, P)
    else:
        m = (b[1] - a[1]) * pow(b[0] - a[0], -1, P)
    x = (m * m - a[0] - b[0]) % P
    return x, (m * (a[0] - x) - a[1]) % P


def ec_mul(k, p=G):
    r = None
    while k:
        if k & 1:
            r = ec_add(r, p)
        p = ec_add(p, p)
        k >>= 1
    return r


def recover_address(h, sig):
    """Адрес EVM из подписи r||s||recid над хешем h."""
    r, s, rec = int.from_bytes(sig[:32], "big"), int.from_bytes(sig[32:64], "big"), sig[64]
    y = pow((r**3 + 7) % P, (P + 1) // 4, P)
    if y & 1 != rec:
        y = P - y
    z = int.from_bytes(h, "big")
    ri = pow(r, -1, N)
    q = ec_add(ec_mul(s * ri % N, (r, y)), ec_mul(-z * ri % N))
    return W.evm_checksum(W.keccak256(q[0].to_bytes(32, "big") + q[1].to_bytes(32, "big"))[12:])


class Sim:
    def __init__(self, binary, d, answer=None):
        env = {**os.environ, "MW_SIM_DIR": d}
        if answer:
            env["MW_SIM_ANSWER"] = answer
        self.p = subprocess.Popen([binary, "lines"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, text=True, env=env)

    def call(self, line):
        self.p.stdin.write(line + "\n")
        self.p.stdin.flush()
        return self.p.stdout.readline().strip()

    def close(self):
        self.p.stdin.close()
        self.p.wait(5)


def build(src, out):
    subprocess.run(["sh", str(ROOT / "sdk/build.sh"), str(src), str(out)], check=True, stdout=subprocess.DEVNULL)
    return Path(out).read_bytes()


def install(sim, wasm):
    r = sim.call(f"app add {len(wasm)} {hashlib.sha256(wasm).hexdigest()}")
    assert r == "ok", r
    for off in range(0, len(wasm), 2048):
        r = sim.call("app add b64 " + base64.b64encode(wasm[off:off + 2048]).decode())
        assert r == "ok", r
    return sim.call("app add end")


def main():
    binary = sys.argv[1]
    ok = 0
    with tempfile.TemporaryDirectory() as tmp:
        eth = build(ROOT / "apps/eth/eth.c", Path(tmp) / "eth.wasm")
        evil = build(ROOT / "tools/sim/test_apps/evil/evil.c", Path(tmp) / "evil.wasm")
        d = Path(tmp) / "dev"
        d.mkdir()
        sim = Sim(binary, str(d))
        assert sim.call("restore " + "00" * 16).startswith("ok"), "restore"
        net = dict(kv.split("=", 1) for kv in sim.call("info")[3:].split() if "=" in kv)["net"]
        chain = 11155111 if net == "testnet" else 1

        # ---- установка ----
        assert install(sim, eth) == "ok"
        assert sim.call("apps") == "ok eth|1.0|Ethereum", sim.call("apps")
        print(f"1. установка: Ethereum {len(eth)} байт, apps -> {sim.call('apps')}")
        ok += 1

        # ---- адрес: тот же путь, что у встроенной BSC ----
        a0 = sim.call("app eth addr 0")
        assert a0 == "ok 0x9858EfFD232B4033E47d90003D41EC34EcaEda94", a0
        assert sim.call("app eth show 3") == sim.call("app eth addr 3")
        print(f"2. адрес #0 {a0[3:]} — как в MetaMask (abandon…about)")
        ok += 1

        # ---- подпись перевода ETH ----
        to = bytes.fromhex("35" * 20)
        tx = W.rlp([7, 2 * 10**9, 21000, to, 10**16, b"", chain, 0, 0])
        r = sim.call(f"app eth sign 0 {tx.hex()}")
        assert r.startswith("ok "), r
        assert recover_address(W.keccak256(tx), bytes.fromhex(r[3:])) == a0[3:]
        print("3. подпись ETH 0.01: восстановленный адрес совпал")
        ok += 1

        # ---- токен из белого списка (USDC: Sepolia или mainnet) ----
        usdc = "1c7D4B196Cb0C7B01d743Fbc6116a902379C7238" if chain != 1 else "A0b86991c6218b36c1d19D4a2e9Eb0cE3606eB48"
        data = bytes.fromhex("a9059cbb") + b"\0" * 12 + to + (12_500_000).to_bytes(32, "big")
        tx = W.rlp([8, 3 * 10**9, 60000, bytes.fromhex(usdc), 0, data, chain, 0, 0])
        r = sim.call(f"app eth sign 1 {tx.hex()}")
        assert r.startswith("ok ") and recover_address(W.keccak256(tx), bytes.fromhex(r[3:])) == sim.call("app eth addr 1")[3:], r
        print("4. подпись USDC 12.5: ok")
        ok += 1

        # ---- отказы ----
        bad = {
            "чужая сеть": W.rlp([1, 1, 21000, to, 1, b"", 56, 0, 0]),
            "неизвестный контракт": W.rlp([1, 1, 60000, b"\x11" * 20, 0, data, chain, 0, 0]),
            "approve": W.rlp([1, 1, 60000, bytes.fromhex(usdc), 0, bytes.fromhex("095ea7b3") + data[4:], chain, 0, 0]),
            "создание контракта": W.rlp([1, 1, 60000, b"", 0, b"\x60\x00", chain, 0, 0]),
        }
        for what, t in bad.items():
            r = sim.call(f"app eth sign 0 {t.hex()}")
            assert r.startswith("err "), (what, r)
        assert sim.call("app eth sign 0 zz").startswith("err ")
        assert sim.call("app nope addr 0") == "err no such app"
        assert sim.call("addr 0 0") == "err unknown command"  # встроенных монет больше нет
        print(f"5. отклонено: {', '.join(bad)}, мусор, чужое приложение")
        ok += 1

        # ---- песочница ----
        assert install(sim, evil) == "ok"
        checks = {
            "steal": "ok denied",            # ключ не из манифеста
            "sign": "ok denied",             # подпись без подтверждения
            "own": "ok allowed",             # свой путь — можно
            "twice": "ok first",             # ответ только один
            "hello": "ok hello",
            "edsteal": "ok denied",          # ed25519 по чужому пути
            "srsteal": "ok denied",          # sr25519 без sr25519=yes
        }
        for c, want in checks.items():
            assert sim.call(f"app evil {c}") == want, (c, sim.call(f"app evil {c}"))
        r = sim.call("app evil oob")
        assert r.startswith("err app crashed"), r
        r = sim.call("app evil trap")
        assert r.startswith("err app crashed"), r
        assert sim.call("app evil hello") == "ok hello"  # после сбоя всё живо
        print("6. песочница: чужой ключ, подпись без «Accept», выход за память, trap — не проходят")
        ok += 1

        # ---- обновление, удаление ----
        assert install(sim, eth) == "ok"  # тот же id — замена в том же слоте
        assert sim.call("apps").count("eth|") == 1
        assert sim.call("app rm evil") == "ok"
        assert sim.call("apps") == "ok eth|1.0|Ethereum"
        sim.close()

        # ---- отказ на экране ----
        rej = Sim(binary, str(d), answer="reject")
        assert rej.call("app eth show 0") == "err rejected"
        assert rej.call(f"app eth sign 0 {W.rlp([7, 1, 21000, to, 1, b'', chain, 0, 0]).hex()}") == "err rejected"
        assert rej.call("app rm eth") == "err rejected"
        bad_sha = rej.call(f"app add {len(evil)} {'00' * 32}")
        for off in range(0, len(evil), 2048):
            rej.call("app add b64 " + base64.b64encode(evil[off:off + 2048]).decode())
        assert bad_sha == "ok" and rej.call("app add end") == "err sha256 mismatch"
        rej.close()
        print("7. обновление в том же слоте, удаление, отказы кнопкой, чужой хеш")
        ok += 1
    print(f"всё прошло: {ok}/7")


if __name__ == "__main__":
    main()
