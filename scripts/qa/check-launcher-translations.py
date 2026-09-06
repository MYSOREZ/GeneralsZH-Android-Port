#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Check that every launcher locale carries every launcher string.

The Android launcher ships 13 locales, and the rule this project has held to
since "Finish the launcher translations" is that all of them stay at 100%: a
string that only exists in values/ silently falls back to English on someone
else's phone, and the strings that fall back first are the ones a person only
ever reads when something has already gone wrong.

Android has no build-time error for a missing translation -- lint has a
warning nobody sees in this pipeline -- so the invariant needs an actual
check. This is it. Run it after touching any strings.xml:

    python3 scripts/qa/check-launcher-translations.py

Exits non-zero and names every missing or extra key if any locale drifts.
"""
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
RES = os.path.join(REPO, "android", "app", "src", "main", "res")

# values/ is the reference set: it is what Android falls back to, so a key
# missing from it is a build error rather than a coverage problem.
REFERENCE = "values"

KEY_RE = re.compile(r'<string\s+name="([^"]+)"')


def locales():
    found = []
    for name in sorted(os.listdir(RES)):
        if name == REFERENCE or not name.startswith("values"):
            continue
        if os.path.isfile(os.path.join(RES, name, "strings.xml")):
            found.append(name)
    return found


def keys_in(locale):
    path = os.path.join(RES, locale, "strings.xml")
    with open(path, encoding="utf-8") as handle:
        return set(KEY_RE.findall(handle.read()))


def main():
    reference = keys_in(REFERENCE)
    print("%-16s %4d keys  (reference)" % (REFERENCE, len(reference)))

    problems = 0
    for locale in locales():
        translated = keys_in(locale)
        missing = sorted(reference - translated)
        extra = sorted(translated - reference)
        coverage = 100.0 * len(reference & translated) / len(reference)
        status = "OK" if not missing and not extra else "DRIFT"
        print("%-16s %4d keys  %6.1f%%  %s" % (locale, len(translated), coverage, status))
        for key in missing:
            print("    missing: %s" % key)
            problems += 1
        for key in extra:
            print("    not in %s: %s" % (REFERENCE, key))
            problems += 1

    if problems:
        print("\n%d problem(s): translation coverage is not 100%%." % problems)
        return 1
    print("\nAll %d locales at 100%% of %d keys." % (len(locales()) + 1, len(reference)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
