#!/usr/bin/env python3
"""pagematch.py REF SHOT [X0 Y0 X1 Y1]: are two screenshots the same screen, ignoring live content? Prints the share of 20x20 tiles
whose mean per-channel difference is over 24 (0.0-1.0) and exits 0 when it is under 0.40.
Widgets, the Clock icon's hands, the date on the Calendar icon and the status bar change on their own, so a pixel count (scrdiff.py)
calls page 1 "a different screen" when a widget redraws; a different screen (the App Library, the Today view, an app) changes most
tiles. Region defaults to below the status bar and above the dock of a half-resolution screenshot (0 60 540 1000)."""
import sys
from PIL import Image, ImageChops, ImageStat

TILE, LEVEL, SAME = 20, 24, 0.40
a = Image.open(sys.argv[1]).convert('RGB')
b = Image.open(sys.argv[2]).convert('RGB')
box = tuple(int(v) for v in sys.argv[3:7]) if len(sys.argv) >= 7 else (0, 60, 540, 1000)
d = ImageChops.difference(a.crop(box), b.crop(box))
w, h = d.size
tiles = differ = 0
for y in range(0, h - TILE + 1, TILE):
    for x in range(0, w - TILE + 1, TILE):
        tiles += 1
        if max(ImageStat.Stat(d.crop((x, y, x + TILE, y + TILE))).mean) > LEVEL:
            differ += 1
share = differ / tiles if tiles else 1.0
print('%.3f tiles differ (%d of %d)' % (share, differ, tiles))
sys.exit(0 if share < SAME else 1)
