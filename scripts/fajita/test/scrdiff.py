#!/usr/bin/env python3
"""scrdiff.py A B [x0 y0 x1 y1]: 'maxdiff N px>8 M total T' of two screenshots (540-wide, default box below the status bar)."""
import sys
from PIL import Image, ImageChops
box = tuple(int(v) for v in sys.argv[3:7]) if len(sys.argv) >= 7 else (0, 60, 540, 1170)
a = Image.open(sys.argv[1]).convert('RGB').crop(box); b = Image.open(sys.argv[2]).convert('RGB').crop(box)
px = list(ImageChops.difference(a, b).get_flattened_data())
print('maxdiff %d px>8 %d total %d' % (max(max(p) for p in px), sum(1 for p in px if max(p) > 8), len(px)))
