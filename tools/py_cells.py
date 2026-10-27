import sys, zipfile, glob
D = zipfile.ZipFile(glob.glob('ref/*[!)]jar')[0]).read('MBF')
b1=list(D[:13]); n=D[13]; L=list(D[14:14+n]); bw=D[14+n:]
class BR:
    def __init__(s,b): s.b,s.i=b,0
    def __call__(s,k):
        v=0
        for j in range(k): v|=((s.b[s.i>>3]>>(s.i&7))&1)<<j; s.i+=1
        return v
lv={}; pos=0
for ln in L:
    blk=bw[pos:pos+ln]; pos+=ln; lv[blk[0]]=blk[1:]
lid=int(sys.argv[1]); r=BR(lv[lid]); r(18); cells={}
for t in range(13):
    if not r(1): continue
    val=b1[t]
    if 480&(1<<t):
        x,y,z=r(4),r(3),r(4); cells[(x,y,z)]=cells.get((x,y,z),0)|val
    else:
        for y in range(8):
            if not r(1): continue
            x0=r(4);dx=r(4);z0=r(4);dz=r(4)
            if dx+dz<2:
                for z in range(z0,z0+dz+1):
                    for x in range(x0,x0+dx+1): cells[(x,y,z)]=cells.get((x,y,z),0)|val
            else:
                for z in range(z0,z0+dz+1):
                    for x in range(x0,x0+dx+1):
                        if r(1): cells[(x,y,z)]=cells.get((x,y,z),0)|val
for y in range(8):
    for z in range(16):
        for x in range(16):
            v=cells.get((x,y,z),0)
            if v: print(x,y,z,v)
