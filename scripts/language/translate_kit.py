#!/usr/bin/env python3
"""Tools for translating the game's text into a new language pack.

A pack is languages/<language>/generals.str (languages/README.md). Translating one by hand
is ~6,400 entries; this splits the English source into chunks a translator can work through,
checks every translated chunk mechanically, and assembles the finished pack.

    # 1. English source, from the game's own table (plus the port's labels, see below)
    python3 scripts/language/csf2str.py EnglishZH.big -o english.str

    # 2. Chunks: chunks/NNN.json, each a list of {"label": ..., "text": ...}
    python3 scripts/language/translate_kit.py split english.str chunks/ --size 200

    # 3. Translate chunks/NNN.json into out/NNN.json (same shape, same labels, same order)
    python3 scripts/language/translate_kit.py check chunks/NNN.json out/NNN.json

    # 4. The pack
    python3 scripts/language/translate_kit.py assemble chunks/ out/ \\
        -o languages/<language>/generals.str --language <language>

"text" in the JSON is the real string: a line break is a real newline, a quote is a quote.
Escaping for the .str format happens only in assemble.

What check enforces, because the game depends on it rather than because of style:
  - the same labels, in the same order, none missing, none added;
  - printf arguments (%s, %d, %ls, %%, ...) identical and in the same order -- a missing or
    reordered one is a crash or garbage on screen;
  - the same number of line breaks (reported as a warning only: it is layout);
  - "&" hotkey markers: one where the English has one (on a letter of the translation),
    none where it has none;
  - a leading "*" (subtitle marker) kept;
  - no empty string where the English has text.
"""

import argparse
import json
import os
import re
import sys

# No space flag: in this game's text "+25% Firepower" is a percent sign followed by a word,
# and reading "% F" as a conversion would demand that every translation keep "% F".
PRINTF = re.compile(r"%(?:%|[-+#0]*\d*(?:\.\d+)?(?:hh|h|ll|l|L|I64)?[sSdiuoxXcCfFeEgGp])")


def unescape(s):
    out = []
    i = 0
    while i < len(s):
        c = s[i]
        if c == "\\" and i + 1 < len(s):
            n = s[i + 1]
            out.append({"n": "\n", "t": "\t", '"': '"', "\\": "\\"}.get(n, "\\" + n))
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def escape(text):
    return (text.replace("\\", "\\\\").replace('"', '\\"')
                .replace("\r", "").replace("\n", "\\n").replace("\t", "\\t"))


def parse_str(path):
    """[(label, text)] from a .str file, in file order."""
    entries = []
    label = None
    text = None
    with open(path, encoding="utf-8") as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith("//"):
                continue
            if label is None:
                label = line
                text = None
            elif line.upper() == "END":
                entries.append((label, text if text is not None else ""))
                label = None
            elif line.startswith('"') and text is None:
                body = line[1:]
                if body.endswith('"') and not body.endswith('\\"'):
                    body = body[:-1]
                elif body.endswith('\\\\"'):
                    body = body[:-1]
                text = unescape(body)
    return entries


def cmd_split(args):
    entries = parse_str(args.source)
    os.makedirs(args.outdir, exist_ok=True)
    n = 0
    for start in range(0, len(entries), args.size):
        chunk = [{"label": l, "text": t} for l, t in entries[start:start + args.size]]
        with open(os.path.join(args.outdir, "%03d.json" % n), "w", encoding="utf-8") as f:
            json.dump(chunk, f, ensure_ascii=False, indent=1)
        n += 1
    print("%d entries -> %d chunks in %s" % (len(entries), n, args.outdir))


def hotkeys(s):
    """Positions of '&' that mark a hotkey (followed by a letter or digit)."""
    return [i for i in range(len(s) - 1) if s[i] == "&" and s[i + 1].isalnum()]


def check_pair(src, dst, warnings=None):
    problems = []
    if warnings is None:
        warnings = []
    if not isinstance(dst, list) or len(dst) != len(src):
        return ["entry count %s, expected %d" % (len(dst) if isinstance(dst, list) else "?", len(src))]
    for i, (s, d) in enumerate(zip(src, dst)):
        where = "#%d %s" % (i, s["label"])
        if not isinstance(d, dict) or d.get("label") != s["label"]:
            problems.append("%s: label mismatch (%r)" % (where, d.get("label") if isinstance(d, dict) else d))
            continue
        st, dt = s["text"], d.get("text")
        if not isinstance(dt, str):
            problems.append("%s: text is not a string" % where)
            continue
        if st.strip() and not dt.strip():
            problems.append("%s: empty translation" % where)
        if PRINTF.findall(st) != PRINTF.findall(dt):
            problems.append("%s: printf %s, expected %s" % (where, PRINTF.findall(dt), PRINTF.findall(st)))
        if st.count("\n") != dt.count("\n"):
            # Layout only: a tooltip wrapped differently still works, and established
            # community translations often break lines where the language needs it.
            warnings.append("%s: %d line breaks, English has %d" % (where, dt.count("\n"), st.count("\n")))
        hs, hd = len(hotkeys(st)), len(hotkeys(dt))
        if hs != hd:
            problems.append("%s: %d '&' hotkeys, expected %d" % (where, hd, hs))
        if st.startswith("*") and not dt.startswith("*"):
            problems.append("%s: leading '*' dropped" % where)
    return problems


def cmd_check(args):
    with open(args.source, encoding="utf-8") as f:
        src = json.load(f)
    try:
        with open(args.translated, encoding="utf-8") as f:
            dst = json.load(f)
    except (OSError, ValueError) as e:
        print("FAIL %s: %s" % (args.translated, e))
        return 1
    problems = check_pair(src, dst)
    if problems:
        print("FAIL %s: %d problem(s)" % (args.translated, len(problems)))
        for p in problems[:50]:
            print("  " + p)
        return 1
    print("OK %s (%d entries)" % (args.translated, len(src)))
    return 0


def cmd_assemble(args):
    names = sorted(n for n in os.listdir(args.chunks) if n.endswith(".json"))
    out = []
    bad = 0
    for name in names:
        with open(os.path.join(args.chunks, name), encoding="utf-8") as f:
            src = json.load(f)
        path = os.path.join(args.translated, name)
        if not os.path.exists(path):
            print("missing %s" % path)
            bad += 1
            continue
        with open(path, encoding="utf-8") as f:
            dst = json.load(f)
        problems = check_pair(src, dst)
        if problems:
            print("FAIL %s: %d problem(s), first: %s" % (name, len(problems), problems[0]))
            bad += 1
            continue
        out.extend(dst)
    if bad:
        print("not assembled: %d chunk(s) missing or failing" % bad)
        return 1
    with open(args.output, "w", encoding="utf-8") as f:
        f.write("// Command & Conquer: Generals Zero Hour -- %s game text\n" % args.language)
        f.write("//\n// Plain UTF-8. One entry is a label line, a quoted string, and END.\n")
        f.write("// Lines starting with // are comments. Escapes: \\n \\t \\\" \\\\\n//\n")
        f.write("// Translated from the English original (EnglishZH.big, generals.csf), checked with\n")
        f.write("// scripts/language/translate_kit.py. See languages/README.md before editing.\n\n")
        for e in out:
            f.write("%s\n\"%s\"\nEND\n\n" % (e["label"], escape(e["text"])))
    print("wrote %d entries to %s" % (len(out), args.output))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("split"); p.add_argument("source"); p.add_argument("outdir"); p.add_argument("--size", type=int, default=200)
    p = sub.add_parser("check"); p.add_argument("source"); p.add_argument("translated")
    p = sub.add_parser("assemble"); p.add_argument("chunks"); p.add_argument("translated")
    p.add_argument("-o", "--output", required=True); p.add_argument("--language", required=True)
    args = ap.parse_args()
    return {"split": cmd_split, "check": cmd_check, "assemble": cmd_assemble}[args.cmd](args) or 0


if __name__ == "__main__":
    sys.exit(main())
