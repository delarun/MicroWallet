#!/usr/bin/env python3
"""
APK для релиза и обновления приложения: android/dist/microwallet-android.apk и рядом
microwallet-android.json — {"versionCode", "versionName", "sha256"}. По нему приложение
на телефоне видит, что в релизе версия новее, не скачивая APK.

  (cd android && ./gradlew assembleRelease) && python3 tools/android_dist.py
"""
import hashlib
import json
import re
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
gradle = (ROOT / "android/app/build.gradle.kts").read_text()
code = int(re.search(r"versionCode\s*=\s*(\d+)", gradle).group(1))
name = re.search(r'versionName\s*=\s*"([^"]+)"', gradle).group(1)
src = ROOT / "android/app/build/outputs/apk/release/app-release.apk"
out = ROOT / "android/dist"
out.mkdir(exist_ok=True)
apk = out / "microwallet-android.apk"
shutil.copyfile(src, apk)
meta = {"versionCode": code, "versionName": name, "sha256": hashlib.sha256(apk.read_bytes()).hexdigest()}
(out / "microwallet-android.json").write_text(json.dumps(meta) + "\n")
print(f"-> {apk} ({apk.stat().st_size} байт), версия {name} ({code})")
