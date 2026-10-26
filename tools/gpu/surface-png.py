#!/usr/bin/env python3
"""surface-png.py RAW WIDTH HEIGHT ROWBYTES OUT.png [--format BGRA|RGBA]: a raw IOSurface dump (fjsurface --dump, pulled with
session.sh pull) as a PNG, plane 0 only, 32 bits per pixel. Alpha is dropped (an all-transparent surface shows as black)."""
import sys

from PIL import Image


def main(argv):
    if len(argv) < 6:
        sys.exit(__doc__)
    raw, width, height, row, out = argv[1], int(argv[2]), int(argv[3]), int(argv[4]), argv[5]
    fmt = argv[argv.index('--format') + 1] if '--format' in argv else 'BGRA'
    data = open(raw, 'rb').read()
    if len(data) < row * height:
        sys.exit(f'{raw}: {len(data)} bytes, need {row * height}')
    img = Image.frombuffer('RGBA', (width, height), data, 'raw', fmt, row, 1)
    img.convert('RGB').save(out)
    alpha = img.getchannel('A').getextrema()
    print(f'{out}: {width}x{height}, alpha range {alpha}')


if __name__ == '__main__':
    main(sys.argv)
