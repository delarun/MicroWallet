#!/usr/bin/env python3
"""
Хранилище приложений без слотов (src/apps.c): заполнение, дыры после удаления,
две копии после «пропавшего питания» посреди обновления, повреждённый модуль.

  tools/sim/build.sh /tmp/mwsim && sh apps/build.sh && python3 tools/sim/test_store.py /tmp/mwsim
"""
import base64
import hashlib
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SECTOR = 4096


class Sim:
    def __init__(self, binary, d):
        self.d = d
        self.p = subprocess.Popen([binary, "lines"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, text=True, env={**os.environ, "MW_SIM_DIR": d})

    def call(self, line):
        self.p.stdin.write(line + "\n")
        self.p.stdin.flush()
        return self.p.stdout.readline().strip()

    def install(self, wasm):
        r = self.call(f"app add {len(wasm)} {hashlib.sha256(wasm).hexdigest()}")
        if r != "ok":
            return r
        for off in range(0, len(wasm), 2048):
            self.call("app add b64 " + base64.b64encode(wasm[off:off + 2048]).decode())
        return self.call("app add end")

    def apps(self):
        r = self.call("apps")[3:]
        return [e.split("|")[0] for e in r.split(";") if e]

    def info(self):
        return dict(kv.split("=", 1) for kv in self.call("info")[3:].split() if "=" in kv)

    def close(self):
        self.p.stdin.close()
        self.p.wait(5)


def variant(tmp, app_id, pad=0):
    """evil.wasm с другим id (и при желании раздутый), через sdk/pack.py."""
    src = ROOT / "tools/sim/test_apps/evil/evil.c"
    raw = Path(tmp) / "raw.wasm"
    if not raw.exists():
        subprocess.run(["clang", "--target=wasm32", "-mcpu=mvp", "-O2", "-nostdlib", "-ffreestanding", "-fno-builtin",
                        f"-I{ROOT}/sdk", "-Wl,--no-entry", "-Wl,--strip-debug", "-Wl,--initial-memory=65536",
                        "-Wl,--max-memory=65536", "-Wl,-z,stack-size=16384", "-o", str(raw), str(src),
                        str(ROOT / "sdk/mw.c")], check=True)
    w = raw.read_bytes()
    if pad:  # пользовательская секция-«балласт»: модуль больше, код тот же
        name = b"pad"
        payload = bytes([len(name)]) + name + b"\0" * pad
        n, leb = len(payload), b""
        while True:
            b = n & 0x7F
            n >>= 7
            leb += bytes([b | (0x80 if n else 0)])
            if not n:
                break
        w += b"\0" + leb + payload
    man = Path(tmp) / f"{app_id}.manifest"
    man.write_text(f"id={app_id}\nname=Store {app_id}\nversion=0.1\npath=44'/9999'\n")
    (Path(tmp) / "in.wasm").write_bytes(w)
    out = Path(tmp) / f"{app_id}.wasm"
    subprocess.run([sys.executable, str(ROOT / "sdk/pack.py"), str(Path(tmp) / "in.wasm"), str(man), str(out)], check=True)
    return out.read_bytes()


def main():
    binary = sys.argv[1]
    with tempfile.TemporaryDirectory() as tmp:
        d = Path(tmp) / "dev"
        d.mkdir()
        sim = Sim(binary, str(d))
        assert sim.call("restore " + "00" * 16).startswith("ok")
        coins = sorted((ROOT / "apps/dist").glob("microwallet-app-*.wasm"))
        for f in coins:
            assert sim.install(f.read_bytes()) == "ok", f.name
        i = sim.info()
        used = sum((512 + f.stat().st_size + SECTOR - 1) // SECTOR * SECTOR for f in coins) // 1024
        assert int(i["apps"]) == 1536 and int(i["apps_free"]) == 1536 - used, i
        print(f"1. пять монет заняли {used} КБ из 1536 (вместо 5 × 64 = 320 с фиксированными слотами)")

        # ---- дыра после удаления используется снова ----
        def where(app_id):
            blob = (d / "apps.bin").read_bytes()
            return next(o for o in range(0, len(blob), SECTOR)
                        if blob[o:o + 6] == b"MWAPP2" and f"id={app_id}\n".encode() in blob[o:o + 512])
        eth_at = where("eth")  # в середине: bsc, btc, eth, prl, tron
        assert sim.call("app rm eth") == "ok"
        assert sim.install(variant(tmp, "hole")) == "ok"
        assert where("hole") == eth_at, (hex(where("hole")), hex(eth_at))
        print(f"2. после удаления Ethereum новое приложение легло на его место ({eth_at:#x})")

        # ---- заполнение: 64 КБ модули до отказа ----
        n = 0
        span_kb = (512 + len(variant(tmp, "big", pad=60000)) + SECTOR - 1) // SECTOR * SECTOR // 1024
        while True:
            r = sim.install(variant(tmp, f"big{n}", pad=60000))
            if r != "ok":
                break
            n += 1
        assert r in ("err not enough space for apps",), r
        apps = sim.apps()
        free = int(sim.info()["apps_free"])
        # отказ честный: ни одного непрерывного свободного участка под такой модуль
        # (сумма свободного может быть и больше — дыры от удалённых приложений)
        blob = (d / "apps.bin").read_bytes()
        busy = [False] * (len(blob) // SECTOR)
        for o in range(0, len(blob), SECTOR):
            if blob[o:o + 6] == b"MWAPP2":
                ln = struct.unpack_from("<I", blob, o + 8)[0]
                for k in range(o // SECTOR, (o + 512 + ln + SECTOR - 1) // SECTOR):
                    busy[k] = True
        run = best = 0
        for b in busy:
            run = 0 if b else run + 1
            best = max(best, run)
        assert best * SECTOR // 1024 < span_kb, (best, span_kb, free)
        print(f"3. заполнено: {len(apps)} приложений, свободно {free} КБ; дальше — «{r[4:]}»")
        for k in range(n):
            assert sim.call(f"app rm big{k}") == "ok"
        assert sim.call("app hole hello") == "ok hello" and sim.call("app btc addr 0").startswith("ok tb1q")
        sim.close()

        # ---- «питание пропало посреди обновления»: две копии, старшая по seq побеждает ----
        blob = bytearray((d / "apps.bin").read_bytes())
        old = next(o for o in range(0, len(blob), SECTOR) if blob[o:o + 6] == b"MWAPP2" and b"id=hole" in blob[o:o + 512])
        ln, seq = struct.unpack_from("<II", blob, old + 8)
        span = (512 + ln + SECTOR - 1) // SECTOR * SECTOR
        free_at = max(o for o in range(0, len(blob), SECTOR) if blob[o:o + 6] == b"MWAPP2")
        free_at += SECTOR * 32  # заведомо свободно
        blob[free_at:free_at + span] = blob[old:old + span]
        struct.pack_into("<I", blob, free_at + 12, seq + 1)
        (d / "apps.bin").write_bytes(bytes(blob))
        sim = Sim(binary, str(d))
        assert sim.apps().count("hole") == 1
        after = (d / "apps.bin").read_bytes()
        assert after[old:old + 6] == b"\xff" * 6 and after[free_at:free_at + 6] == b"MWAPP2"
        assert sim.call("app hole hello") == "ok hello"
        print("4. две копии после сбоя: осталась новая (seq больше), старая стёрта при старте")

        # ---- повреждённый модуль не запускается ----
        sim.close()
        blob = bytearray((d / "apps.bin").read_bytes())
        blob[free_at + 600] ^= 0xFF
        (d / "apps.bin").write_bytes(bytes(blob))
        sim = Sim(binary, str(d))
        assert sim.call("app hole hello") == "err app corrupted, reinstall it"
        sim.close()
        print("5. байт модуля во флеше испорчен — запуск отклонён (SHA-256)")
    print("всё прошло")


if __name__ == "__main__":
    main()
