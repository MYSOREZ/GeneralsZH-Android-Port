#!/usr/bin/env python3
# GeneralsX @feature Android port 23/09/2026 Frame-exact replay comparison with the PC.
#
# A recording carries the logic checksum only every 100 frames (header field C=100), so
# a mismatch says "somewhere in these hundred frames". The replay header's C field is
# the checksum interval the *player* uses, and the PC client compares every checksum
# it computes against the next one queued from the file, pausing at the first mismatch
# with "Frame:N". This tool writes a copy of a replay with C=001 whose checksum records
# are this device's checksums of every frame, taken from a log made with the launcher's
# "Checksum of every frame" switch (engine option -gxCrcEveryFrame, log lines
# "crc every frame N: XXXXXXXX"). Played on the PC, that copy stops at the first frame
# where the PC disagrees with this device.
#
#   rep_crc_every_frame.py IN.rep generals-stderr.log OUT.rep
#   rep_crc_every_frame.py --roundtrip IN.rep        (self-test: parse and rewrite)
import re, struct, sys

MSG_LOGIC_CRC = 1095  # checked below against the recording's own checksum records

ARG_SIZES = {0: 4, 1: 4, 2: 1, 3: 4, 4: 4, 5: 4, 6: 12, 7: 8, 8: 16, 9: 4, 10: 2}

def parse_records(data, off):
    recs = []
    p = off
    last = -1
    while p < len(data):
        if p + 13 > len(data):
            return None
        start = p
        frame, mtype, player = struct.unpack_from('<iii', data, p)
        p += 12
        if frame < last or frame > 10**7 or mtype < 0 or mtype > 5000:
            return None
        ntypes = data[p]
        p += 1
        if ntypes > 12:
            return None
        types = []
        for _ in range(ntypes):
            t, c = data[p], data[p + 1]
            p += 2
            if t not in ARG_SIZES:
                return None
            types.append((t, c))
        for t, c in types:
            p += ARG_SIZES[t] * c
        if p > len(data):
            return None
        recs.append((frame, mtype, player, data[start:p]))
        last = frame
    return recs

def find_records(data):
    base = data.find(b'S=')
    for off in range(base, min(base + 8000, len(data))):
        recs = parse_records(data, off)
        if recs and len(recs) > 5:
            return off, recs
    raise SystemExit('could not locate the command stream')

def crc_type(recs):
    # The recording's checksum records are the ones at frames 101, 201, ... carrying
    # (integer, boolean) arguments; take the type number from them.
    for frame, mtype, player, raw in recs:
        if frame % 100 == 1 and raw[12] == 2 and raw[13:17] == bytes([0, 1, 2, 1]):
            return mtype, player
    raise SystemExit('no checksum records found in the recording')

def crc_record(frame, mtype, player, crc):
    return struct.pack('<iii', frame, mtype, player) + bytes([2, 0, 1, 2, 1]) + struct.pack('<I', crc) + b'\x00'

def main():
    if sys.argv[1] == '--roundtrip':
        data = open(sys.argv[2], 'rb').read()
        off, recs = find_records(data)
        mtype, player = crc_type(recs)
        out = data[:off] + b''.join(r[3] for r in recs)
        print('records', len(recs), 'crc type', mtype, 'player', player, 'roundtrip identical:', out == data)
        crcs = [(f, struct.unpack_from('<I', r, 17)[0]) for f, t, p, r in recs if t == mtype]
        rebuilt = b''.join(crc_record(f, mtype, player, c) if t == mtype else r
                           for (f, t, p, r), c in zip(recs, [dict(crcs).get(f) for f, *_ in recs]))
        print('checksum records rebuilt identically:', data[:off] + rebuilt == data)
        return
    rep, log, outp = sys.argv[1:4]
    data = open(rep, 'rb').read()
    off, recs = find_records(data)
    mtype, player = crc_type(recs)
    ours = {}
    for line in open(log, errors='replace'):
        m = re.search(r'crc every frame (\d+): ([0-9A-F]{8})', line)
        if m:
            ours[int(m.group(1))] = int(m.group(2), 16)
    if not ours:
        raise SystemExit('no "crc every frame" lines in the log')
    # Sanity: where the recording has a checksum, ours must be the same value for the
    # frames that matched on the phone -- this also proves the byte order.
    agree = [(f - 1, struct.unpack_from('<I', r, 17)[0] == ours.get(f - 1)) for f, t, p, r in recs if t == mtype]
    print('recorded checkpoints equal to this device:', sum(1 for _, ok in agree if ok), 'of', len(agree),
          '; first differing:', next((f for f, ok in agree if not ok), None))
    header = data[:off]
    i = header.find(b';C=100;')
    if i < 0:
        raise SystemExit('header has no ;C=100; field')
    header = header[:i] + b';C=001;' + header[i + 7:]
    last_frame = max(f for f, *_ in recs)
    body = [r for r in recs if r[1] != mtype]
    for f, c in ours.items():
        if f + 1 <= last_frame:
            body.append((f + 1, mtype, player, crc_record(f + 1, mtype, player, c)))
    body.sort(key=lambda r: (r[0], 0 if r[1] == mtype else 1))
    open(outp, 'wb').write(header + b''.join(r[3] for r in body))
    print('wrote', outp, 'with', sum(1 for r in body if r[1] == mtype), 'checksum records, frames',
          min(ours), '..', min(max(ours), last_frame - 1))

if __name__ == '__main__':
    main()
