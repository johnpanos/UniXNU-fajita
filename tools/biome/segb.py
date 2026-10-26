#!/usr/bin/env python3
# tools/biome/segb.py FILE... [--frames N]: summarize Biome stream segment files (iOS 16 "SEGB" v1, the files under
# .../Biome/streams/<class>/<Stream>/local/), pulled off the phone with biome-survey.sh PULL=PATH or session.sh pull.
# Per file: header end-of-data offset, frame count by state (1 written, 3 deleted, other), CRC failures, the first/last and min/max
# frame timestamps (Mac absolute time -> UTC), non-monotonic steps and frames from the future relative to --now (unix seconds, default
# the host clock), the largest frames; --frames N prints the first N frames.
# Layout (v1): 56-byte header, int32 end-of-data at 0, 'SEGB' at 52; frames from 56, 8-aligned: int32 length, int32 state,
# double timestamp1, double timestamp2, uint32 crc32(data), int32 pad, then the data.
import argparse, datetime, struct, sys, time, zlib

MAC_EPOCH = 978307200  # 2001-01-01 in unix seconds


def ts(t):
    try:
        return datetime.datetime.fromtimestamp(t + MAC_EPOCH, datetime.timezone.utc).strftime('%Y-%m-%d %H:%M:%S')
    except (OverflowError, OSError, ValueError):
        return 'invalid(%r)' % t


def frames(b):
    off = 56
    while off + 32 <= len(b):
        ln, state, t1, t2, crc, pad = struct.unpack_from('<iiddIi', b, off)
        if ln <= 0 and state == 0:
            return
        if ln < 0 or off + 32 + ln > len(b):
            yield off, ln, state, t1, t2, crc, None
            return
        yield off, ln, state, t1, t2, crc, b[off + 32:off + 32 + ln]
        off += 32 + ln
        off = (off + 7) & ~7


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('files', nargs='+')
    ap.add_argument('--frames', type=int, default=0)
    ap.add_argument('--now', type=float, default=time.time())
    a = ap.parse_args()
    now_mac = a.now - MAC_EPOCH
    for fn in a.files:
        b = open(fn, 'rb').read()
        magic = b[52:56]
        eod = struct.unpack_from('<i', b, 0)[0] if len(b) >= 4 else -1
        print('%s: %d bytes, magic %r, end-of-data %d' % (fn, len(b), magic, eod))
        if magic != b'SEGB':
            print('  not a SEGB v1 segment (first 64 bytes: %s)' % b[:64].hex())
            continue
        states, crcbad, back, future, big, n = {}, 0, 0, 0, [], 0
        first = last = None; tmin = tmax = None; prev = None
        for off, ln, st, t1, t2, crc, data in frames(b):
            if data is None:
                print('  truncated frame at %d (length %d, file %d)' % (off, ln, len(b)))
                break
            n += 1
            states[st] = states.get(st, 0) + 1
            if st == 1 and (zlib.crc32(data) & 0xffffffff) != crc:
                crcbad += 1
            if first is None: first = t1
            last = t1
            tmin = t1 if tmin is None else min(tmin, t1); tmax = t1 if tmax is None else max(tmax, t1)
            if prev is not None and t1 < prev: back += 1
            if t1 > now_mac + 60: future += 1
            prev = t1
            big.append((ln, off))
            if n <= a.frames:
                print('  frame @%d len %d state %d t1 %s (%.3f) t2 %s crc %s' % (off, ln, st, ts(t1), t1, ts(t2),
                      'ok' if (zlib.crc32(data) & 0xffffffff) == crc else 'BAD'))
        print('  %d frames, states %s, crc failures %d, steps back in time %d, from the future %d' % (n, states, crcbad, back, future))
        if n:
            print('  first %s  last %s  min %s  max %s  (now %s)' % (ts(first), ts(last), ts(tmin), ts(tmax), ts(now_mac)))
            big.sort(reverse=True)
            print('  largest frames: %s' % ', '.join('%d@%d' % x for x in big[:5]))


if __name__ == '__main__':
    main()
