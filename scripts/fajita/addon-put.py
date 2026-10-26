#!/usr/bin/env python3
"""addon-put.py: add files to a boot GPU addon (NGPUADD2, scripts/make-gpu-addon.py) in place.

  scripts/fajita/addon-put.py ADDON [--drop PREFIX]... [DEST=SRC[:MODE]]...

The kernel's addon installer (ios16/patches/xnu-8796/zzy-native-gpu-addon-mount.patch, sheng-gpu-addon=1) writes each entry
to /private/var/ngpu/DEST at boot. --drop removes entries whose path starts with PREFIX first, so a rebuild that reuses the
addon is idempotent. MODE is octal (default 755). Used by scripts/fajita/build.sh for fajita-wifid=1: fjdbg/wifid.real, the
iOS 20H392 /usr/sbin/wifid from the extracted root (.cache/mnt/os20H392/root; Apple material, never committed), which the
USB debug agent execs as the com.apple.wifid job (package/usb-debug-tools/fjdbg-agent.c). Inputs are read, never executed.
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from importlib import import_module  # noqa: E402

usbdbg = import_module('make-fajita-usbdbg-addon')   # read_addon / write_addon (the same format and checks)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('addon', type=Path)
    ap.add_argument('--drop', action='append', default=[])
    ap.add_argument('put', nargs='*')
    a = ap.parse_args()
    files = usbdbg.read_addon(a.addon.read_bytes())
    files = [f for f in files if not any(f[0].startswith(p) for p in a.drop)]
    for spec in a.put:
        dest, _, rest = spec.partition('=')
        src, _, mode = rest.partition(':')
        data = Path(src).read_bytes()
        files = [f for f in files if f[0] != dest] + [(dest, int(mode or '755', 8), data)]
    blob = usbdbg.write_addon(files)
    a.addon.write_bytes(blob)
    print(f'{a.addon}: {len(files)} files, {len(blob)} bytes')


if __name__ == '__main__':
    main()
