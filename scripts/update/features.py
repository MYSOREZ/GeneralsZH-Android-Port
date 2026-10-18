#!/usr/bin/env python3
# GeneralsX @feature Android port 18/10/2026 Turn the features listed in update/features.json on
# and off in the signed settings (update/config.json), without a new APK. See
# docs/HOWTO/PUBLISH_UPDATE.md ("Feature switches").
#
#   features.py list            each feature: its value here, in the published manifest, its default
#   features.py on NAME [K=V]   "1" in config.json, plus the companion keys the registry names
#   features.py off NAME [K=V]  "0" likewise
#   features.py check           what publish-update.py runs: every listed feature is "0", "1" or absent
#
# A change here is only in the working tree: publish it with publish-update.py and
# push-updates-branch.sh like any other settings change.
import json, os, sys, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
REGISTRY = os.path.join(HERE, "..", "..", "update", "features.json")
CONFIG = os.path.join(HERE, "..", "..", "update", "config.json")
MANIFEST_URL = "https://raw.githubusercontent.com/MYSOREZ/GeneralsZH-Android-Port/updates/manifest.json"


def load(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def save_config(config):
    with open(CONFIG, "w", encoding="utf-8") as f:
        json.dump(config, f, indent=2, ensure_ascii=False)
        f.write("\n")


def published_config():
    try:
        with urllib.request.urlopen(MANIFEST_URL, timeout=20) as r:
            return json.load(r).get("config", {})
    except Exception as e:
        print("(published manifest unreachable: %s)" % e)
        return None


def state(config, name, default):
    value = config.get(name) if config is not None else None
    if value is None:
        return "default (%s)" % ("on" if default else "off")
    return {"1": "on", "0": "off"}.get(value, "INVALID %r" % value)


def problems(registry, config):
    return ["%s = %r: a feature switch is \"0\" or \"1\"" % (name, config[name])
            for name in registry if name in config and config[name] not in ("0", "1")]


def main(argv):
    registry = load(REGISTRY)
    config = load(CONFIG)
    if len(argv) < 2 or argv[1] not in ("list", "on", "off", "check"):
        sys.exit(__doc__ if __doc__ else "usage: features.py list | on NAME | off NAME | check")
    cmd = argv[1]

    if cmd == "check":
        bad = problems(registry, config)
        for line in bad:
            print(line)
        sys.exit(1 if bad else 0)

    if cmd == "list":
        published = published_config()
        for name, info in sorted(registry.items()):
            print("%s\n  here: %s   published: %s\n  %s\n  read by: %s" % (
                name, state(config, name, info["default"]),
                state(published, name, info["default"]) if published is not None else "?",
                info["summary"], info["read_by"]))
            if info.get("unaware_builds"):
                print("  not read by: " + info["unaware_builds"])
            if info.get("why_off"):
                print("  note: " + info["why_off"])
        return

    if len(argv) < 3 or argv[2] not in registry:
        sys.exit("unknown feature %r; listed: %s" % (argv[2] if len(argv) > 2 else "",
                                                       ", ".join(sorted(registry))))
    name = argv[2]
    overrides = dict(arg.split("=", 1) for arg in argv[3:] if "=" in arg)
    config[name] = "1" if cmd == "on" else "0"
    for key, value in registry[name].get("with", {}).get(cmd, {}).items():
        value = overrides.pop(key, value)
        if value.startswith("<"):
            sys.exit("turning %s %s needs %s=%s" % (name, cmd, key, value))
        config[key] = value
    if overrides:
        sys.exit("not a companion key of %s: %s" % (name, ", ".join(overrides)))
    save_config(config)
    print("%s is %s in %s; publish it with publish-update.py" % (name, cmd, os.path.normpath(CONFIG)))
    if registry[name].get("unaware_builds"):
        print("note: " + registry[name]["unaware_builds"])


if __name__ == "__main__":
    main(sys.argv)
