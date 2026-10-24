#!/usr/bin/env python3
"""Pack the SSH-over-USB userland (the ssh-userland CI artifact) into a GPU-addon file
for the boot payload, so the existing boot-time addon mount delivers it: with
sheng-gpu-addon=1 the kernel writes every file into the Data volume under
/private/var/ngpu and union-mounts /private/var/ngpu/daemons over the sealed
/System/Library/LaunchDaemons, where launchd finds org.blu-sharky.sshd.plist and
starts dropbear. The binaries' cdhashes must be in the owner trust cache
(scripts/make-fajita-owner-trust.py), or AMFI refuses them.

  make-fajita-ssh-addon.py --from .cache/ssh-userland --out ssh-addon.bin

The file format is the one scripts/make-gpu-addon.py produces and the kernel addon
installer (patches/xnu/zzy-native-gpu-addon-mount.patch) validates: "NGPUADD2",
u32 count, u32 0, u64 total; per file u32 path-bytes(+NUL,<=160), u32 mode, u64 size,
u64 zlib size, the path padded to 8, the zlib stream padded to 8.

The CI artifact is untrusted input: it is read by path, not executed, and only the
named files are packed.
"""
import argparse
import struct
import zlib
from pathlib import Path

# device path under /private/var/ngpu  ->  (artifact file name, mode)
LAYOUT = [
    ('bin/dropbearmulti', 'dropbearmulti', 0o755),
    ('bin/fajita-reboot', 'fajita-reboot', 0o755),
    ('etc/dropbear_ed25519_host_key', 'dropbear_ed25519_host_key', 0o600),
    ('daemons/org.blu-sharky.sshd.plist', 'org.blu-sharky.sshd.plist', 0o644),
]


def pad(data):
    return data + bytes(-len(data) % 8)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--from', dest='src', required=True, type=Path, help='the downloaded ssh-userland artifact directory')
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()

    files = []
    for device_rel, artifact_name, mode in LAYOUT:
        path = args.src / artifact_name
        if not path.is_file():
            raise SystemExit(f'missing {path} (run/download the ssh-userland workflow first)')
        files.append((device_rel, mode, path.read_bytes()))

    body = b''
    for name, mode, data in files:
        encoded = name.encode() + b'\0'
        if len(encoded) > 160 or '..' in name.split('/') or name.startswith('/'):
            raise SystemExit('Bad addon path ' + name)
        packed = zlib.compress(data, 9)
        body += struct.pack('<IIQQ', len(encoded), mode, len(data), len(packed)) + pad(encoded) + pad(packed)
    blob = b'NGPUADD2' + struct.pack('<IIQ', len(files), 0, 24 + len(body)) + body
    args.out.write_bytes(blob)
    for name, mode, data in files:
        print(f'{mode:o} {len(data):>10} /private/var/ngpu/{name}')
    print(f'{args.out}: {len(files)} files, {len(blob)} bytes')


if __name__ == '__main__':
    main()
