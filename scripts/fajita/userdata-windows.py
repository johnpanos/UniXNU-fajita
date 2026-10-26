#!/usr/bin/env python3
"""userdata-windows.py: a small sparse image that rewrites some windows of the iOS 16 root image in userdata, not all 7 GiB of it.

Usage: scripts/fajita/userdata-windows.py [--windows OS,SystemOS,AppOS] [--write AppOS] [--out DIR]

The root image (`unixnu build userdata`, [userdata] of ios16/board.toml) is a manifest block at base_mib (16 GiB) and the windows
after it, each 1 MiB aligned, in board order. Appending a window leaves the offsets of the windows before it unchanged, so only the
manifest block and the new window differ from the image already on the phone. This writes:

  OUT/userdata-windows.simg   Android sparse image: DONT_CARE up to the base, the manifest block, DONT_CARE up to each --write
                              window, that window's bytes, DONT_CARE to the end of the image. `fastboot flash userdata` writes
                              only those blocks; the rest of userdata (the iPadOS image, the other windows, the Data store and the
                              log at the end) stays as it is.
  OUT/userdata-windows.json   the layout, the manifest's SHA-256, the flash command.
  OUT/manifest.bin            the 4096-byte manifest block alone.

--windows lists the windows of the manifest (default: all of [userdata] windows); --windows OS,SystemOS with no --write is the
two-window manifest of the earlier image (the way back: a kernel without the AppOS policy refuses a three-window image).
--write defaults to every window beyond the first two. The bytes and the manifest are those `unixnu build userdata` writes
(scripts/unixnu/userdata.py of the stack: the same UDIF reader, the same manifest layout): a window's SHA-256 comes from the
verified-root cache (.cache/verified-root/<name>/image.json) when its size matches, else it is hashed from the image.
Nothing is flashed; the phone must be in fastboot to run the printed command.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import struct
import sys
import tomllib

BOARD = Path(__file__).resolve().parents[2]
STACK = Path(os.environ.get('FAJITA_STACK') or Path.home() / 'src/UniXNU-fajita-stack')
sys.path.insert(0, str(STACK / 'scripts'))
from unixnu import userdata  # noqa: E402  the UDIF reader and sparse constants of the userdata step

MIB = 1 << 20
BLOCK = 4096


def member(manifest, product, device_class, component):
    for identity in manifest['BuildIdentities']:
        info = identity.get('Info', {})
        if info.get('DeviceClass', '').lower() == device_class and info.get('RestoreBehavior') == 'Erase' and component in identity['Manifest']:
            return identity['Manifest'][component]['Info']['Path']
    raise SystemExit(f'{component}: not in the erase identity of {product} ({device_class})')


def digest(name, dmg, size):
    cached = BOARD / '.cache/verified-root' / name.lower() / 'image.json'
    if cached.is_file():
        meta = json.loads(cached.read_text())
        if meta.get('image_bytes') == size:
            return bytes.fromhex(meta['image_sha256'])
    h = hashlib.sha256()
    for piece in userdata.read_image(dmg, size):
        h.update(piece)
    return h.digest()


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--board', default=str(BOARD / 'ios16/board.toml'))
    parser.add_argument('--windows', help='comma-separated window names of the manifest, in order (default: all of [userdata])')
    parser.add_argument('--write', help='comma-separated windows whose bytes go into the image (default: all beyond the first two)')
    parser.add_argument('--out', default=str(Path(os.environ.get('FAJITA_OUT') or BOARD / '.cache/fajita-out') / 'userdata-windows'))
    args = parser.parse_args()
    board = tomllib.loads(Path(args.board).read_text())
    spec, ipsw = board['userdata'], board['ipsw']
    cache = BOARD / '.cache/ipsw' / ipsw['build']
    build_manifest = plistlib.loads((cache / 'BuildManifest.plist').read_bytes())
    windows = spec['windows']
    if args.windows:
        wanted = args.windows.split(',')
        by_name = {w['name']: w for w in windows}
        missing = [n for n in wanted if n not in by_name]
        if missing:
            raise SystemExit(f'not in [userdata] windows: {missing}')
        windows = [by_name[n] for n in wanted]
    write = set(args.write.split(',')) if args.write is not None else {w['name'] for w in windows[2:]}
    if args.write == '':
        write = set()
    if write - {w['name'] for w in windows}:
        raise SystemExit(f'--write names a window outside the manifest: {sorted(write)}')
    base = int(spec['base_mib']) * MIB
    pins = ipsw.get('sha256', {})
    layout, cursor = [], MIB
    for window in windows:
        path = member(build_manifest, ipsw['product'], ipsw['device_class'], window['component'])
        dmg = cache / path
        if not dmg.is_file():
            raise SystemExit(f'{dmg} missing (fetch {window["component"]} into {cache})')
        if path in pins and window['name'] in write:
            got = hashlib.sha256(dmg.read_bytes()).hexdigest() if dmg.stat().st_size < 64 * MIB else None
            if got and got != pins[path]:
                raise SystemExit(f'{path}: sha256 {got}, board.toml pins {pins[path]}')
        size = userdata.image_size(dmg)
        layout.append((window['name'], dmg, cursor, size, digest(window['name'], dmg, size)))
        cursor += (size + MIB - 1) // MIB * MIB
    total = cursor
    header = bytearray(BLOCK)
    header[0:8] = b'SHNGROOT'
    struct.pack_into('<IIQ', header, 8, 2 if base else 1, len(layout), total)
    for index, (name, _, offset, size, sha) in enumerate(layout):
        struct.pack_into('<32sQQII32s40s', header, 24 + 128 * index, name.encode(), offset, size, 512, 1, sha, bytes(40))
    if base:
        struct.pack_into('<Q', header, 4056, base)
    header[4064:4096] = hashlib.sha256(bytes(header[:4064])).digest()

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    (out / 'manifest.bin').write_bytes(header)
    # Chunks: (kind, blocks, source) with source None for DONT_CARE, bytes for the manifest, (dmg, size) for a window.
    chunks, at = [], 0                     # at: blocks into the image
    if base:
        chunks.append((userdata.DONT_CARE_CHUNK, base // BLOCK, None))
    chunks.append((userdata.RAW_CHUNK, 1, bytes(header)))
    at = 1
    for name, dmg, offset, size, _ in layout:
        if name not in write:
            continue
        if offset // BLOCK > at:
            chunks.append((userdata.DONT_CARE_CHUNK, offset // BLOCK - at, None))
        blocks = (size + BLOCK - 1) // BLOCK
        chunks.append((userdata.RAW_CHUNK, blocks, (dmg, size)))
        at = offset // BLOCK + blocks
    if total // BLOCK > at:
        chunks.append((userdata.DONT_CARE_CHUNK, total // BLOCK - at, None))
    image = out / 'userdata-windows.simg'
    with image.open('wb') as target:
        target.write(struct.pack('<IHHHHIIII', userdata.SPARSE_MAGIC, 1, 0, 28, 12, BLOCK, (base + total) // BLOCK, len(chunks), 0))
        for kind, blocks, source in chunks:
            payload = blocks * BLOCK if kind == userdata.RAW_CHUNK else 0
            target.write(struct.pack('<HHII', kind, 0, blocks, 12 + payload))
            if isinstance(source, bytes):
                target.write(source)
            elif source:
                dmg, size = source
                written = 0
                for piece in userdata.read_image(dmg, size):
                    target.write(piece)
                    written += len(piece)
                if written != size:
                    raise SystemExit(f'{dmg.name}: read {written} bytes, expected {size}')
                target.write(bytes(payload - size))
    report = dict(board=str(Path(args.board).relative_to(BOARD)), base_bytes=base, image_bytes=total, manifest_version=2 if base else 1,
                  manifest_sha256=hashlib.sha256(bytes(header)).hexdigest(),
                  windows=[dict(name=n, member=d.name, offset=o, size=s, sha256=h.hex(), written=n in write) for n, d, o, s, h in layout],
                  sparse=str(image), sparse_bytes=image.stat().st_size, flash_command=f'fastboot flash userdata {image}')
    (out / 'userdata-windows.json').write_text(json.dumps(report, indent=2) + '\n')
    for w in report['windows']:
        print(f'{w["name"]:9} {w["member"]:20} offset {w["offset"]:#12x} size {w["size"]:#12x} {"written" if w["written"] else "kept"}')
    print(f'manifest {report["manifest_sha256"]} ({len(layout)} windows, image {total} bytes at {base // MIB} MiB)')
    print(f'{image}: {report["sparse_bytes"]} bytes; flash with: {report["flash_command"]}')


if __name__ == '__main__':
    main()
