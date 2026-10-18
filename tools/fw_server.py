#!/usr/bin/env python3
"""
Раздача прошивок с ПК для приложения на телефоне — в формате GitHub Releases API,
поэтому приложение работает с ним так же, как с релизами на GitHub.

  python3 tools/fw_server.py [--dir esp32/dist] [--apps apps/dist] [--port 8777]
  adb reverse tcp:8777 tcp:8777      # телефон по USB: http://localhost:8777 ведёт сюда

В приложении: Устройство → Прошивка → источник «ПК» → http://localhost:8777
Релиз один — «dev»: прошивки (*-app.bin), приложения (microwallet-app-*.wasm) и
приложение для телефона (android/dist, см. tools/android_dist.py); дата — самый свежий файл.
"""
import argparse
import hashlib
import json
import os
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import unquote

ROOT = Path(__file__).resolve().parent.parent


def dist_files(d: Path, apps: Path, android: Path):
    return (sorted(d.glob("microwallet-*-app.bin")) + sorted(apps.glob("microwallet-app-*.wasm"))
            + sorted(android.glob("microwallet-android.*")))


def release(d: Path, apps: Path, android: Path, base: str):
    files = dist_files(d, apps, android)
    newest = max((f.stat().st_mtime for f in files), default=time.time())
    stamp = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(newest))
    assets = []
    body = ["Сборка с ПК (tools/fw_server.py)", ""]
    for f in files:
        data = f.read_bytes()
        assets.append({"name": f.name, "size": len(data), "browser_download_url": f"{base}/dl/{f.name}",
                       "updated_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(f.stat().st_mtime))})
        body.append(f"{f.name}  sha256 {hashlib.sha256(data).hexdigest()}")
    return {"tag_name": "dev-" + time.strftime("%m%d-%H%M", time.localtime(newest)), "name": "Сборка с ПК",
            "body": "\n".join(body), "prerelease": True, "draft": False, "published_at": stamp, "assets": assets}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dir", default=str(ROOT / "esp32" / "dist"))
    ap.add_argument("--apps", default=str(ROOT / "apps" / "dist"))
    ap.add_argument("--android", default=str(ROOT / "android" / "dist"))
    ap.add_argument("--port", type=int, default=8777)
    a = ap.parse_args()
    d = Path(a.dir)
    apps = Path(a.apps)
    android = Path(a.android)
    dirs = (d, apps, android)

    class H(BaseHTTPRequestHandler):
        def send(self, code, data, ctype):
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            base = f"http://{self.headers.get('Host', f'localhost:{a.port}')}"
            path = unquote(self.path.split("?")[0])
            if path.rstrip("/") in ("", "/releases"):
                self.send(200, json.dumps([release(d, apps, android, base)], ensure_ascii=False, indent=1).encode(),
                          "application/json")
            elif path.startswith("/dl/") and "/" not in path[4:] and any((x / path[4:]).is_file() for x in dirs):
                f = next(x / path[4:] for x in dirs if (x / path[4:]).is_file())
                self.send(200, f.read_bytes(), "application/octet-stream")
            else:
                self.send(404, b"not found", "text/plain")

    srv = ThreadingHTTPServer(("0.0.0.0", a.port), H)
    print(f"Прошивки из {d}:")
    for f in dist_files(d, apps, android):
        print(f"  {f.name}  {time.strftime('%d.%m %H:%M', time.localtime(f.stat().st_mtime))}")
    print(f"\nhttp://localhost:{a.port}/releases")
    print(f"Телефон по USB: adb reverse tcp:{a.port} tcp:{a.port}, в приложении — http://localhost:{a.port}")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    os.chdir(ROOT)
    main()
