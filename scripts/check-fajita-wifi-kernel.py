#!/usr/bin/env python3
"""Compile-check the fajita Wi-Fi units with the real kernel flags, without a full build: replays the compile records a
previous build left in TREE/build/fajita-wifi/compile-commands.json (WifiQmi.c for the C units, FajitaWifiProbe.cpp for
the C++ ones), with this checkout's sources, into .cache/fajita-wifi-kcheck/. Catches what the host tests cannot
(freestanding headers, -mkernel, -Werror under the kernel's clang). Links nothing; the full build is scripts/fajita/build.sh.

  scripts/check-fajita-wifi-kernel.py [--tree TREE] [UNIT...]

TREE defaults to the newest .cache/fajita-out*/unixnu/fajita-ios16/tree (here, or in the ios16-port worktree) with the
records. UNITs default to every unit scripts/build-fajita-wifi.py builds.
"""
import argparse
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WIFI = ROOT / 'drivers/FajitaWifi'
KIT = ROOT / 'package'
OUT = ROOT / '.cache/fajita-wifi-kcheck'
WARNINGS = ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-c99-designator', '-Wno-reorder-init-list',
            '-Wno-deprecated-enum-enum-conversion', '-Wno-unnecessary-virtual-specifier', '-Wno-unknown-warning-option',
            '-Wno-missing-designated-field-initializers']


def find_tree():
    roots = [ROOT, ROOT.parent / 'UniXNU-fajita-ios16']
    trees = [t for r in roots for t in (r / '.cache').glob('fajita-out*/unixnu/fajita-ios16/tree')
             if (t / 'build/fajita-wifi/compile-commands.json').is_file()]
    if not trees:
        raise SystemExit('no build tree with build/fajita-wifi/compile-commands.json; run scripts/fajita/build.sh once')
    return max(trees, key=lambda t: (t / 'build/fajita-wifi/compile-commands.json').stat().st_mtime)


def units():
    """The units build-fajita-wifi.py compiles (parsed from its STACK list and fixed units)."""
    text = (ROOT / 'scripts/build-fajita-wifi.py').read_text()
    stack = eval(text.split('STACK = ', 1)[1].split('\n', 1)[0])
    fixed = ['WifiMdt.c', 'WifiQmi.c', 'WifiQrtr.c', 'WifiBoard.c']
    cpp = ['FajitaWifiProbe.cpp'] + [c for c in eval(text.split('KIT_UNITS = ', 1)[1].split('\n', 1)[0])] \
        if 'KIT_UNITS = ' in text else ['FajitaWifiProbe.cpp']
    return fixed + stack + cpp


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--tree')
    ap.add_argument('unit', nargs='*')
    args = ap.parse_args()
    tree = Path(args.tree) if args.tree else find_tree()
    rec = json.loads((tree / 'build/fajita-wifi/compile-commands.json').read_text())
    OUT.mkdir(parents=True, exist_ok=True)
    objdir = tree / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
    failed = 0
    for unit in args.unit or units():
        cpp = unit.endswith('.cpp')
        base = rec['FajitaWifiProbe.cpp' if cpp else 'WifiQmi.c']
        src = (KIT if cpp else WIFI) / unit
        cmd, i = [], 0
        while i < len(base):
            a = base[i]
            if a.endswith('/FajitaWifiProbe.cpp') or a.endswith('/WifiQmi.c'):
                cmd.append(str(src))
            elif a == '-o':
                cmd += ['-o', str(OUT / (Path(unit).stem + '.o'))]
                i += 1
            elif a == '-I' and i + 1 < len(base) and base[i + 1].endswith(('/kits/fajita', '/drivers/FajitaWifi')):
                cmd += ['-I', str(KIT if base[i + 1].endswith('/kits/fajita') else WIFI)]
                i += 1
            else:
                cmd.append(a)
            i += 1
        cmd += WARNINGS + ([] if cpp else ['-mgeneral-regs-only'])
        cwd = objdir / ('iokit' if cpp else 'osfmk') / 'DEVELOPMENT'
        r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, timeout=180)
        print(('ok   ' if r.returncode == 0 else 'FAIL ') + unit)
        if r.returncode:
            failed += 1
            sys.stdout.write(r.stderr[-3000:])
    print(f'{"PASS" if not failed else "FAIL"}: kernel compile check of {len(args.unit or units())} units against {tree}')
    raise SystemExit(1 if failed else 0)


if __name__ == '__main__':
    main()
