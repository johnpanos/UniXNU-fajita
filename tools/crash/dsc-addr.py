#!/usr/bin/env python3
# tools/crash/dsc-addr.py [--cache DSC] [--slide HEX] ADDR...: which dyld shared cache image (and offset in it) holds each
# address of a kernel CRASH row (PC, LR, bt entries). Reads only the cache's image text table, so it answers in well under a
# second where `ipsw dyld a2s` stalls on the 20H392 cache's local symbols. --slide is the row's "+HEX" field (the shared
# region slide); it is subtracted first. Default DSC: .cache/dsc-20H392/dyld_shared_cache_arm64.
#   tools/crash/dsc-addr.py --slide 28e44000 1afca66cc 1afd0d39c
import argparse, os, struct

BOARD = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ap = argparse.ArgumentParser()
ap.add_argument('--cache', default=os.path.join(BOARD, '.cache/dsc-20H392/dyld_shared_cache_arm64'))
ap.add_argument('--slide', default='0')
ap.add_argument('addr', nargs='+')
a = ap.parse_args()
slide = int(a.slide, 16)
with open(a.cache, 'rb') as f:
    h = f.read(0x100)
    off, cnt = struct.unpack_from('<QQ', h, 0x88)   # imagesTextOffset, imagesTextCount
    f.seek(off)
    d = f.read(cnt * 32)
    images = [struct.unpack_from('<16sQII', d, i * 32)[1:] for i in range(cnt)]   # loadAddress, textSegmentSize, pathOffset
    for s in a.addr:
        x = int(s, 16) - slide
        hit = next(((la, po) for la, sz, po in images if la <= x < la + sz), None)
        if hit is None:
            print(f'{s}: {x:#x} not in any image text')
            continue
        f.seek(hit[1])
        print(f'{s}: {x:#x} {f.read(256).split(b"\0")[0].decode()} +{x - hit[0]:#x}')
