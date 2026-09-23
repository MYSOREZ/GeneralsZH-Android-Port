#!/usr/bin/env python3
# GeneralsX @feature Android port 23/09/2026 Put another machine's checksums into an
# every-frame replay (see rep_crc_every_frame.py).
#
# The PC client pauses at the first frame whose checksum differs from the recording and
# shows only that one. Writing the PC's own value for that frame into the copy moves the
# stop one frame on, so each PC run yields the next frame's value. On the phone, the same
# substitution makes it mismatch at exactly that frame and dump the checksum's words.
#
#   rep_set_crc.py IN.rep OUT.rep FRAME=HEX [FRAME=HEX ...]
# FRAME is the frame the checksum describes, as in the PC's "Frame:N".
import struct, sys
sys.path.insert(0, __import__('os').path.dirname(__file__))
import rep_crc_every_frame as rce

def main():
    src, dst = sys.argv[1:3]
    want = {int(f): int(h, 16) for f, h in (a.split('=') for a in sys.argv[3:])}
    data = open(src, 'rb').read()
    off, recs = rce.find_records(data)
    mtype, player = rce.crc_type(recs)
    out, done = [], set()
    for f, t, p, r in recs:
        if t == mtype and f - 1 in want:
            print('frame %d: %08X -> %08X' % (f - 1, struct.unpack_from('<I', r, 17)[0], want[f - 1]))
            r = rce.crc_record(f, t, p, want[f - 1])
            done.add(f - 1)
        out.append(r)
    missing = set(want) - done
    if missing:
        raise SystemExit('no checksum record for frames %s' % sorted(missing))
    open(dst, 'wb').write(data[:off] + b''.join(out))

if __name__ == '__main__':
    main()
