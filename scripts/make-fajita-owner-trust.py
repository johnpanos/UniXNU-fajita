#!/usr/bin/env python3
"""Build an owner trust-cache bundle for fajita that carries the two unchanged
original target trust-cache modules byte-for-byte plus one owner module whose
entries are the cdhashes of the SSH-over-USB userland (dropbearmulti, fajita-reboot),
built and signed on macOS CI by scripts/build-ssh-userland.py.

This is the fajita-side counterpart of UniXNU's scripts/make-owner-trust-bundle.py
(which hardcodes the Adreno plugin). It takes the original bundle and the cdhashes as
inputs so it runs on the Linux host with nothing from Apple changed:

  make-fajita-owner-trust.py --original-bundle ORIG.bundle \
      --ssh-trust .cache/ssh-userland/trust.json [--cdhash HEX ...] --out owner-ssh.bundle

--original-bundle is the combined original static+System trust bundle the board
already accepts (the one normally passed as --trust-cache; UniXNU's
build-original-system-trust.py produces it, sha256 7416ca4f...). The output replaces
--trust-cache via --owner-trust-cache, which skips the board allow-list.

The bundle layout and the loader's structural rules match
drivers/DarwinBoot/.../DarwinBoot.c ValidTrustCaches and UniXNU's
make-owner-trust-bundle.py exactly: a u32 module count, a u32 start offset per
module, then each module = u32 version(2) + 16-byte uuid + u32 entry count + entries
of (20-byte cdhash + u32 flags), entries sorted ascending within a module.
"""
import argparse
import hashlib
import json
import struct
import uuid
from pathlib import Path


def valid_like_the_loader(data):
    count = struct.unpack_from('<I', data)[0]
    if not 0 < count <= 128:
        return False
    for index in range(count):
        start = struct.unpack_from('<I', data, 4 + 4 * index)[0]
        end = struct.unpack_from('<I', data, 8 + 4 * index)[0] if index + 1 < count else len(data)
        if start < 4 + 4 * count or start & 3 or end > len(data) or end - start < 24 or \
                struct.unpack_from('<I', data, start)[0] != 2:
            return False
        entries = struct.unpack_from('<I', data, start + 20)[0]
        if end - start != 24 + entries * 24:
            return False
        for entry in range(entries):
            at = start + 24 + entry * 24
            if not 1 <= data[at + 20] <= 4 or (entry and data[at - 24:at - 4] >= data[at:at + 20]):
                return False
    return True


def original_modules(blob):
    """Split the original bundle into its module byte ranges, after validating it."""
    if not valid_like_the_loader(blob):
        raise SystemExit('--original-bundle is not a valid trust-cache bundle')
    count = struct.unpack_from('<I', blob)[0]
    starts = [struct.unpack_from('<I', blob, 4 + 4 * i)[0] for i in range(count)]
    ends = starts[1:] + [len(blob)]
    return [blob[s:e] for s, e in zip(starts, ends)]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--original-bundle', required=True, type=Path)
    parser.add_argument('--ssh-trust', type=Path, help='trust.json from the ssh-userland artifact (cdhashes map)')
    parser.add_argument('--cdhash', action='append', default=[], help='an extra 40-hex cdhash to trust (repeatable)')
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()

    hexes = list(args.cdhash)
    if args.ssh_trust:
        data = json.loads(args.ssh_trust.read_text())
        hexes += list((data.get('cdhashes') or data).values()) if isinstance(data.get('cdhashes', data), dict) else []
    if not hexes:
        raise SystemExit('no cdhashes given (pass --ssh-trust and/or --cdhash)')
    try:
        hashes = sorted({bytes.fromhex(h) for h in hexes})
    except ValueError:
        raise SystemExit('cdhashes must be 40 hex characters each')
    if any(len(h) != 20 for h in hashes):
        raise SystemExit('each cdhash must be 20 bytes (40 hex characters)')

    originals = original_modules(args.original_bundle.read_bytes())

    identifier = uuid.uuid5(uuid.NAMESPACE_URL, 'org.blu-sharky.fajita-ssh:' + ''.join(h.hex() for h in hashes))
    own = struct.pack('<I16sI', 2, identifier.bytes, len(hashes)) + b''.join(h + bytes([2, 0, 0, 0]) for h in hashes)
    modules = [own, *originals]

    cursor, starts = 4 + 4 * len(modules), []
    for module in modules:
        starts.append(cursor)
        cursor += len(module)
    bundle = struct.pack('<%dI' % (1 + len(modules)), len(modules), *starts) + b''.join(modules)
    if not valid_like_the_loader(bundle):
        raise SystemExit('the assembled owner bundle would be refused by the boot loader')

    args.out.write_bytes(bundle)
    args.out.with_suffix('.json').write_text(json.dumps(dict(
        bundle_sha256=hashlib.sha256(bundle).hexdigest(),
        owner_cdhashes=[h.hex() for h in hashes],
        original_modules=len(originals),
        original_modules_unchanged=True,
        purpose='owner trust for the SSH-over-USB userland only'), indent=2) + '\n')
    print('PASS: fajita owner trust bundle (SSH userland + the original modules)',
          hashlib.sha256(bundle).hexdigest())


if __name__ == '__main__':
    main()
