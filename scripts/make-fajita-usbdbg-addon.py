#!/usr/bin/env python3
"""Add the fajita USB debug agent to a boot GPU addon (format NGPUADD2, scripts/make-gpu-addon.py).

The payload carries one addon (--gpu-addon); this copies every entry of an existing addon (the board's
wallpaper addon) unchanged and appends:
  sbin/wifid              the signed fjdbg-agent (CI artifact of .github/workflows/usb-debug-agent.yml)
  sbin/fjdbg-agent.txt    a marker the kernel's /usr/sbin union mount checks (stage 5 = agent visible)
With usb-debug=3 the kernel union-mounts /private/var/ngpu/sbin over /usr/sbin, so launchd's KeepAlive root
job com.apple.wifid runs the agent.
  make-fajita-usbdbg-addon.py --base fajita-wallpaper.ngpu --agent .cache/usbdbg-agent/fjdbg-agent --out usbdbg.ngpu
Format: "NGPUADD2", u32 count, u32 0, u64 total; per file u32 path-bytes(+NUL,<=160), u32 mode, u64 size,
u64 zlib size, the path padded to 8, the zlib stream padded to 8. Inputs are read by path, never executed.
"""
import argparse
import struct
import zlib
from pathlib import Path

MAGIC = b'NGPUADD2'


def pad(data):
    return data + bytes(-len(data) % 8)


def read_addon(blob):
    if blob[:8] != MAGIC:
        raise SystemExit('base addon: bad magic')
    count, _, total = struct.unpack_from('<IIQ', blob, 8)
    if total != len(blob):
        raise SystemExit(f'base addon: total {total} != size {len(blob)}')
    at, files = 24, []
    for _ in range(count):
        plen, mode, size, zsize = struct.unpack_from('<IIQQ', blob, at)
        at += 24
        path = blob[at:at + plen].rstrip(b'\0').decode()
        at += plen + (-plen % 8)
        data = zlib.decompress(blob[at:at + zsize])
        at += zsize + (-zsize % 8)
        if len(data) != size:
            raise SystemExit(f'base addon: {path} size mismatch')
        files.append((path, mode, data))
    return files


def write_addon(files):
    body = b''
    for name, mode, data in files:
        encoded = name.encode() + b'\0'
        if len(encoded) > 160 or '..' in name.split('/') or name.startswith('/'):
            raise SystemExit('bad addon path ' + name)
        packed = zlib.compress(data, 9)
        body += struct.pack('<IIQQ', len(encoded), mode, len(data), len(packed)) + pad(encoded) + pad(packed)
    return MAGIC + struct.pack('<IIQ', len(files), 0, 24 + len(body)) + body


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--base', type=Path, help='existing addon to extend (optional)')
    ap.add_argument('--agent', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    a = ap.parse_args()
    files = read_addon(a.base.read_bytes()) if a.base else []
    files = [f for f in files if not f[0].startswith('sbin/')]
    agent = a.agent.read_bytes()
    if agent[:4] != b'\xcf\xfa\xed\xfe':
        raise SystemExit('agent is not a 64-bit Mach-O')
    files += [('sbin/wifid', 0o755, agent), ('sbin/fjdbg-agent.txt', 0o644, b'fajita usb-debug agent\n')]
    blob = write_addon(files)
    assert [f[0] for f in read_addon(blob)] == [f[0] for f in files]   # round-trips
    a.out.write_bytes(blob)
    print(f'{a.out}: {len(files)} files, {len(blob)} bytes')


if __name__ == '__main__':
    main()
