#!/usr/bin/env python3
"""Derive the original IOAcceleratorES base ABI of the 20H392 kernelcache statically, for the staged AGXAccelerator (stage 11).

  audit-fajita-accelerator-base.py KERNELCACHE [--slide -0x1000000] [--write scripts/data/fajita-ioaccelerator-base-abi.json]

The iPadOS 17 record (UniXNU scripts/data/ioaccelerator-base-abi.json) came from a macOS HVF runtime observation of the 21H461 kernel.
Here every fact is read from the unchanged 20H392 kernelcache (sha256 pinned in ios16/board.toml [kernel.prelink]) and each one is
required, not assumed:
- IOGPUFamily's only OSMetaClass constructor call naming "IOAcceleratorES" passes IOService::gMetaClass as the parent and 136 bytes;
- the unique getMetaClass body returning that metaclass sits in slot 7 of a unique 168-entry vtable that equals xnu's IOService
  vtable (exported __ZTV9IOService) everywhere except slots 0, 1 and 7 (destructors and getMetaClass): no operational override;
- exactly two constructor aliases install that vtable, each only calling IOService's C2 (IOService::IOService(const OSMetaClass *));
- slot 0 (the non-deleting destructor) tail-calls IOService's D2.
The output names those addresses as the built kernel has them: the prelink places the kexts at their original address plus the board's
slide ([kernel.prelink] slide). FajitaGpuProbe stage 11 checks the bound metaclass against the live one (OSMetaClass lookup by name,
class size, parent) before it runs the staged initializer, so a wrong record refuses instead of crashing."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
for kits in (ROOT / 'kits/NativePrelink', Path('/home/john/src/UniXNU-ios16-stack/kits/NativePrelink')):
    if (kits / 'native_prelink.py').is_file():
        sys.path.insert(0, str(kits))
        break
from native_prelink import macho, unserialize_xml  # noqa: E402


def require(value, message):
    if not value:
        raise SystemExit('audit: ' + message)


parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('kernelcache', type=Path)
parser.add_argument('--slide', default='-0x1000000')
parser.add_argument('--write', type=Path)
args = parser.parse_args()
raw = args.kernelcache.read_bytes()
digest = hashlib.sha256(raw).hexdigest()
top = macho(raw)
segs = top['segments']


def off(va):
    for s in segs:
        if s['filesize'] and s['va'] <= va < s['va'] + s['filesize']:
            return s['fileoff'] + va - s['va']
    raise SystemExit('audit: unbacked address %#x' % va)


def read(va, n):
    o = off(va)
    return raw[o:o + n]


def u32(va):
    return struct.unpack('<I', read(va, 4))[0]


def tptr(va):
    # Threaded-rebase pointers of this arm64 kernelcache: the low 43 bits of the target, the kernel's high bits implied.
    v = struct.unpack('<Q', read(va, 8))[0]
    if not v or v >> 48 == 0xffff:
        return v
    return (v & 0x7ffffffffff) | 0xfffff80000000000


def adrp(pc, w):
    imm = ((w >> 5) & 0x7ffff) << 2 | ((w >> 29) & 3)
    if imm & (1 << 20):
        imm -= 1 << 21
    return (pc & ~4095) + (imm << 12)


def branch(pc, linked=None):
    w = u32(pc)
    require(w & 0x7c000000 == 0x14000000 and (linked is None or bool(w & 0x80000000) == linked), 'direct branch at %#x' % pc)
    imm = w & 0x3ffffff
    if imm & (1 << 25):
        imm -= 1 << 26
    return pc + 4 * imm


symoff, nsyms, stroff = next(struct.unpack_from('<3I', raw, p + 8) for c, p, _ in top['commands'] if c == 2)
names = {}
for i in range(nsyms):
    n, _, _, _, v = struct.unpack_from('<IBBHQ', raw, symoff + 16 * i)
    names[raw[stroff + n:raw.index(b'\0', stroff + n)].decode('latin1')] = v
inverse = {v: k for k, v in names.items()}


def stub_target(pc):
    x, y, z = struct.unpack('<3I', read(pc, 12))
    require(x & 0x9f00001f == 0x90000010 and y & 0xffc003ff == 0xf9400210 and z == 0xd61f0200, 'import stub at %#x' % pc)
    return tptr(adrp(pc, x) + ((y >> 10) & 4095) * 8)


info = next(s for s in segs if s['name'] == '__PRELINK_INFO')
kexts = {e['CFBundleIdentifier']: e for e in
         unserialize_xml(raw[info['fileoff']:info['fileoff'] + info['filesize']].split(b'\0')[0])['_PrelinkInfoDictionary']}
family = kexts['com.apple.iokit.IOGPUFamily']
kext = macho(raw, off(family['_PrelinkExecutableLoadAddr']))['segments']
text = next(s for s in kext if s['name'] == '__TEXT_EXEC')
code = read(text['va'], text['size'])
words = struct.unpack('<%dI' % (len(code) // 4), code)
strings = {s['va'] + m.start() for s in segs if s['filesize'] for m in
           re.finditer(rb'IOAcceleratorES\0', raw[s['fileoff']:s['fileoff'] + s['filesize']])}

# The metaclass constructor: adrp/add x1 "IOAcceleratorES", the parent in x2, movz w3 #size, bl OSMetaClass::OSMetaClass.
meta_calls = []
for i in range(len(words) - 8):
    pc = text['va'] + 4 * i
    a, b = words[i], words[i + 1]
    if a & 0x9f00001f == 0x90000001 and b & 0xffc003ff == 0x91000021 and adrp(pc, a) + ((b >> 10) & 4095) in strings:
        window = range(i - 4, i + 8)
        size = [((words[j] >> 5) & 0xffff) for j in window if words[j] & 0xffe0001f == 0x52800003]
        calls = [text['va'] + 4 * j for j in window if words[j] & 0xfc000000 == 0x94000000]
        if not size or not calls or inverse.get(stub_target(branch(calls[0], True))) != '__ZN11OSMetaClassC2EPKcPKS_j':
            continue
        parent = [j for j in window if words[j] & 0x9f00001f == 0x90000002]
        require(parent and words[parent[0] + 1] & 0xffc003ff == 0xf9400042, 'parent metaclass load')
        parent_meta = tptr(adrp(text['va'] + 4 * parent[0], words[parent[0]]) + ((words[parent[0] + 1] >> 10) & 4095) * 8)
        # The metaclass object (x0 at the call): an adrp/add pair into x19 (moved to x0) or x0 just before; a constructor that takes it
        # as its own argument (no such pair) is the C2 body, not a construction site.
        this = [j for j in range(i - 3, i) if (words[j] & 0x9f00001f, words[j + 1] & 0xffc003ff) in
                ((0x90000013, 0x91000273), (0x90000000, 0x91000000))]
        if not this:
            continue
        meta = adrp(text['va'] + 4 * this[-1], words[this[-1]]) + ((words[this[-1] + 1] >> 10) & 4095)
        meta_calls.append((pc, meta, size[0], parent_meta))
metas = {m[1] for m in meta_calls}
require(len(metas) == 1, 'one IOAcceleratorES metaclass object, found %s' % sorted(hex(m) for m in metas))
ctor_pc, meta, size, parent_meta = meta_calls[-1]
require(all(m[2] == size and m[3] == parent_meta for m in meta_calls), 'every IOAcceleratorES metaclass construction agrees')
require(size == 136, 'instance size %d, expected 136' % size)
require(inverse.get(parent_meta) == '__ZN9IOService10gMetaClassE', 'parent is IOService')

getters = [text['va'] + 4 * i for i in range(len(words) - 3)
           if words[i] & 0x9f00001f == 0x90000000 and words[i + 1] & 0xffc003ff == 0x91000000 and words[i + 2] == 0xd65f03c0
           and adrp(text['va'] + 4 * i, words[i]) + ((words[i + 1] >> 10) & 4095) == meta]
require(len(getters) == 1, 'unique getMetaClass')
service = [tptr(names['__ZTV9IOService'] + 16 + 8 * i) for i in range(260)]
entries = service.index(0)
require(entries == 168, 'IOService vtable has %d entries, expected 168' % entries)
tables = []
for s in kext:
    if s['name'] not in ('__DATA_CONST', '__DATA'):
        continue
    for o in range(0, s['size'] - 8, 8):
        if tptr(s['va'] + o) == getters[0]:
            vp = s['va'] + o - 56
            v = [tptr(vp + 8 * i) for i in range(entries + 1)]
            if v[entries] == 0 and [i for i in range(entries) if v[i] != service[i]] == [0, 1, 7]:
                tables.append((vp, v))
require(len(tables) == 1, 'unique 168-entry IOAcceleratorES vtable differing from IOService only in slots 0, 1, 7')
vp, vtable = tables[0]
constructors = []
for i in range(3, len(words) - 4):
    pc = text['va'] + 4 * i
    if words[i] & 0x9f00001f == 0x90000008 and words[i + 1] & 0xffc003ff == 0x91000108 and words[i + 2] == 0xf9000008 \
            and adrp(pc, words[i]) + ((words[i + 1] >> 10) & 4095) == vp:
        start = pc - 12
        require(words[i - 3:i - 1] == (0xa9bf7bfd, 0x910003fd) and words[i + 3:i + 5] == (0xa8c17bfd, 0xd65f03c0),
                'constructor frame at %#x' % start)
        require(inverse.get(stub_target(branch(start + 8, True))) == '__ZN9IOServiceC2EPK11OSMetaClass',
                'constructor %#x calls IOService C2' % start)
        constructors.append(start)
require(len(constructors) == 2, 'two constructor aliases, found %d' % len(constructors))
require(inverse.get(stub_target(branch(vtable[0], False))) == '__ZN9IOServiceD2Ev', 'slot 0 tail-calls IOService D2')
slide = int(args.slide, 0)
record = dict(kernelcache_sha256=digest, kext='com.apple.iokit.IOGPUFamily', kext_version=family.get('CFBundleVersion'),
              slide=hex(slide), instance_bytes=size, virtual_entries=entries, metadata_overrides=[0, 1, 7], operational_overrides=[],
              metaclass=hex(meta + slide), vptr=hex(vp + slide), get_meta_class=hex(getters[0] + slide),
              protected_compatible_constructor_aliases=[hex(c + slide) for c in constructors],
              nondeleting_destructor=hex(vtable[0] + slide),
              original=dict(metaclass=hex(meta), metaclass_constructor_call=hex(ctor_pc), vptr=hex(vp)),
              original_entry_sha256={hex(a + slide): hashlib.sha256(read(a, 32)).hexdigest() for a in (*constructors, vtable[0], getters[0])},
              static_only=True, runtime_check='FajitaGpuProbe stage 11: OSMetaClass lookup, size and parent')
text_out = json.dumps(record, indent=2) + '\n'
if args.write:
    args.write.write_text(text_out)
print(text_out, end='')
print('PASS: 20H392 IOAcceleratorES is a 136-byte IOService subclass with the 168-entry IOService vtable (slots 0, 1, 7 its own)')
