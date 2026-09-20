#!/usr/bin/env python3
# GeneralsX @feature Android port 20/09/2026
#
# Dump a Zero Hour .map file's sides, teams and scripts as text.
#
# Why this exists: when a cross-play checksum divergence shows up "only on one
# map", the first question is what that map's scripts actually do in the frames
# before the first checkpoint -- how many objects they create or delete, and how
# hard they lean on the logic RNG. Guessing costs a round of testing per map;
# reading the file costs nothing. See
# docs/WORKDIR/lessons/LESSON-cross-play-desync-method.md.
#
# The format is not reverse engineered. It is the engine's own:
#   "EAR\0" + uncompressed size + RefPack   -- Core/Libraries/Source/Compression
#   "CkMp" + name table + chunk tree         -- Common/System/DataChunk.cpp
#   Script/Condition/Action/Parameter layout  -- ScriptEngine/Scripts.cpp
#
# Usage:
#   scripts/qa/replay/dump-map-scripts.py "<map>.map"
#   scripts/qa/replay/dump-map-scripts.py "<map>.map" | grep SET_RANDOM_TIMER
#
# The tail of the output is a tally of every script action the map uses, which
# is the quickest read on whether a map is a neutral test harness.

import sys
import struct


def refpack(data):
    hdr = (data[0] << 8) | data[1]
    pos = 2
    big = bool(hdr & 0x8000)
    w = 4 if big else 3
    if hdr & 0x0100:
        pos += w
    size = 0
    for i in range(w):
        size = (size << 8) | data[pos + i]
    pos += w
    out = bytearray()
    while pos < len(data):
        b0 = data[pos]; pos += 1
        if not (b0 & 0x80):
            b1 = data[pos]; pos += 1
            run = b0 & 0x03
            out += data[pos:pos+run]; pos += run
            ref = ((b0 & 0x60) << 3) | b1
            ln = ((b0 & 0x1c) >> 2) + 3
            src = len(out) - ref - 1
            for i in range(ln):
                out.append(out[src + i])
        elif not (b0 & 0x40):
            b1 = data[pos]; b2 = data[pos+1]; pos += 2
            run = (b1 >> 6) & 0x03
            out += data[pos:pos+run]; pos += run
            ref = ((b1 & 0x3f) << 8) | b2
            ln = (b0 & 0x3f) + 4
            src = len(out) - ref - 1
            for i in range(ln):
                out.append(out[src + i])
        elif not (b0 & 0x20):
            b1 = data[pos]; b2 = data[pos+1]; b3 = data[pos+2]; pos += 3
            run = b0 & 0x03
            out += data[pos:pos+run]; pos += run
            ref = ((b0 & 0x10) << 12) | (b1 << 8) | b2
            ln = (((b0 & 0x0c) << 6) | b3) + 5
            src = len(out) - ref - 1
            for i in range(ln):
                out.append(out[src + i])
        else:
            run = ((b0 & 0x1f) << 2) + 4
            if run <= 0x70:
                out += data[pos:pos+run]; pos += run
            else:
                run = b0 & 0x03
                out += data[pos:pos+run]; pos += run
                break
    if len(out) != size:
        sys.stderr.write("WARN: got %d expected %d\n" % (len(out), size))
    return bytes(out)

def load(path):
    raw = open(path, 'rb').read()
    if raw[:4] == b'EAR\0':
        return refpack(raw[8:])
    return raw



DT = {0:'BOOL',1:'INT',2:'REAL',3:'ASCII',4:'UNI'}

class R:
    def __init__(s, d, p=0): s.d=d; s.p=p
    def i32(s):
        v=struct.unpack_from('<i',s.d,s.p)[0]; s.p+=4; return v
    def u32(s):
        v=struct.unpack_from('<I',s.d,s.p)[0]; s.p+=4; return v
    def u16(s):
        v=struct.unpack_from('<H',s.d,s.p)[0]; s.p+=2; return v
    def u8(s):
        v=s.d[s.p]; s.p+=1; return v
    def f32(s):
        v=struct.unpack_from('<f',s.d,s.p)[0]; s.p+=4; return v
    def astr(s):
        n=s.u16(); v=s.d[s.p:s.p+n].decode('latin-1'); s.p+=n; return v
    def ustr(s):
        n=s.u16(); v=s.d[s.p:s.p+n*2].decode('utf-16-le','replace'); s.p+=n*2; return v

def read_toc(d):
    assert d[:4]==b'CkMp'
    r=R(d,4); cnt=r.i32(); names={}
    for _ in range(cnt):
        ln=r.u8(); nm=d[r.p:r.p+ln].decode('latin-1'); r.p+=ln; names[r.u32()]=nm
    return names, r.p

def hdr(d,p):
    cid,ver,size=struct.unpack_from('<IHi',d,p); return cid,ver,size,p+10

def rdict(r,names):
    n=r.u16(); out=[]
    for _ in range(n):
        kt=r.i32(); t=kt&0xff; k=names.get(kt>>8,'?%d'%(kt>>8))
        if t==0: v=r.u8()
        elif t==1: v=r.i32()
        elif t==2: v=r.f32()
        elif t==3: v=r.astr()
        elif t==4: v=r.ustr()
        else: raise Exception('bad dict type %d at %d'%(t,r.p))
        out.append((k,DT.get(t,t),v))
    return out

def rparam(r,names):
    pt=r.i32()
    if pt==22:  # COORD3D -- verified below by name table if needed
        return ('coord', r.f32(), r.f32(), r.f32())
    i=r.i32(); f=r.f32(); s=r.astr()
    return (pt,i,f,s)

def parse_paramlist(r,names,n,coord_type):
    out=[]
    for _ in range(n):
        pt=r.i32()
        if pt==coord_type:
            out.append('coord(%g,%g,%g)'%(r.f32(),r.f32(),r.f32()))
        else:
            i=r.i32(); f=r.f32(); s=r.astr()
            bits=[]
            if s: bits.append(repr(s))
            if i: bits.append('i=%d'%i)
            if f: bits.append('f=%g'%f)
            out.append('/'.join(bits) if bits else '0')
    return out

def walk_scripts(d,names,start,end,depth,out,coord_type,ctx):
    p=start
    while p+10<=end:
        cid,ver,size,body=hdr(d,p)
        nm=names.get(cid,'?%d'%cid)
        if size<0 or body+size>end:
            out.append('%sBAD %s size=%d'%('  '*depth,nm,size)); return
        r=R(d,body)
        if nm=='PlayerScriptsList':
            walk_scripts(d,names,body,body+size,depth,out,coord_type,ctx)
        elif nm=='ScriptList':
            ctx['list']+=1
            out.append('=== ScriptList #%d (player %d) ==='%(ctx['list'],ctx['list']-1))
            walk_scripts(d,names,body,body+size,depth,out,coord_type,ctx)
        elif nm=='ScriptGroup':
            gname=r.astr(); act=r.u8(); sub=r.u8()
            out.append('%s[Group] %r active=%d subroutine=%d'%('  '*depth,gname,act,sub))
            walk_scripts(d,names,r.p,body+size,depth+1,out,coord_type,ctx)
        elif nm=='Script':
            sname=r.astr(); com=r.astr(); ccom=r.astr(); acom=r.astr()
            active=r.u8(); oneshot=r.u8(); easy=r.u8(); normal=r.u8(); hard=r.u8(); subr=r.u8()
            delay = r.i32() if ver>=2 else 0
            ctx['scripts']+=1
            out.append('%sSCRIPT %r active=%d oneShot=%d sub=%d delay=%d'%('  '*depth,sname,active,oneshot,subr,delay))
            walk_scripts(d,names,r.p,body+size,depth+1,out,coord_type,ctx)
        elif nm in ('OrCondition',):
            out.append('%sOR:'%('  '*depth))
            walk_scripts(d,names,body,body+size,depth+1,out,coord_type,ctx)
        elif nm=='Condition':
            ctype=r.i32(); key=r.i32()>>8 if ver>=4 else None
            kname=names.get(key,'?') if key is not None else '?'
            n=r.i32(); parms=parse_paramlist(r,names,n,coord_type)
            out.append('%sIF %s(%s)'%('  '*depth,kname,', '.join(parms)))
        elif nm in ('ScriptAction','ScriptActionFalse'):
            atype=r.i32(); key=r.i32()>>8 if ver>=2 else None
            kname=names.get(key,'?') if key is not None else '?'
            n=r.i32(); parms=parse_paramlist(r,names,n,coord_type)
            tag='DO' if nm=='ScriptAction' else 'ELSE'
            ctx['actions'][kname]=ctx['actions'].get(kname,0)+1
            out.append('%s%s %s(%s)'%('  '*depth,tag,kname,', '.join(parms)))
        p=body+size

def main(path, coord_type):
    d=load(path); names,off=read_toc(d)
    p=off
    out=[]; ctx={'list':0,'scripts':0,'actions':{}}
    while p+10<=len(d):
        cid,ver,size,body=hdr(d,p); nm=names.get(cid,'?%d'%cid)
        if nm=='SidesList':
            r=R(d,body); nsides=r.i32()
            sides=[]
            for i in range(nsides):
                dd=rdict(r,names)
                nb=r.i32(); bl=[]
                for j in range(nb):
                    bn=r.astr(); tn=r.astr(); x=r.f32(); y=r.f32(); z=r.f32(); ang=r.f32()
                    ib=r.u8(); nr=r.i32()
                    if ver>=3:
                        sc=r.astr(); hp=r.i32(); wh=r.u8(); un=r.u8(); rp=r.u8()
                    bl.append(tn)
                sides.append((dict((k,v) for k,t,v in dd), bl))
            nteams=r.i32() if ver>=2 else 0
            teams=[rdict(r,names) for _ in range(nteams)]
            print("=== SIDES (%d) ==="%nsides)
            for i,(dd,bl) in enumerate(sides):
                print("  side %d: name=%r human=%s faction=%r buildlist=%d"%(
                    i, dd.get('playerName'), dd.get('playerIsHuman'), dd.get('playerFaction'), len(bl)))
            print("=== TEAMS: %d ==="%nteams)
            walk_scripts(d,names,r.p,body+size,0,out,coord_type,ctx)
        p=body+size
    print("\n".join(out))
    print("\n=== TOTALS: %d script lists, %d scripts ==="%(ctx['list'],ctx['scripts']))
    for k,v in sorted(ctx['actions'].items(), key=lambda kv:-kv[1]):
        print("  %4d  %s"%(v,k))

if __name__=='__main__':
    main(sys.argv[1], int(sys.argv[2]) if len(sys.argv)>2 else 22)
