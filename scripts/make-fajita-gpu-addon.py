#!/usr/bin/env python3
"""Pack files into a GPU addon (the NGPUADD2 format of UniXNU scripts/make-gpu-addon.py) for the fajita boot payload.

  make-fajita-gpu-addon.py OUTPUT [--base ADDON] [--file DEST=SOURCE[:MODE]]... [--tree DEST=DIR]...

DEST is the path below /private/var/ngpu: sbin/ngpu-probe lands in /private/var/ngpu/sbin, which usb-debug=3 mounts over /usr/sbin;
addons/NativeAdrenoMetal.bundle/... is the plugin in the union view of /System/Library/Extensions. --base keeps every file of an
existing addon (a later --file or --tree with the same path replaces it). --tree adds a directory recursively, executables 0755 and
everything else 0644. The kernel writes each file into Data before the mounts (patch zzy-native-gpu-addon-mount). The files are
data here; nothing is executed."""
import argparse
import os
from pathlib import Path
import struct
import zlib


def pad(data):
    return data + bytes(-len(data) % 8)


def read_addon(path):
    data = path.read_bytes()
    if data[:8] != b'NGPUADD2':
        raise SystemExit('not an NGPUADD2 addon: %s' % path)
    count, = struct.unpack_from('<I', data, 8)
    at, files = 24, {}
    for _ in range(count):
        name_bytes, mode, size, packed = struct.unpack_from('<IIQQ', data, at)
        at += 24
        name = data[at:at + name_bytes - 1].decode()
        at += (name_bytes + 7) & ~7
        files[name] = (mode, zlib.decompress(data[at:at + packed]))
        if len(files[name][1]) != size:
            raise SystemExit('bad file size in base addon: ' + name)
        at += (packed + 7) & ~7
    return files


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('output', type=Path)
    parser.add_argument('--base', type=Path)
    parser.add_argument('--file', action='append', default=[])
    parser.add_argument('--tree', action='append', default=[])
    args = parser.parse_args()
    files = read_addon(args.base) if args.base else {}
    for spec in args.file:
        dest, source = spec.split('=', 1)
        mode = 0o755
        if ':' in source:
            source, text = source.rsplit(':', 1)
            mode = int(text, 8)
        files[dest] = (mode, Path(source).read_bytes())
    for spec in args.tree:
        dest, source = spec.split('=', 1)
        root = Path(source)
        for path in sorted(p for p in root.rglob('*') if p.is_file()):
            files[dest.rstrip('/') + '/' + path.relative_to(root).as_posix()] = (
                0o755 if os.access(path, os.X_OK) else 0o644, path.read_bytes())
    body = b''
    for name in sorted(files):
        mode, data = files[name]
        encoded = name.encode() + b'\0'
        if len(encoded) > 160 or name.startswith('/') or '..' in name.split('/'):
            raise SystemExit('bad addon path: ' + name)
        packed = zlib.compress(data, 9)
        body += struct.pack('<IIQQ', len(encoded), mode, len(data), len(packed)) + pad(encoded) + pad(packed)
    blob = b'NGPUADD2' + struct.pack('<IIQ', len(files), 0, 24 + len(body)) + body
    args.output.write_bytes(blob)
    print('%s: %d files, %d bytes' % (args.output, len(files), len(blob)))


if __name__ == '__main__':
    main()
