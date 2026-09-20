#!/usr/bin/env python3
"""Turn an Android crash.log's bare offsets into function names.

The crash handler (GeneralsMD/Code/Main/AndroidCrashHandler.cpp) writes lines
like

    crash PC=0x7c2790a268 is in /data/app/.../lib/arm64/libmain.so+0x11f5268

because resolving a symbol inside a signal handler is not async-signal-safe.
Newer builds append a "  -> <symbol>+0x.." line themselves; this script exists
for the reports that do not have one -- every build shipped before 19/09/2026,
and any frame whose dladdr lookup was skipped.

Finding the right library is the hard half, and the reason this is a script
rather than a one-line addr2line. An offset only means something against the
exact binary that produced it, and:

  * The "[build compiled <date>]" stamp cannot be trusted. ccache replays a
    cached object file with the preprocessor date baked into it (see
    GameEngine.cpp's own note). A report received on 19/09/2026 was stamped
    30/08/2026 and had in fact been built on 06/09/2026.
  * CI keeps its symbol bundles for a limited time, so the bundle for an
    older report is usually gone.

So this looks for the library in three ways, best first:

  1. --lib, if you already know which one.
  2. The build id, which newer crash logs carry. That is a hash of the linked
     output, so a match is exact.
  3. The "[texchurn]" anchor. The engine's texture-churn diagnostic prints
     "fileoff=0x... nearest_sym=..." pairs computed with the same load-bias
     arithmetic the crash handler uses, so a library where that offset really
     does land inside that symbol is the library that produced the log. This
     is what makes an old, build-id-less report solvable at all: apk/ is
     git-tracked, so every historical APK in the repository's history is its
     own symbol bundle.

Usage:
    scripts/qa/symbolize-android-crash.py crash.log
    scripts/qa/symbolize-android-crash.py crash.log --lib /path/to/libmain.so
    scripts/qa/symbolize-android-crash.py crash.log --search-history

Only function names come back, never file:line -- release builds are stripped
with --strip-unneeded, which keeps .dynsym (hence the names) and drops the
DWARF (hence no lines).
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

OFFSET_RE = re.compile(r'^(?P<head>.*?(?P<lib>lib[\w.\-]+\.so)\+0x(?P<off>[0-9a-fA-F]+))\s*$')
BUILDID_RE = re.compile(r'build id ([0-9a-f]{8,})')
TEXCHURN_RE = re.compile(r'fileoff=0x([0-9a-fA-F]+)\s+nearest_sym=(\S+)')


def find_tool(name):
    ndk = os.environ.get("ANDROID_NDK_HOME") or "/opt/android-sdk/ndk/27.2.12479018"
    candidate = os.path.join(ndk, "toolchains/llvm/prebuilt/linux-x86_64/bin", name)
    if os.path.isfile(candidate):
        return candidate
    found = shutil.which(name) or shutil.which("llvm-" + name)
    if found:
        return found
    sys.exit("error: %s not found; set ANDROID_NDK_HOME or install llvm" % name)


def build_id_of(path):
    readelf = find_tool("llvm-readelf")
    try:
        out = subprocess.run([readelf, "--notes", path], capture_output=True,
                             text=True, check=True).stdout
    except subprocess.CalledProcessError:
        return None
    m = re.search(r'Build ID:\s*([0-9a-f]+)', out)
    return m.group(1) if m else None


def symbol_table(path):
    """[(start, size, name)] from .dynsym, which survives --strip-unneeded."""
    nm = find_tool("llvm-nm")
    out = subprocess.run([nm, "--dynamic", "--defined-only", "--print-size",
                          "--numeric-sort", path],
                         capture_output=True, text=True).stdout
    syms = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 4:
            try:
                syms.append((int(parts[0], 16), int(parts[1], 16), parts[3]))
            except ValueError:
                continue
    return syms


def anchor_matches(path, offset, expected_sym):
    for start, size, name in symbol_table(path):
        if start <= offset < start + size:
            return name == expected_sym or expected_sym.endswith(name)
    return False


def candidate_libs(include_history):
    """On-disk libraries first; then, optionally, every APK ever committed."""
    for root, _dirs, files in os.walk(os.path.join(REPO, "android")):
        for f in files:
            if f.startswith("libmain") and f.endswith(".so"):
                yield os.path.join(root, f), None
    for f in sorted(os.listdir(os.path.join(REPO, "apk"))) if os.path.isdir(os.path.join(REPO, "apk")) else []:
        if f.endswith(".apk"):
            yield os.path.join(REPO, "apk", f), None

    if not include_history:
        return

    # Every APK blob the repository has ever held. Slow, and the last resort --
    # but it is what solved a report whose CI symbol bundle had already expired.
    listing = subprocess.run(
        ["git", "-C", REPO, "rev-list", "--objects", "--all", "--", "apk/"],
        capture_output=True, text=True).stdout
    seen = set()
    for line in listing.splitlines():
        parts = line.split(maxsplit=1)
        if len(parts) == 2 and parts[1].endswith(".apk") and parts[0] not in seen:
            seen.add(parts[0])
            yield None, (parts[0], parts[1])


def extract_so(apk_path):
    """Pull lib/arm64-v8a/libmain*.so out of an APK into a temp file."""
    try:
        zf = zipfile.ZipFile(apk_path)
    except zipfile.BadZipFile:
        return None
    for entry in zf.namelist():
        if entry.startswith("lib/arm64") and "libmain" in entry and entry.endswith(".so"):
            tmp = tempfile.NamedTemporaryFile(suffix=".so", delete=False)
            tmp.write(zf.read(entry))
            tmp.close()
            return tmp.name
    return None


def resolve_library(args, want_build_id, anchor):
    if args.lib:
        return args.lib, "given on the command line"

    for path, blob in candidate_libs(args.search_history):
        local, cleanup = path, False
        if blob is not None:
            sha, name = blob
            data = subprocess.run(["git", "-C", REPO, "cat-file", "blob", sha],
                                  capture_output=True).stdout
            tmp = tempfile.NamedTemporaryFile(suffix=".apk", delete=False)
            tmp.write(data)
            tmp.close()
            local, cleanup = tmp.name, True
            label = "%s (git blob %s)" % (name, sha[:9])
        else:
            label = os.path.relpath(path, REPO)

        so = extract_so(local) if local.endswith(".apk") else local
        try:
            if so is None:
                continue
            if want_build_id:
                if build_id_of(so) == want_build_id:
                    return so, "build id match in %s" % label
            elif anchor:
                offset, sym = anchor
                if anchor_matches(so, offset, sym):
                    return so, "texchurn anchor match in %s" % label
        finally:
            if cleanup:
                os.unlink(local)

    return None, None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("crashlog", help="crash.log, or the whole shared log bundle")
    ap.add_argument("--lib", help="libmain.so or .apk to resolve against")
    ap.add_argument("--search-history", action="store_true",
                    help="also search every APK ever committed under apk/ (slow)")
    args = ap.parse_args()

    text = open(args.crashlog, encoding="utf-8", errors="replace").read()

    want_build_id = None
    m = BUILDID_RE.search(text)
    if m:
        want_build_id = m.group(1)

    anchor = None
    m = TEXCHURN_RE.search(text)
    if m:
        anchor = (int(m.group(1), 16), m.group(2))

    if not want_build_id and not anchor and not args.lib:
        sys.exit("error: the log carries neither a build id nor a [texchurn] anchor, "
                 "so the library cannot be identified -- pass --lib explicitly")

    lib, how = resolve_library(args, want_build_id, anchor)
    if lib is None:
        sys.exit("error: no matching library found. Try --search-history, or pass "
                 "--lib with the unstripped .so from that build's CI run.")

    if lib.endswith(".apk"):
        lib = extract_so(lib)

    print("# resolved against: %s" % how, file=sys.stderr)
    if anchor and not want_build_id:
        print("# (anchor: 0x%x should be inside %s)" % anchor, file=sys.stderr)

    addr2line = find_tool("llvm-addr2line")
    cache = {}
    for line in text.splitlines():
        m = OFFSET_RE.match(line)
        if not m or "libmain" not in m.group("lib"):
            print(line)
            continue
        off = m.group("off")
        if off not in cache:
            out = subprocess.run([addr2line, "-f", "-C", "-e", lib, "0x" + off],
                                 capture_output=True, text=True).stdout.splitlines()
            cache[off] = out[0] if out else "??"
        print("%s  <%s>" % (line.rstrip(), cache[off]))


if __name__ == "__main__":
    main()
