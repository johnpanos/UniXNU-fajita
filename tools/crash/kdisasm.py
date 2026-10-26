#!/usr/bin/env python3
# tools/crash/kdisasm.py [--km KERNEL] ADDR [COUNT] [--back N] [--refs STRING]: disassemble COUNT (default 40) arm64 instructions of a
# kernel Mach-O (default the build's kernel.macho; the storage candidate .cache/fajita-inputs/storage-candidate/kernel.macho carries
# the prelinked original kexts) at virtual address ADDR, each adrp+add/ldr target annotated when it is a C string. --refs STRING
# instead lists the code addresses that load the address of the first STRING in the kernel (adrp + add): the functions that use a
# log message, for finding an original kext's code by its strings. Needs python3-capstone.
#   tools/crash/kdisasm.py --km .cache/fajita-inputs/storage-candidate/kernel.macho --refs "dropped frame info message"
import argparse, os, struct
import capstone

BOARD = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ap = argparse.ArgumentParser()
ap.add_argument('--km', default=os.environ.get('KM') or os.path.join(BOARD, '.cache/fajita-out/unixnu/fajita-ios16/kernel/kernel.macho'))
ap.add_argument('--back', type=int, default=0)
ap.add_argument('--refs')
ap.add_argument('addr', nargs='?')
ap.add_argument('count', nargs='?', type=int, default=40)
a = ap.parse_args()
d = open(a.km, 'rb').read()
ncmds = struct.unpack_from('<I', d, 16)[0]
off, segs = 32, []
for _ in range(ncmds):
    cmd, sz = struct.unpack_from('<II', d, off)
    if cmd == 0x19:
        name = d[off + 8:off + 24].split(b'\0')[0].decode()
        va, vs, fo, fs = struct.unpack_from('<QQQQ', d, off + 24)
        segs.append((name, va, vs, fo, fs))
    off += sz


def v2f(va):
    for _, sva, vs, fo, fs in segs:
        if sva <= va < sva + fs:
            return fo + va - sva
    return None


def f2v(fo):
    for _, sva, vs, sfo, fs in segs:
        if sfo <= fo < sfo + fs:
            return sva + fo - sfo
    return None


def cstr(va):
    fo = v2f(va)
    if fo is None:
        return None
    t = d[fo:fo + 160].split(b'\0')[0]
    return t.decode() if len(t) >= 3 and all(32 <= c < 127 for c in t) else None


if a.refs:
    s = d.find(a.refs.encode())
    if s < 0:
        raise SystemExit('string not found')
    sva = f2v(s)
    print('string at %#x' % sva)
    for name, va, vs, fo, fs in segs:
        if 'TEXT_EXEC' not in name:
            continue
        code = d[fo:fo + fs]
        for i in range(0, len(code) - 16, 4):
            w = struct.unpack_from('<I', code, i)[0]
            if (w & 0x9f000000) != 0x90000000:
                continue
            rd, imm = w & 31, (((w >> 5) & 0x7ffff) << 2 | (w >> 29) & 3) << 12
            if imm & (1 << 32):
                imm -= 1 << 33
            page = ((va + i) & ~0xfff) + imm
            if page != sva & ~0xfff:
                continue
            for j in range(1, 4):
                w2 = struct.unpack_from('<I', code, i + 4 * j)[0]
                if (w2 & 0xffc00000) == 0x91000000 and (w2 >> 5) & 31 == rd and page + ((w2 >> 10) & 0xfff) == sva:
                    print('ref at %#x' % (va + i))
    raise SystemExit
va = int(a.addr, 16) - 4 * a.back
fo = v2f(va)
if fo is None:
    raise SystemExit('address not in the kernel')
md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
md.detail = True
pages = {}
for i in md.disasm(d[fo:fo + 4 * (a.count + a.back)], va):
    line = '%#x: %s %s' % (i.address, i.mnemonic, i.op_str)
    ops = i.operands
    if i.mnemonic == 'adrp' and len(ops) == 2:
        pages[ops[0].reg] = ops[1].imm
    elif i.mnemonic == 'add' and len(ops) == 3 and ops[1].reg in pages and ops[2].type == capstone.arm64.ARM64_OP_IMM:
        t = cstr(pages[ops[1].reg] + ops[2].imm)
        if t:
            line += '    ; ' + repr(t)
    print(line)
