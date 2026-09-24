#!/usr/bin/env python3
# Dump the conditions and actions of scripts in a .scb file (e.g. Data/Scripts/SkirmishScripts.scb)
# whose name contains a filter string, with action/condition names from Scripts.h.
#   scb_dump.py <file.scb> [name filter]
# Used to read what a skirmish AI script actually does when a GX trace names it (by=<script>).
import re,struct,sys
import os
HERE=os.path.dirname(os.path.abspath(__file__))
s=open(os.path.join(HERE,'../../../GeneralsMD/Code/GameEngine/Include/GameLogic/Scripts.h')).read()
def enum(name):
    i=s.index('enum '+name); body=s[i:s.index('};',i)]
    out=[]
    for line in body.split('\n')[1:]:
        line=line.split('//')[0].strip()
        m=re.match(r'([A-Z0-9_]+)\s*(=\s*(\w+))?\s*,?',line)
        if m and m.group(1): out.append(m.group(1))
    return out
acts=enum('ScriptActionType'); conds=enum('ConditionType')
d=open(sys.argv[1],'rb').read()
p=4; n=struct.unpack('<I',d[p:p+4])[0]; p+=4; names={}
for i in range(n):
    l=d[p]; p+=1; nm=d[p:p+l].decode(); p+=l; cid=struct.unpack('<I',d[p:p+4])[0]; p+=4; names[cid]=nm
top=p
def rstr(q):
    l=struct.unpack('<H',d[q:q+2])[0]; return d[q+2:q+2+l].decode('latin1'), q+2+l
def params(q,end,np):
    out=[]
    for k in range(np):
        t,iv,rv=struct.unpack('<iif',d[q:q+12]); q+=12
        st,q=rstr(q)
        if t==16: # COORD3D? skip
            pass
        out.append((t,iv,round(rv,3),st))
    return out
def walk(q,end,depth,want):
    while q<end:
        cid,ver,size=struct.unpack('<IHI',d[q:q+10]); nm=names.get(cid,'?'); body=q+10; nxt=body+size
        if nm=='Script':
            sn,_=rstr(body)
            if want in sn:
                print(' '*depth+'Script',sn)
                dump(body,nxt,depth+1)
        elif nm in ('PlayerScriptsList','ScriptList','ScriptGroup'):
            b=body
            if nm=='ScriptGroup':
                gn,b=rstr(body); b+=2
            walk(b,nxt,depth+1,want)
        q=nxt
def dump(body,end,depth):
    sn,q=rstr(body); cm,q=rstr(q); cc,q=rstr(q); ac,q=rstr(q)
    q+=4+ (4 if True else 0)
    # skip flags: active,oneshot,easy,normal,hard,subroutine (6 bytes) + delay int
    # find child chunks by scanning for known ids
    k=body
    while k<end-10:
        cid,ver,size=struct.unpack('<IHI',d[k:k+10])
        nm=names.get(cid)
        if nm in ('OrCondition','Condition','ScriptAction','ScriptActionFalse') and 0<size<5000 and k+10+size<=end:
            if nm=='OrCondition': print(' '*depth+'OR'); k+=10; continue
            b=k+10; t=struct.unpack('<i',d[b:b+4])[0]; b+=4
            if ver>=2: b+=4
            np_=struct.unpack('<i',d[b:b+4])[0]; b+=4
            try: ps=params(b,k+10+size,np_)
            except Exception as e: ps=str(e)
            lab=(conds[t] if nm=='Condition' else acts[t]) if 0<=t<len(acts) else t
            print(' '*depth+nm,lab,ps)
            k=k+10+size; continue
        k+=1
walk(top,len(d),0,sys.argv[2] if len(sys.argv)>2 else '')
