#!/usr/bin/env python3
"""ts.py [N]: per-submission timeline from the ring timestamps (kernel: $KM, default the last build.sh kernel): submit (CPU, WPTR write), CP start, work drained (GPU,
RB_DONE_TS), and the kernel's retire (submit ring [3]); plus acquire detail. Repeat N times (default 1) to sample several windows."""
import os, struct, subprocess, sys, statistics as st
K = os.environ.get('KM') or os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', '.cache', 'fajita-out', 'unixnu', 'fajita-ios16', 'kernel', 'kernel.macho')
HUB = ['python3', '-I', os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'fajita-usbdbg-hub.py'), 'call', '--timeout', '8']
syms = {}
for l in subprocess.run(['llvm-nm', K], capture_output=True, text=True).stdout.splitlines():
    p = l.split()
    if len(p) == 3: syms[p[2]] = int(p[0], 16)
def rv(addr, n):
    out = subprocess.run(HUB + ['readv', hex(addr), str(n)], capture_output=True, text=True).stdout.strip()
    return bytes.fromhex(out.split()[-1])
TICK = 1 / 19.2e6 * 1e3   # ms per 19.2 MHz tick
for rep in range(int(sys.argv[1]) if len(sys.argv) > 1 else 1):
    base = struct.unpack('<Q', rv(syms['_native_adreno_ring_ts_base'], 8))[0]
    slots = struct.unpack('<128Q', rv(base, 1024))
    cpu = struct.unpack('<64Q', rv(syms['_native_adreno_ring_ts_cpu'], 512))
    mach, aon = struct.unpack('<2Q', rv(syms['_native_adreno_ring_clock'], 16))
    acq = struct.unpack('<6Q', rv(syms['_native_adreno_acquire_detail'], 48))
    off = aon - mach   # always-on minus mach, both 19.2 MHz
    rows = []
    for i in range(64):
        s, e, c = slots[2 * i], slots[2 * i + 1], cpu[i]
        if not (s and e and c) or e < s: continue
        rows.append((c, (s - off - c) * TICK, (e - s) * TICK, (e - off - c) * TICK))
    rows.sort()
    if not rows: print('no timestamps'); continue
    q = [r[1] for r in rows]; d = [r[2] for r in rows]; t = [r[3] for r in rows]
    span = (rows[-1][0] - rows[0][0]) * TICK
    busy = 0; last_end = None
    ends = sorted(((r[0] * TICK + r[1], r[0] * TICK + r[3]) for r in rows))
    for a, b in ends:
        if last_end is None or a >= last_end: busy += b - a; last_end = b
        elif b > last_end: busy += b - last_end; last_end = b
    print('%d submits over %.0f ms | submit->CP start med %.1f max %.1f ms | CP start->drained med %.1f max %.1f sum %.0f ms | submit->drained med %.1f ms | GPU busy %.0f%%' %
          (len(rows), span, st.median(q), max(q), st.median(d), max(d), sum(d), st.median(t), 100 * busy / span if span else 0))
    print('acquire calls %d imports %d bytes %.0f MiB us %d (%.2f ms/call) max %.1f ms skipped %d' %
          (acq[0], acq[1], acq[2] / 1048576, acq[3], acq[3] / max(acq[0], 1) / 1000, acq[4] / 1000, acq[5]))
    if os.environ.get('V'):
        for r in rows[-20:]: print('  q %6.1f gpu %6.1f done %6.1f' % r[1:])
