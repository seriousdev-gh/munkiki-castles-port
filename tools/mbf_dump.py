# mbf_dump.py - independent MBF decoder per docs/formats-and-mechanics.md 2.2-2.3
# Prints, per level: id, spawn(x,y,z face), counts, and a per-layer ASCII map.
import sys, zipfile, glob

jar = glob.glob('ref/*[!)]jar')[0]
D = zipfile.ZipFile(jar).read('MBF')
assert len(D) == 1312, len(D)
b1 = list(D[:13]); n = D[13]; L = list(D[14:14+n]); bw = D[14+n:]
print('b1=', b1, 'levels=', n)

class BR:
    def __init__(s, b): s.b, s.i = b, 0
    def __call__(s, k):
        v = 0
        for j in range(k):
            v |= ((s.b[s.i >> 3] >> (s.i & 7)) & 1) << j
            s.i += 1
        return v

lv = {}
pos = 0
for ln in L:
    blk = bw[pos:pos+ln]; pos += ln
    lv[blk[0]] = blk[1:]

for lid in sorted(lv):
    r = BR(lv[lid])
    face = r(2); par = r(16)
    cells = {}
    counts = {}; bx = by = bz = -1
    for t in range(13):
        if not r(1):
            continue
        val = b1[t]
        if 480 & (1 << t):          # single placement
            x, y, z = r(4), r(3), r(4)
            cells[(x, y, z)] = cells.get((x, y, z), 0) | val
            if val == 40: bx, by, bz = x, y, z
        else:                        # 8 horizontal layers
            for y in range(8):
                if not r(1):
                    continue
                x0 = r(4); dx = r(4); z0 = r(4); dz = r(4)
                if dx + dz < 2:      # full rect, no per-cell bits
                    for z in range(z0, z0 + dz + 1):
                        for x in range(x0, x0 + dx + 1):
                            cells[(x, y, z)] = cells.get((x, y, z), 0) | val
                else:
                    for z in range(z0, z0 + dz + 1):
                        for x in range(x0, x0 + dx + 1):
                            if r(1):
                                cells[(x, y, z)] = cells.get((x, y, z), 0) | val
        counts[t] = counts.get(t, 0) + 1
    notes = sum(1 for v in cells.values() if v == 8 or (v & 56) == 8)
    items = sum(1 for v in cells.values() if (v & 56) and (v & 7) == 0)
    blocks = sum(1 for v in cells.values() if (v & 7))
    print(f'level {lid}: face={face} par={par} spawn=({bx},{by},{bz}) cells={len(cells)} blocks={blocks} items={items}')

# full map dump mode: python tools/mbf_dump.py <levelid>
if len(sys.argv) > 1:
    lid = int(sys.argv[1])
    r = BR(lv[lid]); r(18)
    cells = {}
    for t in range(13):
        if not r(1): continue
        val = b1[t]
        if 480 & (1 << t):
            x, y, z = r(4), r(3), r(4)
            cells[(x,y,z)] = cells.get((x,y,z),0) | val
        else:
            for y in range(8):
                if not r(1): continue
                x0=r(4); dx=r(4); z0=r(4); dz=r(4)
                if dx+dz < 2:
                    for z in range(z0,z0+dz+1):
                        for x in range(x0,x0+dx+1): cells[(x,y,z)] = cells.get((x,y,z),0) | val
                else:
                    for z in range(z0,z0+dz+1):
                        for x in range(x0,x0+dx+1):
                            if r(1): cells[(x,y,z)] = cells.get((x,y,z),0) | val
    for y in range(8):
        print(f'--- y={y}')
        for z in range(16):
            print(''.join({0:'.',1:'#',2:'B',3:'X',4:'=',5:'|',6:'!',8:'o',9:'o',11:'O',16:'K',19:'K',24:'k',32:'L',35:'L',40:'@',64:'~',65:'~',128:'~',129:'~',131:'O',144:'~',192:'~',193:'~'}.get(cells.get((x,y,z),0),'?') for x in range(16)))
