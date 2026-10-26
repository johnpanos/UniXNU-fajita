#!/usr/bin/env python3
"""kvar.py [--km KERNEL] SYMBOL[:BYTES]...: read kernel globals of the running (held) session over the hub (readv), by symbol.

Each SYMBOL (with or without the leading underscore) is looked up in the kernel's symbol table (llvm-nm of the build's kernel.macho,
$KM or --km; default .cache/fajita-out/unixnu/fajita-ios16/kernel/kernel.macho) and BYTES (default 4) are read at it: 4 and 8 print as
unsigned numbers (and hex), anything else as hex. A pattern with '*' reads every matching symbol, e.g. 'native_legacy_retire*'.
  scripts/fajita/test/kvar.py native_legacy_retirement_calls native_legacy_transfer_calls native_gop_gpu_offers:8
"""
import fnmatch
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
HUB = ['python3', '-I', os.path.join(HERE, '..', '..', 'fajita-usbdbg-hub.py'), 'call', '--timeout', '8']


def main():
    args = sys.argv[1:]
    km = os.environ.get('KM', os.path.join(HERE, '..', '..', '..', '.cache', 'fajita-out', 'unixnu', 'fajita-ios16', 'kernel', 'kernel.macho'))
    if args[:1] == ['--km']:
        km, args = args[1], args[2:]
    if not args:
        print(__doc__.strip())
        return 2
    table = {}
    for line in subprocess.run(['llvm-nm', km], capture_output=True, text=True).stdout.splitlines():
        parts = line.split()
        if len(parts) == 3:
            table.setdefault(parts[2].lstrip('_'), int(parts[0], 16))
    bad = 0
    for arg in args:
        name, _, size = arg.partition(':')
        size = int(size or 4)
        names = sorted(n for n in table if fnmatch.fnmatch(n, name.lstrip('_'))) if '*' in name else [name.lstrip('_')]
        for n in names:
            if n not in table:
                print(f'{n}: not in {km}')
                bad += 1
                continue
            out = subprocess.run(HUB + ['readv', hex(table[n]), str(size)], capture_output=True, text=True).stdout.strip()
            try:
                data = bytes.fromhex(out)
            except ValueError:
                print(f'{n}: read failed: {out[:80]}')
                bad += 1
                continue
            if size in (4, 8) and len(data) == size:
                value = int.from_bytes(data, 'little')
                print(f'{n} = {value} ({value:#x})')
            else:
                print(f'{n} = {data.hex()}')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
