#!/usr/bin/env python3
"""Compute a game folder's INI checksum without running the game.

GeneralsOnline refuses a lobby whose ini_crc differs from the host's, and the
only way to see that number used to be to install the data on a device, launch
the game, and read the [GX-CRC] line it prints. That makes "is this copy of the
data the same as that one?" an hour-long question. This answers it in a second,
for any folder reachable from this machine.

It is a direct transcription of what the engine does, not an approximation:

  * XferCRC::addCRC / xferImplementation  (Core/.../System/XferCRC.cpp)
  * INI::readLine, which feeds the checksum one line at a time, with a
    semicolon terminating the string and control characters folded to spaces
    (Core/.../Common/INI/INI.cpp)
  * the exact order of INI loads in GameEngine::init, which is what makes the
    checksum order-sensitive (GeneralsMD/.../Common/GameEngine.cpp)

  * the point in GameEngine::init where ArchiveFileSystem::loadMods() mounts
    the community patch -- after the GameData INI has already been read from
    the retail archive, and before the weather INI. That ordering is not a
    detail: mounting the patch a few lines earlier changes the result.

The transcription is checked, not assumed: the engine carries two hardcoded
checkpoint values for retail data (0xA1E7F8E6 after the weather INI, 0x6209AF6E
after the object INI), and this tool reports whether it reproduced them. If it
did, the final number is trustworthy; if it did not, the data is unusual enough
that the number means nothing and the tool says so. The checkpoints describe
retail data only, so they are not expected to hold once a patch is mounted --
what stands in for them there is the known 2180732466.

Usage:
    scripts/qa/ini_crc.py <game folder> [community patch .big]

The second argument is GeneralsOnline's community data patch, which its client
downloads into the user-data folder and mounts ahead of the retail archives on
every launch. Pass it to see the number a normal GeneralsOnline install reports
rather than the number the retail data alone produces.

Known values:
    4272612339  untouched retail data -- GeneralsOnline calls this VANILLA_INI_CRC
    2180732466  what every PC GeneralsOnline lobby reports
"""

import os
import struct
import sys

MASK = 0xFFFFFFFF

# Checkpoints the engine itself hardcodes for retail data.
CHECKPOINT_WEATHER = 0xA1E7F8E6
CHECKPOINT_OBJECT = 0x6209AF6E

# Directory pairs fed to the checksum, in GameEngine::init order. A pair is
# (default directory, override directory); either may be absent.
CRC_LOAD_ORDER = [
    ("Data\\INI\\Default\\GameData", "Data\\INI\\GameData"),
    # ArchiveFileSystem::loadMods() sits exactly here, between the GameData INI
    # and the weather INI -- so the GameData files are read from the retail
    # archive and everything after them from the patch.
    ("__mount_patch__", None),
    (None, "Data\\INI\\Default\\Water"),
    (None, "Data\\INI\\Water"),
    (None, "Data\\INI\\Default\\Weather"),
    (None, "Data\\INI\\Weather"),
    ("__checkpoint_weather__", None),
    ("Data\\INI\\Default\\Science", "Data\\INI\\Science"),
    ("Data\\INI\\Default\\Multiplayer", "Data\\INI\\Multiplayer"),
    ("Data\\INI\\Default\\Terrain", "Data\\INI\\Terrain"),
    ("Data\\INI\\Default\\Roads", "Data\\INI\\Roads"),
    (None, "Data\\INI\\Rank"),
    ("Data\\INI\\Default\\PlayerTemplate", "Data\\INI\\PlayerTemplate"),
    ("Data\\INI\\Default\\FXList", "Data\\INI\\FXList"),
    (None, "Data\\INI\\Weapon"),
    ("Data\\INI\\Default\\ObjectCreationList", "Data\\INI\\ObjectCreationList"),
    (None, "Data\\INI\\Locomotor"),
    ("Data\\INI\\Default\\SpecialPower", "Data\\INI\\SpecialPower"),
    (None, "Data\\INI\\DamageFX"),
    (None, "Data\\INI\\Armor"),
    ("Data\\INI\\Default\\Object", "Data\\INI\\Object"),
    ("__checkpoint_object__", None),
    ("Data\\INI\\Default\\Upgrade", "Data\\INI\\Upgrade"),
    ("Data\\INI\\Default\\AIData", "Data\\INI\\AIData"),
    ("Data\\INI\\Default\\Crate", "Data\\INI\\Crate"),
]


class XferCRC(object):
    """XferCRC from Core/GameEngine/Source/Common/System/XferCRC.cpp."""

    def __init__(self):
        self.crc = 0

    def _add(self, val):
        self.crc = ((self.crc << 1) + val + ((self.crc >> 31) & 1)) & MASK

    def xfer(self, data):
        whole = len(data) // 4
        for i in range(whole):
            self._add(int.from_bytes(data[i * 4:i * 4 + 4], "big"))
        rest = data[whole * 4:]
        if rest:
            val = rest[0]
            if len(rest) >= 2:
                val += rest[1] << 8
            if len(rest) >= 3:
                val += rest[2] << 16
            self._add(val)

    def value(self):
        return int.from_bytes((self.crc & MASK).to_bytes(4, "little"), "big")


def ini_lines(raw):
    """Split a file the way INI::readLine hands lines to the checksum.

    A semicolon terminates the string in place and the rest of the line is
    never seen by strlen(); control characters become spaces; the trailing
    fragment of a file with no final newline is still one line.
    """
    lines = []
    buf = bytearray()
    i = 0
    n = len(raw)
    while i < n:
        b = raw[i]
        i += 1
        if b == 0x0A:
            lines.append(bytes(buf))
            buf = bytearray()
        elif b == 0x3B:
            while i < n and raw[i] != 0x0A:
                i += 1
            if i < n:
                i += 1
            lines.append(bytes(buf))
            buf = bytearray()
        else:
            buf.append(0x20 if 0 < b < 32 else b)
    lines.append(bytes(buf))
    return lines


def read_big(path):
    """Return [(name, offset, size)] for a BIGF archive, or None."""
    with open(path, "rb") as handle:
        header = handle.read(16)
        if len(header) < 16 or header[0:4] not in (b"BIGF", b"BIG4"):
            return None
        count = struct.unpack(">I", header[8:12])[0]
        blob = handle.read()
    entries = []
    pos = 0
    for _ in range(count):
        if pos + 8 > len(blob):
            break
        offset, size = struct.unpack(">II", blob[pos:pos + 8])
        pos += 8
        end = blob.find(b"\x00", pos)
        if end < 0:
            break
        entries.append((blob[pos:end].decode("latin-1"), offset, size))
        pos = end + 1
    return entries


class GameFiles(object):
    """The merged archive tree, with the engine's first-archive-wins rule."""

    def __init__(self, root, community_patch=None):
        self.files = {}
        self.dirs = {}
        # Held back until mount_patch(): the engine mounts it from loadMods(),
        # partway through the INI loads rather than alongside the game folder.
        self._pending_patch = community_patch
        for display, real in self._archives(root):
            # The engine skips a duplicate INIZH.big in Data\\INI; some SKUs
            # shipped two, and the one in Data\\INI would otherwise win.
            if display.lower().replace("/", "\\").endswith("data\\ini\\inizh.big"):
                continue
            self._mount(real)

    @staticmethod
    def _archives(root):
        found = []
        for dirpath, _, filenames in os.walk(root):
            for name in filenames:
                if name.lower().endswith(".big"):
                    real = os.path.join(dirpath, name)
                    rel = os.path.relpath(real, root).replace(os.sep, "/")
                    found.append(("./" + rel if "/" not in rel else rel, real))
        # The engine walks a case-insensitive sorted set, so mount order is
        # stable across platforms and so is which archive wins a duplicate.
        found.sort(key=lambda pair: pair[0].lower())
        return found

    def _mount(self, real):
        entries = read_big(real)
        if entries is None:
            return
        for name, offset, size in entries:
            key = name.replace("/", "\\")
            directory, _, filename = key.rpartition("\\")
            full = (directory + "\\" + filename).lower() if directory else filename.lower()
            self.files.setdefault(full, []).append((real, offset, size))
            self.dirs.setdefault(directory.lower(), set()).add(filename.lower())

    def mount_patch(self):
        """Mount the community patch ahead of the retail archives.

        Its "500_900_" prefix sorts before every retail archive name, which is
        what the addon-number convention is for: digits come before letters, so
        the patch wins every file it defines and later packs slot in by number.
        """
        if self._pending_patch is None:
            return
        retail_files, retail_dirs = self.files, self.dirs
        self.files, self.dirs = {}, {}
        self._mount(self._pending_patch)
        for key, locations in retail_files.items():
            self.files.setdefault(key, []).extend(locations)
        for key, names in retail_dirs.items():
            self.dirs.setdefault(key, set()).update(names)
        self._pending_patch = None

    def exists(self, path):
        return path.replace("/", "\\").lower() in self.files

    def read(self, path):
        found = self.files.get(path.replace("/", "\\").lower())
        if not found:
            return None
        real, offset, size = found[0]
        with open(real, "rb") as handle:
            handle.seek(offset)
            return handle.read(size)

    def source(self, path):
        found = self.files.get(path.replace("/", "\\").lower())
        return os.path.basename(found[0][0]) if found else None

    def ini_files_under(self, directory):
        wanted = directory.replace("/", "\\").lower().rstrip("\\")
        found = []
        for key, names in self.dirs.items():
            if key == wanted or key.startswith(wanted + "\\"):
                found.extend(key + "\\" + name for name in names if name.endswith(".ini"))
        found.sort(key=str.lower)
        return found


def load_directory(files, name, crc, loaded):
    """INI::loadFileDirectory: the .ini file first, then the directory."""
    ini_file = name if name.lower().endswith(".ini") else name + ".ini"
    ini_dir = name[:-4] if name.lower().endswith(".ini") else name

    def load(path):
        raw = files.read(path)
        if raw is None:
            return 0
        for line in ini_lines(raw):
            crc.xfer(line)
        loaded.append(path)
        return 1

    read = 0
    if files.exists(ini_file):
        read += load(ini_file)

    prefix = ini_dir.replace("/", "\\").lower() + "\\"
    contents = files.ini_files_under(ini_dir)
    # Files in the directory itself load before files in its subdirectories.
    for path in [p for p in contents if "\\" not in p[len(prefix):]]:
        read += load(path)
    for path in [p for p in contents if "\\" in p[len(prefix):]]:
        read += load(path)

    if read == 0:
        raise RuntimeError("no INI files found for " + name)
    return read


def main(argv):
    if len(argv) not in (2, 3):
        sys.stderr.write("usage: ini_crc.py <game folder> [community patch .big]\n")
        return 2

    root = argv[1]
    if not os.path.isdir(root):
        sys.stderr.write("not a directory: %s\n" % root)
        return 2

    patch = argv[2] if len(argv) == 3 else None
    if patch and not os.path.isfile(patch):
        sys.stderr.write("not a file: %s\n" % patch)
        return 2

    duplicate = None
    for dirpath, _, filenames in os.walk(root):
        for name in filenames:
            joined = os.path.join(dirpath, name).replace(os.sep, "/").lower()
            if joined.endswith("data/ini/inizh.big"):
                duplicate = os.path.join(dirpath, name)

    files = GameFiles(root, community_patch=patch)
    crc = XferCRC()
    loaded = []
    checkpoints = {}

    for default_dir, override_dir in CRC_LOAD_ORDER:
        if default_dir == "__mount_patch__":
            files.mount_patch()
            continue
        if default_dir == "__checkpoint_weather__":
            checkpoints["weather"] = crc.value()
            continue
        if default_dir == "__checkpoint_object__":
            checkpoints["object"] = crc.value()
            continue
        for directory in (default_dir, override_dir):
            if directory:
                load_directory(files, directory, crc, loaded)

    sources = sorted({files.source(path) for path in loaded})
    weather_ok = checkpoints.get("weather") == CHECKPOINT_WEATHER
    object_ok = checkpoints.get("object") == CHECKPOINT_OBJECT

    print("folder          : %s" % os.path.abspath(root))
    print("community patch : %s" % (os.path.abspath(patch) if patch else "none"))
    print("INI files hashed: %d, from %s" % (len(loaded), ", ".join(sources)))
    print("checkpoints     : weather %08X (%s), object %08X (%s)" % (
        checkpoints.get("weather", 0), "ok" if weather_ok else "UNEXPECTED",
        checkpoints.get("object", 0), "ok" if object_ok else "UNEXPECTED"))
    if duplicate:
        print("duplicate INIZH : %s -- skipped, as the engine does" % duplicate)
    print("ini_crc         : %u (0x%08X)" % (crc.value(), crc.value()))
    print("                  4272612339 = untouched retail data")
    print("                  2180732466 = what PC GeneralsOnline lobbies report")

    if patch:
        print("")
        print("The retail checkpoints are not expected to hold with a patch mounted --")
        print("the patch redefines the data they were computed over. 2180732466 is the")
        print("value to compare against here.")
    elif not (weather_ok and object_ok):
        print("")
        print("The checkpoint values the engine hardcodes for retail data did not come")
        print("out, so this folder's INI is not the set those constants describe and the")
        print("checksum above should not be compared against the known values.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
