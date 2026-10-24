#!/usr/bin/env python3
"""rows.py [FILTER]: the native Metal frontend's telemetry rows (native_adreno_metal_rows[48][96]) read over the hub (readv)."""
import os, subprocess, sys
HUB = ['python3', '-I', os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'fajita-usbdbg-hub.py'), 'call', '--timeout', '8']
BASE = int(sys.argv[2], 16) if len(sys.argv) > 2 else int(subprocess.run(['llvm-nm', os.environ.get('KM',os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', '.cache', 'fajita-out', 'unixnu', 'fajita-ios16', 'kernel', 'kernel.macho'))], capture_output=True, text=True).stdout.split(' S _native_adreno_metal_rows\n')[0].split()[-1], 16)
data = b''
for off in range(0, 48 * 96, 1024):
    out = subprocess.run(HUB + ['readv', hex(BASE + off), str(min(1024, 48 * 96 - off))], capture_output=True, text=True).stdout.strip()
    data += bytes.fromhex(out.split()[-1]) if out and not out.startswith('status') else b'\0' * min(1024, 48 * 96 - off)
for r in range(48):
    t = data[r * 96:(r + 1) * 96].split(b'\0')[0].decode(errors='replace')
    if t and (len(sys.argv) < 2 or sys.argv[1] in t or sys.argv[1] == '-'):
        print(t)
