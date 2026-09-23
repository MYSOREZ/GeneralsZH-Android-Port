import struct,numpy as np
F=np.float32
d=open('ABSUPPLYCT.W3D','rb').read()
def chunks(buf,off,end,depth,out):
    while off<end:
        cid,sz=struct.unpack('<II',buf[off:off+8]); sub=sz>>31; sz&=0x7fffffff
        out.append((depth,cid,off+8,sz))
        if sub: chunks(buf,off+8,off+8+sz,depth+1,out)
        off+=8+sz
    return out
L=chunks(d,0,len(d),0,[])
piv=[]; chans=[]; hdr=None
for dep,cid,o,sz in L:
    if cid==0x102:
        for i in range(sz//60):
            b=d[o+i*60:o+i*60+60]
            name=b[:16].split(b'\0')[0].decode(); parent=struct.unpack('<i',b[16:20])[0]
            t=struct.unpack('<3f',b[20:32]); q=struct.unpack('<4f',b[44:60])
            piv.append((name,parent,t,q))
    if cid==0x201:
        hdr=struct.unpack('<I16s16sII',d[o:o+44])
    if cid==0x202:
        ff,lf,vl,fl,pv,pad=struct.unpack('<6H',d[o:o+12]); n=(sz-12)//4
        data=struct.unpack('<%df'%n,d[o+12:o+12+4*n]); chans.append((ff,lf,vl,fl,pv,data))
print('anim',hdr)
for i,p in enumerate(piv): print(i,p[0],p[1])
for c in chans: print('chan first',c[0],'last',c[1],'len',c[2],'flags',c[3],'pivot',c[4],'first',c[5][:c[2]])
def qmat(q):
    q=[F(x) for x in q]
    m=np.zeros((3,4),dtype=np.float32)
    m[0,0]=F(1.0-2.0*float(F(F(q[1]*q[1])+F(q[2]*q[2]))))
    m[0,1]=F(2.0*float(F(F(q[0]*q[1])-F(q[2]*q[3]))))
    m[0,2]=F(2.0*float(F(F(q[2]*q[0])+F(q[1]*q[3]))))
    m[1,0]=F(2.0*float(F(F(q[0]*q[1])+F(q[2]*q[3]))))
    m[1,1]=F(1.0-float(F(F(2.0)*F(F(q[2]*q[2])+F(q[0]*q[0])))))  # 2.0f*(float) -> float
    m[1,2]=F(2.0*float(F(F(q[1]*q[2])-F(q[0]*q[3]))))
    m[2,0]=F(2.0*float(F(F(q[2]*q[0])-F(q[1]*q[3]))))
    m[2,1]=F(2.0*float(F(F(q[1]*q[2])+F(q[0]*q[3]))))
    m[2,2]=F(1.0-2.0*float(F(F(q[1]*q[1])+F(q[0]*q[0]))))
    return m
def mul(A,B):
    R=np.zeros((3,4),dtype=np.float32)
    for i in range(3):
        for j in range(4):
            s=F(F(F(A[i,0]*B[0,j])+F(A[i,1]*B[1,j]))+F(A[i,2]*B[2,j]))
            if j==3: s=F(s+A[i,3])
            R[i,j]=s
    return R
def postmul(A,B):
    R=A.copy()
    for i in range(3):
        row=A[i]
        tx=F(F(F(row[0]*B[0,0])+F(row[1]*B[1,0]))+F(row[2]*B[2,0]))
        ty=F(F(F(row[0]*B[0,1])+F(row[1]*B[1,1]))+F(row[2]*B[2,1]))
        tz=F(F(F(row[0]*B[0,2])+F(row[1]*B[1,2]))+F(row[2]*B[2,2]))
        tw=F(F(F(row[0]*B[0,3])+F(row[1]*B[1,3]))+F(row[2]*B[2,3]))
        R[i,0]=tx;R[i,1]=ty;R[i,2]=tz;R[i,3]=F(row[3]+tw)
    return R
def translate(M,t):
    M=M.copy()
    for i in range(3):
        M[i,3]=F(M[i,3]+F(F(F(M[i,0]*t[0])+F(M[i,1]*t[1]))+F(M[i,2]*t[2])))
    return M
I=np.zeros((3,4),dtype=np.float32); I[0,0]=I[1,1]=I[2,2]=1
base=[]
for name,parent,t,q in piv:
    B=translate(I,[F(x) for x in t]); B=postmul(B,qmat(q)); base.append(B)
# animation channel lookup at frame 0
anim={}
for ff,lf,vl,fl,pv,data in chans:
    if ff<=0<=lf: anim.setdefault(pv,{})[fl]=data[0:vl]
T=[None]*len(piv); T[0]=I.copy()
for i in range(1,len(piv)):
    p=piv[i][1]
    T[i]=mul(T[p],base[i])
    a=anim.get(i,{})
    tr=[F(a[0][0]) if 0 in a else F(0),F(a[1][0]) if 1 in a else F(0),F(a[2][0]) if 2 in a else F(0)]
    T[i]=translate(T[i],tr)
    if 6 in a: T[i]=postmul(T[i],qmat(a[6]))
for i,(name,parent,t,q) in enumerate(piv):
    if 'dock' in name.lower():
        x,y,z=T[i][0,3],T[i][1,3],T[i][2,3]
        h=lambda v: '%08X'%struct.unpack('<I',struct.pack('<f',v))[0]
        print(name, h(x),h(y),h(z),(float(x),float(y),float(z)))
