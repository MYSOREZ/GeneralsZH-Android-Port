#!/usr/bin/env python3
"""Compare two replay event records (.gamestats.json or .json.gz).

The PC GeneralsOnline client writes one with
    GeneralsOnlineZH_60.exe -headless -replay <name>.rep -exportStats
(Replays/<name>.gamestats.json.gz); the Android launcher's Replay check writes the
same record (Replays/<name>.gamestats.json). Both come from the simulation itself, so
the first event on which they disagree -- a unit built on another frame or at another
spot, money differing at a snapshot, a kill that one side has -- dates the divergence
to the frame, where the replay's checksum only says "somewhere in these 100 frames".

Usage: compare_gamestats.py PC.gamestats.json.gz ANDROID.gamestats.json [--upto FRAME]
"""
import gzip, json, sys


def load(path):
    opener = gzip.open if path.endswith('.gz') else open
    with opener(path, 'rt', encoding='utf-8') as f:
        return json.load(f)


def events(doc, upto):
    out = []
    for key, value in doc.items():
        if key.endswith('Events') and isinstance(value, list):
            for ev in value:
                if isinstance(ev, dict) and ev.get('frame', 0) <= upto:
                    out.append((ev.get('frame', 0), key, json.dumps(ev, sort_keys=True)))
    # timeSeries holds one snapshot per ~30 logic frames with no frame numbers; the
    # snapshot index times 30 is the frame to within the cadence.
    ts = doc.get('timeSeries')
    if isinstance(ts, dict):
        for p in ts.get('players', []):
            for k2, series in p.items():
                if isinstance(series, list):
                    for i, v in enumerate(series):
                        if i * 30 <= upto:
                            out.append((i * 30, 'snapshot#%d.player%s.%s' % (i, p.get('index'), k2), json.dumps(v)))
    out.sort()
    return out


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    upto = 10 ** 9
    if '--upto' in sys.argv:
        upto = int(sys.argv[sys.argv.index('--upto') + 1])
    pc, an = load(args[0]), load(args[1])
    # Our record stops shortly after the first checksum mismatch; compare up to where both run.
    ep, ea = events(pc, upto), events(an, upto)
    last_an = max((e[0] for e in ea), default=0)
    ep = [e for e in ep if e[0] <= last_an]
    sp, sa = set(ep), set(ea)
    only_pc = sorted(sp - sa)
    only_an = sorted(sa - sp)
    print('events compared up to frame %d: PC %d, Android %d' % (last_an, len(ep), len(ea)))
    first = min([e[0] for e in only_pc + only_an], default=None)
    if first is None:
        print('identical')
        return
    print('first difference at frame %d' % first)
    for tag, lst in (('PC only', only_pc), ('Android only', only_an)):
        print('--- %s (first 25)' % tag)
        for e in lst[:25]:
            print('  frame %6d %-28s %s' % e)


if __name__ == '__main__':
    main()
