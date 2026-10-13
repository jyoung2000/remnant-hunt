#!/usr/bin/env python3
"""Dev tool: hit windows of a Remnant II creature's attack animations, from its limb speeds.

Asks the Linux build of r2read (the player's own Remnant II install) for the skeleton and the animations, runs forward
kinematics, and prints when each named bone moves fastest: the windows used in sheets/boss_attacks.json.
usage: attack_timing.py <anim,anim,...> <bone,bone,...>   (or: attack_timing.py names)
"""
import subprocess,struct,os,sys
import numpy as np
exe="build/linux/r2read"; r2=os.path.expanduser("~/mnt/Remnant2")
p=subprocess.Popen([exe,r2],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.DEVNULL)
def ask(c):
    p.stdin.write((c+"\n").encode());p.stdin.flush()
    h=p.stdout.readline().decode().strip()
    if not h.startswith("OK"): raise SystemExit(c+" "+h)
    return p.stdout.read(int(h.split()[-1]))
p.stdin.write(b"HELLO\n");p.stdin.flush();p.stdout.readline()
def bones(b,q):
    n=struct.unpack_from("<I",b,q)[0];q+=4;out=[]
    for i in range(n):
        l=struct.unpack_from("<H",b,q)[0];name=b[q+2:q+2+l].decode();q+=2+l
        par=struct.unpack_from("<i",b,q)[0];v=struct.unpack_from("<10f",b,q+4);q+=44
        out.append((name,par,v))
    return out,q
sk,_=bones(ask("SKEL\tRemnant2/Content/World_Jungle/Characters/Pan_Wolf/Mesh/Pan_Wolf_Skeleton.uasset"),4)
names=[s[0] for s in sk]
if len(sys.argv)>1 and sys.argv[1]=="names": print(names); raise SystemExit
def qm(q):
    x,y,z,w=q
    return np.array([[1-2*(y*y+z*z),2*(x*y-z*w),2*(x*z+y*w)],[2*(x*y+z*w),1-2*(x*x+z*z),2*(y*z-x*w)],[2*(x*z-y*w),2*(y*z+x*w),1-2*(x*x+y*y)]])
want=sys.argv[2].split(",")
for a in sys.argv[1].split(","):
    r=ask("ANIM\tRemnant2/Content/World_Jungle/Characters/Pan_Wolf/Animations/%s.uasset"%a)
    tr,sm,rate=struct.unpack_from("<IIf",r,4); t2b=struct.unpack_from("<%di"%tr,r,16); o=16+tr*4
    D=np.frombuffer(r,dtype=np.float32,count=sm*tr*10,offset=o).reshape(sm,tr,10)
    trk={b:i for i,b in enumerate(t2b)}
    res={w:[] for w in want}
    for s in range(sm):
        G=[None]*len(sk)
        for i,(n,par,v) in enumerate(sk):
            x=D[s,trk[i]] if i in trk else np.array(v,dtype=np.float32)
            R=qm(x[0:4]); T=x[4:7]
            if par<0: G[i]=(R,T)
            else:
                PR,PT=G[par]; G[i]=(PR@R,PR@T+PT)
        for w in want:
            res[w].append(G[names.index(w)][1].copy())
    print(a,"len %.2f"%((sm-1)/rate))
    for w in want:
        P=np.array(res[w]); V=np.linalg.norm(np.diff(P,axis=0),axis=1)*rate
        k=int(np.argmax(V)); pk=[i for i in range(1,len(V)-1) if V[i]>=V[i-1] and V[i]>=V[i+1] and V[i]>0.45*V[k]]; print("   peaks",w,[round(i/(sm-1),2) for i in pk][:8])
        # window where speed > 50% of peak around k
        lo=k; hi=k
        while lo>0 and V[lo-1]>0.5*V[k]: lo-=1
        while hi<len(V)-1 and V[hi+1]>0.5*V[k]: hi+=1
        print("  %-14s peak %.0f cm/s at %.2f  window %.2f-%.2f  pos@peak %s  maxY %.0f"%(w,V[k],k/(sm-1),lo/(sm-1),(hi+1)/(sm-1),np.round(P[k]).astype(int),P[:,1].max()))
p.kill()
