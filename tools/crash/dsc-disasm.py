#!/usr/bin/env python3
# tools/crash/dsc-disasm.py [--cache DSC] IMAGE-SUBSTR|- OFFSET [COUNT] [--back N]: disassemble COUNT (default 40) arm64
# instructions of a dyld shared cache image at image-relative OFFSET (hex), e.g. the "WebKit +0x1e6dac" from dsc-addr.py.
# --grep RE: only the matching instructions of COUNT (search an image: e.g. COUNT 900000 and RE '#0x30d\\]').
# Reads the main cache and its .NN subcaches' mappings directly (no extraction). Needs python3-capstone.
#   tools/crash/dsc-disasm.py WebKit.framework/WebKit 1e6dac 30 --back 20
import argparse, glob, os, struct
import capstone
BOARD = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ap = argparse.ArgumentParser()
ap.add_argument('--cache', default=os.path.join(BOARD, '.cache/dsc-20H392/dyld_shared_cache_arm64'))
ap.add_argument('--data', action='store_true', help='dump COUNT 64-bit words instead of disassembling')
ap.add_argument('--back', type=int, default=0)
ap.add_argument('--strings', action='store_true', help='annotate adrp+add/ldr targets that are C strings, CFStrings or selectors')
ap.add_argument('--grep', help='print only the instructions matching this regular expression (search COUNT instructions)')
ap.add_argument('image'); ap.add_argument('offset'); ap.add_argument('count', nargs='?', type=int, default=40)
a = ap.parse_args()
files = [a.cache] + sorted(glob.glob(a.cache + '.[0-9]*'))
maps = []
for fn in files:
    if fn.endswith('.map'): continue
    with open(fn, 'rb') as f:
        h = f.read(0x100)
        mo, mc = struct.unpack_from('<II', h, 0x10)
        f.seek(mo); d = f.read(mc * 32)
        for i in range(mc):
            ad, sz, fo = struct.unpack_from('<QQQ', d, i * 32)
            maps.append((ad, sz, fo, fn))
with open(a.cache, 'rb') as f:
    h = f.read(0x100)
    off, cnt = struct.unpack_from('<QQ', h, 0x88)
    f.seek(off); d = f.read(cnt * 32)
    imgs = [struct.unpack_from('<16sQII', d, i * 32)[1:] for i in range(cnt)]
    hit = None
    for la, sz, po in ([] if a.image == '-' else imgs):
        f.seek(po); p = f.read(256).split(b'\0')[0].decode()
        if a.image in p: hit = (la, p); break
if a.image == '-': hit = (0, '-')   # absolute (unslid cache) address
if not hit: raise SystemExit('image not found')
va = hit[0] + int(a.offset, 16) - a.back * 4
data = None
for ad, sz, fo, fn in maps:
    if ad <= va < ad + sz:
        with open(fn, 'rb') as f:
            f.seek(fo + va - ad); data = f.read((a.count + a.back) * (8 if a.data else 4))
        break
if data is None: raise SystemExit('address not mapped')
if a.data:
    for i in range(a.count):
        print(f'{va + 8 * i:#x}: {struct.unpack_from("<Q", data, 8 * i)[0]:#018x}')
    raise SystemExit
def read(addr, n):
    for ad, sz, fo, fn in maps:
        if ad <= addr < ad + sz:
            with open(fn, 'rb') as f:
                f.seek(fo + addr - ad)
                return f.read(n)
    return None
def cstr(addr):
    b = read(addr, 200)
    if not b or b'\0' not in b: return None
    t = b.split(b'\0')[0]
    return t.decode() if len(t) >= 2 and all(32 <= c < 127 for c in t) else None
def describe(addr):
    """A C string at addr, a CFString (isa, flags, cstr pointer, length) there, or a pointer to a C string (a selector reference)."""
    t = cstr(addr)
    if t: return repr(t)
    w = read(addr, 32)
    if w and len(w) == 32:
        isa, flags, ptr, ln = struct.unpack('<QQQQ', w)
        ptr &= 0xfffffffff
        if 0 < ln < 200 and (flags & 0xffffffff) in (0x7c8, 0x7d0, 0x7c0):
            t = cstr(ptr)
            if t: return 'CF' + repr(t)
        p = struct.unpack('<Q', w[:8])[0] & 0xfffffffff
        t = cstr(p) if p else None
        if t: return '@' + repr(t)
    return None
md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
md.detail = a.strings
pages = {}
import re
pat = re.compile(a.grep) if a.grep else None
pos = 0
while pos < len(data):   # disasm stops at an undecodable word (data in text): skip it and go on
    last = pos
    for i in md.disasm(data[pos:], va + pos):
        line = f'{i.address - hit[0]:#x}: {i.mnemonic} {i.op_str}'
        if a.strings:
            ops = i.operands
            if i.mnemonic == 'adrp' and len(ops) == 2:
                pages[ops[0].reg] = ops[1].imm
            elif i.mnemonic in ('add', 'ldr') and len(ops) >= 2 and ops[1].type in (capstone.arm64.ARM64_OP_REG, capstone.arm64.ARM64_OP_MEM):
                base = ops[1].reg if ops[1].type == capstone.arm64.ARM64_OP_REG else ops[1].mem.base
                if base in pages:
                    off = ops[2].imm if i.mnemonic == 'add' and len(ops) > 2 else ops[1].mem.disp if ops[1].type == capstone.arm64.ARM64_OP_MEM else 0
                    target = pages[base] + off
                    d = describe(target) if i.mnemonic == 'add' else None
                    if i.mnemonic == 'ldr':
                        w = read(target, 8)
                        if w: d = describe(struct.unpack('<Q', w)[0] & 0xfffffffff)
                    if d: line += '    ; ' + d
        if not pat or pat.search(line): print(line)
        last = i.address - va + 4
    pos = max(last, pos) + 4 if last <= pos else last
