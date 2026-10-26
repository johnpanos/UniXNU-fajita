#!/usr/bin/env python3
"""ufspart.py: back up named UFS GPT partitions from a running fajita (read-only), through the hub of a held session.

  ufspart.py find NAME...                 the partition's LUN, range and GUIDs (kcmd ufspart find)
  ufspart.py list LUN                     every entry of LUN's GPT
  ufspart.py backup [--out DIR] [NAME...] read each partition twice, chunk by chunk (kcmd ufspart read + readv, each chunk
                                          CRC-checked), require both passes identical, write DIR/NAME.img + DIR/manifest.json
                                          (default DIR .cache/efs-backup, names modemst1 modemst2 fsg fsc persist)
  ufspart.py read NAME OFF COUNT --to F   COUNT 4096-byte blocks at block OFF of NAME (any size, chunked and CRC-checked) into F,
                                          e.g. the iOS 16 root manifest: read userdata 4194304 1 --to manifest.bin (16 GiB in)
  ufspart.py verify [--out DIR]           re-check the backup against the phone's GPT (size, LUN, range, type and unique
                                          GUIDs) and against the manifest's sha256; exit 1 on any mismatch

Needs a kernel booted with native-ufs-partread=1 (drivers/NativeUfs/PartRead.h) and a session (session.sh start). Never writes
to the phone. Python stdlib only."""
import argparse
import hashlib
import json
import os
import re
import socket
import sys
import time
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SOCK = os.environ.get('FAJITA_USBDBG_HUB', os.path.join(os.environ.get('XDG_RUNTIME_DIR', '/tmp'), 'fjdbg-hub.sock'))
NAMES = ['modemst1', 'modemst2', 'fsg', 'fsc', 'persist']
CHUNK = 32          # blocks per kcmd read (the kernel buffer is 128 KiB)
READV = 1520        # bytes per readv (kMaxReplyData 1528)


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


def touch_activity():
    try:
        with open(SOCK + '.activity', 'a'):
            os.utime(SOCK + '.activity')
    except OSError:
        pass


def find(name):
    ok, out = hub('kcmd', 'ufspart', 'find', name)
    m = re.match(r'(\S+) lun=(\d+) index=(\d+) first=(\d+) last=(\d+) blocks=(\d+) type=(\S+) uniq=(\S+) attr=(\S+) ents=(\S+)', out)
    if not ok or not m:
        raise SystemExit('find %s: %s' % (name, out.strip()))
    return {'name': name, 'lun': int(m[2]), 'index': int(m[3]), 'first': int(m[4]), 'last': int(m[5]), 'blocks': int(m[6]),
            'type': m[7], 'uniq': m[8], 'attr': m[9], 'entries_crc': m[10]}


def read_chunk(name, off, count):
    for attempt in range(3):
        ok, out = hub('kcmd', 'ufspart', 'read', name, off, count, timeout=30)
        m = re.search(r'ok va=(0x[0-9a-f]+) bytes=(\d+) crc=([0-9a-f]+)', out)
        if not ok or not m:
            raise SystemExit('read %s %d+%d: %s' % (name, off, count, out.strip()))
        va, size, crc = int(m[1], 16), int(m[2]), int(m[3], 16)
        parts, at = [], 0
        while at < size:
            n = min(READV, size - at)
            ok, out = hub('readv', hex(va + at), n)
            if not ok:
                break
            parts.append(bytes.fromhex(out.strip()))
            at += n
        data = b''.join(parts)
        if len(data) == size and zlib.crc32(data) == crc:
            return data
        print('  %s chunk %d: crc/size mismatch, retry %d' % (name, off, attempt + 1), flush=True)
    raise SystemExit('read %s %d+%d: 3 bad transfers' % (name, off, count))


def read_part(p):
    h, parts, t0 = hashlib.sha256(), [], time.time()
    for off in range(0, p['blocks'], CHUNK):
        data = read_chunk(p['name'], off, min(CHUNK, p['blocks'] - off))
        h.update(data)
        parts.append(data)
        touch_activity()
        if (off // CHUNK) % 32 == 31:
            print('  %s %d/%d blocks (%.0f s)' % (p['name'], off + CHUNK, p['blocks'], time.time() - t0), flush=True)
    return b''.join(parts), h.hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('cmd', choices=['find', 'list', 'backup', 'verify', 'read'])
    ap.add_argument('names', nargs='*')
    ap.add_argument('--out', default=str(ROOT / '.cache' / 'efs-backup'))
    ap.add_argument('--passes', type=int, default=2)
    ap.add_argument('--to', help='read: the output file')
    a = ap.parse_args()
    out = Path(a.out)
    if a.cmd == 'read':
        if len(a.names) != 3 or not a.to:
            raise SystemExit('read NAME OFF COUNT --to FILE')
        name, off, count = a.names[0], int(a.names[1], 0), int(a.names[2], 0)
        data = b''.join(read_chunk(name, at, min(CHUNK, off + count - at)) for at in range(off, off + count, CHUNK))
        Path(a.to).write_bytes(data)
        print('%s blocks %d+%d: %d bytes sha256 %s -> %s' % (name, off, count, len(data), hashlib.sha256(data).hexdigest(), a.to))
        return 0
    if a.cmd == 'find':
        for n in a.names or NAMES:
            print(json.dumps(find(n)))
        return 0
    if a.cmd == 'list':
        lun = a.names[0] if a.names else '0'
        start = 0
        while True:
            ok, text = hub('kcmd', 'ufspart', 'list', lun, start)
            sys.stdout.write(''.join(l + '\n' for l in text.splitlines() if not l.startswith('more ') and (start == 0 or not l.startswith('lun '))))
            m = re.search(r'^more (\d+)$', text, re.M)
            if not m:
                return 0 if ok else 1
            start = int(m[1])
    if a.cmd == 'backup':
        out.mkdir(parents=True, exist_ok=True)
        manifest = {'when': time.strftime('%Y-%m-%dT%H:%M:%S'), 'parts': {}}
        for n in a.names or NAMES:
            p = find(n)
            print('%s: lun %d blocks %d (%.1f MiB) uniq %s' % (n, p['lun'], p['blocks'], p['blocks'] * 4096 / 1048576, p['uniq']), flush=True)
            digests, data = [], None
            for i in range(a.passes):
                data, d = read_part(p)
                digests.append(d)
                print('  pass %d sha256 %s' % (i + 1, d), flush=True)
            if len(set(digests)) != 1:
                raise SystemExit('%s: passes differ: %s' % (n, digests))
            if len(data) != p['blocks'] * 4096:
                raise SystemExit('%s: %d bytes, GPT says %d' % (n, len(data), p['blocks'] * 4096))
            again = find(n)
            if again != p:
                raise SystemExit('%s: GPT entry changed during the backup' % n)
            (out / (n + '.img')).write_bytes(data)
            os.chmod(out / (n + '.img'), 0o400)
            manifest['parts'][n] = dict(p, bytes=len(data), sha256=digests[0], passes=a.passes)
            (out / 'manifest.json').write_text(json.dumps(manifest, indent=1) + '\n')
        print('backup ok -> %s' % out)
        return 0
    # verify
    manifest = json.loads((out / 'manifest.json').read_text())
    bad = 0
    for n, m in manifest['parts'].items():
        f = out / (n + '.img')
        data = f.read_bytes() if f.exists() else b''
        live = find(n)
        problems = [k for k in ('lun', 'index', 'first', 'last', 'blocks', 'type', 'uniq') if live[k] != m[k]]
        if len(data) != live['blocks'] * 4096:
            problems.append('size %d != GPT %d' % (len(data), live['blocks'] * 4096))
        if hashlib.sha256(data).hexdigest() != m['sha256']:
            problems.append('sha256')
        print('%s %s %d bytes sha256 %s%s' % ('OK ' if not problems else 'BAD', n, len(data), m['sha256'][:16],
                                              '' if not problems else ' (' + ', '.join(problems) + ')'))
        bad += bool(problems)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
