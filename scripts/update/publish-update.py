#!/usr/bin/env python3
# GeneralsX @feature Android port 27/09/2026 Build a signed update for the launcher's updater
# (android/.../UpdateManager.java). See docs/HOWTO/PUBLISH_UPDATE.md.
#
# Produces, in --out, exactly what the `updates` branch holds:
#   manifest.json        what the launcher reads (config, optional engine)
#   manifest.json.sig    base64 ECDSA-P256/SHA-256 signature of manifest.json's bytes
#   datapack-manifest.json  when config's datapack_manifest_url points at the updates branch: the
#                        GeneralsOnline CDN manifest with its fields trimmed, for launchers up to
#                        1.3.0 that fail on the CDN's " 0E45..." sha256 (newer ones read the CDN)
#   support/<sha>.json   the launcher's "Support the project" card (with --support; its SHA-256
#                        is in the manifest, so the manifest's signature covers it)
#   engine/<seq>/libmain.so.gz, libmain60.so.gz   (with --apk)
#
# The engine entry names the SHA-256 of every other native library in the APK
# (requires_libs): the launcher runs a downloaded engine only on an install whose libraries
# are exactly those, so an engine linked against a different SDL/OpenAL/DXVK never loads.
import datetime
import argparse, base64, gzip, hashlib, json, os, subprocess, sys, tempfile, urllib.request, zipfile

BASE_URL = "https://raw.githubusercontent.com/MYSOREZ/GeneralsZH-Android-Port/updates/"
ENGINE_LIBS = ("libmain.so", "libmain60.so")
DATAPACK_CDN_MANIFEST = "https://cdn.playgenerals.online/manifest.json"


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def current_serial():
    try:
        with urllib.request.urlopen(BASE_URL + "manifest.json", timeout=20) as r:
            return int(json.load(r).get("serial", 0))
    except Exception:
        return 0


def fetch(url, timeout):
    # The CDN answers 403 to urllib's default User-Agent.
    return urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "GeneralsX-publish/1"}),
                                  timeout=timeout)


def mirror_datapack_manifest(out_dir, name):
    """The CDN manifest with every string trimmed, after checking that the trimmed sha256 and the
    size really are those of the package it names -- a mirror must never be what breaks installs."""
    with fetch(DATAPACK_CDN_MANIFEST, 30) as r:
        cdn = json.load(r)
    fixed = {k: v.strip() if isinstance(v, str) else v for k, v in cdn.items()}
    h = hashlib.sha256()
    size = 0
    with fetch(fixed["download_url"], 120) as r:
        while True:
            chunk = r.read(1 << 20)
            if not chunk:
                break
            h.update(chunk)
            size += len(chunk)
    if h.hexdigest().lower() != fixed["sha256"].lower() or size != fixed.get("size", size):
        sys.exit("data package %s does not match its own manifest (sha256 %s, size %d)"
                 % (fixed.get("version"), h.hexdigest(), size))
    with open(os.path.join(out_dir, name), "w", encoding="utf-8") as f:
        json.dump(fixed, f, indent=2, ensure_ascii=False)
        f.write("\n")
    print("data package manifest mirrored: %s" % fixed.get("version"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", default=os.path.join(os.path.dirname(__file__), "..", "..", "update", "config.json"))
    ap.add_argument("--support", default=os.path.join(os.path.dirname(__file__), "..", "..", "update", "support.json"),
                    help="the support card's file; a missing file withdraws the card")
    ap.add_argument("--apk", help="APK whose engine to publish; omit for a settings-only update")
    ap.add_argument("--key", help="PEM private key (never commit it); omit to leave signing to the Sign update workflow")
    ap.add_argument("--out", required=True)
    ap.add_argument("--serial", type=int, help="default: the published serial + 1")
    ap.add_argument("--note", default="")
    a = ap.parse_args()

    os.makedirs(a.out, exist_ok=True)
    manifest = {"schema": 1, "serial": a.serial or current_serial() + 1}
    # What the launcher shows players ("Network settings: from 27.09.2026"); the serial is only
    # the anti-rollback counter.
    manifest["published"] = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d")
    if a.note:
        manifest["note"] = a.note
    with open(a.config, encoding="utf-8") as f:
        manifest["config"] = json.load(f)
    datapack_url = manifest["config"].get("datapack_manifest_url", "")
    if datapack_url.startswith(BASE_URL):
        mirror_datapack_manifest(a.out, datapack_url[len(BASE_URL):])

    if os.path.isfile(a.support):
        with open(a.support, "rb") as f:
            data = f.read()
        doc = json.loads(data.decode("utf-8"))
        if not doc.get("entries") or "en" not in doc.get("text", {}):
            sys.exit("support.json needs entries and an \"en\" text (the fallback language)")
        # Named by its digest, like the engine files by seq: raw.githubusercontent caches every
        # path for five minutes on its own, so a fixed name could pair a fresh manifest with the
        # previous file, which the launcher then (rightly) refuses -- and keeps the old card.
        rel = "support/%s.json" % sha256(data)[:16]
        os.makedirs(os.path.join(a.out, "support"), exist_ok=True)
        with open(os.path.join(a.out, rel), "wb") as f:
            f.write(data)
        manifest["support"] = {"url": BASE_URL + rel, "sha256": sha256(data), "size": len(data)}

    if a.apk:
        with zipfile.ZipFile(a.apk) as z:
            seq = int(z.read("assets/engine_build.txt").decode().strip())
            libs = {os.path.basename(n): n for n in z.namelist()
                    if n.startswith("lib/arm64-v8a/") and n.endswith(".so")}
            engine = {"seq": seq, "files": {}, "requires_libs": {}}
            for name, path in sorted(libs.items()):
                data = z.read(path)
                if name in ENGINE_LIBS:
                    rel = "engine/%d/%s.gz" % (seq, name)
                    dest = os.path.join(a.out, rel)
                    os.makedirs(os.path.dirname(dest), exist_ok=True)
                    with open(dest, "wb") as f:
                        f.write(gzip.compress(data, 9, mtime=0))
                    engine["files"][name] = {"url": BASE_URL + rel, "sha256": sha256(data), "size": len(data)}
                else:
                    engine["requires_libs"][name] = sha256(data)
            missing = [n for n in ENGINE_LIBS if n not in engine["files"]]
            if missing:
                sys.exit("APK has no " + ", ".join(missing))
        manifest["engine"] = engine

    body = (json.dumps(manifest, indent=2, sort_keys=True, ensure_ascii=False) + "\n").encode("utf-8")
    mpath = os.path.join(a.out, "manifest.json")
    with open(mpath, "wb") as f:
        f.write(body)
    if a.key:
        sig = subprocess.run(["openssl", "dgst", "-sha256", "-sign", a.key, mpath],
                             check=True, capture_output=True).stdout
        with open(mpath + ".sig", "w") as f:
            f.write(base64.b64encode(sig).decode() + "\n")
    else:
        print("manifest left unsigned: push it, then run the 'Sign update' workflow")
    print("serial %d%s -> %s" % (manifest["serial"],
          ", engine %d" % manifest["engine"]["seq"] if "engine" in manifest else "", a.out))


if __name__ == "__main__":
    main()
