# Extracts all game assets from decompiled/ into src/assets.h (C arrays).
# Sources: MBF (levels), d (sounds+geometry+sprites+cm anims+level sfx map),
# imageV3 (monkey sprite sheets), PNG textures/HUD images, localization files.
import struct, zlib, sys, os

ROOT = os.path.join(os.path.dirname(__file__), '..')
D = lambda n: open(os.path.join(ROOT, 'decompiled', n), 'rb').read()

def png_to_rgba(raw):
    """Minimal PNG decoder -> (w, h, bytearray[2] per pixel: gray, alpha 0..255).
    Supports grayscale / palette / gray+alpha / RGB / RGBA, depth 1/2/4/8, no interlace."""
    assert raw[:8] == b'\x89PNG\r\n\x1a\n'
    off = 8
    idat = b''
    pal = None
    trns = None
    while off < len(raw):
        ln = struct.unpack('>I', raw[off:off+4])[0]; typ = raw[off+4:off+8]
        data = raw[off+8:off+8+ln]
        if typ == b'IHDR':
            w, h, depth, ctype, comp, filt, inter = struct.unpack('>IIBBBBB', data)
            assert inter == 0 and comp == 0 and filt == 0, (inter, comp, filt)
        elif typ == b'PLTE':
            pal = data
        elif typ == b'tRNS':
            trns = data
        elif typ == b'IDAT':
            idat += data
        elif typ == b'IEND':
            break
        off += 12 + ln
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    bpp_bits = channels * depth
    # filter step in bytes: the PNG spec clamps it to 1 for sub-byte depths
    # (a raw bpp_bits//8 would be 0 and index the wrong pixel)
    fstep = max(1, bpp_bits // 8)
    stride = (w * bpp_bits + 7) // 8
    pos = 0
    prev = bytearray(stride)
    out = bytearray(w * h * 2)  # gray, alpha bytes per pixel
    dat = zlib.decompress(idat)
    for y in range(h):
        f = dat[pos]; pos += 1
        line = bytearray(dat[pos:pos+stride]); pos += stride
        if f == 0:
            pass
        elif f == 1:  # Sub
            for i in range(fstep, stride):
                line[i] = (line[i] + line[i - fstep]) & 255
        elif f == 2:  # Up
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 255
        elif f == 3:  # Average
            for i in range(stride):
                a = line[i - fstep] if i >= fstep else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 255
        elif f == 4:  # Paeth
            for i in range(stride):
                a = line[i - fstep] if i >= fstep else 0
                b = prev[i]
                c = prev[i - fstep] if i >= fstep else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        prev = line
        # unpack samples -> gray/alpha
        mask = (1 << depth) - 1
        for x in range(w):
            bitpos = x * bpp_bits
            byte = bitpos // 8
            shift = 8 - depth - (bitpos % 8)
            smp = (line[byte] >> shift) & mask
            v = smp * 255 // mask if depth < 8 else smp
            g, a = v, 255
            if ctype == 3:
                g = (pal[smp * 3] + pal[smp * 3 + 1] + pal[smp * 3 + 2]) // 3
                if trns is not None and smp < len(trns) and trns[smp] == 0:
                    a = 0
            elif ctype == 0 and trns is not None and len(trns) >= 2:
                tg = (trns[0] << 8) | trns[1]
                if v == (tg * 255 // ((1 << depth) - 1) if depth < 8 else tg >> (8 - depth) if depth < 8 else tg & 255):
                    a = 0
            elif ctype == 4:
                ashift = 8 - depth - ((x * bpp_bits + depth) % 8)
                ab = (line[byte + 1] >> ashift) & mask if (byte + 1) < stride else mask
                a = ab * 255 // mask if depth < 8 else ab
            elif ctype == 6:
                abytes_off = byte + 3
                a = line[min(abytes_off, stride - 1)]
            out[(y * w + x) * 2] = g
            out[(y * w + x) * 2 + 1] = a
    return w, h, out

def read_strings(name):
    E = D(name)
    out, cur = [], []
    for i in range(0, len(E) - 1, 2):
        ch = (E[i] << 8) | E[i + 1]
        if ch == 0:
            if cur: out.append(''.join(cur)); cur = []
        else:
            cur.append(chr(ch))
    if cur: out.append(''.join(cur))
    return out

def c_arr(name, data):
    lines = [f'static const unsigned char {name}[] = {{']
    for i in range(0, len(data), 24):
        lines.append(','.join(str(b) for b in data[i:i+24]) + ',')
    lines.append('};')
    return '\n'.join(lines)

# --- consistency guards: a jar change that truncates/replaces a member must
#     fail HERE loudly, not silently emit shifted arrays (the C runtime only
#     sanity-checks at startup, and no C harness runs at generation time).
MBF_, D_, IMG_ = D('MBF'), D('d'), D('imageV3')
assert len(MBF_) == 1312 and MBF_[13] == 36, (len(MBF_), MBF_[13])
assert sum(MBF_[14:14 + MBF_[13]]) == len(MBF_) - (14 + MBF_[13]), 'MBF block sizes'
assert len(D_) == 4068 and 1945 < len(D_), len(D_)      # MK_D_GEOM/MK_D_CM in range
assert len(IMG_) == 16400, len(IMG_)
for _lg in ['en', 'de', 'fr', 'it', 'nl-NL', 'tr-TR']:
    _n = len(read_strings(_lg))
    assert _n == 32, (_lg, _n)

out = []
out.append('// AUTO-GENERATED by tools/extract_assets.py - DO NOT EDIT.\n'
           '// Game assets extracted from the original Munkiki\'s Castles (2002) jar.\n'
           '#ifndef MK_ASSETS_H\n#define MK_ASSETS_H\n')

# --- blobs
out.append(c_arr('MK_MBF', D('MBF')))
out.append('#define MK_MBF_LEN %d' % len(D('MBF')))
out.append(c_arr('MK_D', D('d')))
out.append('#define MK_D_LEN %d' % len(D('d')))
out.append('#define MK_D_GEOM 554   // geometry/sprites start')
out.append('#define MK_D_CM 1945    // cm animation tables start')
out.append(c_arr('MK_IMG', D('imageV3')))
out.append('#define MK_IMG_LEN %d' % len(D('imageV3')))
out.append('#define MK_FRAME_BITS 1600 // full 40x40 frame, bit offset into MK_IMG')

# --- textures & HUD images (1bpp, MSB first)
for var, fn, note in [('note', 'note.png', 'billboard tex 0'),
                      ('key',  'key.png',  'billboard tex 1'),
                      ('warp', 'warp.png', 'billboard tex 2 (magic item)'),
                      ('life', 'life.png', 'billboard tex 3'),
                      ('kb',   'kb.png',   'HUD life icon'),
                      ('ky',   'ky.png',   'HUD key icon')]:
    w, h, rgba = png_to_rgba(D(fn))
    out.append(f'// {note}: {w}x{h}, per-pixel byte = gray4<<4 | alpha4')
    out.append(f'static const int MK_TEX_{var.upper()}_W = {w}, MK_TEX_{var.upper()}_H = {h};')
    out.append(c_arr(f'MK_TEX_{var.upper()}', bytes(((rgba[i*2] >> 4) << 4) | (rgba[i*2+1] >> 4) for i in range(w * h))))

# --- localization
langs = ['en', 'de', 'fr', 'it', 'nl-NL', 'tr-TR']
# PC keyboard adaptation, English hints only (other locales kept the original
# phone keys): this port binds smash to 'e' and pause/restart to 'r', so the
# English hint names keys a PC user actually has. MUST stay in sync with the
# bindings in src/game.c game_key().
EN_KEY_FIX = [
    ('stick. Press 0 to hit', 'stick. Press e to hit'),
    ('stuck, press * to restart', 'stuck, press r to restart'),
]
out.append('// localization: MK_STR[lang][string], %N replaced at runtime')
out.append('static const char* MK_STR[][32] = {')
for lg in langs:
    out.append(f' {{ // {lg}')
    for s in read_strings(lg):
        if lg == 'en':
            for a, b in EN_KEY_FIX:
                s = s.replace(a, b)
        esc = s.replace('\\', '\\\\').replace('"', '\\"').replace('%N', '%s')
        out.append(f'  "{esc}",')
    out.append(' },')
out.append('};')
out.append('static const char* MK_LANG_NAMES[] = {' + ','.join(f'"{x}"' for x in ['English','Deutsch','Francais','Italiano','Nederlands','Turkce']) + '};')
out.append('#define MK_LANG_COUNT %d' % len(langs))
out.append('#endif // MK_ASSETS_H\n')

dst = os.path.join(ROOT, 'src', 'assets.h')
open(dst, 'w', newline='\n').write('\n'.join(out))
print('wrote', dst, os.path.getsize(dst), 'bytes')
# sanity
w,h,b = png_to_rgba(D('note.png')); print('note.png', w, 'x', h, 'px:', len(b))
w,h,b = png_to_rgba(D('kb.png')); print('kb.png', w, 'x', h, 'alphas:', sorted(set(b[i*2+1] for i in range(w*h))))
print('en strings:', len(read_strings('en')))
