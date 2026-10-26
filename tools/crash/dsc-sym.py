#!/usr/bin/env python3
# tools/crash/dsc-sym.py [--cache DSC] [--slide HEX] ADDR...      symbolize shared-cache addresses (nearest local symbol)
# tools/crash/dsc-sym.py [--cache DSC] --image SUBSTR OFF...      same, for image-relative offsets (dsc-addr.py's "+0x...")
# tools/crash/dsc-sym.py [--cache DSC] --image SUBSTR --grep RE   list an image's symbols matching RE (offset, name)
# Reads the cache's .symbols subcache (unslid nlist table of every image's local symbols) and each image's export trie directly; `ipsw dyld a2s`
# stalls on the 20H392 cache. Default DSC: .cache/dsc-20H392/dyld_shared_cache_arm64. --demangle pipes names through c++filt.
#   tools/crash/dsc-sym.py --slide 464ec000 1ce7ef0c0
#   tools/crash/dsc-sym.py --image QuartzCore.framework/QuartzCore 203e28 7f0c0
import argparse, bisect, glob, os, re, struct, subprocess

BOARD = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ap = argparse.ArgumentParser()
ap.add_argument('--cache', default=os.path.join(BOARD, '.cache/dsc-20H392/dyld_shared_cache_arm64'))
ap.add_argument('--slide', default='0')
ap.add_argument('--image')
ap.add_argument('--grep')
ap.add_argument('--demangle', action='store_true')
ap.add_argument('addr', nargs='*')
a = ap.parse_args()

with open(a.cache, 'rb') as f:
    h = f.read(0x100)
    mo, = struct.unpack_from('<I', h, 0x10)
    f.seek(mo); base, = struct.unpack_from('<Q', f.read(8))          # first mapping = unslid cache base
    off, cnt = struct.unpack_from('<QQ', h, 0x88)                     # imagesText
    f.seek(off); d = f.read(cnt * 32)
    images = []
    for i in range(cnt):
        la, sz, po = struct.unpack_from('<16sQII', d, i * 32)[1:]
        f.seek(po); images.append((la, sz, f.read(256).split(b'\0')[0].decode()))

sf = open(a.cache + '.symbols', 'rb')
lo, _ = struct.unpack_from('<QQ', sf.read(0x100), 0x48)
sf.seek(lo)
nlo, ncnt, so, ssz, eo, ecnt = struct.unpack_from('<6I', sf.read(24))
sf.seek(lo + eo)
ents = {}
ed = sf.read(ecnt * 16)
for i in range(ecnt):
    dyoff, start, n = struct.unpack_from('<QII', ed, i * 16)
    ents[base + dyoff] = (start, n)

maps = []
for fn in [a.cache] + sorted(glob.glob(a.cache + '.[0-9]*')):
    if fn.endswith('.map'): continue
    with open(fn, 'rb') as f:
        hh = f.read(0x100)
        mo2, mc2 = struct.unpack_from('<II', hh, 0x10)
        f.seek(mo2); md = f.read(mc2 * 32)
    for i in range(mc2):
        ad, sz, fo = struct.unpack_from('<QQQ', md, i * 32)
        maps.append((ad, sz, fo, fn))

def rd(va, n):
    for ad, sz, fo, fn in maps:
        if ad <= va < ad + sz:
            with open(fn, 'rb') as f:
                f.seek(fo + va - ad); return f.read(n)
    return b''

def uleb(d, i):
    v = s = 0
    while True:
        b = d[i]; i += 1; v |= (b & 0x7f) << s; s += 7
        if b < 0x80: return v, i

def exports_of(la):
    """The image's exported symbols (LC_DYLD_EXPORTS_TRIE), which the .symbols nlist table does not hold."""
    hd = rd(la, 0x8000)
    ncmds, = struct.unpack_from('<I', hd, 16)
    o, le, trie = 32, None, None
    for _ in range(ncmds):
        cmd, sz = struct.unpack_from('<II', hd, o)
        if cmd == 0x19 and hd[o + 8:o + 24].rstrip(b'\0') == b'__LINKEDIT':
            le = struct.unpack_from('<QQQ', hd, o + 24)       # vmaddr, vmsize, fileoff
        if cmd == 0x80000033: trie = struct.unpack_from('<II', hd, o + 8)
        o += sz
    if not le or not trie or not trie[1]: return []
    d = rd(le[0] + trie[0] - le[2], trie[1])
    out, stack = [], [(0, b'')]
    while stack:
        node, pre = stack.pop()
        if node >= len(d): continue
        tsz, i = uleb(d, node)
        if tsz:
            flags, j = uleb(d, i)
            if not flags & 0x08:                               # not a re-export
                off, _ = uleb(d, j); out.append((la + off, pre))
        i += tsz
        nch = d[i]; i += 1
        for _ in range(nch):
            e = d.index(b'\0', i); lab = d[i:e]; i = e + 1
            ch, i = uleb(d, i); stack.append((ch, pre + lab))
    return out

def syms_of(la):
    start, n = ents[la]
    sf.seek(lo + nlo + start * 16); nd = sf.read(n * 16)
    out = []
    for i in range(n):
        sx, ty, sect, desc, val = struct.unpack_from('<IBBHQ', nd, i * 16)
        if ty & 0xe0 or not (ty & 0x0e) == 0x0e or val == 0: continue    # skip stabs; keep N_SECT
        out.append((val, sx))
    out += [(v, n.decode(errors='replace')) for v, n in exports_of(la)]  # a str name: an export
    out.sort(key=lambda t: t[0])
    return out

def name_at(sx):
    if isinstance(sx, str): return sx
    sf.seek(lo + so + sx); return sf.read(512).split(b'\0')[0].decode(errors='replace')

def dem(names):
    if not a.demangle or not names: return names
    r = subprocess.run(['c++filt', '-_'], input='\n'.join(names), capture_output=True, text=True)
    return r.stdout.splitlines() if r.returncode == 0 else names

cache = {}
def lookup(x):
    img = next(((la, sz, p) for la, sz, p in images if la <= x < la + sz), None)
    if not img: return f'{x:#x} not in any image text'
    if img[0] not in cache: cache[img[0]] = syms_of(img[0])
    s = cache[img[0]]
    i = bisect.bisect_right([v for v, _ in s], x) - 1
    if i < 0: return f'{x:#x} {os.path.basename(img[2])} +{x - img[0]:#x} (no symbol)'
    v, sx = s[i]
    return f'{x:#x} {os.path.basename(img[2])} +{x - img[0]:#x} {dem([name_at(sx)])[0]} +{x - v:#x}'

if a.image:
    img = next(((la, sz, p) for la, sz, p in images if a.image in p), None)
    if not img: raise SystemExit('image not found')
    if a.grep:
        s = syms_of(img[0]); rx = re.compile(a.grep)
        rows = [(v, name_at(sx)) for v, sx in s]
        rows = [(v, n) for v, n in rows if rx.search(n)]
        for (v, _), n in zip(rows, dem([n for _, n in rows])): print(f'+{v - img[0]:#x} {n}')
    for o in a.addr: print(o + ': ' + lookup(img[0] + int(o, 16)))
else:
    slide = int(a.slide, 16)
    for s in a.addr: print(s + ': ' + lookup(int(s, 16) - slide))
