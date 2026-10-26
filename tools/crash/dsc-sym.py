#!/usr/bin/env python3
# tools/crash/dsc-sym.py [--cache DSC] [--slide HEX] ADDR...      symbolize shared-cache addresses (nearest local symbol)
# tools/crash/dsc-sym.py [--cache DSC] --image SUBSTR OFF...      same, for image-relative offsets (dsc-addr.py's "+0x...")
# tools/crash/dsc-sym.py [--cache DSC] --image SUBSTR --grep RE   list an image's symbols matching RE (offset, name)
# Reads the cache's .symbols subcache (unslid nlist table of every image's local symbols) directly; `ipsw dyld a2s`
# stalls on the 20H392 cache. Default DSC: .cache/dsc-20H392/dyld_shared_cache_arm64. --demangle pipes names through c++filt.
#   tools/crash/dsc-sym.py --slide 464ec000 1ce7ef0c0
#   tools/crash/dsc-sym.py --image QuartzCore.framework/QuartzCore 203e28 7f0c0
import argparse, bisect, os, re, struct, subprocess

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

def syms_of(la):
    start, n = ents[la]
    sf.seek(lo + nlo + start * 16); nd = sf.read(n * 16)
    out = []
    strs_needed = []
    for i in range(n):
        sx, ty, sect, desc, val = struct.unpack_from('<IBBHQ', nd, i * 16)
        if ty & 0xe0 or not (ty & 0x0e) == 0x0e or val == 0: continue    # skip stabs; keep N_SECT
        out.append((val, sx))
    out.sort()
    return out

def name_at(sx):
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
