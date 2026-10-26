#!/usr/bin/env python3
"""smem-snap.py: pull a read-only SMEM snapshot and the lower-layer survey rows from a running fajita, through the hub of a
held session.

  smem-snap.py [--out DIR]     kcmd wifi snap (the kernel copies the AP-readable SMEM ranges into RAM), then readv each
                               range, check the CRC32 of the whole 2 MiB image, and write DIR/smem.bin (mode 400) +
                               DIR/rows.txt (kcmd wifi rows). Default DIR .cache/smem-snapshot (gitignored, never committed).
  smem-snap.py --rows          only print the survey rows of the last run (kcmd wifi rows)
  smem-snap.py --rerun         re-run the survey now (kcmd wifi ro) and print its rows

Needs a kernel booted with fajita-wifi=1 (FajitaWifiLower.cpp registers kcmd wifi) and a session (session.sh start). Never
writes to the phone's SMEM; the host tests then parse the snapshot (scripts/check-fajita-wifi-host.py --smem). Stdlib only."""
import argparse
import json
import os
import re
import socket
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SOCK = os.environ.get('FAJITA_USBDBG_HUB', os.path.join(os.environ.get('XDG_RUNTIME_DIR', '/tmp'), 'fjdbg-hub.sock'))
READV = 1520        # bytes per readv (kMaxReplyData 1528)
SIZE = 2 << 20


def hub(op, *args, timeout=20):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout + 10)
    s.connect(SOCK)
    f = s.makefile('rwb')
    f.write((json.dumps({'op': op, 'args': [str(a) for a in args], 'timeout': timeout}) + '\n').encode())
    f.flush()
    rep = json.loads(f.readline())
    s.close()
    return rep['ok'], rep['out']


def all_rows(sub):
    out, start = [], 0
    while True:
        ok, text = hub('kcmd', 'wifi', sub if not start else 'rows', start)
        if not ok:
            raise SystemExit('kcmd wifi %s: %s' % (sub, text.strip()))
        lines = text.rstrip('\n').split('\n') if text.strip() else []
        more = [l for l in lines if l.startswith('more ')]
        out += [l for l in lines if not l.startswith('more ')]
        if not more:
            return out
        start = int(more[0].split()[1])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', default=str(ROOT / '.cache/smem-snapshot'))
    ap.add_argument('--rows', action='store_true')
    ap.add_argument('--rerun', action='store_true')
    a = ap.parse_args()
    if a.rows or a.rerun:
        print('\n'.join(all_rows('ro' if a.rerun else 'rows')))
        return
    ok, out = hub('kcmd', 'wifi', 'snap', timeout=30)
    m = re.match(r'ok va=0x([0-9a-f]+) bytes=(\d+) init=(-?\d+) ranges=(\S*) crc=([0-9a-f]+)', out)
    if not ok or not m:
        raise SystemExit('wifi snap: %s' % out.strip())
    va, size, crc = int(m[1], 16), int(m[2]), int(m[5], 16)
    if size != SIZE:
        raise SystemExit('wifi snap: unexpected size %d' % size)
    img = bytearray(size)
    ranges = [tuple(int(x, 16) for x in r.split('+')) for r in m[4].split(',') if r]
    for off, n in ranges:
        at = 0
        while at < n:
            k = min(READV, n - at)
            ok, hx = hub('readv', hex(va + off + at), k)
            if not ok:
                raise SystemExit('readv %#x: %s' % (va + off + at, hx.strip()))
            img[off + at:off + at + k] = bytes.fromhex(hx.strip())
            at += k
        print('  %#08x+%#x' % (off, n), flush=True)
    if zlib.crc32(bytes(img)) != crc:
        raise SystemExit('smem snapshot: CRC mismatch (%08x != %08x)' % (zlib.crc32(bytes(img)), crc))
    d = Path(a.out)
    d.mkdir(parents=True, exist_ok=True)
    p = d / 'smem.bin'
    if p.exists():
        p.chmod(0o600)
    p.write_bytes(bytes(img))
    p.chmod(0o400)
    (d / 'rows.txt').write_text('\n'.join(all_rows('rows')) + '\n')
    print('smem snapshot: init %s, %d ranges, crc %08x ok -> %s (+ rows.txt)' % (m[3], len(ranges), crc, p))


if __name__ == '__main__':
    main()
