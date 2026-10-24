#!/usr/bin/env python3
# pack.py OUT.bin DIR... : archive app bundles for `fsx unpack` (records: "D <mode> <len>\npath" / "F <mode> <size> <len>\npath"+data). Drops non-English .lproj unless KEEP_LPROJ=1.
import os, sys, struct
out = open(sys.argv[1], 'wb'); keep = os.environ.get('KEEP_LPROJ')
def rec(kind, path, mode, data=None):
    p = path.encode()
    out.write(f"{kind} {mode:o} {len(data) if data is not None else 0} {len(p)}\n".encode() + p + (data or b''))
for top in sys.argv[2:]:
    base = os.path.dirname(os.path.abspath(top))
    for r, ds, fs in os.walk(top):
        ds[:] = [d for d in ds if keep or not d.endswith('.lproj') or d in ('en.lproj', 'Base.lproj', 'English.lproj')]
        rel = os.path.relpath(r, base); rec('D', rel, os.stat(r).st_mode & 0o7777)
        for f in fs:
            p = os.path.join(r, f)
            if os.path.islink(p): continue
            rec('F', os.path.relpath(p, base), os.stat(p).st_mode & 0o7777, open(p, 'rb').read())
